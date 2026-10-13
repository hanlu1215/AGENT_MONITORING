// 运行状态持久化：文件读取偏移、线程状态、已推送去重表。
// 与 Python 版 codex_notify/store.py 行为一致。
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json.h"

namespace cn {

class StateStore {
public:
    explicit StateStore(const std::string& path);

    // ---- 读写 ----
    // 从磁盘合并：绝不回退内存里更新的记录（按 updated_ts 逐条比）
    void Load();
    // 有变化才写盘；force=true 强制写
    bool Save(bool force = false);
    void MarkDirty() { dirty_ = true; }

    // ---- 偏移 ----
    long long Offset(const std::string& path);
    void SetOffset(const std::string& path, long long offset);

    // ---- 线程状态 ----
    // 注意：返回的是引用，调用方需持有锁（用 WithLock）或独占使用
    Json& Thread(const std::string& thread_id);
    void SetThreadStatus(const std::string& thread_id, const Json& fields);
    bool HasThread(const std::string& thread_id) const;
    std::pair<std::string, Json> CurrentThread();
    // 按 agent 分组取最新线程（给校准 / 界面用）
    std::map<std::string, Json> LatestByAgent(
        const std::vector<std::string>& ignore_name_substrings, bool only_user_threads);

    // ---- 去重 ----
    bool IsNotified(const std::string& key) const;
    void MarkNotified(const std::string& key);

    void Bump(const std::string& counter, int delta = 1);
    Json& Data() { return data_; }
    const Json& Data() const { return data_; }

    // 对整份数据加锁执行（跨字段的读改写用它）
    template <typename Fn>
    auto WithLock(Fn&& fn) -> decltype(fn(std::declval<Json&>())) {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        return fn(data_);
    }

    std::string path() const { return path_; }

    // 把一段线程记录合并进内存（Load 用的合并规则，暴露给自测）
    void MergeFromDisk(const Json& disk);

private:
    std::string path_;
    mutable std::recursive_mutex lock_;
    bool dirty_ = false;
    Json data_;
};

// 生成一份 "YYYY-MM-DDTHH:MM:SS" 时间戳
std::string ThreadTimestamp();

}  // namespace cn
