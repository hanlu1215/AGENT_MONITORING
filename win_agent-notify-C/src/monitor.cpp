#include "monitor.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "device.h"
#include "feishu.h"
#include "push.h"
#include "queue.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

std::string ThreadKey(const Event& event) {
    return event.thread_id.empty() ? "unknown" : event.thread_id;
}

}  // namespace

bool IsInternalRecord(const Json& record, const std::map<std::string, std::string>& names,
                      const std::vector<std::string>& ignore_name_substrings,
                      bool only_user_threads) {
    if (only_user_threads) {
        std::string source = ToLowerAscii(Trim(record.get_str("thread_source")));
        if (!source.empty() && source != "user") return true;
    }
    std::string name = ToLowerAscii(Trim(record.get_str("name")));
    if (name.empty()) {
        std::string id = record.get_str("id");
        auto found = names.find(id);
        if (found != names.end()) name = ToLowerAscii(Trim(found->second));
    }
    for (const std::string& pattern : ignore_name_substrings) {
        if (!pattern.empty() && name.find(ToLowerAscii(pattern)) != std::string::npos) {
            return true;
        }
    }
    const Json& agent_source = record.get("agent_source");
    if (agent_source.is_object() && agent_source.has("subagent")) return true;
    if (agent_source.is_string() &&
        ToLowerAscii(agent_source.as_string()).find("subagent") != std::string::npos) {
        return true;
    }
    return false;
}

Json SnapshotFromStore(StateStore* store, const std::map<std::string, std::string>& names,
                       const std::vector<std::string>& ignore_name_substrings,
                       bool only_user_threads) {
    Json threads = Json::Array();
    const Json& all = store->Data().get("threads");

    std::string current_id;
    Json current;
    double best_ts = -1.0;
    for (size_t i = 0; i < all.size(); ++i) {
        const std::string& tid = all.key_ref(i);
        const Json& record = all.at(i);
        if (!record.is_object()) continue;
        std::string status = record.get_str("status");
        if (status.empty() || status == "meta") continue;
        if (IsInternalRecord(record, names, ignore_name_substrings, only_user_threads)) continue;

        Json entry = Json::Object();
        entry.set("thread_id", Json::Str(tid));
        std::string name = record.get_str("name");
        if (name.empty()) {
            auto found = names.find(tid);
            if (found != names.end()) name = found->second;
        }
        entry.set("thread_name", Json::Str(name));
        entry.set("status", Json::Str(status));
        std::string label = record.get_str("status_label");
        entry.set("status_label", Json::Str(label.empty() ? StatusLabel(status) : label));
        entry.set("turn_id", record.get("turn_id"));
        entry.set("summary", Json::Str(TruncateCodePoints(record.get_str("summary"), 400)));
        entry.set("last_progress", record.get("last_progress"));
        entry.set("cwd", record.get("cwd"));
        entry.set("updated_at", record.get("updated_at"));
        threads.push_back(entry);

        double ts = record.get_double("updated_ts", 0.0);
        if (ts > best_ts) {
            best_ts = ts;
            current_id = tid;
            current = record;
        }
    }

    Json current_out;
    if (!current.is_null()) {
        Json entry = Json::Object();
        entry.set("thread_id", Json::Str(current_id));
        std::string name = current.get_str("name");
        if (name.empty()) {
            auto found = names.find(current_id);
            if (found != names.end()) name = found->second;
        }
        std::string status = current.get_str("status");
        entry.set("thread_name", Json::Str(name));
        entry.set("status", Json::Str(status.empty() ? "unknown" : status));
        entry.set("status_label", Json::Str(StatusLabel(status.empty() ? "info" : status)));
        entry.set("summary", Json::Str(TruncateCodePoints(current.get_str("summary"), 400)));
        entry.set("updated_at", current.get("updated_at"));
        current_out = entry;
    }

    Json out = Json::Object();
    out.set("current", current_out);
    out.set("threads", threads);
    out.set("counters", store->Data().get("counters"));
    out.set("baseline_done", Json::Bool(store->Data().get_bool("baseline_done", false)));
    return out;
}

Monitor::Monitor(std::shared_ptr<Config> cfg, StateStore* store)
    : cfg_(std::move(cfg)), store_(store) {}

void Monitor::SetConfig(std::shared_ptr<Config> cfg) {
    std::lock_guard<std::mutex> guard(lock_);
    cfg_ = std::move(cfg);
}

