// 统一推送入口：一次状态变化，同时送到本机声卡 + 局域网设备 + 飞书。
// 顺序：本机提示音（立刻能听到）→ 设备（局域网，毫秒级）→ 飞书（要联网，可能重试）。
#pragma once

#include <string>
#include <utility>

#include "config.h"

namespace cn {

struct PushOptions {
    std::string text;
    std::string thread_name;
    std::string cwd;
    std::string title;
    std::string source;
    std::string agent;
    bool to_feishu = true;
    bool to_device = true;
    bool to_local = true;
    // 「测试推送」这种明确要发卡片的调用，不受 feishu.on 订阅表限制
    bool force_feishu = false;
};

// 返回 (飞书是否成功, 说明文案)。to_feishu=false 时返回的是设备是否成功。
std::pair<bool, std::string> PushStatus(const Config& cfg, const std::string& status,
                                        const PushOptions& options);

}  // namespace cn
