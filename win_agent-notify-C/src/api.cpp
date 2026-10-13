#include "api.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <stdexcept>

#include "device.h"
#include "feishu.h"
#include "monitor.h"
#include "push.h"
#include "util/log.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

constexpr size_t kMaxRequestBytes = 1u << 20;

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int high = hex(text[i + 1]);
            int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(char((high << 4) | low));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> ParseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    for (const std::string& pair : Split(query, '&')) {
        if (pair.empty()) continue;
        size_t at = pair.find('=');
        std::string key = at == std::string::npos ? pair : pair.substr(0, at);
        std::string value = at == std::string::npos ? std::string() : pair.substr(at + 1);
        out[UrlDecode(key)] = UrlDecode(value);
    }
    return out;
}

bool SendAll(unsigned long long handle, const std::string& data) {
    SOCKET socket_handle = (SOCKET)handle;
    size_t sent = 0;
    while (sent < data.size()) {
        int chunk = int((std::min)(size_t(64 * 1024), data.size() - sent));
        int written = send(socket_handle, data.data() + sent, chunk, 0);
        if (written <= 0) return false;
        sent += size_t(written);
    }
    return true;
}

std::string HttpDateNow() { return FormatLocal(NowUnix(), "%a, %d %b %Y %H:%M:%S GMT"); }

}  // namespace

const char* ApiUsage() {
    return
        "Codex Notify 本地推送接口\n"
        "\n"
        "GET  /health          健康检查\n"
        "GET  /config          当前生效配置（脱敏）\n"
        "GET  /status          当前状态 + 各线程状态\n"
        "POST /status          推送状态：{\"status\":\"working|waiting_approval|completed|"
        "interrupted|info\",\n"
        "                                  \"text\":\"附加说明\",\"thread\":\"任务名或线程id\","
        "\"title\":\"自定义标题\"}\n"
        "POST /notify          自由推送：{\"title\":\"标题\",\"text\":\"内容\",\"status\":\"info\"}\n"
        "POST /test            发送一张测试卡片\n"
        "POST /config/reload   重新加载 config.json\n";
}

namespace {

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;   // 键已小写
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

std::string SerializeResponse(const Response& response) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      (response.status == 200   ? "OK"
                       : response.status == 401 ? "Unauthorized"
                       : response.status == 404 ? "Not Found"
                                                : "Bad Gateway") +
                      "\r\n";
    out += "Server: CodexNotify/1.0\r\n";
    out += "Content-Type: " + response.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Date: " + HttpDateNow() + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += response.body;
    return out;
}

}  // namespace

struct ApiHandler {
    ApiServer* server;
    Request request;

    std::shared_ptr<Config> cfg() const { return server->ConfigPtr(); }
    StateStore* store() const { return server->store_; }

    Response JsonResponse(int status, const Json& payload) {
        Response response;
        response.status = status;
        response.body = payload.Dump(2);
        return response;
    }

    Response TextResponse(int status, const std::string& text) {
        Response response;
        response.status = status;
        response.content_type = "text/plain; charset=utf-8";
        response.body = text;
        return response;
    }

    bool Authorized() {
        std::string token = Trim(cfg()->str("api", "token"));
        if (token.empty()) return true;
        auto found = request.headers.find("authorization");
        if (found != request.headers.end()) {
            std::string value = Trim(found->second);
            if (value.size() > 7 && ToLowerAscii(value.substr(0, 7)) == "bearer " &&
                Trim(value.substr(7)) == token) {
                return true;
            }
        }
        auto query = ParseQuery(request.query);
        return query["token"] == token;
    }

    Json ParseBody() {
        if (request.body.empty()) return Json::Object();
        Json parsed = Json::Parse(request.body);
        if (parsed.is_object()) return parsed;
        // 也支持 key=value&... 的简易表单
        Json out = Json::Object();
        for (const auto& [key, value] : ParseQuery(request.body)) {
            out.set(key, Json::Str(value));
        }
        return out;
    }