void Monitor::RefreshNames(bool force) {
    std::lock_guard<std::mutex> guard(lock_);
    double now = NowUnix();
    if (!force && now - names_loaded_at_ < 30) return;
    std::string index = cfg_->str("watch", "session_index");
    if (!index.empty()) {
        auto loaded = LoadThreadNames(ResolvePath(index));
        names_.insert(loaded.begin(), loaded.end());
    }
    names_loaded_at_ = now;
}

std::string Monitor::ThreadName(const std::string& thread_id) {
    if (thread_id.empty()) return std::string();
    auto found = names_.find(thread_id);
    return found == names_.end() ? std::string() : found->second;
}

bool Monitor::IsInternalThread(const std::string& thread_id) {
    if (thread_id.empty()) return false;
    Json record = store_->WithLock([&](Json& data) -> Json {
        const Json& threads = data.get("threads");
        return threads.has(thread_id) ? threads.get(thread_id) : Json::Object();
    });
    if (!record.get_str("name").empty()) {
        // 用记录里的名字判定
    } else {
        std::string name = ThreadName(thread_id);
        if (!name.empty()) record.set("name", Json::Str(name));
    }
    return IsInternalRecord(record, names_, cfg_->str_list("watch", "ignore_thread_names"),
                            cfg_->flag("watch", "only_user_threads", true));
}

std::vector<std::pair<Json, std::string>> Monitor::ReadRecords(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock_);
    std::unique_ptr<SessionTail>& tail = tails_[path];
    if (!tail) tail.reset(new SessionTail(path, store_->Offset(path)));
    std::vector<Json> records = tail->ReadNew();
    if (tail->was_reset()) memo_.files.erase(path);   // 文件重写了，旧账一笔勾销
    store_->SetOffset(path, tail->offset());

    // 线程 id **优先用文件名里的**：内部子代理（Guardian review）的 session_meta
    // 里写的是"父线程 id"，只有文件名才是它自己的 id。要是反过来，子代理的事件
    // 会挂到父线程上、还顺手把父线程标成内部线程，真实任务就从列表里消失了。
    std::string thread_id = file_thread_[path];
    if (thread_id.empty()) {
        thread_id = ThreadIdFromPath(path);
        if (!thread_id.empty()) file_thread_[path] = thread_id;
    }

    std::vector<std::pair<Json, std::string>> out;
    out.reserve(records.size());
    for (const Json& record : records) {
        if (record.get_str("type") == "session_meta") {
            const Json& payload = record.get("payload");
            std::string meta_id;
            if (payload.is_object()) {
                meta_id = payload.get_str("session_id");
                if (meta_id.empty()) meta_id = payload.get_str("id");
            }
            if (!meta_id.empty() && thread_id.empty()) {   // 文件名里没有 id 时才用 meta 的
                thread_id = meta_id;
                file_thread_[path] = thread_id;
            }
        }
        out.emplace_back(record, thread_id);
    }
    return out;
}

