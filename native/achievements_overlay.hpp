#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4::achievements {

enum class ToastKind {
    Info,
    Login,
    GameLoaded,
    AchievementUnlocked,
    Mastery,
    Leaderboard,
    Warning,
};

struct OverlayToast {
    ToastKind kind = ToastKind::Info;
    std::string header;
    std::string title;
    std::string subtitle;
    std::string badge_tag;
    std::wstring badge_path;
    bool hardcore = false;
    double duration_sec = 4.5;
};

struct ChallengeIndicatorItem {
    uint32_t id = 0;
    std::string title;
    std::wstring badge_path;
};

struct ProgressIndicatorState {
    bool visible = false;
    uint32_t id = 0;
    std::string title;
    std::string progress_text;
    std::wstring badge_path;
};

struct LeaderboardTrackerItem {
    uint32_t id = 0;
    std::string display;
};

class AchievementsOverlay {
public:
    AchievementsOverlay() = default;
    ~AchievementsOverlay();

    AchievementsOverlay(const AchievementsOverlay&) = delete;
    AchievementsOverlay& operator=(const AchievementsOverlay&) = delete;

    void initialize(HWND owner_window);
    void shutdown();

    void enqueue_toast(OverlayToast toast);
    void set_progress_indicator(const ProgressIndicatorState& state);
    void add_challenge_indicator(const ChallengeIndicatorItem& item);
    void remove_challenge_indicator(uint32_t id);
    void set_leaderboard_tracker(const LeaderboardTrackerItem& item);
    void remove_leaderboard_tracker(uint32_t id);
    void notify_badge_ready(const std::wstring& badge_path);
    void clear_session_indicators();

    void tick(HWND owner_window);
    void on_owner_pos_changed(HWND owner_window);

    // Pure helper exposed for unit tests: computes toast opacity/slide from elapsed time.
    static void compute_toast_anim(double elapsed_sec, double total_sec, float& out_alpha, float& out_slide_px);

private:
    struct ActiveToast {
        OverlayToast data;
        uint64_t start_tick_ms = 0;
    };

    void ensure_window(HWND owner_window);
    void update_placement(HWND owner_window);
    void render(HWND owner_window, uint64_t now_ms);
    bool has_visible_content() const;

    HWND owner_hwnd_ = nullptr;
    HWND overlay_hwnd_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    bool gdiplus_initialized_ = false;
    bool dirty_ = false;

    HDC mem_dc_ = nullptr;
    HBITMAP dib_bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    void* dib_bits_ = nullptr;
    int dib_width_ = 0;
    int dib_height_ = 0;

    std::vector<ActiveToast> toast_queue_;
    ProgressIndicatorState progress_;
    std::vector<ChallengeIndicatorItem> challenges_;
    std::vector<LeaderboardTrackerItem> trackers_;
};

} // namespace gt4::achievements
