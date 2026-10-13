#include "device.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "http.h"
#include "util/log.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

std::mutex g_result_lock;

// 目标熔断：失败过的设备在冷却期内直接跳过。
// 为什么需要它：设备不在线时，每次推送要等 timeout×重复次数（实测 3×5 秒 = 16 秒），
// 而监控循环是同步推送的 —— 一分钟推几次，循环就被卡住几次，状态报得又晚又乱。
std::mutex g_cooldown_lock;
std::map<std::string, double> g_cooldown_until;   // 目标名 -> 可以再试的时间
std::map<std::string, double> g_fail_streak;      // 连败次数 -> 退避长度

bool CoolingDown(const std::string& name, double* until) {
    std::lock_guard<std::mutex> guard(g_cooldown_lock);
    auto found = g_cooldown_until.find(name);
    if (found == g_cooldown_until.end()) return false;
    if (NowUnix() >= found->second) {
        g_cooldown_until.erase(found);
        return false;
    }
    if (until) *until = found->second;
    return true;
}

// 指数退避：一直关着的设备，探测间隔 60s -> 120s -> 240s ... 最多 10 分钟，
// 这样"每次探测干等几秒"的代价会越来越小
void SetCooldown(const std::string& name, double seconds) {
    if (seconds <= 0) return;
    std::lock_guard<std::mutex> guard(g_cooldown_lock);
    double& streak = g_fail_streak[name];
    streak = streak > 0 ? (std::min)(streak * 2, 600.0) : seconds;
    g_cooldown_until[name] = NowUnix() + streak;
}

void ClearCooldown(const std::string& name) {
    std::lock_guard<std::mutex> guard(g_cooldown_lock);
    g_cooldown_until.erase(name);
    g_fail_streak.erase(name);
}

// 把推送地址换算成设备根地址，例如 http://192.168.1.50:80
std::string BaseUrl(const Json& target) {
    std::string url = DeviceResolveUrl(target);
    size_t at = url.rfind("/api/");
    return at == std::string::npos ? std::string() : url.substr(0, at);
}

double TimeoutOf(const Json& target, double fallback = 3.0) {
    double value = target.get_double("timeout_sec", fallback);
    return value > 0 ? value : fallback;
}

void Remember(const Json& target, const std::string& status, bool ok, const std::string& detail) {
    DeviceResult result;
    result.name = target.get_str("name");
    result.url = DeviceResolveUrl(target);
    result.at = NowStamp();
    result.status = status;
    result.ok = ok;
    result.detail = detail;
    std::lock_guard<std::mutex> guard(g_result_lock);
    DeviceLastResults()[result.name] = result;
}

// 对每个目标并行执行 worker，返回 (是否全部成功, "name=ok，name2=<错误>")
std::pair<bool, std::string> FanOut(const std::vector<Json>& targets,
                                    const std::function<std::pair<bool, std::string>(
                                        const Json&)>& worker) {
    if (targets.empty()) return {false, "未配置设备"};

    std::map<std::string, std::pair<bool, std::string>> results;
    std::mutex lock;
    std::vector<std::thread> threads;
    threads.reserve(targets.size());
    for (const Json& target : targets) {
        Json captured = target;
        threads.emplace_back([&, captured] {
            std::pair<bool, std::string> outcome;
            try {
                outcome = worker(captured);
            } catch (const std::exception& error) {
                outcome = {false, std::string("异常: ") + error.what()};
            } catch (...) {
                outcome = {false, "异常"};
            }
            std::lock_guard<std::mutex> guard(lock);
            results[captured.get_str("name")] = outcome;
        });
    }

    // 等够"每个目标最坏情况下"的耗时，否则重试还没跑完就被判成超时
    double worst = 0.0;
    for (const Json& target : targets) {
        double retries = double(target.get_int("retries", 0));
        worst = (std::max)(worst, TimeoutOf(target) * (retries + 1) + 0.5 * retries);
    }
    (void)worst;
    for (std::thread& thread : threads) thread.join();

    bool all_ok = !results.empty();
    std::vector<std::string> parts;
    for (const Json& target : targets) {
        std::string name = target.get_str("name");
        auto found = results.find(name);
        if (found == results.end()) {
            parts.push_back(name + "=超时");
            all_ok = false;
            continue;
        }
        const auto& [ok, detail] = found->second;
        parts.push_back(ok ? name + "=ok" : name + "=" + detail);
        if (!ok) all_ok = false;
    }
    return {all_ok, Join(parts, "，")};
}

std::pair<bool, std::string> PushOne(const Json& target, const std::string& status,
                                     const std::string& text, const std::string& thread_name,
                                     const std::string& source, const std::string& agent) {
    std::string url = DeviceResolveUrl(target);
    Json payload = Json::Object();
    payload.set("status", Json::Str(status));
    payload.set("text", Json::Str(TruncateCodePoints(text, 200)));
    payload.set("thread", Json::Str(thread_name));
    payload.set("source", Json::Str(source));
    payload.set("agent", Json::Str(agent));

    double timeout = TimeoutOf(target);
    int retries = int(target.get_int("retries", 0));
    std::string last_error;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        HttpResult result = HttpPostJson(url, payload.Dump(0), "application/json; charset=utf-8",
                                         timeout);
        if (result.ok) {
            std::string detail = Trim(result.body);
            Remember(target, status, true, detail.empty() ? "ok" : detail);
            ClearCooldown(target.get_str("name"));
            LogDebug("device", Cat("设备推送成功 [", status, "] ", url, " (",
                                   target.get_str("name"), ")"));
            return {true, "ok"};
        }
        last_error = result.error;
        // 超时说明设备根本不在（或地址不对），再重试只是白等 —— 立刻放弃
        bool timed_out = result.error.find("12002") != std::string::npos ||
                         result.error.find("timeout") != std::string::npos;
        if (timed_out) break;
        if (attempt < retries) Sleep(400);
    }
    Remember(target, status, false, last_error);
    LogDebug("device", Cat("设备推送失败 [", status, "] ", url, " -> ", last_error));
    return {false, last_error};
}

