#include "claude_monitor.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "http.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

// Claude Code 注册表里 status 的全部取值（从它的程序里抠出来的官方枚举）
bool IsIdleValue(const std::string& value) { return value == "idle"; }
bool IsWaitingValue(const std::string& value) { return value == "waiting"; }
bool IsBusyValue(const std::string& value) { return value == "busy" || value == "shell"; }
bool IsKnownValue(const std::string& value) {
    return IsIdleValue(value) || IsWaitingValue(value) || IsBusyValue(value);
}

// 读活会话注册表：读不到/写坏的一律跳过（Claude Code 正在写的时候可能读到半截）
std::vector<Json> LoadRegistry(const std::string& directory) {
    std::vector<Json> out;
    for (const std::string& path : ListFilesSorted(directory, {".json"})) {
        std::string text;
        if (!ReadFile(path, &text)) continue;
        std::string error;
        Json data = Json::Parse(text, &error);
        if (!error.empty() || !data.is_object()) continue;
        if (!data.has_str("sessionId")) continue;
        data.set("_file", Json::Str(path));
        data.set("_mtime", Json::Num(FileMTime(path)));
        out.push_back(data);
    }
    return out;
}

// 按 sessionId 找会话记录文件（项目目录名是 cwd 编码出来的，所以直接搜）
std::string TranscriptPath(const std::string& projects_dir, const std::string& session_id) {
    if (session_id.empty() || !DirExists(projects_dir)) return std::string();
    std::string best;
    double best_mtime = -1.0;
    for (const std::string& sub : ListSubdirs(projects_dir)) {
        std::string candidate = PathJoin(sub, session_id + ".jsonl");
        if (!FileExists(candidate)) continue;
        double mtime = FileMTime(candidate);
        if (mtime > best_mtime) {
            best_mtime = mtime;
            best = candidate;
        }
    }
    return best;
}

std::string ProjectName(const Json& session) {
    std::string cwd = session.get_str("cwd");
    if (!cwd.empty()) {
        std::string name = PathFileName(cwd);
        if (!name.empty()) return name;
    }
    std::string name = session.get_str("name");
    return name.empty() ? "Claude Code" : name;
}

}  // namespace

TranscriptState ReadTranscriptState(const std::string& path, long long tail_bytes) {
    TranscriptState state;
    if (path.empty() || !FileExists(path)) return state;
    long long size = FileSize(path);
    state.last_ts = FileMTime(path);

    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) return state;
    if (size > tail_bytes) {
        file.seekg(size - tail_bytes);
        std::string discard;
        std::getline(file, discard);   // 丢掉半行
    }
    std::string line;
    std::vector<std::string> order;
    std::map<std::string, std::string> used;
    std::set<std::string> done;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        std::string error;
        Json record = Json::Parse(line, &error);
        if (!error.empty() || !record.is_object()) continue;
        const Json& message = record.get("message");
        if (!message.is_object()) continue;
        const Json& content = message.get("content");
        if (!content.is_array()) continue;
        for (size_t i = 0; i < content.size(); ++i) {
            const Json& item = content.at(i);
            if (!item.is_object()) continue;
            std::string type = item.get_str("type");
            if (type == "tool_use") {
                std::string id = item.get_str("id");
                if (id.empty()) continue;
                std::string name = item.get_str("name");
                used[id] = name.empty() ? "工具" : SanitizeUtf8(name);
                order.push_back(id);
            } else if (type == "tool_result") {
                std::string id = item.get_str("tool_use_id");
                if (!id.empty()) done.insert(id);
            } else if (type == "text" && record.get_str("type") == "assistant") {
                std::string text = CollapseWhitespace(SanitizeUtf8(item.get_str("text")));
                if (!text.empty()) state.summary = text;
            }
        }
    }
    std::vector<std::string> left;
    for (const std::string& id : order) {
        if (used.count(id) && !done.count(id)) left.push_back(id);
    }
    state.unresolved = int(left.size());
    if (!left.empty()) state.pending_tool = used[left.back()];
    return state;
}

namespace {

// 串行推送队列：保证 working → completed 的顺序，且不阻塞监控循环
class PushQueue {
public:
    void Start() {
        std::lock_guard<std::mutex> guard(lock_);
        if (worker_.joinable()) return;
        stopping_ = false;
        worker_ = std::thread([this] { Loop(); });
    }

    void Push(std::string url, std::string token, std::string body, std::string describe) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (queue_.size() >= 32) queue_.pop_front();
            queue_.push_back({std::move(url), std::move(token), std::move(body),
                              std::move(describe)});
        }
        Start();
        cv_.notify_all();
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> guard(lock_);
            stopping_ = true;
        }
        cv_.notify_all();
        std::thread worker;
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (worker_.joinable()) worker = std::move(worker_);
        }
        if (worker.joinable()) worker.join();
    }

