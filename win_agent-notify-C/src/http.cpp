#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include "util/log.h"
#include "util/str.h"

namespace cn {
namespace {

class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~WinHttpHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) WinHttpCloseHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

struct UrlParts {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;   // 含查询串
    bool valid = false;
};

UrlParts SplitUrl(const std::string& url) {
    UrlParts parts;
    std::string text = Trim(url);
    if (StartsWith(text, "https://")) {
        parts.https = true;
        parts.port = INTERNET_DEFAULT_HTTPS_PORT;
        text = text.substr(8);
    } else if (StartsWith(text, "http://")) {
        parts.port = INTERNET_DEFAULT_HTTP_PORT;
        text = text.substr(7);
    } else {
        return parts;   // 不接受没有 scheme 的地址
    }
    size_t slash = text.find('/');
    std::string authority = slash == std::string::npos ? text : text.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : text.substr(slash);

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        std::string port_text = authority.substr(colon + 1);
        authority = authority.substr(0, colon);
        try {
            int port = std::stoi(port_text);
            if (port > 0 && port <= 65535) parts.port = (INTERNET_PORT)port;
        } catch (...) {
            return parts;
        }
    }
    if (authority.empty() || path.empty()) return parts;
    parts.host = Utf8ToWide(authority);
    parts.path = Utf8ToWide(path);
    parts.valid = true;
    return parts;
}

HttpResult Request(const std::wstring& method, const std::string& url, const std::string* body,
                   const std::string& content_type, double timeout_sec,
                   const std::vector<std::string>& extra_headers) {
    HttpResult result;
    UrlParts parts = SplitUrl(url);
    if (!parts.valid) {
        result.error = "invalid url";
        return result;
    }

    WinHttpHandle session(WinHttpOpen(L"win-agent-notify-C/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        result.error = "WinHttpOpen: " + std::to_string(GetLastError());
        return result;
    }
    int timeout_ms = int(timeout_sec * 1000.0);
    if (timeout_ms <= 0) timeout_ms = 1000;
    // 分阶段超时（不是总时限）——和 Python 的 socket timeout 语义一致
    WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    DWORD connect_retries = 0;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_CONNECT_RETRIES, &connect_retries,
                     sizeof(connect_retries));

    WinHttpHandle connection(WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
    if (!connection) {
        result.error = "WinHttpConnect: " + std::to_string(GetLastError());
        return result;
    }

    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), parts.path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        result.error = "WinHttpOpenRequest: " + std::to_string(GetLastError());
        return result;
    }

    std::wstring headers;
    if (!content_type.empty() && body) {
        headers += L"Content-Type: " + Utf8ToWide(content_type) + L"\r\n";
    }
    for (const std::string& extra : extra_headers) {
        headers += Utf8ToWide(extra) + L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(
        request.get(),
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : DWORD(headers.size()),
        body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
        body ? DWORD(body->size()) : 0, body ? DWORD(body->size()) : 0, 0);
    if (!sent) {
        result.error = "WinHttpSendRequest: " + std::to_string(GetLastError());
        return result;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = "WinHttpReceiveResponse: " + std::to_string(GetLastError());
        return result;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    result.status = int(status);

    std::string raw;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.get(), &available) && available > 0) {
        std::string chunk(size_t(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &read) || read == 0) break;
        chunk.resize(read);
        raw += chunk;
        if (raw.size() > 1u << 20) break;   // 防止异常响应把内存吃光
    }
    if (result.status >= 400) {
        result.error = "HTTP " + std::to_string(result.status);
        result.body = raw;
        return result;   // ok 保持 false，等价 Python 的 urllib.error.HTTPError
    }
    if (result.status == 0) {
        result.error = "no status code";
        return result;
    }
    result.ok = true;
    result.body = raw;
    return result;
}

}  // namespace

HttpResult HttpPostJson(const std::string& url, const std::string& body,
                        const std::string& content_type, double timeout_sec,
                        const std::vector<std::string>& extra_headers) {
    return Request(L"POST", url, &body, content_type, timeout_sec, extra_headers);
}

HttpResult HttpGet(const std::string& url, double timeout_sec,
                   const std::vector<std::string>& extra_headers) {
    return Request(L"GET", url, nullptr, "", timeout_sec, extra_headers);
}

}  // namespace cn
