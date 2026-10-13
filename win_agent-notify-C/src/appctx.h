// 应用上下文：持有配置 / 状态 / 两个监控 / 本地接口 / 隐私屏，统一启动与停止。
// 界面和命令行两个入口共用它。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "claude_monitor.h"
#include "config.h"
#include "monitor.h"
#include "privacy.h"
#include "store.h"

namespace cn {

class AppContext {
public:
    AppContext() = default;
    ~AppContext();

    // 读配置、建状态文件（不启动线程）
    bool Init(const std::string& config_path, std::string* error);

    // 启动接口 + 后台监控循环。返回空串表示成功，否则是错误说明。
    std::string Start();
    void Stop();
    void Shutdown();   // 退出前调用（停监控 → 关隐私屏 → 关本机声音）

    bool running() const { return running_.load(); }

    bool ready() const { return cfg_ != nullptr; }
    Config& config() { return *cfg_; }
    std::shared_ptr<Config> config_ptr() { return cfg_; }
    StateStore& store() { return *store_; }
    Monitor& monitor() { return *monitor_; }
    ClaudeMonitor& claude() { return *claude_; }
    ApiServer& api() { return *api_; }
    PrivacyScreen& privacy() { return privacy_; }

    int api_port() const { return api_port_; }
    std::string api_host() const {
        return cfg_ ? cfg_->str("api", "host", "127.0.0.1") : std::string("127.0.0.1");
    }

    bool codex_on() const { return codex_on_.load(); }
    bool claude_on() const { return claude_on_.load(); }
    void SetCodexOn(bool on) { codex_on_.store(on); }
    void SetClaudeOn(bool on) { claude_on_.store(on); }

    // 重新读 config.json（接口的 /config/reload 也会调它）
    void ReloadConfig();
    // 把界面上的开关写回 config.json，只动这几项，其它字段和顺序都保留
    void PersistToggles(bool feishu_enabled, const std::vector<bool>& device_enabled, bool tone,
                        bool speech, bool claude_enabled, bool privacy_enabled,
                        const std::vector<std::string>& wallpapers);
    void PersistPrivacyEnabled(bool enabled);

    std::string config_path() const { return cfg_ ? cfg_->path : std::string(); }

private:
    void RunnerLoop();
    void ApplyConfigToChildren();

    std::shared_ptr<Config> cfg_;
    std::unique_ptr<StateStore> store_;
    std::unique_ptr<Monitor> monitor_;
    std::unique_ptr<ClaudeMonitor> claude_;
    std::unique_ptr<ApiServer> api_;
    PrivacyScreen privacy_;

    std::thread runner_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> codex_on_{true};
    std::atomic<bool> claude_on_{true};
    std::mutex lock_;
    int api_port_ = 0;
};

}  // namespace cn