std::vector<Json> Monitor::ApplyEvent(const Event& event, bool notify) {
    std::shared_ptr<Config> cfg = cfg_;
    if (event.kind == "meta") {
        if (!event.thread_id.empty()) {
            store_->WithLock([&](Json& data) {
                Json& record = data["threads"][event.thread_id];
                if (!record.is_object()) record = Json::Object();
                if (record.get_str("id") != event.thread_id) {
                    record.set("id", Json::Str(event.thread_id));
                }
                const Json& detail = event.detail;
                if (detail.is_object()) {
                    for (const char* key : {"cwd", "cli_version", "thread_source"}) {
                        const Json& value = detail.get(key);
                        if (!value.is_null() && value.as_string() != record.get_str(key)) {
                            record.set(key, value);
                        }
                    }
                    if (!detail.get("source").is_null()) {
                        record.set("agent_source", detail.get("source"));
                    }
                }
                std::string name = ThreadName(event.thread_id);
                if (!name.empty() && record.get_str("name") != name) {
                    record.set("name", Json::Str(name));
                }
            });
            store_->MarkDirty();
        }
        return {};
    }

    if (event.kind == "progress") {
        std::string text = Trim(event.text);
        if (!event.thread_id.empty() && !text.empty() && !LooksInternal(text)) {
            store_->WithLock([&](Json& data) {
                Json& record = data["threads"][event.thread_id];
                if (!record.is_object()) record = Json::Object();
                if (record.get_str("last_progress") != TruncateCodePoints(text, 200)) {
                    record.set("last_progress", Json::Str(TruncateCodePoints(text, 200)));
                }
            });
            store_->MarkDirty();
        }
        if (!(notify && cfg->flag("notify", "progress", false))) return {};
        return NotifyEvent(event);
    }

    if (!event.thread_id.empty()) {
        std::string summary = TruncateCodePoints(event.text, 1000);
        if (event.kind == "completed" && (summary.empty() || LooksInternal(summary))) {
            // 内部复核报文的「完成」不带有效摘要，保留上一轮的内容
            summary = store_->WithLock(
                [&](Json& data) { return data["threads"][event.thread_id].get_str("summary"); });
        }
        Json fields = Json::Object();
        fields.set("status", Json::Str(event.kind));
        fields.set("status_label", Json::Str(StatusLabel(event.kind)));
        if (!event.turn_id.empty()) fields.set("turn_id", Json::Str(event.turn_id));
        fields.set("summary", Json::Str(summary));
        store_->SetThreadStatus(event.thread_id, fields);

        // 「完成」延迟确认：应用内部经常每秒起停一轮，立即推会刷屏。
        // 延迟期间只要又来了新事件（说明任务还在继续），就把这次完成取消。
        if (event.kind == "completed") {
            double delay = cfg->num("notify", "complete_delay_sec", 0.0);
            if (delay > 0 && notify) {
                Json item = Json::Object();
                item.set("kind", Json::Str(event.kind));
                item.set("thread_id", Json::Str(event.thread_id));
                item.set("turn_id", Json::Str(event.turn_id));
                item.set("text", Json::Str(event.text));
                item.set("timestamp", Json::Num(event.timestamp));
                item.set("dedupe", Json::Str(event.dedupe));
                item.set("detail", event.detail);
                item.set("due", Json::Num(NowUnix() + delay));
                item.set("wait_from", Json::Num(NowUnix()));
                std::lock_guard<std::mutex> guard(lock_);
                pending_complete_[event.thread_id] = item;
                return {};
            }
        } else {
            std::lock_guard<std::mutex> guard(lock_);
            pending_complete_.erase(event.thread_id);
        }

        // 续读时读不到 session_meta，任务名从 session_index 补齐
        store_->WithLock([&](Json& data) {
            Json& record = data["threads"][event.thread_id];
            if (!record.is_object()) record = Json::Object();
            if (record.get_str("name").empty()) {
                std::string name = ThreadName(event.thread_id);
                if (!name.empty()) record.set("name", Json::Str(name));
            }
            if (event.kind == "working") record.set("started_ts", Json::Num(event.timestamp));
        });
        store_->MarkDirty();
    }
    if (!notify) return {};
    return NotifyEvent(event);
}

