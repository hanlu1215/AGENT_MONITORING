#include "events.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

// 各版本的 Codex 可能用 snake_case（会话日志）或 kebab-case（hooks），
// 所以统一小写 + 空格换下划线后按**子串**匹配，升级后也不会失配。
const char* kStartHints[] = {"task_started", "turn_started", "task-start", "turn-start"};
const char* kDoneHints[] = {"task_complete", "turn_complete", "agent-turn-complete",
                            "task-completed"};
const char* kAbortHints[] = {"aborted", "interrupted", "stream_error", "task_failed", "error"};
const char* kApprovalHints[] = {"approval",          "permission_request", "request_permission",
                                "permissions_request", "elicitation",      "permission-request"};

std::string Normalize(const Json& value) {
    if (!value.is_string()) return std::string();
    std::string text = ToLowerAscii(Trim(value.as_string()));
    for (char& ch : text) {
        if (ch == ' ') ch = '_';
    }
    return text;
}

bool MatchesAny(const std::string& payload_type, const char* const* hints, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (payload_type.find(hints[i]) != std::string::npos) return true;
    }
    return false;
}

bool Is(const std::string& payload_type, const char* const* hints, size_t count) {
    return MatchesAny(payload_type, hints, count);
}

std::string TextOf(const Json& content) {
    if (content.is_string()) return content.as_string();
    if (content.is_array()) {
        std::vector<std::string> parts;
        for (size_t i = 0; i < content.size(); ++i) {
            const Json& item = content.at(i);
            if (item.is_object()) {
                std::string text = item.get_str("text");
                if (text.empty()) text = item.get_str("content");
                if (!text.empty()) parts.push_back(text);
            } else if (item.is_string()) {
                parts.push_back(item.as_string());
            }
        }
        return Join(parts, "\n");
    }
    if (content.is_object()) return content.get_str("text");
    return std::string();
}

std::string DescribeItem(const Json& item) {
    std::string kind = Normalize(item.get("type"));
    if (kind == "commandexecution") {
        std::string command;
        const Json& value = item.get("command");
        if (value.is_array()) {
            std::vector<std::string> parts;
            for (size_t i = 0; i < value.size(); ++i) parts.push_back(value.at(i).as_string());
            command = Join(parts, " ");
        } else if (value.is_string()) {
            command = value.as_string();
        }
        if (command.empty()) command = item.get_str("parsed_cmd");
        return Trim("执行命令：" + command);
    }
    if (kind == "filechange") {
        std::vector<std::string> paths;
        const Json& changes = item.get("changes");
        for (size_t i = 0; i < changes.size(); ++i) {
            const Json& change = changes.at(i);
            if (!change.is_object()) continue;
            std::string path = change.get_str("path");
            if (path.empty()) path = change.get_str("file");
            if (!path.empty()) paths.push_back(path);
        }
        std::string joined = Join(paths, "、");
        return joined.empty() ? "修改文件" : "修改文件：" + joined;
    }
    if (kind == "agentmessage") {
        std::string text = Trim(TextOf(item.get("content")));
        return LooksInternal(text) ? std::string() : text;
    }
    if (kind == "reasoning") return std::string();
    std::string text = Trim(TextOf(item.get("content")));
    return LooksInternal(text) ? std::string() : text;
}

std::string JsonText(const Json& value) {
    return value.is_string() ? value.as_string() : std::string();
}

}  // namespace

bool LooksInternal(const std::string& text) {
    std::string stripped = Trim(text);
    if (stripped.empty()) return false;
    if (stripped.size() < 400 && stripped.front() == '{' && stripped.back() == '}') return true;
    if (stripped.find("The following is the Codex agent history") != std::string::npos) return true;
    if (StartsWith(stripped, ">>> TRANSCRIPT")) return true;
    return false;
}

std::string ThreadIdFromPath(const std::string& path) {
    return FindUuid(PathFileName(path));
}

