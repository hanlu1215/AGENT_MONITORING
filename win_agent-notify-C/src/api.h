// 本地 HTTP 推送接口（Winsock2，只监听本机）。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "json.h"
#include "store.h"

namespace cn {

const char* ApiUsage();

class ApiServer {
public:
    ApiServer(std::shared_ptr<Config> cfg, StateStore* store);
    ~ApiServer();

    // 绑定并开始接受连接；失败抛 std::runtime_error（带可读的原因）
    void Start();
    void Stop();

    // 热加载后换一份新配置（处理请求的线程可能正在读，所以要加锁换）
    void SetConfig(std::shared_ptr<Config> cfg) {
        std::lock_guard<std::mutex> guard(cfg_lock_);
        cfg_ = std::move(cfg);
    }
    std::shared_ptr<Config> ConfigPtr() const {
        std::lock_guard<std::mutex> guard(cfg_lock_);
        return cfg_;
    }

    int port() const { return port_; }
    double started_at() const { return started_at_; }
    bool running() const { return running_.load(); }

    void Remember(const Json& entry);

private:
    void AcceptLoop();
    void HandleClient(unsigned long long socket_handle);
    friend struct ApiHandler;

    std::shared_ptr<Config> cfg_;
    mutable std::mutex cfg_lock_;
    StateStore* store_;
    std::atomic<bool> running_{false};
    unsigned long long listener_ = ~0ull;
    int port_ = 0;
    double started_at_ = 0.0;
    std::thread accept_thread_;
    std::mutex clients_lock_;
    std::atomic<int> client_count_{0};
    std::mutex log_lock_;
    std::vector<Json> log_;
};

}  // namespace cn
