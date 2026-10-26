#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "achievements_http.hpp"
#include "rc_api_request.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace gt4::achievements {
namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

// The callback context remains alive until HANDLE_CLOSING, the final notification for this request.
struct Request {
    std::mutex mutex;
    std::mutex call_mutex;
    std::condition_variable changed;
    HINTERNET handle = nullptr;
    DWORD completion = 0;
    DWORD byte_count = 0;
    bool failed = false;
    bool cancelled = false;
    bool closing = false;

    static void CALLBACK status(HINTERNET, DWORD_PTR context, DWORD status_code, LPVOID data, DWORD length) {
        auto* request = reinterpret_cast<Request*>(context);
        if (!request) return;
        std::lock_guard lock(request->mutex);
        if (status_code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            request->closing = true;
        } else if (status_code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            request->failed = true;
        } else if (status_code == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                   status_code == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                   status_code == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE ||
                   status_code == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            request->completion = status_code;
            request->byte_count = status_code == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && data
                ? *static_cast<DWORD*>(data) : length;
        }
        request->changed.notify_all();
    }

    bool wait(DWORD expected, DWORD* bytes = nullptr) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return completion == expected || failed || cancelled; });
        if (failed || cancelled) return false;
        if (bytes) *bytes = byte_count;
        completion = 0;
        return true;
    }

    void cancel() {
        {
            std::lock_guard lock(mutex);
            cancelled = true;
            changed.notify_all();
        }
        std::lock_guard call_lock(call_mutex);
        HINTERNET to_close = std::exchange(handle, nullptr);
        if (to_close) WinHttpCloseHandle(to_close);
    }

    void finish() {
        HINTERNET to_close;
        {
            std::lock_guard lock(call_mutex);
            to_close = std::exchange(handle, nullptr);
            if (to_close) WinHttpCloseHandle(to_close);
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return closing; });
    }

    // Keep the handle stable through each nonblocking WinHTTP call. Cancellation closes it between calls.
    template<class Operation> bool begin(Operation operation) {
        std::lock_guard call_lock(call_mutex);
        {
            std::lock_guard lock(mutex);
            if (cancelled || failed || !handle) return false;
        }
        // WinHTTP may call status() inline; never hold its state mutex during the API call.
        if (operation(handle) != FALSE) return true;
        std::lock_guard lock(mutex);
        failed = true;
        changed.notify_all();
        return false;
    }

    bool successful() {
        std::lock_guard lock(mutex);
        return !failed && !cancelled;
    }
};

} // namespace

struct HttpTransport::Impl {
    std::mutex mutex;
    bool cancelled = false;
    std::shared_ptr<Request> active;
};

HttpTransport::HttpTransport() : impl_(std::make_unique<Impl>()) {}
HttpTransport::~HttpTransport() { cancel(); }

void HttpTransport::cancel() {
    std::shared_ptr<Request> active;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->cancelled = true;
        active = impl_->active;
    }
    if (active) active->cancel();
}

void HttpTransport::reset() {
    std::lock_guard lock(impl_->mutex);
    impl_->cancelled = false;
}

HttpResult HttpTransport::request(const std::string& url, const std::string& post_data,
                                  const std::string& content_type, const std::wstring& user_agent) {
    HttpResult result;
    result.status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->cancelled) return result;
    }
    const std::wstring address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts)) return result;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring route(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) route.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (route.empty()) route = L"/";
    InternetHandle session(WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.value) return result;
    WinHttpSetTimeouts(session.value, 8000, 8000, 10000, 15000);
    InternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    if (!connection.value) return result;
    const bool post = !post_data.empty();
    auto active = std::make_shared<Request>();
    active->handle = WinHttpOpenRequest(connection.value, post ? L"POST" : L"GET", route.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!active->handle) return result;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(active.get());
    if (!WinHttpSetOption(active->handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(active->handle, Request::status,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        WinHttpCloseHandle(active->handle);
        return result;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active = active;
        if (impl_->cancelled) active->cancel();
    }
    const std::wstring headers = post && !content_type.empty() ? L"Content-Type: " + wide(content_type) + L"\r\n" : L"";
    const bool sent = active->begin([&](HINTERNET handle) {
        return WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.size()), post ? const_cast<char*>(post_data.data()) : WINHTTP_NO_REQUEST_DATA,
            static_cast<DWORD>(post_data.size()), static_cast<DWORD>(post_data.size()), context);
    }) && active->wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    if (sent && active->begin([](HINTERNET handle) { return WinHttpReceiveResponse(handle, nullptr); }) &&
        active->wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) {
        DWORD code = 0;
        DWORD size = sizeof(code);
        if (active->begin([&](HINTERNET handle) {
            return WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        })) {
            result.status_code = static_cast<int>(code);
        }
        DWORD available = 0;
        while (active->begin([](HINTERNET handle) { return WinHttpQueryDataAvailable(handle, nullptr); }) &&
               active->wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &available) && available) {
            const size_t offset = result.body.size();
            result.body.resize(offset + available);
            DWORD read = 0;
            if (!active->begin([&](HINTERNET handle) {
                return WinHttpReadData(handle, result.body.data() + offset, available, nullptr);
            }) || !active->wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, &read)) {
                result.body.resize(offset);
                result.status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
                break;
            }
            result.body.resize(offset + read);
        }
    }
    if (!active->successful()) result.status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    active->finish();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active.reset();
    }
    return result;
}

} // namespace gt4::achievements