    Json Snapshot() {
        store()->Load();   // 接口可能落后于监控线程写入的状态
        Json data = SnapshotFromStore(
            store(), {}, cfg()->str_list("watch", "ignore_thread_names"),
            cfg()->flag("watch", "only_user_threads", true));
        data.set("uptime_sec", Json::Num((long long)(NowUnix() - server->started_at_)));
        {
            std::lock_guard<std::mutex> guard(server->log_lock_);
            Json recent = Json::Array();
            size_t begin = server->log_.size() > 20 ? server->log_.size() - 20 : 0;
            for (size_t i = begin; i < server->log_.size(); ++i) {
                recent.push_back(server->log_[i]);
            }
            data.set("recent", recent);
        }
        return data;
    }

    Response HandleGet() {
        std::string route = request.path;
        if (route.empty()) route = "/";
        if (route == "/" || route == "/help") return TextResponse(200, ApiUsage());
        if (!Authorized()) return JsonResponse(401, Json::Object().set(
                                                      "ok", Json::Bool(false)).set(
                                                      "error", Json::Str("unauthorized")));

        if (route == "/health") {
            Json devices = Json::Array();
            for (const Json& target : DeviceTargets(*cfg())) {
                Json entry = Json::Object();
                std::string name = target.get_str("name");
                entry.set("name", Json::Str(name));
                entry.set("url", Json::Str(DeviceResolveUrl(target)));
                auto found = DeviceLastResults().find(name);
                if (found != DeviceLastResults().end()) {
                    Json last = Json::Object();
                    last.set("name", Json::Str(found->second.name));
                    last.set("url", Json::Str(found->second.url));
                    last.set("at", Json::Str(found->second.at));
                    last.set("status", Json::Str(found->second.status));
                    last.set("ok", Json::Bool(found->second.ok));
                    last.set("detail", Json::Str(found->second.detail));
                    entry.set("last", last);
                } else {
                    entry.set("last", Json());
                }
                devices.push_back(entry);
            }
            Json out = Json::Object();
            out.set("ok", Json::Bool(true));
            out.set("pid", Json::Num((long long)GetCurrentProcessId()));
            out.set("dry_run", Json::Bool(cfg()->data.get_bool("dry_run", false)));
            out.set("webhook_configured", Json::Bool(!Trim(cfg()->str("feishu", "webhook_url"))
                                                          .empty()));
            out.set("devices", devices);
            out.set("uptime_sec", Json::Num((long long)(NowUnix() - server->started_at_)));
            out.set("monitor", Json::Bool(true));
            return JsonResponse(200, out);
        }
        if (route == "/status") {
            Json out = Snapshot();
            out.set("ok", Json::Bool(true));
            return JsonResponse(200, out);
        }
        if (route == "/config") {
            Json out = Json::Object();
            out.set("ok", Json::Bool(true));
            out.set("config", PublicConfig(cfg()->data));
            return JsonResponse(200, out);
        }
        Json out = Json::Object();
        out.set("ok", Json::Bool(false));
        out.set("error", Json::Str("未知路径 " + route));
        out.set("usage", Json::Str(ApiUsage()));
        return JsonResponse(404, out);
    }

