// 配置：默认值、与用户 config.json 深合并、脱敏、保存。
// 与 Python 版 codex_notify/config.py 的字段一一对应。
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json.h"

namespace cn {

struct Config {
    Json data;              // 合并后的完整配置（含默认值）
    std::string path;       // config.json 的绝对路径
    std::string project_dir;

    const Json& get(const std::string& key) const { return data.get(key); }
    Json& operator[](const std::string& key) { return data[key]; }

    // 常用取值（带默认值，字段缺失也不会崩）
    std::string str(const std::string& section, const std::string& key,
                    const std::string& fallback = "") const;
    double num(const std::string& section, const std::string& key, double fallback) const;
    long long integer(const std::string& section, const std::string& key,
                      long long fallback) const;
    bool flag(const std::string& section, const std::string& key, bool fallback) const;
    // 字符串数组字段（例如 watch.ignore_thread_names）；不是数组时返回空
    std::vector<std::string> str_list(const std::string& section, const std::string& key) const;
};

// 默认配置（和 Python 版 DEFAULTS 一样的一份树）
const Json& DefaultConfig();

// 配置路径：参数 > 环境变量 CODEX_NOTIFY_CONFIG > exe 目录下的 config.json
std::string ConfigPath(const std::string& explicit_path);

// 读取配置：文件不存在就用默认值；支持 CODEX_NOTIFY_WEBHOOK / _SECRET 兜底
Config LoadConfig(const std::string& explicit_path = "");

// 把配置写回文件（只写非 _ 开头的字段；UTF-8 无 BOM、2 空格缩进）
bool SaveConfig(const Json& data, const std::string& path);

// 接口返回用的脱敏配置
Json PublicConfig(const Json& data);

// 线程安全地持有当前配置：热加载时整体换一份新的
class ConfigStore {
public:
    void Store(std::shared_ptr<Config> config);
    std::shared_ptr<Config> Current() const;
    unsigned long long Generation() const;

private:
    mutable std::mutex lock_;
    std::shared_ptr<Config> current_;
    unsigned long long generation_ = 0;
};

}  // namespace cn
