// 轮询监控：读取会话文件 → 状态机 → 按策略推送。
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "events.h"
#include "json.h"
#include "store.h"

namespace cn {

class Monitor {
public:
    Monitor(std::shared_ptr<Config> cfg, StateStore* store);

    void SetConfig(std::shared_ptr<Config> cfg);

    // ---- 基础信息 ----
    void RefreshNames(bool force = false);
    std::string ThreadName(const std::string& thread_id);
    bool IsInternalThread(const std::string& thread_id);

    // ---- 扫描 ----
    Json ScanOnce(bool notify = true);
    void RunForever(std::atomic<bool>* stop);

    // ---- 状态机 ----
    std::vector<Json> ApplyEvent(const Event& event, bool notify);
    std::vector<Json> NotifyEvent(const Event& event);
    std::vector<Json> FlushPending();
    std::vector<Json> HeartbeatOnce();
    // 有一条工具调用发出去了、结果迟迟没回来 -> 判成"等待审批"（见 README 的代价说明）
    std::vector<Json> ApprovalCheckOnce();
    std::vector<Json> DeviceSyncOnce(bool force = false);

    // ---- 查询 ----
    std::map<std::string, Json> LatestByAgent();
    Json Snapshot();

private:
    std::vector<std::pair<Json, std::string>> ReadRecords(const std::string& path);
    bool HasQueuedWork(const std::string& thread_id);
    std::string CurrentConfigSignature() const;

    std::shared_ptr<Config> cfg_;
    StateStore* store_;
    std::mutex lock_;                     // 保护下面这些跨记录状态

    std::map<std::string, std::unique_ptr<SessionTail>> tails_;
    std::map<std::string, std::string> names_;
    double names_loaded_at_ = 0.0;
    std::map<std::string, double> last_notify_at_;
    std::map<std::string, std::string> last_kind_;
    std::map<std::string, std::string> last_pushed_status_;
    std::map<std::string, Json> pending_complete_;   // 线程 -> {event, due, wait_from}
    double last_heartbeat_at_ = 0.0;
    double last_device_sync_ = 0.0;
    std::map<std::string, std::string> last_sync_sent_;   // agent -> signature
    std::map<std::string, std::string> file_thread_;
    ClassifyMemo memo_;
};

// 从状态文件生成快照（接口/界面用）
Json SnapshotFromStore(StateStore* store, const std::map<std::string, std::string>& names,
                       const std::vector<std::string>& ignore_name_substrings,
                       bool only_user_threads);

}  // namespace cn
