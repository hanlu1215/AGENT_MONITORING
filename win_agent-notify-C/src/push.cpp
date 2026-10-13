#include "push.h"

#include "device.h"
#include "feishu.h"
#include "localsound.h"
#include "util/str.h"

namespace cn {

std::pair<bool, std::string> PushStatus(const Config& cfg, const std::string& status,
                                        const PushOptions& options) {
    std::string text = CleanText(options.text);
    std::string thread_name = CleanText(options.thread_name);
    std::string cwd = CleanText(options.cwd);
    std::string title = CleanText(options.title);
    std::string source = CleanText(options.source);
    std::string agent = CleanText(options.agent);

    std::vector<std::string> parts;
    bool device_ok = true;

    // 本机提示音放最前面：它是"立刻能听到"的那一路，不该被联网/设备拖慢。
    // （状态校准那种"只推设备"的调用要传 to_local=false，否则每 60 秒响一次）
    if (options.to_local && !cfg.data.get_bool("dry_run", false)) {
        LocalSoundAlert(cfg, status, text, agent);
    }

    // 不看老 device 段的 enabled —— 目标列表由 DeviceTargets() 统一决定
    if (options.to_device) {
        auto [ok, detail] = DevicePushStatus(cfg, status, text, thread_name, source, agent);
        device_ok = ok;
        if (ok) {
            parts.push_back(Cat("设备=", detail.empty() ? "ok" : detail));
        } else if (detail != "未配置设备" && detail != "dry-run" && detail != "device 未启用") {
            parts.push_back(Cat("设备=", detail));
        }
    }

    // 飞书订阅表：只推你关心的状态（默认只有「等待审批」和「已完成」）。
    // 设备 / 本机提示音不受它影响 —— 它们由 notify.on 控制。
    bool feishu_subscribed = options.force_feishu;
    if (!feishu_subscribed) {
        const Json& subscribed = cfg.get("feishu").get("on");
        std::string normalized = NormalizeStatus(status);
        if (subscribed.is_array()) {
            for (size_t i = 0; i < subscribed.size(); ++i) {
                if (NormalizeStatus(subscribed.at(i).as_string()) == normalized) {
                    feishu_subscribed = true;
                    break;
                }
            }
        } else {
            feishu_subscribed = true;   // 没写这个字段 = 不过滤（老配置行为）
        }
    }

    if (!options.to_feishu || !feishu_subscribed) {
        // 不推飞书时，"成功"就看设备那一路；只推板子时也一样
        if (!options.to_feishu) {
            return {device_ok, parts.empty() ? std::string("只推板子") : Join(parts, " | ")};
        }
        parts.push_back(Cat("飞书=跳过(", StatusLabel(status), "不在 feishu.on 里)"));
        return {device_ok, Join(parts, " | ")};
    }

    auto [feishu_ok, feishu_detail] =
        FeishuPushStatus(cfg, status, text, thread_name, cwd, title, source, agent);
    parts.push_back(feishu_ok ? "飞书=ok" : Cat("飞书=", feishu_detail));
    return {feishu_ok, Join(parts, " | ")};
}

}  // namespace cn
