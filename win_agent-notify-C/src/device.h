// 推送到局域网设备（ESP32 状态灯 / Ubuntu 语音播报……），支持多个目标并行推。
// 协议：POST http://<ip>:<port>/api/codex  {"status","text","thread","source","agent"}
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "json.h"

namespace cn {

// 读取设备的完整 URL；未配置时返回空
std::string DeviceResolveUrl(const Json& target);

// 兼容旧版 device 字段，并返回启用的设备目标列表
std::vector<Json> DeviceTargets(const Config& cfg);

std::string DeviceDescribe(const Config& cfg);   // 一行描述，日志用

// 每个目标最近一次的推送结果（给 /health 看）
struct DeviceResult {
    std::string name;
    std::string url;
    std::string at;
    std::string status;
    bool ok = false;
    std::string detail;
};
std::map<std::string, DeviceResult>& DeviceLastResults();

// 推给所有设备目标。返回 (是否全部成功, 说明文案)
std::pair<bool, std::string> DevicePushStatus(const Config& cfg, const std::string& status,
                                              const std::string& text,
                                              const std::string& thread_name,
                                              const std::string& source,
                                              const std::string& agent);

// GET /api/status 看设备在不在线
std::pair<bool, Json> DeviceProbe(const Config& cfg);
// GET /api/beep?n=&ms= 让设备响几声
std::pair<bool, Json> DeviceBeep(const Config& cfg, int times, int ms);

}  // namespace cn