std::vector<Json> Monitor::NotifyEvent(const Event& event) {
    std::shared_ptr<Config> cfg = cfg_;
    const Json& on = cfg->get("notify").get("on");
    bool enabled = false;
    for (size_t i = 0; i < on.size(); ++i) {
        if (on.at(i).as_string() == event.kind) {
            enabled = true;
            break;
        }
    }
    if (!enabled) return {};

    // 内部线程（Guardian review / 子代理）不推：它们每秒起停一轮，会把真实任务顶掉
    if (IsInternalThread(event.thread_id)) {
        LogDebug("monitor", Cat("跳过内部线程 ", event.thread_id));
        return {};
    }

    std::string thread_id = ThreadKey(event);
    std::string key = Cat(thread_id, "|", event.kind, "|",
                          event.dedupe.empty() ? event.turn_id : event.dedupe);
    if (store_->IsNotified(key)) return {};

    {
        std::lock_guard<std::mutex> guard(lock_);
        auto found = last_pushed_status_.find(thread_id);
        if (found != last_pushed_status_.end() && found->second == event.kind) {
            store_->MarkNotified(key);
            return {};
        }
    }

    double age = event.timestamp > 0 ? NowUnix() - event.timestamp : 0.0;
    double max_age = cfg->num("notify", "max_event_age_sec", 0.0);
    if (max_age > 0 && age > max_age) {
        store_->MarkNotified(key);
        LogInfo("monitor", Cat("跳过过期事件 ", key, "（", FormatDouble(age, 0), "s 前）"));
        return {};
    }

    double min_interval = cfg->num("notify", "min_interval_sec", 0.0);
    {
        std::lock_guard<std::mutex> guard(lock_);
        double last = last_notify_at_[thread_id];
        auto kind = last_kind_.find(thread_id);
        // 只压制"同一个状态"的连发；不同状态一定要推出去
        if (min_interval > 0 && NowUnix() - last < min_interval && kind != last_kind_.end() &&
            kind->second == event.kind) {
            return {};
        }
    }

    Json thread = store_->WithLock([&](Json& data) -> Json {
        const Json& threads = data.get("threads");
        return threads.has(thread_id) ? threads.get(thread_id) : Json::Object();
    });
    std::string project = thread.get_str("name");
    if (project.empty()) project = ThreadName(thread_id);
    std::string cwd = thread.get_str("cwd");

    PushOptions options;
    options.text = Trim(event.text);
    options.thread_name = project;
    options.cwd = cwd;
    options.source = "会话日志";
    options.agent = "codex";
    auto [ok, detail] = PushStatus(*cfg, event.kind, options);

    {
        std::lock_guard<std::mutex> guard(lock_);
        last_notify_at_[thread_id] = NowUnix();
        last_kind_[thread_id] = event.kind;
        last_pushed_status_[thread_id] = event.kind;
    }
    store_->MarkNotified(key);

    bool broken_device = false;
    if (ok) {
        store_->Bump("pushed");
    } else if (detail != "dry-run") {
        store_->Bump("failed");
    }

    Json record = Json::Object();
    record.set("thread_id", Json::Str(thread_id));
    record.set("thread_name", Json::Str(project));
    record.set("status", Json::Str(event.kind));
    record.set("ok", Json::Bool(ok));
    record.set("detail", Json::Str(detail));
    record.set("at", Json::Str(NowStamp()));

    std::string what = project.empty() ? thread_id : project;
    if (ok) {
        LogInfo("monitor", Cat("已推送 [", event.kind, "] ", what, "（", detail, "）"));
        // 设备漏收不是"世界末日"：校准循环一分钟内会补推
        for (const std::string& part : Split(detail, '|')) {
            std::string item = Trim(part);
            if (item.find('=') != std::string::npos && !EndsWith(item, "=ok")) {
                broken_device = true;
            }
        }
        if (broken_device) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                last_device_sync_ = 0.0;
                last_sync_sent_.erase("codex");
            }
            LogWarn("monitor", Cat("有目标没收到 [", event.kind, "] ", what, " -> ", detail,
                                   "（稍后会自动补推）"));
        }
    } else if (detail == "dry-run") {
        LogInfo("monitor", Cat("dry-run [", event.kind, "] ", what, "（未实际发送）"));
    } else {
        LogWarn("monitor", Cat("推送失败 [", event.kind, "] ", what, " -> ", detail));
    }
    return {record};
}