    Response HandlePost() {
        std::string route = request.path;
        if (!Authorized()) return JsonResponse(401, Json::Object().set(
                                                      "ok", Json::Bool(false)).set(
                                                      "error", Json::Str("unauthorized")));

        if (route == "/config/reload") {
            Config fresh = LoadConfig(cfg()->path);
            auto shared = std::make_shared<Config>(std::move(fresh));
            server->cfg_ = shared;
            LogInfo("api", "配置已热加载");
            Json out = Json::Object();
            out.set("ok", Json::Bool(true));
            out.set("config", PublicConfig(shared->data));
            return JsonResponse(200, out);
        }

        Json data = ParseBody();

        if (route == "/test") {
            PushOptions options;
            options.text = "这是一条测试消息，如果你看到它，说明 Webhook 配置正确。";
            options.title = "🔔 Codex Watcher 测试";
            options.source = "接口测试";
            options.force_feishu = true;   // 测试就是要验证飞书能不能收到
            auto [ok, detail] = PushStatus(*cfg(), "info", options);
            Json entry = Json::Object();
            entry.set("at", Json::Str(NowClockHms()));
            entry.set("status", Json::Str("info"));
            entry.set("ok", Json::Bool(ok));
            entry.set("detail", Json::Str(detail));
            server->Remember(entry);
            Json out = Json::Object();
            out.set("ok", Json::Bool(ok));
            out.set("detail", Json::Str(detail));
            return JsonResponse(ok ? 200 : 502, out);
        }

        if (route == "/notify") {
            std::string title = data.get_str("title", "🔔 Codex 通知");
            std::string text = data.get_str("text");
            if (text.empty()) text = data.get_str("message");
            std::string status = NormalizeStatus(data.get_str("status", "info"));
            Json card = BuildCard(title, StatusTemplate(status), text.empty() ? "（无内容）" : text,
                                  "Codex Notify · " + FormatLocal(NowUnix(), "%Y-%m-%d %H:%M:%S"),
                                  data.get_bool("mention_all", false));
            DevicePushStatus(*cfg(), status, text, "", "接口 notify", "");
            auto [ok, detail] = SendPayload(*cfg(), card);
            Json entry = Json::Object();
            entry.set("at", Json::Str(NowClockHms()));
            entry.set("status", Json::Str("notify"));
            entry.set("ok", Json::Bool(ok));
            entry.set("detail", Json::Str(detail));
            server->Remember(entry);
            Json out = Json::Object();
            out.set("ok", Json::Bool(ok));
            out.set("detail", Json::Str(detail));
            return JsonResponse(ok ? 200 : 502, out);
        }

        if (route == "/status") {
            std::string status = NormalizeStatus(data.get_str("status", "info"));
            std::string text = Trim(data.get_str("text"));
            if (text.empty()) text = Trim(data.get_str("message"));
            std::string thread = Trim(data.get_str("thread"));
            if (thread.empty()) thread = Trim(data.get_str("thread_id"));
            std::string title = Trim(data.get_str("title"));
            std::string source = Trim(data.get_str("source", "外部接口"));
            std::string agent = ToLowerAscii(Trim(data.get_str("agent")));
            if (agent.empty()) agent = "codex";

            std::string thread_id = thread.empty() ? "__manual__" : thread;

            // 去重：同一个线程短时间内重复推同一个状态只发一次
            double window = cfg()->num("notify", "dedup_window_sec", 0.0);
            Json prev = store()->WithLock([&](Json& data_ref) -> Json {
                const Json& threads = data_ref.get("threads");
                return threads.has(thread_id) ? threads.get(thread_id) : Json::Object();
            });
            double prev_ts = prev.get_double("updated_ts", 0.0);
            if (!data.get_bool("force", false) && window > 0 &&
                prev.get_str("status") == status && prev_ts > 0 &&
                NowUnix() - prev_ts < window) {
                LogDebug("api", Cat("重复状态 ", thread_id, " -> ", status, "，跳过推送"));
                Json entry = Json::Object();
                entry.set("at", Json::Str(NowClockHms()));
                entry.set("status", Json::Str(status));
                entry.set("ok", Json::Bool(true));
                entry.set("detail", Json::Str("重复状态，已跳过"));
                server->Remember(entry);
                Json out = Json::Object();
                out.set("ok", Json::Bool(true));
                out.set("duplicate", Json::Bool(true));
                out.set("detail", Json::Str("与上一次状态相同且间隔很短，已跳过推送"));
                out.set("status", Json::Str(status));
                out.set("status_label", Json::Str(StatusLabel(status)));
                out.set("thread_id", Json::Str(thread_id));
                out.set("agent", Json::Str(agent));
                out.set("agent_label", Json::Str(AgentLabel(agent)));
                return JsonResponse(200, out);
            }

            Json fields = Json::Object();
            std::string name = thread;
            if (name.empty()) {
                name = prev.get_str("name");
                if (name.empty()) name = "手动推送";
            }
            fields.set("name", Json::Str(name));
            fields.set("status", Json::Str(status));
            fields.set("status_label", Json::Str(StatusLabel(status)));
            fields.set("summary", Json::Str(TruncateCodePoints(text, 1000)));
            fields.set("agent", Json::Str(agent));
            fields.set("agent_label", Json::Str(AgentLabel(agent)));
            store()->SetThreadStatus(thread_id, fields);
            store()->Bump("pushed");
            store()->Save();

            PushOptions options;
            options.text = text;
            options.thread_name = name;
            options.title = title;
            options.source = source;
            options.agent = agent;
            auto [ok, detail] = PushStatus(*cfg(), status, options);

            Json entry = Json::Object();
            entry.set("at", Json::Str(NowClockHms()));
            entry.set("status", Json::Str(status));
            entry.set("ok", Json::Bool(ok));
            entry.set("detail", Json::Str(detail));
            entry.set("text", Json::Str(TruncateCodePoints(text, 120)));
            server->Remember(entry);
            if (!ok) store()->Bump("failed");

            Json out = Json::Object();
            out.set("ok", Json::Bool(ok));
            out.set("detail", Json::Str(detail));
            out.set("status", Json::Str(status));
            out.set("status_label", Json::Str(StatusLabel(status)));
            out.set("thread_id", Json::Str(thread_id));
            out.set("agent", Json::Str(agent));
            out.set("agent_label", Json::Str(AgentLabel(agent)));
            return JsonResponse(ok ? 200 : 502, out);
        }

        Json out = Json::Object();
        out.set("ok", Json::Bool(false));
        out.set("error", Json::Str("未知路径 " + route));
        out.set("usage", Json::Str(ApiUsage()));
        return JsonResponse(404, out);
    }
};

