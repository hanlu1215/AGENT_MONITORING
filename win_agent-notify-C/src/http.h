// WinHTTP 封装：发 JSON / 收响应，http 与 https 同一套。
#pragma once

#include <string>
#include <vector>

namespace cn {

struct HttpResult {
    bool ok = false;          // 传输成功（不含 4xx/5xx）
    int status = 0;           // HTTP 状态码
    std::string body;
    std::string error;        // 失败原因，例如 "HTTP 404" / "WinHttpSendRequest: 12002"
};

// 分阶段超时（对应 Python 的 socket timeout：每次操作都按这个时间等）
HttpResult HttpPostJson(const std::string& url, const std::string& body,
                        const std::string& content_type, double timeout_sec,
                        const std::vector<std::string>& extra_headers = {});

HttpResult HttpGet(const std::string& url, double timeout_sec,
                   const std::vector<std::string>& extra_headers = {});

}  // namespace cn