private:
    struct Item {
        std::string url;
        std::string token;
        std::string body;
        std::string describe;
    };

    void Loop() {
        while (true) {
            Item item;
            {
                std::unique_lock<std::mutex> guard(lock_);
                cv_.wait(guard, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            std::vector<std::string> headers;
            if (!item.token.empty()) headers.push_back("Authorization: Bearer " + item.token);
            // 本地接口收到后会顺带推设备 + 飞书，可能要好幾秒；给宽松点，
            // 否则会莫名其妙报「Claude 状态推送失败」（其实对方已经处理完了）
            HttpResult result = HttpPostJson(item.url, item.body,
                                             "application/json; charset=utf-8", 20.0, headers);
            if (result.ok) {
                LogInfo("claude", item.describe);
            } else {
                LogWarn("claude", Cat("Claude 状态推送失败（", item.describe, "）：",
                                      result.error));
            }
        }
    }

    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::thread worker_;
    bool stopping_ = false;
};

PushQueue& Queue() {
    static PushQueue queue;
    return queue;
}

}  // namespace

ClaudeMonitor::ClaudeMonitor(std::shared_ptr<Config> cfg) : cfg_(std::move(cfg)) {}

void ClaudeMonitor::SetConfig(std::shared_ptr<Config> cfg) { cfg_ = std::move(cfg); }