Json Monitor::ScanOnce(bool notify) {
    std::shared_ptr<Config> cfg = cfg_;
    RefreshNames();

    std::vector<std::string> dirs;
    const Json& watch_dirs = cfg->get("watch").get("dirs");
    for (size_t i = 0; i < watch_dirs.size(); ++i) {
        std::string value = watch_dirs.at(i).as_string();
        if (!value.empty()) dirs.push_back(ResolvePath(value));
    }

    bool first_run = !store_->Data().get_bool("baseline_done", false);
    bool baseline_mode = first_run && cfg->str("watch", "first_run", "baseline") == "baseline";
    if (first_run) {
        for (const std::string& dir : dirs) {
            if (!DirExists(dir)) LogWarn("monitor", Cat("监控目录不存在：", dir));
        }
    }

    std::vector<Json> notifications;
    int scanned = 0;
    int events = 0;
    std::set<std::string> scanned_paths;
    for (const std::string& path :
         IterSessionFiles(dirs, int(cfg->integer("watch", "max_files", 30)),
                          int(cfg->integer("watch", "max_file_age_days", 14)))) {
        ++scanned;
        scanned_paths.insert(path);
        for (auto& [record, thread_id] : ReadRecords(path)) {
            for (const Event& event : Classify(record, thread_id, &memo_, path)) {
                ++events;
                std::vector<Json> pushed = ApplyEvent(event, notify && !baseline_mode);
                notifications.insert(notifications.end(), pushed.begin(), pushed.end());
            }
        }
    }

    // 文件被删掉/移走了：它的"待定调用"不能一直悬着，否则会永远停在"等待审批"
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = memo_.files.begin(); it != memo_.files.end();) {
            if (!scanned_paths.count(it->first)) {
                memo_.pushed_approval.erase(it->first);
                it = memo_.files.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (baseline_mode) {
        LogInfo("monitor", Cat("首次运行：已建立基线（", scanned, " 个文件 / ", events,
                               " 个事件），不推送历史"));
    }
    store_->WithLock([&](Json& data) {
        if (!data.get_bool("baseline_done", false)) {
            data.set("baseline_done", Json::Bool(true));
        }
    });
    store_->MarkDirty();
    store_->Save();   // 没有变化时这里会直接跳过写盘

    std::vector<Json> pending = FlushPending();
    notifications.insert(notifications.end(), pending.begin(), pending.end());

    Json out = Json::Object();
    out.set("files", Json::Num(scanned));
    out.set("events", Json::Num(events));
    out.set("notifications", Json::Num((long long)notifications.size()));
    return out;
}

std::vector<Json> Monitor::FlushPending() {
    std::shared_ptr<Config> cfg = cfg_;
    double now = NowUnix();
    double max_wait = cfg->num("notify", "complete_queue_wait_sec", 0.0);
    std::vector<Json> results;
    std::vector<Event> due;

    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = pending_complete_.begin(); it != pending_complete_.end();) {
            Json& item = it->second;
            if (now < item.get_double("due", 0.0)) {
                ++it;
                continue;
            }
            double waited = now - item.get_double("wait_from", now);
            std::string thread_id = it->first;
            // 该线程还有排队消息 -> 任务其实没完，继续等（但要有个上限）
            if (HasQueuedWork(thread_id) && (max_wait <= 0 || waited < max_wait)) {
                item.set("due", Json::Num(now + 2.0));
                ++it;
                continue;
            }
            if (HasQueuedWork(thread_id)) {
                LogInfo("monitor", Cat("队列里还有消息，但这一轮已经完成 ", FormatDouble(waited, 0),
                                       " 秒，照常报「完成」"));
            }
            Event event;
            event.kind = item.get_str("kind");
            event.thread_id = item.get_str("thread_id");
            event.turn_id = item.get_str("turn_id");
            event.text = item.get_str("text");
            event.timestamp = item.get_double("timestamp", 0.0);
            event.dedupe = item.get_str("dedupe");
            event.detail = item.get("detail");
            due.push_back(event);
            it = pending_complete_.erase(it);
        }
    }
    for (const Event& event : due) {
        std::vector<Json> pushed = NotifyEvent(event);
        results.insert(results.end(), pushed.begin(), pushed.end());
    }
    return results;
}

