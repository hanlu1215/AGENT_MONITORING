// 只读监控 Claude Code —— 不装钩子、不改它的任何配置。
//
// 数据源（都是只读）：
//   1. ~/.claude/sessions/<pid>.json   活会话注册表，直接带 status
//   2. ~/.claude/projects/**/*.jsonl   会话记录，取摘要 + 兜底判断卡在等授权
//
// 推送和 Python 版一样走本地接口 POST /status（agent=claude）。
#pragma once

#include <map>
#include <memory>
#include <string>

#include "config.h"
#include "json.h"

namespace cn {

struct TranscriptState {
    int unresolved = 0;
    std::string pending_tool;
    std::string summary;
    double last_ts = 0.0;
};

// 从会话记录尾部取：未完成的工具调用数 + 最后一条助手文字
TranscriptState ReadTranscriptState(const std::string& path, long long tail_bytes = 262144);

class ClaudeMonitor {
public:
    explicit ClaudeMonitor(std::shared_ptr<Config> cfg);
    void SetConfig(std::shared_ptr<Config> cfg);

    // 返回本次推送的状态列表（可能为空）
    Json ScanOnce(bool force = false);

    // 自己的推送线程（保证 working → completed 的顺序）
    void Shutdown();

private:
    void Emit(const std::string& status, const std::string& text, const std::string& thread,
              const std::string& source);

    std::shared_ptr<Config> cfg_;
    std::map<std::string, std::string> prev_;        // sessionId -> idle / busy
    std::map<std::string, std::string> sub_;         // busy 里细分：working / waiting
    std::map<std::string, double> busy_since_;
    std::map<std::string, double> seen_runtime_;
    double last_poll_ = 0.0;
    bool baseline_done_ = false;
};

}  // namespace cn