ApiServer::ApiServer(std::shared_ptr<Config> cfg, StateStore* store)
    : cfg_(std::move(cfg)), store_(store) {}

ApiServer::~ApiServer() { Stop(); }

void ApiServer::Start() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup 失败");
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error("socket 创建失败");
    }
    // Windows 上 SO_REUSEADDR 会让两个实例绑同一端口互相顶掉，这里用独占
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive,
               sizeof(exclusive));

    std::string host = cfg_->str("api", "host", "127.0.0.1");
    if (host.empty()) host = "127.0.0.1";
    int port = int(cfg_->integer("api", "port", 17865));
    if (port <= 0) port = 17865;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(listener, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        closesocket(listener);
        WSACleanup();
        throw std::runtime_error(
            Cat("无法监听 ", host, ":", port, "（WSA ", error,
                "）。Windows 存在系统保留端口段，可先执行 "
                "'netsh interface ipv4 show excludedportrange protocol=tcp' 查看，"
                "再改 config.json 的 api.port。"));
    }
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        throw std::runtime_error("listen 失败");
    }

    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(listener, (sockaddr*)&bound, &bound_len) == 0) {
        port = ntohs(bound.sin_port);
    }

    listener_ = (unsigned long long)listener;
    port_ = port;
    started_at_ = NowUnix();
    running_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    LogInfo("api", Cat("推送接口已启动：http://", host, ":", port_, "/（POST /status）"));
}

