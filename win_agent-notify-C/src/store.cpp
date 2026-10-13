#include "store.h"

#include <algorithm>

#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

constexpr size_t kMaxNotifiedKeys = 4000;

Json EmptyState() {
    Json data = Json::Object();
    data.set("version", Json::Num(1));
    data.set("baseline_done", Json::Bool(false));
    data.set("offsets", Json::Object());
    data.set("threads", Json::Object());
    data.set("notified", Json::Object());
    Json counters = Json::Object();
    counters.set("pushed", Json::Num(0));
    counters.set("failed", Json::Num(0));
    data.set("counters", counters);
    data.set("updated_at", Json());
    return data;
}

double RecordTs(const Json& record) {
    if (!record.is_object()) return 0.0;
    return record.get_double("updated_ts", 0.0);
}

}  // namespace

std::string ThreadTimestamp() { return NowStamp(); }

StateStore::StateStore(const std::string& path) : path_(path), data_(EmptyState()) {
    Load();
}

void StateStore::Load() {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (!FileExists(path_)) return;
    Json disk;
    std::string error;
    if (!Json::ParseFile(path_, &disk, &error) || !disk.is_object()) {
        // 状态文件损坏时从空状态重建，避免监控崩溃
        LogWarn("store", Cat("状态文件损坏，已重建：", path_, "（", error, "）"));
        return;
    }
    MergeFromDisk(disk);
}

void StateStore::MergeFromDisk(const Json& disk) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    const Json& disk_threads = disk.get("threads");
    for (size_t i = 0; i < disk_threads.size(); ++i) {
        const std::string& key = disk_threads.key_ref(i);
        const Json& record = disk_threads.at(i);
        if (!record.is_object()) continue;
        Json& mine = data_["threads"][key];
        if (!mine.is_object()) {
            mine = record;
            continue;
        }
        if (RecordTs(record) >= RecordTs(mine)) {
            mine = record;
        } else {
            // 磁盘更旧：只把内存里缺的字段补上，别动状态
            for (size_t f = 0; f < record.size(); ++f) {
                const std::string& field = record.key_ref(f);
                if (!mine.has(field)) mine.set(field, record.at(f));
            }
        }
    }

    const Json& disk_offsets = disk.get("offsets");
    for (size_t i = 0; i < disk_offsets.size(); ++i) {
        long long offset = disk_offsets.at(i).as_int(0);
        const std::string& key = disk_offsets.key_ref(i);
        if (offset > data_["offsets"].get_int(key, 0)) {
            data_["offsets"].set(key, Json::Num(offset));
        }
    }

    const Json& disk_notified = disk.get("notified");
    for (size_t i = 0; i < disk_notified.size(); ++i) {
        const std::string& key = disk_notified.key_ref(i);
        double when = disk_notified.at(i).as_double(0.0);
        Json& mine = data_["notified"][key];
        if (mine.is_null() || when > mine.as_double(0.0)) {
            mine = Json::Num(when);
        }
    }

    const Json& disk_counters = disk.get("counters");
    for (size_t i = 0; i < disk_counters.size(); ++i) {
        const std::string& key = disk_counters.key_ref(i);
        long long count = disk_counters.at(i).as_int(0);
        if (count > data_["counters"].get_int(key, 0)) {
            data_["counters"].set(key, Json::Num(count));
        }
    }

    if (disk.get_bool("baseline_done", false)) {
        data_["baseline_done"] = Json::Bool(true);
    }
    if (!disk.get("version").is_null()) data_["version"] = disk.get("version");
    if (!disk.get("updated_at").is_null() && !disk.get("updated_at").as_string().empty()) {
        data_["updated_at"] = disk.get("updated_at");
    }
}

bool StateStore::Save(bool force) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (!force && !dirty_ && FileExists(path_)) return false;
    data_.set("updated_at", Json::Str(NowStamp()));
    std::string dir = PathDirName(path_);
    if (!dir.empty()) EnsureDir(dir);
    if (!WriteFileAtomic(path_, data_.Dump(2))) {
        LogWarn("store", Cat("状态写盘失败：", path_));
        return false;
    }
    dirty_ = false;
    return true;
}

long long StateStore::Offset(const std::string& path) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    return data_["offsets"].get_int(path, 0);
}