std::vector<Json> Monitor::ApprovalCheckOnce() {
    std::shared_ptr<Config> cfg = cfg_;
    std::vector<Json> results;
    if (!store_->Data().get_bool("baseline_done", false)) return results;   // 首次建基线不推

    double threshold = cfg->num("watch", "approval_after_sec", 20.0);
    if (threshold <= 0) return results;   // 0 = 关掉这个判断
    double now = NowUnix();

    std::vector<std::pair<std::string, ClassifyMemo::PendingFile>> files;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (const auto& item : memo_.files) files.push_back(item);
    }

    for (const auto& [file, pending] : files) {
        std::string thread_id;
        {
            std::lock_guard<std::mutex> guard(lock_);
            auto found = file_thread_.find(file);
            if (found != file_thread_.end()) thread_id = found->second;
        }
        if (thread_id.empty()) continue;

        // 这一批待定的调用；按 call_id 排序当签名，同一批只推一次
        std::vector<std::string> ids(pending.open.begin(), pending.open.end());
        std::sort(ids.begin(), ids.end());
        std::string signature = Join(ids, ",");

        if (pending.open.empty()) {
            // 结果回来了：如果之前推过"等待审批"，现在补一条"继续工作"
            bool pushed_before = false;
            {
                std::lock_guard<std::mutex> guard(lock_);
                auto found = memo_.pushed_approval.find(file);
                if (found != memo_.pushed_approval.end()) {
                    pushed_before = true;
                    memo_.pushed_approval.erase(found);
                }
            }
            if (pushed_before) {
                Json thread = store_->WithLock([&](Json& data) -> Json {
                    const Json& threads = data.get("threads");
                    return threads.has(thread_id) ? threads.get(thread_id) : Json::Object();
                });
                Json fields = Json::Object();
                fields.set("status", Json::Str("working"));
                fields.set("status_label", Json::Str(StatusLabel("working")));
                fields.set("summary", Json::Str("已授权，继续执行"));
                store_->SetThreadStatus(thread_id, fields);
                store_->Save();

                Event event;
                event.kind = "working";
                event.thread_id = thread_id;
                event.turn_id = thread.get_str("turn_id");
                event.text = "已授权，继续执行";
                event.timestamp = now;
                event.dedupe = Cat("resumed:", signature);
                std::vector<Json> pushed = NotifyEvent(event);
                results.insert(results.end(), pushed.begin(), pushed.end());
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(lock_);
            auto found = memo_.pushed_approval.find(file);
            if (found != memo_.pushed_approval.end() && found->second == signature) {
                continue;   // 这一批已经推过了
            }
        }

        // 只有"这个线程正在干活"才可能是等授权；已经结束/空闲的不算
        Json thread = store_->WithLock([&](Json& data) -> Json {
            const Json& threads = data.get("threads");
            return threads.has(thread_id) ? threads.get(thread_id) : Json::Object();
        });
        if (thread.get_str("status") != "working") continue;

        double idle = now - pending.last_record_ts;
        if (pending.last_record_ts > 0 && idle < threshold) continue;   // 还在动，多半是命令在跑

        Event event;
        event.kind = "waiting_approval";
        event.thread_id = thread_id;
        event.turn_id = thread.get_str("turn_id");
        // 只归一化命令本身：如果把前缀和命令拼起来再转，命令里只要有一个非法字节，
        // 整串就不是合法 UTF-8，连正确的前缀也会被当成 GBK 重解一遍 —— 反而更乱。
        std::string command = NormalizeToUtf8(pending.last_command);
        event.text = command.empty() ? std::string("有一条命令在等你授权")
                                     : "等你授权执行：" + command;
        event.timestamp = now;
        event.dedupe = Cat("approval-pending:", signature);
        event.detail = Json::Object();
        event.detail.set("call_id", Json::Str(pending.last_call_id));

        // 状态先落库（和 ApplyEvent 一样），否则界面/灯还停在"工作中"
        Json fields = Json::Object();
        fields.set("status", Json::Str("waiting_approval"));
        fields.set("status_label", Json::Str(StatusLabel("waiting_approval")));
        fields.set("summary", Json::Str(event.text));
        store_->SetThreadStatus(thread_id, fields);
        store_->Save();

        std::vector<Json> pushed = NotifyEvent(event);
        if (!pushed.empty()) {
            std::lock_guard<std::mutex> guard(lock_);
            memo_.pushed_approval[file] = signature;
        }
        results.insert(results.end(), pushed.begin(), pushed.end());
    }
    return results;
}

std::vector<Json> Monitor::HeartbeatOnce() {
    std::shared_ptr<Config> cfg = cfg_;
    std::vector<Json> results;
    double now = NowUnix();

    double interval = cfg->num("notify", "heartbeat_sec", 0.0);
    if (interval > 0 && now - last_heartbeat_at_ >= interval) {
        auto [thread_id, thread] = store_->CurrentThread();
        if (!thread_id.empty() && thread.get_str("status") == "working") {
            Event event;
            event.kind = "working";
            event.thread_id = thread_id;
            event.turn_id = thread.get_str("turn_id");
            event.text = Cat("仍在工作中（已持续 ",
                             FormatInt((long long)(now - thread.get_double("started_ts", now))),
                             " 秒）");
            event.timestamp = now;
            event.dedupe = Cat("heartbeat:", FormatInt((long long)(now / interval)));
            std::vector<Json> pushed = NotifyEvent(event);
            results.insert(results.end(), pushed.begin(), pushed.end());
            last_heartbeat_at_ = now;
        }
    }

    double stall = cfg->num("notify", "stall_after_sec", 0.0);
    if (stall > 0) {
        auto [thread_id, thread] = store_->CurrentThread();
        if (!thread_id.empty() && thread.get_str("status") == "working" &&
            now - thread.get_double("updated_ts", now) > stall) {
            Json fields = Json::Object();
            fields.set("status", Json::Str("idle"));
            fields.set("status_label", Json::Str("空闲"));
            store_->SetThreadStatus(thread_id, fields);
            store_->Save();
        }
    }
    return results;
}