void ApiServer::Stop() {
    if (!running_.exchange(false)) {
        if (listener_ != ~0ull) {
            closesocket((SOCKET)listener_);
            listener_ = ~0ull;
        }
        return;
    }
    if (listener_ != ~0ull) {
        closesocket((SOCKET)listener_);   // accept 会立刻返回错误
        listener_ = ~0ull;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    // 等连接线程收尾（它们都是短请求，最多几秒）
    for (int i = 0; i < 30 && client_count_.load() > 0; ++i) {
        Sleep(100);
    }
    WSACleanup();
}

void ApiServer::Remember(const Json& entry) {
    std::lock_guard<std::mutex> guard(log_lock_);
    log_.push_back(entry);
    if (log_.size() > 200) log_.erase(log_.begin(), log_.begin() + (log_.size() - 200));
}

void ApiServer::AcceptLoop() {
    while (running_.load()) {
        SOCKET client = accept((SOCKET)listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        if (client_count_.load() >= 32) {   // 防止连接堆积
            closesocket(client);
            continue;
        }
        client_count_.fetch_add(1);
        std::thread([this, client] {
            HandleClient((unsigned long long)client);
            client_count_.fetch_sub(1);
        }).detach();
    }
}

void ApiServer::HandleClient(unsigned long long handle) {
    SOCKET socket_handle = (SOCKET)handle;
    DWORD timeout = 10000;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    std::string buffer;
    char chunk[8192];
    size_t header_end = std::string::npos;
    while (buffer.size() < kMaxRequestBytes) {
        int read = recv(socket_handle, chunk, sizeof(chunk), 0);
        if (read <= 0) break;
        buffer.append(chunk, size_t(read));
        header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
    }
    if (header_end == std::string::npos) {
        closesocket(socket_handle);
        return;
    }

    Request request;
    std::string head = buffer.substr(0, header_end);
    size_t line_end = head.find("\r\n");
    std::string request_line = line_end == std::string::npos ? head : head.substr(0, line_end);
    std::vector<std::string> parts = Split(request_line, ' ');
    if (parts.size() < 2) {
        closesocket(socket_handle);
        return;
    }
    request.method = parts[0];
    std::string target = parts[1];
    size_t question = target.find('?');
    request.path = question == std::string::npos ? target : target.substr(0, question);
    request.query = question == std::string::npos ? "" : target.substr(question + 1);
    while (request.path.size() > 1 && request.path.back() == '/') request.path.pop_back();

    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    long long content_length = 0;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        std::string line = head.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = end == std::string::npos ? head.size() : end + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = ToLowerAscii(Trim(line.substr(0, colon)));
        std::string value = Trim(line.substr(colon + 1));
        request.headers[key] = value;
        if (key == "content-length") {
            try {
                content_length = std::stoll(value);
            } catch (...) {
                content_length = 0;
            }
        }
    }

    std::string body = buffer.substr(header_end + 4);
    while ((long long)body.size() < content_length && body.size() < kMaxRequestBytes) {
        int read = recv(socket_handle, chunk, sizeof(chunk), 0);
        if (read <= 0) break;
        body.append(chunk, size_t(read));
    }
    if ((long long)body.size() > content_length && content_length > 0) {
        body.resize(size_t(content_length));
    }
    // 调用方可能是 Windows 控制台里的 curl —— 那种情况中文是 GBK 发出来的。
    // 先按系统编码归一化成 UTF-8，界面和卡片才不会显示成一串乱码。
    request.body = NormalizeToUtf8(body);

    ApiHandler handler;
    handler.server = this;
    handler.request = std::move(request);

    Response response;
    try {
        if (handler.request.method == "GET") {
            response = handler.HandleGet();
        } else if (handler.request.method == "POST") {
            response = handler.HandlePost();
        } else {
            response.status = 404;
            response.body = handler.JsonResponse(404, Json::Object().set(
                                                          "ok", Json::Bool(false)).set(
                                                          "error", Json::Str("不支持的方法")))
                                .body;
        }
    } catch (const std::exception& error) {
        LogError("api", Cat("处理请求异常: ", error.what()));
        response.status = 500;
        response.body = "{\"ok\": false, \"error\": \"internal error\"}";
    }
    SendAll(handle, SerializeResponse(response));
    shutdown(socket_handle, SD_SEND);
    closesocket(socket_handle);
}

}  // namespace cn