void StateStore::SetOffset(const std::string& path, long long offset) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (data_["offsets"].get_int(path, 0) != offset) {
        data_["offsets"].set(path, Json::Num(offset));
        dirty_ = true;
    }
}

Json& StateStore::Thread(const std::string& thread_id) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    return data_["threads"][thread_id];
}

bool StateStore::HasThread(const std::string& thread_id) const {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    return data_.get("threads").has(thread_id);
}

void StateStore::SetThreadStatus(const std::string& thread_id, const Json& fields) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    Json& record = data_["threads"][thread_id];
    if (!record.is_object()) record = Json::Object();
    for (size_t i = 0; i < fields.size(); ++i) {
        record.set(fields.key_ref(i), fields.at(i));
    }
    record.set("updated_at", Json::Str(NowStamp()));
    record.set("updated_ts", Json::Num(NowUnix()));
    dirty_ = true;
}

std::pair<std::string, Json> StateStore::CurrentThread() {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    const Json& threads = data_["threads"];
    std::string best_id;
    double best_ts = -1.0;
    Json best;
    for (size_t i = 0; i < threads.size(); ++i) {
        double ts = RecordTs(threads.at(i));
        if (ts > best_ts) {
            best_ts = ts;
            best_id = threads.key_ref(i);
            best = threads.at(i);
        }
    }
    return {best_id, best};
}

std::map<std::string, Json> StateStore::LatestByAgent(
    const std::vector<std::string>& ignore_name_substrings, bool only_user_threads) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    std::map<std::string, Json> best;
    const Json& threads = data_["threads"];
    for (size_t i = 0; i < threads.size(); ++i) {
        Json record = threads.at(i);
        if (!record.is_object() || !record.has_str("status")) continue;

        if (only_user_threads) {
            std::string source = ToLowerAscii(Trim(record.get_str("thread_source")));
            if (!source.empty() && source != "user") continue;
        }
        {
            std::string name = ToLowerAscii(Trim(record.get_str("name")));
            bool ignored = false;
            for (const std::string& pattern : ignore_name_substrings) {
                if (!pattern.empty() && name.find(ToLowerAscii(pattern)) != std::string::npos) {
                    ignored = true;
                    break;
                }
            }
            if (ignored) continue;
        }
        const Json& agent_source = record.get("agent_source");
        if (agent_source.is_object() && agent_source.has("subagent")) continue;
        if (agent_source.is_string() &&
            ToLowerAscii(agent_source.as_string()).find("subagent") != std::string::npos) {
            continue;
        }

        std::string agent = ToLowerAscii(Trim(record.get_str("agent", "codex")));
        if (agent != "codex" && agent != "claude") agent = "codex";
        auto found = best.find(agent);
        if (found == best.end() || RecordTs(record) > RecordTs(found->second)) {
            record.set("thread_id", Json::Str(threads.key_ref(i)));
            best[agent] = record;
        }
    }
    return best;
}

bool StateStore::IsNotified(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    return data_.get("notified").has(key);
}

void StateStore::MarkNotified(const std::string& key) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    data_["notified"].set(key, Json::Num(NowUnix()));
    dirty_ = true;
    Json& notified = data_["notified"];
    if (notified.size() > kMaxNotifiedKeys) {
        // 只留最新的一半
        std::vector<std::pair<std::string, double>> items;
        items.reserve(notified.size());
        for (size_t i = 0; i < notified.size(); ++i) {
            items.emplace_back(notified.key_ref(i), notified.at(i).as_double(0.0));
        }
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        Json trimmed = Json::Object();
        size_t keep = kMaxNotifiedKeys / 2;
        for (size_t i = 0; i < items.size() && i < keep; ++i) {
            trimmed.set(items[i].first, Json::Num(items[i].second));
        }
        notified = trimmed;
    }
}

void StateStore::Bump(const std::string& counter, int delta) {
    std::lock_guard<std::recursive_mutex> guard(lock_);
    Json& counters = data_["counters"];
    if (!counters.is_object()) counters = Json::Object();
    counters.set(counter, Json::Num(counters.get_int(counter, 0) + delta));
    dirty_ = true;
}

}  // namespace cn
