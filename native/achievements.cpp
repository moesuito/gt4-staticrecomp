#include "achievements.hpp"
#include "achievements_overlay.hpp"

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"

#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace gt4::achievements {
namespace {

bool parse_bool_str(const std::string& raw, bool default_val) {
    if (raw.empty()) return default_val;
    std::string lower;
    lower.reserve(raw.size());
    for (unsigned char c : raw) {
        if (!std::isspace(c)) {
            lower.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") return true;
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") return false;
    return default_val;
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), needed);
    return out;
}

std::string sanitize_badge_filename(const char* name) {
    if (!name || !*name) return "default";
    std::string out;
    for (const char* p = name; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        }
    }
    return out.empty() ? "default" : out;
}

struct HttpJob {
    bool is_badge = false;
    std::string url;
    std::string post_data;
    std::string content_type;
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    std::wstring badge_dest_path;
};

struct CompletedHttpJob {
    bool is_badge = false;
    std::string body;
    int http_status = 0;
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    std::wstring badge_dest_path;
    bool badge_ok = false;
};

struct HttpResult {
    int status_code = 0;
    std::string body;
};

HttpResult perform_winhttp_request(
    const std::string& url,
    const std::string& post_data,
    const std::string& content_type,
    const std::wstring& user_agent) {
    HttpResult result{};
    const std::wstring w_url = utf8_to_wide(url);
    if (w_url.empty()) return result;

    URL_COMPONENTS url_comp{};
    url_comp.dwStructSize = sizeof(url_comp);
    wchar_t host_buf[256]{};
    wchar_t path_buf[2048]{};
    url_comp.lpszHostName = host_buf;
    url_comp.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
    url_comp.lpszUrlPath = path_buf;
    url_comp.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));

    if (!WinHttpCrackUrl(w_url.c_str(), static_cast<DWORD>(w_url.size()), 0, &url_comp)) {
        return result;
    }

    HINTERNET h_session = WinHttpOpen(
        user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!h_session) return result;

    WinHttpSetTimeouts(h_session, 8000, 8000, 10000, 15000);

    HINTERNET h_connect = WinHttpConnect(h_session, host_buf, url_comp.nPort, 0);
    if (!h_connect) {
        WinHttpCloseHandle(h_session);
        return result;
    }

    const bool is_post = !post_data.empty();
    const DWORD flags = (url_comp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET h_request = WinHttpOpenRequest(
        h_connect,
        is_post ? L"POST" : L"GET",
        path_buf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!h_request) {
        WinHttpCloseHandle(h_connect);
        WinHttpCloseHandle(h_session);
        return result;
    }

    std::wstring headers;
    if (is_post && !content_type.empty()) {
        headers = L"Content-Type: " + utf8_to_wide(content_type) + L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(
        h_request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        is_post ? const_cast<char*>(post_data.data()) : WINHTTP_NO_REQUEST_DATA,
        is_post ? static_cast<DWORD>(post_data.size()) : 0,
        is_post ? static_cast<DWORD>(post_data.size()) : 0,
        0);

    if (sent && WinHttpReceiveResponse(h_request, nullptr)) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (WinHttpQueryHeaders(
                h_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status_code,
                &status_size,
                WINHTTP_NO_HEADER_INDEX)) {
            result.status_code = static_cast<int>(status_code);
        }

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(h_request, &avail) && avail > 0) {
            const size_t offset = result.body.size();
            result.body.resize(offset + avail);
            DWORD read_bytes = 0;
            if (!WinHttpReadData(h_request, result.body.data() + offset, avail, &read_bytes)) {
                result.body.resize(offset);
                break;
            }
            result.body.resize(offset + read_bytes);
        }
    } else {
        result.status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }

    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return result;
}

class RuntimeManager {
public:
    static RuntimeManager& instance() {
        static RuntimeManager mgr;
        return mgr;
    }

