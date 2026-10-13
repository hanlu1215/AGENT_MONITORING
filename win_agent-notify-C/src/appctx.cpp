#include "appctx.h"

#include <windows.h>

#include <chrono>
#include <stdexcept>

#include "localsound.h"
#include "util/log.h"
#include "util/times.h"
#include "util/paths.h"
#include "util/str.h"

namespace cn {

AppContext::~AppContext() { Shutdown(); }

bool AppContext::Init(const std::string& config_path, std::string* error) {
    try {
        auto config = std::make_shared<Config>(LoadConfig(config_path));
        cfg_ = config;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }

    store_.reset(new StateStore(ResolvePath(cfg_->str("state", "path", "state.json"))));
    monitor_.reset(new Monitor(cfg_, store_.get()));
    claude_.reset(new ClaudeMonitor(cfg_));
    api_.reset(new ApiServer(cfg_, store_.get()));
    privacy_.Init(cfg_.get());

    codex_on_.store(true);
    claude_on_.store(cfg_->flag("claude", "enabled", true));
    return true;
}

std::string AppContext::Start() {
    if (!cfg_) return "配置未加载";
    if (running_.load()) return std::string();

    try {
        api_->Start();
    } catch (const std::exception& exception) {
        return exception.what();
    }
    api_port_ = api_->port();
    stop_.store(false);
    running_.store(true);
    runner_ = std::thread([this] { RunnerLoop(); });
    return std::string();
}

void AppContext::Stop() {
    if (!running_.exchange(false)) {
        // 接口可能还在（比如上次 Start 后被 Stop）—— 交给下面的 join 收尾
    }
    stop_.store(true);
    if (runner_.joinable()) runner_.join();
    if (api_) api_->Stop();
    if (store_) store_->Save();
}

void AppContext::Shutdown() {
    privacy_.Hide();
    Stop();
    if (claude_) claude_->Shutdown();
    LocalSoundShutdown();
    LogInfo("app", "程序退出");
    LogShutdown();
}

void AppContext::RunnerLoop() {
    LogInfo("app", "监控启动");
    double next_codex = 0.0;
    double next_claude = 0.0;
    while (!stop_.load()) {
        double now = NowUnix();
        std::shared_ptr<Config> cfg = cfg_;
        try {
            if (codex_on_.load() && now >= next_codex) {
                double interval = cfg->num("watch", "poll_interval_sec", 5.0);
                next_codex = now + (interval > 0 ? interval : 5.0);
                monitor_->ScanOnce();
                monitor_->HeartbeatOnce();
                monitor_->DeviceSyncOnce();
            }
            if (claude_on_.load() && now >= next_claude) {
                double interval = cfg->num("claude", "poll_interval_sec", 1.0);
                next_claude = now + (interval > 0 ? interval : 1.0);
                claude_->ScanOnce();
            }
            // 这两个不能等扫描周期：它们越勤快，「等待审批」报得越准
            monitor_->ApprovalCheckOnce();
            monitor_->FlushPending();
        } catch (const std::exception& exception) {
            LogError("app", Cat("监控循环异常: ", exception.what()));
        } catch (...) {
            LogError("app", "监控循环异常");
        }
        for (int i = 0; i < 10 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    LogInfo("app", "监控已停止");
}

void AppContext::ApplyConfigToChildren() {
    monitor_->SetConfig(cfg_);
    claude_->SetConfig(cfg_);
    api_->SetConfig(cfg_);
    privacy_.SetConfig(cfg_.get());
}

void AppContext::ReloadConfig() {
    std::lock_guard<std::mutex> guard(lock_);
    auto fresh = std::make_shared<Config>(LoadConfig(cfg_ ? cfg_->path : std::string()));
    cfg_ = fresh;
    ApplyConfigToChildren();
    // 设备列表可能变了、session_index 也可能变了 —— 让下一轮重新对齐
    monitor_->RefreshNames(true);
    monitor_->DeviceSyncOnce(true);
    LogInfo("app", "配置已热加载");
}

void AppContext::PersistToggles(bool feishu_enabled, const std::vector<bool>& device_enabled,
                                bool tone, bool speech, bool claude_enabled, bool privacy_enabled,
                                const std::vector<std::string>& wallpapers) {
    if (!cfg_) return;
    std::string path = cfg_->path;
    Json raw = Json::Object();
    if (FileExists(path)) {
        std::string error;
        Json parsed;
        if (Json::ParseFile(path, &parsed, &error) && parsed.is_object()) raw = parsed;
    }

    Json& feishu = raw["feishu"];
    if (!feishu.is_object()) feishu = Json::Object();
    feishu.set("enabled", Json::Bool(feishu_enabled));

    Json& legacy = raw["device"];
    size_t index = 0;
    const bool has_legacy_target = legacy.is_object() &&
        !Trim(legacy.get_str("url")).empty();
    if (has_legacy_target && index < device_enabled.size()) {
        legacy.set("enabled", Json::Bool(device_enabled[index++]));
    }
    Json& devices = raw["devices"];
    if (!devices.is_array()) devices = Json::Array();
    for (size_t i = 0; i < devices.size() && index < device_enabled.size(); ++i, ++index) {
        if (devices.at(i).is_object()) {
            devices.at(i).set("enabled", Json::Bool(device_enabled[index]));
        }
    }

    Json& claude = raw["claude"];
    if (!claude.is_object()) claude = Json::Object();
    claude.set("enabled", Json::Bool(claude_enabled));

    Json& local = raw["local_sound"];
    if (!local.is_object()) local = Json::Object();
    local.set("enabled", Json::Bool(tone));
    local.set("tone", Json::Bool(tone));
    local.set("speech", Json::Bool(speech));

    Json& privacy = raw["privacy"];
    if (!privacy.is_object()) privacy = Json::Object();
    privacy.set("enabled", Json::Bool(privacy_enabled));
    if (!wallpapers.empty()) {
        Json list = Json::Array();
        for (const std::string& item : wallpapers) list.push_back(Json::Str(item));
        privacy.set("wallpaper", list);
    }

    if (!SaveConfig(raw, path)) LogWarn("app", Cat("保存配置失败：", path));
}

void AppContext::PersistPrivacyEnabled(bool enabled) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!cfg_) return;
    (*cfg_)["privacy"].set("enabled", Json::Bool(enabled));
    Json raw = Json::Object();
    std::string path = cfg_->path;
    if (FileExists(path)) {
        std::string error;
        Json parsed;
        if (Json::ParseFile(path, &parsed, &error) && parsed.is_object()) raw = parsed;
    }
    Json& privacy = raw["privacy"];
    if (!privacy.is_object()) privacy = Json::Object();
    privacy.set("enabled", Json::Bool(enabled));
    // 密码 / 壁纸这些在界面上做的改动也要落盘
    const Json& mine = cfg_->get("privacy");
    for (const char* key : {"password_sha256", "wallpaper"}) {
        const Json& value = mine.get(key);
        if (!value.is_null() && !(value.is_string() && value.as_string().empty())) {
            privacy.set(key, value);
        }
    }
    if (!SaveConfig(raw, path)) LogWarn("app", Cat("保存配置失败：", path));
}

}  // namespace cn