std::vector<Event> Classify(const Json& record, const std::string& thread_id, ClassifyMemo* memo,
                            const std::string& file_key) {
    std::vector<Event> events;
    if (!record.is_object()) return events;
    static thread_local ClassifyMemo kThrowaway;
    if (!memo) memo = &kThrowaway;

    std::string top_type = Normalize(record.get("type"));
    const Json& payload = record.get("payload");
    const Json& payload_obj = payload.is_object() ? payload : Json();
    std::string payload_type;
    if (payload_obj.is_object()) {
        payload_type = Normalize(payload_obj.get("type"));
        if (payload_type.empty()) payload_type = Normalize(payload_obj.get("event"));
        if (payload_type.empty()) payload_type = Normalize(payload_obj.get("hook_event_name"));
    }
    const Json& at_value = record.get("timestamp");
    std::string at = JsonText(at_value);
    double ts = ParseIso8601(at);
    if (ts <= 0.0) ts = NowUnix();
    // 记住"这个文件最后活动是什么时候"，判断等待审批要靠它
    if (!file_key.empty()) {
        ClassifyMemo::PendingFile& file = memo->files[file_key];
        if (ts > file.last_record_ts) file.last_record_ts = ts;
    }
    long long ordinal = record.get_int("ordinal", 0);

    if (top_type == "session_meta") {
        Event event;
        event.kind = "meta";
        event.thread_id = thread_id;
        if (event.thread_id.empty()) {
            event.thread_id = payload_obj.get_str("session_id");
            if (event.thread_id.empty()) event.thread_id = payload_obj.get_str("id");
        }
        event.at = at;
        event.timestamp = ts;
        event.text = payload_obj.get_str("cwd");
        event.dedupe = Cat("meta:", ordinal);
        event.detail = Json::Object();
        event.detail.set("cwd", payload_obj.get("cwd"));
        event.detail.set("cli_version", payload_obj.get("cli_version"));
        event.detail.set("model_provider", payload_obj.get("model_provider"));
        event.detail.set("originator", payload_obj.get("originator"));
        event.detail.set("thread_source", payload_obj.get("thread_source"));
        event.detail.set("source", payload_obj.get("source"));
        events.push_back(event);
        return events;
    }

    if (top_type != "event_msg" && top_type != "event" && top_type != "response_item" &&
        top_type != "notification") {
        return events;
    }

    std::string turn_id = payload_obj.get_str("turn_id");
    if (turn_id.empty()) turn_id = payload_obj.get_str("turn-id");
    if (turn_id.empty()) turn_id = payload_obj.get_str("turnId");
    std::string tid = payload_obj.get_str("thread_id");
    if (tid.empty()) tid = payload_obj.get_str("thread-id");
    if (tid.empty()) tid = payload_obj.get_str("session_id");
    if (tid.empty()) tid = thread_id;

    Json base_detail = Json::Object();
    base_detail.set("source", Json::Str(top_type));
    base_detail.set("payload_type", Json::Str(payload_type));

    auto make = [&](const std::string& kind, const std::string& text, const std::string& dedupe) {
        Event event;
        event.kind = kind;
        event.thread_id = tid;
        event.turn_id = turn_id;
        event.text = text;
        event.at = at;
        event.timestamp = ts;
        event.dedupe = dedupe;
        event.detail = base_detail;
        return event;
    };

    if (top_type == "event_msg" || top_type == "event") {
        if (Is(payload_type, kApprovalHints, std::size(kApprovalHints))) {
            std::string text = Trim(payload_obj.get_str("reason"));
            if (text.empty()) text = Trim(payload_obj.get_str("message"));
            if (text.empty()) text = "Codex 需要你确认一项操作";
            Event event = make("waiting_approval", text,
                               Cat("approval:", ordinal, ":", payload_type));
            event.detail.set("raw_type", payload_obj.get("type"));
            events.push_back(event);
            return events;
        }
        if (Is(payload_type, kDoneHints, std::size(kDoneHints))) {
            std::string summary = Trim(payload_obj.get_str("last_agent_message"));
            if (summary.empty()) summary = Trim(payload_obj.get_str("message"));
            if (LooksInternal(summary)) summary.clear();
            events.push_back(make("completed", summary,
                                  Cat("completed:", turn_id.empty() ? std::to_string(ordinal)
                                                                    : turn_id)));
            return events;
        }
        if (Is(payload_type, kStartHints, std::size(kStartHints))) {
            Event event = make("working", "",
                               Cat("working:", turn_id.empty() ? std::to_string(ordinal)
                                                               : turn_id));
            event.detail.set("started_at", payload_obj.get("started_at"));
            events.push_back(event);
            return events;
        }
        if (Is(payload_type, kAbortHints, std::size(kAbortHints))) {
            std::string text = Trim(payload_obj.get_str("reason"));
            if (text.empty()) text = Trim(payload_obj.get_str("message"));
            Event event = make("interrupted", text,
                               Cat("interrupted:", turn_id.empty() ? std::to_string(ordinal)
                                                                   : turn_id));
            event.detail.set("raw_type", payload_obj.get("type"));
            events.push_back(event);
            return events;
        }
        if (payload_type == "item_completed") {
            const Json& item = payload_obj.get("item");
            Json item_obj = item.is_object() ? item : Json::Object();
            Event event = make("progress", DescribeItem(item_obj),
                               Cat("progress:", ordinal, ":", Normalize(item_obj.get("type"))));
            event.detail.set("item_type", item_obj.get("type"));
            event.detail.set("item_id", item_obj.get("id"));
            event.detail.set("phase", item_obj.get("phase"));
            events.push_back(event);
            return events;
        }
        return events;
    }

    if (top_type == "response_item") {
        if (payload_type == "function_call" || payload_type == "custom_tool_call") {
            std::string name = payload_obj.get_str("name");
            if (name.empty()) name = payload_obj.get_str("namespace");
            if (name.empty()) name = "tool";
            std::string call_id = payload_obj.get_str("call_id");
            if (call_id.empty()) call_id = payload_obj.get_str("id");
            std::string arguments_text;
            // 登记"这个调用还没有结果"：Codex 弹审批框时，日志里就停在这一步
            if (!file_key.empty() && !call_id.empty()) {
                ClassifyMemo::PendingFile& file = memo->files[file_key];
                file.open.insert(call_id);
                file.last_call_id = call_id;
                if (arguments_text.empty()) {
                    const Json& args = payload_obj.get("arguments");
                    arguments_text = args.is_string() ? args.as_string() : args.Dump(0);
                }
                file.last_command = TruncateCodePoints(CollapseWhitespace(arguments_text), 160);
                if (ts > file.last_record_ts) file.last_record_ts = ts;
            }
            std::string lowered = ToLowerAscii(name);
            if (lowered.find("permission") != std::string::npos ||
                lowered.find("approval") != std::string::npos) {
                std::string reason;
                const Json& arguments = payload_obj.get("arguments");
                if (arguments.is_string()) {
                    Json parsed = Json::Parse(arguments.as_string());
                    if (parsed.is_object()) reason = Trim(parsed.get_str("reason"));
                } else if (arguments.is_object()) {
                    reason = Trim(arguments.get_str("reason"));
                }
                if (!call_id.empty()) memo->approvals.insert(call_id);
                Event event = make("waiting_approval",
                                   reason.empty() ? "Codex 需要你确认一项操作" : reason,
                                   Cat("approval:", call_id.empty() ? std::to_string(ordinal)
                                                                    : call_id));
                event.detail.set("tool", Json::Str(name));
                event.detail.set("call_id", Json::Str(call_id));
                events.push_back(event);
                return events;
            }
            Event event = make("progress", "调用工具：" + name,
                               Cat("progress:tool:", call_id.empty() ? std::to_string(ordinal)
                                                                     : call_id));
            event.detail.set("tool", Json::Str(name));
            events.push_back(event);
            return events;
        }
        if (payload_type == "function_call_output" || payload_type == "custom_tool_call_output") {
            std::string call_id = payload_obj.get_str("call_id");
            // 有结果了 = 这个调用不再"待定"（授权给了、或者命令跑完了）
            if (!file_key.empty() && !call_id.empty()) {
                auto found = memo->files.find(file_key);
                if (found != memo->files.end()) found->second.open.erase(call_id);
            }
            if (!call_id.empty() && memo->approvals.count(call_id)) {
                memo->approvals.erase(call_id);
                Event event = make("working", "已收到确认，继续执行", "resumed:" + call_id);
                event.detail.set("call_id", Json::Str(call_id));
                events.push_back(event);
            }
            return events;
        }
        if ((payload_type.empty() || payload_type == "message") &&
            Normalize(payload_obj.get("role")) == "assistant") {
            std::string text = Trim(TextOf(payload_obj.get("content")));
            if (!text.empty()) {
                std::string id = payload_obj.get_str("id");
                Event event = make("progress", text,
                                   Cat("progress:assistant:", id.empty() ? std::to_string(ordinal)
                                                                         : id));
                event.detail.set("role", Json::Str("assistant"));
                events.push_back(event);
            }
            return events;
        }
        return events;
    }

    if (top_type == "notification") {
        if (Is(payload_type, kApprovalHints, std::size(kApprovalHints))) {
            std::string text = payload_obj.get_str("message");
            events.push_back(make("waiting_approval",
                                  text.empty() ? "Codex 需要你确认一项操作" : text,
                                  Cat("approval:notify:", ordinal)));
        } else if (Is(payload_type, kDoneHints, std::size(kDoneHints))) {
            std::string text = payload_obj.get_str("last_assistant_message");
            if (text.empty()) text = payload_obj.get_str("message");
            events.push_back(make("completed", text,
                                  Cat("completed:notify:",
                                      turn_id.empty() ? std::to_string(ordinal) : turn_id)));
        }
        return events;
    }

    return events;
}