    void init(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size) {
        shutdown();

        cfg_ = config_from_environment();
        if (!cfg_.enabled) {
            return;
        }
        if (cfg_.username.empty() || (cfg_.token.empty() && cfg_.password.empty())) {
            std::fprintf(stderr, "[RA] RetroAchievements enabled but credentials are missing; skipping session.\n");
            return;
        }

        ram_ = ram;
        ram_size_ = ram_size;
        spr_ = spr;
        spr_size_ = spr_size;

        if (cfg_.cache_dir.empty()) {
            wchar_t temp_buf[MAX_PATH]{};
            if (GetTempPathW(MAX_PATH, temp_buf) > 0) {
                cfg_.cache_dir = std::wstring(temp_buf) + L"GT4Recomp\\ra_cache";
            }
        }
        if (!cfg_.cache_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(cfg_.cache_dir) / L"badges", ec);
        }

        overlay_.initialize(game_window);

        client_ = rc_client_create(&RuntimeManager::read_memory_thunk, &RuntimeManager::server_call_thunk);
        if (!client_) {
            std::fprintf(stderr, "[RA] Failed to allocate rc_client.\n");
            overlay_.shutdown();
            return;
        }

        rc_client_set_userdata(client_, this);
        rc_client_enable_logging(client_, RC_CLIENT_LOG_LEVEL_INFO, &RuntimeManager::log_thunk);
        rc_client_set_event_handler(client_, &RuntimeManager::event_thunk);
        rc_client_set_hardcore_enabled(client_, cfg_.hardcore ? 1 : 0);
        rc_client_set_encore_mode_enabled(client_, cfg_.encore ? 1 : 0);
        if (!cfg_.host.empty()) {
            rc_client_set_host(client_, cfg_.host.c_str());
        }

        char ua_clause[64]{};
        rc_client_get_user_agent_clause(client_, ua_clause, sizeof(ua_clause));
        std::string ua = std::string("GT4Recomp/1.2.1 ") + ua_clause;
        user_agent_ = utf8_to_wide(ua);

        stop_worker_.store(false);
        worker_thread_ = std::thread(&RuntimeManager::worker_loop, this);
        active_.store(true);

        std::fprintf(
            stderr,
            "[RA] Signing in as '%s' (hardcore=%d, encore=%d)...\n",
            cfg_.username.c_str(),
            cfg_.hardcore ? 1 : 0,
            cfg_.encore ? 1 : 0);

        if (!cfg_.token.empty()) {
            rc_client_begin_login_with_token(
                client_,
                cfg_.username.c_str(),
                cfg_.token.c_str(),
                &RuntimeManager::login_callback_thunk,
                this);
        } else {
            rc_client_begin_login_with_password(
                client_,
                cfg_.username.c_str(),
                cfg_.password.c_str(),
                &RuntimeManager::login_callback_thunk,
                this);
        }
    }

    void tick(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size) {
        if (!active_.load(std::memory_order_relaxed) || !client_) return;
        ram_ = ram;
        ram_size_ = ram_size;
        spr_ = spr;
        spr_size_ = spr_size;

        drain_completed_jobs();

        if (game_loaded_) {
            rc_client_do_frame(client_);
        } else {
            rc_client_idle(client_);
        }

        overlay_.tick(game_window);
    }

    void on_pos_changed(HWND game_window) {
        if (!active_.load(std::memory_order_relaxed)) return;
        overlay_.on_owner_pos_changed(game_window);
    }

    void reset() {
        if (!active_.load(std::memory_order_relaxed) || !client_) return;
        overlay_.clear_session_indicators();
        rc_client_reset(client_);
    }

    void shutdown() {
        active_.store(false);
        game_loaded_ = false;

        if (client_) {
            rc_client_unload_game(client_);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_worker_.store(true);
            pending_jobs_.clear();
        }
        queue_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(completed_mutex_);
            completed_jobs_.clear();
        }
        requested_badges_.clear();

        if (client_) {
            rc_client_destroy(client_);
            client_ = nullptr;
        }
        overlay_.shutdown();
    }

    bool is_active() const {
        return active_.load(std::memory_order_relaxed);
    }