std::pair<bool, Json> GetJson(const std::string& url, double timeout) {
    HttpResult result = HttpGet(url, timeout);
    if (!result.ok) return {false, Json::Str(result.error)};
    Json parsed = Json::Parse(result.body);
    if (parsed.is_null()) return {true, Json::Str(TruncateUtf8Bytes(result.body, 300))};
    return {true, parsed};
}

}  // namespace

std::string DeviceResolveUrl(const Json& target) {
    if (!target.is_object()) return std::string();
    return Trim(target.get_str("url"));
}

std::vector<Json> DeviceTargets(const Config& cfg) {
    static const Json kBase = [] {
        Json base = Json::Object();
        base.set("name", Json::Str(""));
        base.set("enabled", Json::Bool(true));
        base.set("url", Json::Str(""));
        base.set("timeout_sec", Json::Num(2.0));
        base.set("retries", Json::Num(0));
        return base;
    }();

    std::vector<Json> raw;
    const Json& legacy = cfg.get("device");
    if (legacy.is_object() &&
        !Trim(legacy.get_str("url")).empty()) {
        raw.push_back(legacy);
    }
    const Json& devices = cfg.get("devices");
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices.at(i).is_object()) raw.push_back(devices.at(i));
    }

    std::vector<Json> out;
    for (size_t index = 0; index < raw.size(); ++index) {
        Json merged = JsonDeepMerge(kBase, raw[index]);
        std::string name = Trim(merged.get_str("name"));
        if (name.empty()) name = "device" + std::to_string(index + 1);
        merged.set("name", Json::Str(name));
        if (!merged.get_bool("enabled", true)) continue;
        if (DeviceResolveUrl(merged).empty()) continue;
        out.push_back(merged);
    }
    return out;
}

std::string DeviceDescribe(const Config& cfg) {
    std::vector<Json> targets = DeviceTargets(cfg);
    if (targets.empty()) return "未配置设备";
    std::vector<std::string> parts;
    for (const Json& target : targets) {
        parts.push_back(target.get_str("name") + " -> " + DeviceResolveUrl(target));
    }
    return Join(parts, "；");
}

std::map<std::string, DeviceResult>& DeviceLastResults() {
    static std::map<std::string, DeviceResult> kResults;
    return kResults;
}

std::pair<bool, std::string> DevicePushStatus(const Config& cfg, const std::string& status,
                                              const std::string& text,
                                              const std::string& thread_name,
                                              const std::string& source,
                                              const std::string& agent) {
    std::vector<Json> targets = DeviceTargets(cfg);
    if (targets.empty()) return {false, "未配置设备"};
    if (cfg.data.get_bool("dry_run", false)) {
        for (const Json& target : targets) {
            LogInfo("device", Cat("[device dry-run] ", DeviceResolveUrl(target), " <- ", status));
        }
        return {false, "dry-run"};
    }
    double cooldown = cfg.num("notify", "device_retry_after_sec", 60.0);
    return FanOut(targets, [&](const Json& target) {
        std::string name = target.get_str("name");
        double until = 0.0;
        if (CoolingDown(name, &until)) {
            // 刚失败过，先别碰它：报成"不在线"，但不占时间
            return std::make_pair(false, std::string("不在线(冷却中)"));
        }
        auto outcome = PushOne(target, status, text, thread_name, source, agent);
        if (!outcome.first) SetCooldown(name, cooldown);
        return outcome;
    });
}

namespace {

// probe / beep 共用的实现：GET 每个目标，收集结果
std::pair<bool, Json> QueryAll(const Config& cfg, const std::function<std::string(const Json&)>& make_url) {
    std::vector<Json> targets = DeviceTargets(cfg);
    Json results = Json::Object();
    if (targets.empty()) return {false, results};
    std::mutex lock;
    bool all_ok = FanOut(targets, [&](const Json& target) {
        auto [ok, detail] = GetJson(make_url(target), TimeoutOf(target));
        Json entry = Json::Object();
        entry.set("url", Json::Str(DeviceResolveUrl(target)));
        entry.set("ok", Json::Bool(ok));
        entry.set("result", detail);
        std::string text = detail.is_string() ? detail.as_string() : detail.Dump(0);
        std::lock_guard<std::mutex> guard(lock);
        results.set(target.get_str("name"), entry);
        return std::make_pair(ok, TruncateCodePoints(text, 120));
    }).first;
    return {all_ok, results};
}

}  // namespace

std::pair<bool, Json> DeviceProbe(const Config& cfg) {
    return QueryAll(cfg, [](const Json& target) { return BaseUrl(target) + "/api/status"; });
}

std::pair<bool, Json> DeviceBeep(const Config& cfg, int times, int ms) {
    return QueryAll(cfg, [times, ms](const Json& target) {
        return BaseUrl(target) + "/api/beep?n=" + std::to_string(times) +
               "&ms=" + std::to_string(ms);
    });
}

}  // namespace cn