SessionTail::SessionTail(std::string path, long long offset)
    : path_(std::move(path)), offset_(offset < 0 ? 0 : offset) {}

std::vector<Json> SessionTail::ReadNew() {
    std::vector<Json> records;
    long long size = FileSize(path_);
    if (size < 0) return records;
    was_reset_ = false;
    if (size < offset_) {   // 文件被截断/重建
        offset_ = 0;
        was_reset_ = true;
        buffer_.clear();
    }
    if (size == offset_) return records;

    std::ifstream file(std::filesystem::u8path(path_), std::ios::binary);
    if (!file) return records;
    file.seekg(offset_);
    std::string chunk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    offset_ += (long long)chunk.size();

    std::string data = buffer_ + chunk;
    buffer_.clear();
    size_t begin = 0;
    while (true) {
        size_t at = data.find('\n', begin);
        if (at == std::string::npos) {
            buffer_ = data.substr(begin);   // 最后一行可能只写了一半，留到下次
            break;
        }
        std::string line = Trim(data.substr(begin, at - begin));
        begin = at + 1;
        if (line.empty()) continue;
        std::string error;
        Json parsed = Json::Parse(line, &error);
        if (!error.empty()) continue;
        records.push_back(std::move(parsed));
    }
    return records;
}

std::vector<std::string> IterSessionFiles(const std::vector<std::string>& dirs, int max_files,
                                          int max_age_days) {
    double cutoff = max_age_days > 0 ? NowUnix() - double(max_age_days) * 86400.0 : 0.0;
    std::vector<std::pair<double, std::string>> found;
    for (const std::string& dir : dirs) {
        if (!DirExists(dir)) continue;
        for (std::string& path : ListFilesRecursive(dir, {".jsonl"})) {
            double mtime = FileMTime(path);
            if (cutoff > 0 && mtime < cutoff) continue;
            found.emplace_back(mtime, std::move(path));
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;   // 修改时间倒序
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < found.size() && (int)out.size() < max_files; ++i) {
        out.push_back(found[i].second);
    }
    return out;
}

std::map<std::string, std::string> LoadThreadNames(const std::string& index_file) {
    std::map<std::string, std::string> names;
    if (!FileExists(index_file)) return names;
    std::ifstream file(std::filesystem::u8path(index_file), std::ios::binary);
    if (!file) return names;
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        std::string error;
        Json record = Json::Parse(line, &error);
        if (!error.empty() || !record.is_object()) continue;
        std::string id = record.get_str("id");
        if (id.empty()) id = record.get_str("thread_id");
        std::string name = record.get_str("thread_name");
        if (name.empty()) name = record.get_str("name");
        if (!id.empty() && !name.empty()) names[id] = name;
    }
    return names;
}

}  // namespace cn
