#pragma once

#include <memory>
#include <string>

namespace gt4::achievements {

struct HttpResult {
    int status_code = 0;
    std::string body;
};

// One worker calls request(); shutdown may cancel it from the game thread. Reset only after the worker joins.
class HttpTransport {
public:
    HttpTransport();
    ~HttpTransport();
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    HttpResult request(const std::string& url, const std::string& post_data,
                       const std::string& content_type, const std::wstring& user_agent);
    void cancel();
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gt4::achievements