std::map<std::string, Json> Monitor::LatestByAgent() {
    std::shared_ptr<Config> cfg = cfg_;
    return store_->LatestByAgent(cfg->str_list("watch", "ignore_thread_names"),
                                 cfg->flag("watch", "only_user_threads", true));
}

std::vector<Json> Monitor::DeviceSyncOnce(bool force) {
    std::shared_ptr<Config> cfg = cfg_;
    std::vector<Json> results;
    double interval = cfg->num("notify", "device_resync_sec", 0.0);
    double now = NowUnix();
    if (!force && (interval <= 0 || now - last_device_sync_ < interval)) return results;
    if (DeviceTargets(*cfg).empty()) return results;
    last_device_sync_ = now;

    for (auto& [agent, record] : LatestByAgent()) {
        std::string status = record.get_str("status");
        if (status.empty()) continue;
        std::string text = record.get_str("summary");
        std::string signature = status + "\x1f" + text;
        {
            std::lock_guard<std::mutex> guard(lock_);
            auto found = last_sync_sent_.find(agent);
            if (!force && found != last_sync_sent_.end() && found->second == signature) {
                continue;   // 和上次校准一样，不打扰设备
            }
        }
        PushOptions options;
        options.text = text;
        options.thread_name = record.get_str("name");
        if (options.thread_name.empty()) options.thread_name = record.get_str("thread_id");
        options.cwd = record.get_str("cwd");
        options.source = "状态校准";
        options.agent = agent;
        options.to_feishu = false;
        options.to_local = false;   // 校准是"对答案"，不该每 60 秒响一声
        auto [ok, detail] = PushStatus(*cfg, status, options);

        {
            std::lock_guard<std::mutex> guard(lock_);
            if (ok) {
                last_sync_sent_[agent] = signature;
            } else {
                last_sync_sent_.erase(agent);   // 没成功就别记成"已同步"
            }
        }
        LogInfo("monitor", Cat("状态校准 [", agent, "] ",
                               options.thread_name.empty() ? record.get_str("thread_id")
                                                           : options.thread_name,
                               " -> ", detail));
        Json entry = Json::Object();
        entry.set("agent", Json::Str(agent));
        entry.set("status", Json::Str(status));
        entry.set("ok", Json::Bool(ok));
        entry.set("detail", Json::Str(detail));
        results.push_back(entry);
    }
    return results;
}

bool Monitor::HasQueuedWork(const std::string& thread_id) {
    std::shared_ptr<Config> cfg = cfg_;
    if (!cfg->flag("watch", "check_queue", true)) return false;
    try {
        return cn::HasQueuedWork(thread_id, cfg->str("watch", "queue_db", "~/.codex/queue_1.sqlite"),
                                 cfg->num("watch", "queue_max_age_sec", 0.0));
    } catch (...) {
        return false;   // 读不到就当没有排队
    }
}

Json Monitor::Snapshot() {
    std::shared_ptr<Config> cfg = cfg_;
    store_->Load();   // 别人（接口进程 / 上一次运行）可能写过状态
    std::lock_guard<std::mutex> guard(lock_);   // names_ 归监控线程改，接口线程读它要拿锁
    return SnapshotFromStore(store_, names_, cfg->str_list("watch", "ignore_thread_names"),
                             cfg->flag("watch", "only_user_threads", true));
}

void Monitor::RunForever(std::atomic<bool>* stop) {
    std::shared_ptr<Config> cfg = cfg_;
    double interval = cfg->num("watch", "poll_interval_sec", 3.0);
    LogInfo("monitor", Cat("监控启动：轮询间隔 ", FormatDouble(interval, 1), "s"));
    while (!(stop && stop->load())) {
        try {
            ScanOnce();
            ApprovalCheckOnce();
            HeartbeatOnce();
            DeviceSyncOnce();
        } catch (const std::exception& error) {
            LogError("monitor", Cat("扫描异常: ", error.what()));
        }
        // 细分睡眠：让"延迟完成"到点就能推出去，不用等下一轮扫描
        double deadline = NowUnix() + interval;
        while (!(stop && stop->load()) && NowUnix() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            try {
                FlushPending();
            } catch (const std::exception& error) {
                LogError("monitor", Cat("推送延迟完成异常: ", error.what()));
            }
        }
    }
    LogInfo("monitor", "监控已停止");
}

}  // namespace cn