private:
    static uint32_t RC_CCONV read_memory_thunk(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
        auto* self = static_cast<RuntimeManager*>(rc_client_get_userdata(client));
        if (!self) return 0;
        return read_ps2_memory(address, buffer, num_bytes, self->ram_, self->ram_size_, self->spr_, self->spr_size_);
    }

    static void RC_CCONV server_call_thunk(
        const rc_api_request_t* request,
        rc_client_server_callback_t callback,
        void* callback_data,
        rc_client_t* client) {
        auto* self = static_cast<RuntimeManager*>(rc_client_get_userdata(client));
        if (!self || !request) return;

        HttpJob job;
        job.is_badge = false;
        job.url = request->url ? request->url : "";
        job.post_data = request->post_data ? request->post_data : "";
        job.content_type = request->content_type ? request->content_type : "application/x-www-form-urlencoded";
        job.callback = callback;
        job.callback_data = callback_data;

        {
            std::lock_guard<std::mutex> lock(self->queue_mutex_);
            self->pending_jobs_.push_back(std::move(job));
        }
        self->queue_cv_.notify_one();
    }

    static void RC_CCONV log_thunk(const char* message, const rc_client_t* /*client*/) {
        if (message) {
            std::fprintf(stderr, "[RA] %s\n", message);
        }
    }

    static void RC_CCONV login_callback_thunk(int result, const char* error_message, rc_client_t* client, void* userdata) {
        auto* self = static_cast<RuntimeManager*>(userdata);
        if (!self) return;
        self->on_login_complete(result, error_message, client);
    }

    static void RC_CCONV load_game_callback_thunk(int result, const char* error_message, rc_client_t* client, void* userdata) {
        auto* self = static_cast<RuntimeManager*>(userdata);
        if (!self) return;
        self->on_load_game_complete(result, error_message, client);
    }

    static void RC_CCONV event_thunk(const rc_client_event_t* event, rc_client_t* client) {
        auto* self = static_cast<RuntimeManager*>(rc_client_get_userdata(client));
        if (!self || !event) return;
        self->on_client_event(event);
    }

    void on_login_complete(int result, const char* error_message, rc_client_t* client) {
        if (result != RC_OK) {
            std::fprintf(stderr, "[RA] Login failed (%d): %s\n", result, error_message ? error_message : "unknown error");
            OverlayToast toast;
            toast.kind = ToastKind::Warning;
            toast.header = "RETROACHIEVEMENTS";
            toast.title = "Sign-in failed";
            toast.subtitle = error_message ? error_message : "Check your account credentials in the launcher.";
            toast.duration_sec = 5.5;
            overlay_.enqueue_toast(std::move(toast));
            return;
        }

        const rc_client_user_t* user = rc_client_get_user_info(client);
        if (user) {
            std::wstring avatar_path;
            char avatar_url[256]{};
            if (rc_client_user_get_image_url(user, avatar_url, sizeof(avatar_url)) == RC_OK) {
                avatar_path = request_badge_download(std::string("user_") + sanitize_badge_filename(user->username), avatar_url);
            }

            const uint32_t pts = cfg_.hardcore ? user->score : user->score_casual;
            std::ostringstream sub;
            sub << (user->display_name ? user->display_name : user->username) << "  ·  " << pts << " pts";

            OverlayToast toast;
            toast.kind = ToastKind::Login;
            toast.header = cfg_.hardcore ? "RETROACHIEVEMENTS · HARDCORE" : "RETROACHIEVEMENTS · SOFTCORE";
            toast.title = "Signed in";
            toast.subtitle = sub.str();
            toast.badge_path = avatar_path;
            toast.hardcore = cfg_.hardcore;
            toast.duration_sec = 4.0;
            overlay_.enqueue_toast(std::move(toast));
        }

        std::string hash_to_load = resolve_game_hash();
        std::fprintf(stderr, "[RA] Loading game hash %s...\n", hash_to_load.c_str());
        rc_client_begin_load_game(client_, hash_to_load.c_str(), &RuntimeManager::load_game_callback_thunk, this);
    }

    void on_load_game_complete(int result, const char* error_message, rc_client_t* client) {
        if (result != RC_OK) {
            std::fprintf(stderr, "[RA] Load game failed (%d): %s\n", result, error_message ? error_message : "unknown error");
            OverlayToast toast;
            toast.kind = ToastKind::Warning;
            toast.header = "RETROACHIEVEMENTS";
            toast.title = "Could not load achievement set";
            toast.subtitle = error_message ? error_message : "Failed to load Gran Turismo 4 achievements.";
            toast.duration_sec = 5.5;
            overlay_.enqueue_toast(std::move(toast));
            return;
        }

        game_loaded_ = true;

        const rc_client_game_t* game = rc_client_get_game_info(client);
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);

        std::wstring game_badge_path;
        if (game) {
            char badge_url[256]{};
            if (rc_client_game_get_image_url(game, badge_url, sizeof(badge_url)) == RC_OK) {
                game_badge_path = request_badge_download(
                    std::string("game_") + sanitize_badge_filename(game->badge_name),
                    badge_url);
            }
        }

        // Pre-cache active/unlocked achievement badges so unlock popups render with the icon immediately.
        prefetch_achievement_badges();

        std::ostringstream sub;
        sub << "Unlocked " << summary.num_unlocked_achievements << " of " << summary.num_promoted_achievements
            << " (" << summary.points_unlocked << "/" << summary.points_available << " pts)";

        OverlayToast toast;
        toast.kind = ToastKind::GameLoaded;
        toast.header = cfg_.hardcore ? "GRAN TURISMO 4 · HARDCORE" : "GRAN TURISMO 4 · SOFTCORE";
        toast.title = (game && game->title && *game->title) ? game->title : "Gran Turismo 4";
        toast.subtitle = sub.str();
        toast.badge_path = game_badge_path;
        toast.hardcore = cfg_.hardcore;
        toast.duration_sec = 4.5;
        overlay_.enqueue_toast(std::move(toast));
    }

    void prefetch_achievement_badges() {
        if (!client_) return;
        rc_client_achievement_list_t* list = rc_client_create_achievement_list(
            client_,
            RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED_AND_UNPROMOTED,
            RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
        if (!list) return;

        size_t prefetched = 0;
        for (uint32_t b = 0; b < list->num_buckets && prefetched < 24; ++b) {
            const auto& bucket = list->buckets[b];
            for (uint32_t i = 0; i < bucket.num_achievements && prefetched < 24; ++i) {
                const rc_client_achievement_t* ach = bucket.achievements[i];
                if (!ach || !ach->badge_name || !*ach->badge_name) continue;
                char url[256]{};
                if (rc_client_achievement_get_image_url(ach, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url)) == RC_OK) {
                    request_badge_download(std::string("ach_") + sanitize_badge_filename(ach->badge_name), url);
                    ++prefetched;
                }
            }
        }
        rc_client_destroy_achievement_list(list);
    }

    std::wstring badge_path_for_achievement(const rc_client_achievement_t* ach, int state) {
        if (!ach || !ach->badge_name || !*ach->badge_name) return {};
        char url[256]{};
        if (rc_client_achievement_get_image_url(ach, state, url, sizeof(url)) != RC_OK) {
            return {};
        }
        std::string key = std::string("ach_") + sanitize_badge_filename(ach->badge_name);
        if (state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
            key += "_lock";
        }
        return request_badge_download(key, url);
    }

    std::wstring request_badge_download(const std::string& cache_key, const std::string& url) {
        if (cfg_.cache_dir.empty() || cache_key.empty() || url.empty()) return {};
        std::filesystem::path dest = std::filesystem::path(cfg_.cache_dir) / L"badges" / (utf8_to_wide(cache_key) + L".png");
        std::wstring dest_w = dest.wstring();

        const DWORD attrs = GetFileAttributesW(dest_w.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return dest_w;
        }

        if (requested_badges_.insert(cache_key).second) {
            HttpJob job;
            job.is_badge = true;
            job.url = url;
            job.badge_dest_path = dest_w;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                pending_jobs_.push_back(std::move(job));
            }
            queue_cv_.notify_one();
        }
        return dest_w;
    }

    void on_client_event(const rc_client_event_t* event) {
        const bool hardcore = rc_client_get_hardcore_enabled(client_) != 0;

        switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
            const rc_client_achievement_t* ach = event->achievement;
            if (!ach) break;
            std::ostringstream title;
            title << (ach->title ? ach->title : "Achievement") << " (" << ach->points << " pts)";

            OverlayToast toast;
            toast.kind = ToastKind::AchievementUnlocked;
            toast.header = hardcore ? "HARDCORE ACHIEVEMENT UNLOCKED" : "ACHIEVEMENT UNLOCKED";
            toast.title = title.str();
            toast.subtitle = ach->description ? ach->description : "";
            toast.badge_path = badge_path_for_achievement(ach, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
            toast.hardcore = hardcore;
            toast.duration_sec = 5.5;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_GAME_COMPLETED: {
            const rc_client_game_t* game = rc_client_get_game_info(client_);
            rc_client_user_game_summary_t summary{};
            rc_client_get_user_game_summary(client_, &summary);
            std::ostringstream sub;
            sub << "All " << summary.num_promoted_achievements << " achievements unlocked (" << summary.points_available << " pts)!";

            OverlayToast toast;
            toast.kind = ToastKind::Mastery;
            toast.header = hardcore ? "GAME MASTERED · HARDCORE" : "GAME COMPLETED · SOFTCORE";
            toast.title = (game && game->title) ? game->title : "Gran Turismo 4";
            toast.subtitle = sub.str();
            toast.hardcore = hardcore;
            toast.duration_sec = 7.0;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE: {
            const rc_client_achievement_t* ach = event->achievement;
            if (!ach) break;
            ProgressIndicatorState st;
            st.visible = true;
            st.id = ach->id;
            st.title = ach->title ? ach->title : "Achievement Progress";
            st.progress_text = ach->measured_progress ? ach->measured_progress : "";
            st.badge_path = badge_path_for_achievement(ach, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
            overlay_.set_progress_indicator(st);
            break;
        }

        case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE: {
            ProgressIndicatorState st;
            st.visible = false;
            overlay_.set_progress_indicator(st);
            break;
        }

        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW: {
            const rc_client_achievement_t* ach = event->achievement;
            if (!ach) break;
            ChallengeIndicatorItem item;
            item.id = ach->id;
            item.title = ach->title ? ach->title : "";
            item.badge_path = badge_path_for_achievement(ach, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
            overlay_.add_challenge_indicator(item);
            break;
        }

        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE: {
            const rc_client_achievement_t* ach = event->achievement;
            if (!ach) break;
            overlay_.remove_challenge_indicator(ach->id);
            break;
        }

        case RC_CLIENT_EVENT_LEADERBOARD_STARTED: {
            const rc_client_leaderboard_t* lb = event->leaderboard;
            if (!lb) break;
            OverlayToast toast;
            toast.kind = ToastKind::Leaderboard;
            toast.header = "LEADERBOARD STARTED";
            toast.title = lb->title ? lb->title : "Leaderboard";
            toast.subtitle = lb->description ? lb->description : "";
            toast.hardcore = hardcore;
            toast.duration_sec = 3.5;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED: {
            const rc_client_leaderboard_t* lb = event->leaderboard;
            if (!lb) break;
            std::string title = lb->title ? lb->title : "Leaderboard";
            if (lb->tracker_value && *lb->tracker_value) {
                title += std::string(" · ") + lb->tracker_value;
            }
            OverlayToast toast;
            toast.kind = ToastKind::Leaderboard;
            toast.header = "LEADERBOARD SUBMITTED";
            toast.title = title;
            toast.subtitle = lb->description ? lb->description : "";
            toast.hardcore = hardcore;
            toast.duration_sec = 4.5;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_LEADERBOARD_FAILED: {
            const rc_client_leaderboard_t* lb = event->leaderboard;
            if (!lb) break;
            OverlayToast toast;
            toast.kind = ToastKind::Info;
            toast.header = "LEADERBOARD CANCELED";
            toast.title = lb->title ? lb->title : "Leaderboard";
            toast.subtitle = "Attempt canceled.";
            toast.duration_sec = 3.0;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE: {
            const rc_client_leaderboard_tracker_t* tr = event->leaderboard_tracker;
            if (!tr) break;
            LeaderboardTrackerItem item;
            item.id = tr->id;
            item.display = tr->display;
            overlay_.set_leaderboard_tracker(item);
            break;
        }

        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE: {
            const rc_client_leaderboard_tracker_t* tr = event->leaderboard_tracker;
            if (!tr) break;
            overlay_.remove_leaderboard_tracker(tr->id);
            break;
        }

        case RC_CLIENT_EVENT_DISCONNECTED: {
            OverlayToast toast;
            toast.kind = ToastKind::Warning;
            toast.header = "RETROACHIEVEMENTS";
            toast.title = "Server connection interrupted";
            toast.subtitle = "Pending unlocks will be retried automatically.";
            toast.duration_sec = 4.5;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        case RC_CLIENT_EVENT_RECONNECTED: {
            OverlayToast toast;
            toast.kind = ToastKind::Info;
            toast.header = "RETROACHIEVEMENTS";
            toast.title = "Reconnected to server";
            toast.subtitle = "All pending unlocks have been synced.";
            toast.duration_sec = 3.5;
            overlay_.enqueue_toast(std::move(toast));
            break;
        }

        default:
            break;
        }
    }

    std::string resolve_game_hash() const {
        if (!cfg_.game_hash.empty() && cfg_.game_hash.size() == 32) {
            return cfg_.game_hash;
        }
        if (!cfg_.disc_path.empty()) {
            char computed[33]{};
            if (rc_hash_generate_from_file(computed, RC_CONSOLE_PLAYSTATION_2, cfg_.disc_path.c_str()) != 0 &&
                std::strlen(computed) == 32) {
                return std::string(computed);
            }
        }
        return std::string(kGt4NtscUHash);
    }

    void worker_loop() {
        while (true) {
            HttpJob job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] { return stop_worker_.load() || !pending_jobs_.empty(); });
                if (stop_worker_.load() && pending_jobs_.empty()) {
                    break;
                }
                job = std::move(pending_jobs_.front());
                pending_jobs_.pop_front();
            }

            if (job.is_badge) {
                HttpResult http = perform_winhttp_request(job.url, "", "", user_agent_);
                bool saved = false;
                if (http.status_code == 200 && !http.body.empty() && !job.badge_dest_path.empty()) {
                    std::wstring tmp_path = job.badge_dest_path + L".tmp";
                    std::ofstream out(std::filesystem::path(tmp_path), std::ios::binary | std::ios::trunc);
                    if (out) {
                        out.write(http.body.data(), static_cast<std::streamsize>(http.body.size()));
                        out.close();
                        if (MoveFileExW(tmp_path.c_str(), job.badge_dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                            saved = true;
                        } else {
                            DeleteFileW(tmp_path.c_str());
                        }
                    }
                }
                CompletedHttpJob done;
                done.is_badge = true;
                done.badge_dest_path = std::move(job.badge_dest_path);
                done.badge_ok = saved;
                std::lock_guard<std::mutex> lock(completed_mutex_);
                completed_jobs_.push_back(std::move(done));
            } else {
                HttpResult http = perform_winhttp_request(job.url, job.post_data, job.content_type, user_agent_);
                CompletedHttpJob done;
                done.is_badge = false;
                done.http_status = http.status_code;
                done.body = std::move(http.body);
                done.callback = job.callback;
                done.callback_data = job.callback_data;
                std::lock_guard<std::mutex> lock(completed_mutex_);
                completed_jobs_.push_back(std::move(done));
            }
        }
    }

    void drain_completed_jobs() {
        std::deque<CompletedHttpJob> local;
        {
            std::lock_guard<std::mutex> lock(completed_mutex_);
            if (completed_jobs_.empty()) return;
            local.swap(completed_jobs_);
        }

        for (auto& done : local) {
            if (done.is_badge) {
                if (done.badge_ok) {
                    overlay_.notify_badge_ready(done.badge_dest_path);
                }
            } else if (done.callback) {
                rc_api_server_response_t resp{};
                resp.body = done.body.c_str();
                resp.body_length = done.body.size();
                resp.http_status_code = done.http_status;
                done.callback(&resp, done.callback_data);
            }
        }
    }

    Config cfg_;
    rc_client_t* client_ = nullptr;
    AchievementsOverlay overlay_;
    const uint8_t* ram_ = nullptr;
    size_t ram_size_ = 0;
    const uint8_t* spr_ = nullptr;
    size_t spr_size_ = 0;
    std::wstring user_agent_ = L"GT4Recomp/1.2.1";

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_worker_{false};
    bool game_loaded_ = false;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<HttpJob> pending_jobs_;
    std::thread worker_thread_;

    std::mutex completed_mutex_;
    std::deque<CompletedHttpJob> completed_jobs_;
    std::unordered_set<std::string> requested_badges_;
};

} // namespace

Config parse_config(const std::function<std::string(const char*)>& get_env) {
    Config cfg;
    cfg.enabled = parse_bool_str(get_env("GT4_RA_ENABLED"), false);
    cfg.hardcore = parse_bool_str(get_env("GT4_RA_HARDCORE"), false);
    cfg.encore = parse_bool_str(get_env("GT4_RA_ENCORE"), false);
    cfg.username = get_env("GT4_RA_USERNAME");
    cfg.token = get_env("GT4_RA_TOKEN");
    cfg.password = get_env("GT4_RA_PASSWORD");
    const std::string custom_hash = get_env("GT4_RA_HASH");
    cfg.game_hash = (custom_hash.size() == 32) ? custom_hash : std::string(kGt4NtscUHash);
    cfg.host = get_env("GT4_RA_HOST");
    cfg.cache_dir = utf8_to_wide(get_env("GT4_RA_CACHE_DIR"));
    cfg.disc_path = get_env("GT4_DISC_PATH");
    return cfg;
}

Config config_from_environment() {
    return parse_config([](const char* name) -> std::string {
        char buf[4096]{};
        const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (len == 0 || len >= sizeof(buf)) {
            return {};
        }
        return std::string(buf, len);
    });
}

uint32_t read_ps2_memory(
    uint32_t address,
    uint8_t* buffer,
    uint32_t num_bytes,
    const uint8_t* ram,
    size_t ram_size,
    const uint8_t* spr,
    size_t spr_size) {
    if (!buffer || num_bytes == 0) return 0;

    // 1. Hardware PS2 Scratchpad address space (0x70000000..0x70003FFF).
    if (address >= 0x70000000u && address < 0x70000000u + spr_size) {
        const size_t offset = static_cast<size_t>(address - 0x70000000u);
        const size_t avail = std::min<size_t>(num_bytes, spr ? (spr_size - offset) : 0);
        if (avail > 0) {
            std::memcpy(buffer, spr + offset, avail);
        }
        if (avail < num_bytes) {
            std::memset(buffer + avail, 0, num_bytes - avail);
        }
        return static_cast<uint32_t>(avail);
    }

    // 2. rcheevos RC_CONSOLE_PLAYSTATION_2 Scratchpad flat address (0x02000000..0x02003FFF).
    if (address >= 0x02000000u && address < 0x02000000u + spr_size) {
        const size_t offset = static_cast<size_t>(address - 0x02000000u);
        const size_t avail = std::min<size_t>(num_bytes, spr ? (spr_size - offset) : 0);
        if (avail > 0) {
            std::memcpy(buffer, spr + offset, avail);
        }
        if (avail < num_bytes) {
            std::memset(buffer + avail, 0, num_bytes - avail);
        }
        return static_cast<uint32_t>(avail);
    }

    // 3. Strip PS2 kernel/uncached mirror upper bits if present (0x20xxxxxx, 0x30xxxxxx, 0x80xxxxxx, 0xA0xxxxxx).
    uint32_t phys = address;
    const uint32_t top_nibble = address & 0xF0000000u;
    if (top_nibble == 0x20000000u || top_nibble == 0x30000000u || top_nibble == 0x80000000u || top_nibble == 0xA0000000u) {
        phys = address & 0x0FFFFFFFu;
    }

    if (phys < ram_size && ram) {
        const size_t offset = static_cast<size_t>(phys);
        const size_t avail = std::min<size_t>(num_bytes, ram_size - offset);
        if (avail > 0) {
            std::memcpy(buffer, ram + offset, avail);
        }
        if (avail < num_bytes) {
            std::memset(buffer + avail, 0, num_bytes - avail);
        }
        return static_cast<uint32_t>(avail);
    }

    std::memset(buffer, 0, num_bytes);
    return 0;
}

void initialize(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size) {
    RuntimeManager::instance().init(game_window, ram, ram_size, spr, spr_size);
}

void on_vblank(HWND game_window, const uint8_t* ram, size_t ram_size, const uint8_t* spr, size_t spr_size) {
    RuntimeManager::instance().tick(game_window, ram, ram_size, spr, spr_size);
}

void on_window_pos_changed(HWND game_window) {
    RuntimeManager::instance().on_pos_changed(game_window);
}

void reset_session() {
    RuntimeManager::instance().reset();
}

void shutdown() {
    RuntimeManager::instance().shutdown();
}

bool is_active() {
    return RuntimeManager::instance().is_active();
}

} // namespace gt4::achievements