Json ClaudeMonitor::ScanOnce(bool force) {
    std::shared_ptr<Config> cfg = cfg_;
    const Json& section = cfg->get("claude");
    if (!section.is_object() || !section.get_bool("enabled", true)) return Json::Object();

    double interval = section.get_double("poll_interval_sec", 2.0);
    double now = NowUnix();
    if (!force && now - last_poll_ < interval) return Json::Object();
    last_poll_ = now;

    std::string sessions_dir = ResolvePath(section.get_str("sessions_dir", "~/.claude/sessions"));
    std::string projects_dir = ResolvePath(section.get_str("projects_dir", "~/.claude/projects"));
    const Json& approval_raw = section.get("approval_after_sec");
    double approval_after = approval_raw.is_null() ? 0.0 : (std::max)(0.0, approval_raw.as_double(0.0));
    const Json& stale_raw = section.get("stale_busy_sec");
    double stale_after = stale_raw.is_null() ? 1800.0 : (std::max)(0.0, stale_raw.as_double(0.0));
    bool baseline = !baseline_done_ && section.get_str("first_run", "baseline") == "baseline";

    Json pushed = Json::Array();
    std::set<std::string> seen;
    std::vector<Json> registry = LoadRegistry(sessions_dir);

    for (const Json& session : registry) {
        std::string session_id = session.get_str("sessionId");
        if (session_id.empty()) continue;
        seen.insert(session_id);
        std::string raw_status = ToLowerAscii(Trim(session.get_str("status")));
        if (!raw_status.empty() && !IsKnownValue(raw_status)) {
            if (!seen_runtime_.count(raw_status)) {
                seen_runtime_[raw_status] = now;
                LogWarn("claude", Cat("Claude 注册表里出现了没见过的状态值 '", raw_status,
                                      "'（先按'工作中'处理）"));
            }
        }
        bool busy = !raw_status.empty() && !IsIdleValue(raw_status);
        bool waiting = IsWaitingValue(raw_status);

        std::string name = ProjectName(session);
        TranscriptState detail =
            ReadTranscriptState(TranscriptPath(projects_dir, session_id));
        bool quiet = detail.last_ts > 0 && (now - detail.last_ts) > approval_after;

        auto prev_found = prev_.find(session_id);
        std::string previous = prev_found == prev_.end() ? std::string() : prev_found->second;
        bool had_previous = prev_found != prev_.end();
        prev_[session_id] = busy ? "busy" : "idle";

        if (baseline) {
            sub_[session_id] = waiting ? "waiting" : (busy ? "working" : "");
            busy_since_[session_id] = now;
            continue;
        }

        if (busy) {
            // ① 官方给的"在等你"状态：最准的一条，直接报等待审批
            if (waiting) {
                if (sub_[session_id] != "waiting") {
                    sub_[session_id] = "waiting";
                    Emit("waiting_approval",
                         "等待你授权使用 " + (detail.pending_tool.empty() ? "工具"
                                                                          : detail.pending_tool),
                         name, "Claude Code 注册表 status=waiting");
                    Json entry = Json::Object();
                    entry.set("status", Json::Str("waiting_approval"));
                    entry.set("thread", Json::Str(name));
                    pushed.push_back(entry);
                }
                continue;
            }
            // ② 从 idle 开始干活，或者刚从 waiting 被放行 —— 都推一条"工作中"
            if (!had_previous || previous != "busy" || sub_[session_id] == "waiting") {
                bool back_from_wait = sub_[session_id] == "waiting";
                busy_since_[session_id] = now;
                sub_[session_id] = "working";
                Emit("working", back_from_wait ? "已授权，继续执行" : "", name,
                     Cat("Claude Code 注册表 status=", raw_status.empty() ? "busy" : raw_status));
                Json entry = Json::Object();
                entry.set("status", Json::Str("working"));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
                continue;
            }
            // ③ 兜底猜测（默认关）：busy 卡着 + 有没结果的工具调用 + 文件静默
            bool stuck = approval_after > 0 && sub_[session_id] != "waiting" &&
                         now - busy_since_[session_id] >= approval_after &&
                         detail.unresolved > 0 && quiet;
            if (stuck) {
                sub_[session_id] = "waiting";
                Emit("waiting_approval",
                     "等待你授权使用 " + (detail.pending_tool.empty() ? "工具"
                                                                      : detail.pending_tool),
                     name, "Claude Code 待授权检测");
                Json entry = Json::Object();
                entry.set("status", Json::Str("waiting_approval"));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
            } else if (stale_after > 0 &&
                       now - double(session.get_int("statusUpdatedAt", 0)) / 1000.0 > stale_after &&
                       now - (detail.last_ts > 0 ? detail.last_ts : now) > stale_after) {
                // 会话卡了很久又毫无动静（进程被强杀之类）：当成结束
                sub_[session_id] = "";
                Emit("completed", detail.summary, name, "Claude Code 会话失联");
                Json entry = Json::Object();
                entry.set("status", Json::Str("completed"));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
            }
        } else {
            if (had_previous && previous == "busy") {
                sub_[session_id] = "";
                Emit("completed", detail.summary, name, "Claude Code 会话注册表");
                Json entry = Json::Object();
                entry.set("status", Json::Str("completed"));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
            } else if (!had_previous) {
                Emit("idle", "", name, "Claude Code 会话注册表");
                Json entry = Json::Object();
                entry.set("status", Json::Str("idle"));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
            }
        }
    }

    // 注册表里消失的会话（CC 关了）：只清本地记忆，不推状态
    for (auto it = prev_.begin(); it != prev_.end();) {
        if (!seen.count(it->first)) {
            sub_.erase(it->first);
            busy_since_.erase(it->first);
            it = prev_.erase(it);
        } else {
            ++it;
        }
    }

    if (baseline) {
        baseline_done_ = true;
        LogInfo("claude", Cat("Claude 只读监控：首次运行已建立基线（", (long long)seen.size(),
                              " 个会话），不补推历史"));
        // 但当前状态要推一次：否则重启服务后画面会停在上次那个旧状态
        if (section.get_bool("push_current", true) && !seen.empty()) {
            const Json* newest = nullptr;
            for (const Json& session : registry) {
                if (!seen.count(session.get_str("sessionId"))) continue;
                if (!newest || session.get_int("statusUpdatedAt", 0) >
                                   newest->get_int("statusUpdatedAt", 0)) {
                    newest = &session;
                }
            }
            if (newest) {
                std::string session_id = newest->get_str("sessionId");
                std::string raw = ToLowerAscii(Trim(newest->get_str("status")));
                bool busy = !raw.empty() && !IsIdleValue(raw);
                std::string name = ProjectName(*newest);
                TranscriptState detail =
                    ReadTranscriptState(TranscriptPath(projects_dir, session_id));
                std::string status_now;
                std::string text_now;
                if (IsWaitingValue(raw)) {
                    status_now = "waiting_approval";
                    text_now = "等待你授权使用 " +
                               (detail.pending_tool.empty() ? "工具" : detail.pending_tool);
                } else if (busy) {
                    status_now = "working";
                } else {
                    status_now = "idle";
                    text_now = detail.summary;
                }
                Emit(status_now, text_now, name, "Claude Code 会话注册表（启动校准）");
                Json entry = Json::Object();
                entry.set("status", Json::Str(status_now));
                entry.set("thread", Json::Str(name));
                pushed.push_back(entry);
            }
        }
    }

    Json out = Json::Object();
    out.set("sessions", Json::Num((long long)seen.size()));
    out.set("pushed", pushed);
    return out;
}

void ClaudeMonitor::Emit(const std::string& status, const std::string& text,
                         const std::string& thread, const std::string& source) {
    std::shared_ptr<Config> cfg = cfg_;
    const Json& api = cfg->get("api");
    std::string host = api.is_object() ? api.get_str("host", "127.0.0.1") : "127.0.0.1";
    if (host.empty()) host = "127.0.0.1";
    long long port = api.is_object() ? api.get_int("port", 17865) : 17865;
    std::string token = api.is_object() ? api.get_str("token") : std::string();

    Json body = Json::Object();
    body.set("status", Json::Str(status));
    body.set("text", Json::Str(TruncateCodePoints(CleanText(text), 400)));
    body.set("thread", Json::Str(CleanText(thread)));
    body.set("source", Json::Str(source));
    body.set("agent", Json::Str("claude"));

    std::string url = Cat("http://", host, ":", port, "/status");
    std::string describe = thread.empty() ? std::string("(无项目名)") : thread;
    Queue().Push(url, token, body.Dump(0), Cat("Claude 状态 ", status, " → ", describe));
}

void ClaudeMonitor::Shutdown() { Queue().Shutdown(); }

}  // namespace cn
