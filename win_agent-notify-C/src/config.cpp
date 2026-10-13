#include "config.h"

#include <windows.h>

#include <mutex>

#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"

namespace cn {
namespace {

Json BuildDefaults() {
    Json d = Json::Object();

    {
        Json feishu = Json::Object();
        feishu.set("webhook_url", Json::Str("https://open.feishu.cn/open-apis/bot/v2/hook/xxxxx"));
        feishu.set("enabled", Json::Bool(false));
        feishu.set("secret", Json::Str(""));
        feishu.set("mention_all_on_waiting", Json::Bool(false));
        feishu.set("timeout_sec", Json::Num(10));
        // 只把这几个状态发到飞书（设备 / 本机提示音不受影响，它们看 notify.on）
        Json feishu_on = Json::Array();
        feishu_on.push_back(Json::Str("waiting_approval"));
        feishu_on.push_back(Json::Str("completed"));
        feishu.set("on", feishu_on);
        feishu.set("retries", Json::Num(2));
        d.set("feishu", feishu);
    }
    {
        Json watch = Json::Object();
        Json dirs = Json::Array();
        dirs.push_back(Json::Str("~/.codex/sessions"));
        watch.set("dirs", dirs);
        watch.set("session_index", Json::Str("~/.codex/session_index.jsonl"));
        watch.set("poll_interval_sec", Json::Num(5.0));
        watch.set("max_files", Json::Num(30));
        watch.set("max_file_age_days", Json::Num(3));
        watch.set("first_run", Json::Str("baseline"));
        watch.set("only_user_threads", Json::Bool(true));
        Json ignore = Json::Array();
        ignore.push_back(Json::Str("guardian review"));
        watch.set("ignore_thread_names", ignore);
        watch.set("check_queue", Json::Bool(true));
        watch.set("queue_db", Json::Str("~/.codex/queue_1.sqlite"));
        watch.set("queue_max_age_sec", Json::Num(900));
        // 工具调用"发出去了、超过这个时间还没有结果"就当作在等你授权（0 = 关闭）
        watch.set("approval_after_sec", Json::Num(20.0));
        d.set("watch", watch);
    }
    {
        Json notify = Json::Object();
        Json on = Json::Array();
        on.push_back(Json::Str("working"));
        on.push_back(Json::Str("waiting_approval"));
        on.push_back(Json::Str("completed"));
        on.push_back(Json::Str("interrupted"));
        notify.set("on", on);
        notify.set("progress", Json::Bool(false));
        notify.set("max_event_age_sec", Json::Num(600));
        notify.set("min_interval_sec", Json::Num(1.0));
        notify.set("complete_delay_sec", Json::Num(4.0));
        notify.set("complete_queue_wait_sec", Json::Num(60.0));
        notify.set("max_summary_chars", Json::Num(500));
        notify.set("include_cwd", Json::Bool(true));
        notify.set("heartbeat_sec", Json::Num(0));
        notify.set("stall_after_sec", Json::Num(0));
        notify.set("dedup_window_sec", Json::Num(10.0));
        notify.set("device_resync_sec", Json::Num(60.0));
        // 设备推送失败后，冷却期内不再碰它（0 = 关闭熔断，每轮都重试）
        notify.set("device_retry_after_sec", Json::Num(60.0));
        d.set("notify", notify);
    }
    {
        Json sound = Json::Object();
        sound.set("enabled", Json::Bool(true));
        sound.set("tone", Json::Bool(true));
        sound.set("tone_volume", Json::Num(0.32));
        sound.set("speech", Json::Bool(true));
        sound.set("speech_backend", Json::Str("edge"));
        sound.set("voice", Json::Str("zh-CN-XiaoxiaoNeural"));
        sound.set("voice_rate", Json::Str("+0%"));
        sound.set("voice_volume", Json::Str("+0%"));
        sound.set("cache_dir", Json::Str("audio_cache"));
        sound.set("speech_rate", Json::Num(1));
        sound.set("min_interval_sec", Json::Num(3.0));
        sound.set("tones", Json::Object());
        sound.set("phrases", Json::Object());
        d.set("local_sound", sound);
    }
    {
        Json privacy = Json::Object();
        privacy.set("enabled", Json::Bool(false));
        privacy.set("wallpaper", Json::Array());
        privacy.set("password_sha256", Json::Str(""));
        privacy.set("headline_working", Json::Str("Agent 工作中，请勿关机"));
        privacy.set("headline_waiting_approval", Json::Str("Agent 等待审批，请勿关机"));
        privacy.set("headline_completed", Json::Str("任务已完成"));
        privacy.set("dim_alpha", Json::Num(0.35));
        privacy.set("show_clock", Json::Bool(true));
        privacy.set("show_date", Json::Bool(true));
        privacy.set("show_idle_agents", Json::Bool(false));
        privacy.set("anti_burn_in", Json::Bool(true));
        privacy.set("drift_sec", Json::Num(60.0));
        privacy.set("drift_px", Json::Num(40));
        privacy.set("show_hint", Json::Bool(false));
        d.set("privacy", privacy);
    }
    {
        Json gui = Json::Object();
        gui.set("auto_start", Json::Bool(true));
        d.set("gui", gui);
    }
    {
        Json claude = Json::Object();
        claude.set("enabled", Json::Bool(true));
        claude.set("sessions_dir", Json::Str("~/.claude/sessions"));
        claude.set("projects_dir", Json::Str("~/.claude/projects"));
        claude.set("poll_interval_sec", Json::Num(1.0));
        claude.set("approval_after_sec", Json::Num(0.0));
        claude.set("stale_busy_sec", Json::Num(1800.0));
        claude.set("first_run", Json::Str("baseline"));
        claude.set("push_current", Json::Bool(true));
        d.set("claude", claude);
    }
    {
        Json api = Json::Object();
        api.set("host", Json::Str("127.0.0.1"));
        api.set("port", Json::Num(17865));
        api.set("token", Json::Str(""));
        d.set("api", api);
    }
    {
        Json device = Json::Object();
        device.set("enabled", Json::Bool(true));
        device.set("url", Json::Str(""));
        device.set("timeout_sec", Json::Num(2.0));
        device.set("retries", Json::Num(0));
        d.set("device", device);
    }
    {
        Json devices = Json::Array();
        Json esp32 = Json::Object();
        esp32.set("name", Json::Str("esp32-led"));
        esp32.set("enabled", Json::Bool(false));
        esp32.set("url", Json::Str("http://127.0.0.1:12345/api/codex"));
        esp32.set("timeout_sec", Json::Num(5.0));
        esp32.set("retries", Json::Num(2));
        devices.push_back(esp32);
        Json ubuntu = Json::Object();
        ubuntu.set("name", Json::Str("ubuntu-voice"));
        ubuntu.set("enabled", Json::Bool(false));
        ubuntu.set("url", Json::Str("http://127.0.0.1:12345/api/codex"));
        ubuntu.set("timeout_sec", Json::Num(5.0));
        ubuntu.set("retries", Json::Num(2));
        devices.push_back(ubuntu);
        d.set("devices", devices);
    }
    {
        Json state = Json::Object();
        state.set("path", Json::Str("state.json"));
        state.set("log_path", Json::Str("codex-notify.log"));
        d.set("state", state);
    }
    d.set("dry_run", Json::Bool(false));
    return d;
}

std::string EnvUtf8(const wchar_t* name) {
    wchar_t buf[4096];
    DWORD len = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return std::string();
    return WideToUtf8(std::wstring(buf, len));
}

bool NormalizeDeviceAddress(Json& target) {
    if (!target.is_object()) return false;
    bool changed = false;
    std::string url = Trim(target.get_str("url"));
    if (url.empty()) {
        std::string ip = Trim(target.get_str("ip"));
        if (!ip.empty()) {
            long long port = target.get_int("port", 80);
            if (port <= 0) port = 80;
            std::string path = Trim(target.get_str("path", "/api/codex"));
            if (path.empty()) path = "/api/codex";
            if (path.front() != '/') path.insert(path.begin(), '/');

            std::string base;
            if (StartsWith(ip, "http://") || StartsWith(ip, "https://")) {
                base = ip;
                while (!base.empty() && base.back() == '/') base.pop_back();
            } else if (ip.find(':') != std::string::npos) {
                base = "http://" + ip;
            } else {
                base = "http://" + ip + ":" + std::to_string(port);
            }
            url = base + path;
            target.set("url", Json::Str(url));
            changed = true;
        }
    }
    for (const char* key : {"ip", "port", "path"}) {
        if (target.has(key)) {
            target.remove(key);
            changed = true;
        }
    }
    return changed;
}

bool NormalizeDeviceAddresses(Json& config) {
    bool changed = false;
    if (config.has("device")) changed |= NormalizeDeviceAddress(config["device"]);
    if (config.has("devices")) {
        Json& devices = config["devices"];
        for (size_t i = 0; i < devices.size(); ++i) {
            changed |= NormalizeDeviceAddress(devices.at(i));
        }
    }
    return changed;
}

}  // namespace

const Json& DefaultConfig() {
    static const Json kDefaults = BuildDefaults();
    return kDefaults;
}

std::string Config::str(const std::string& section, const std::string& key,
                        const std::string& fallback) const {
    const Json& sec = data.get(section);
    if (!sec.is_object()) return fallback;
    const Json& value = sec.get(key);
    return value.is_string() ? value.as_string() : fallback;
}

double Config::num(const std::string& section, const std::string& key, double fallback) const {
    const Json& sec = data.get(section);
    if (!sec.is_object()) return fallback;
    const Json& value = sec.get(key);
    return value.is_null() ? fallback : value.as_double(fallback);
}

long long Config::integer(const std::string& section, const std::string& key,
                          long long fallback) const {
    const Json& sec = data.get(section);
    if (!sec.is_object()) return fallback;
    const Json& value = sec.get(key);
    return value.is_null() ? fallback : value.as_int(fallback);
}

std::vector<std::string> Config::str_list(const std::string& section,
                                          const std::string& key) const {
    std::vector<std::string> out;
    const Json& sec = data.get(section);
    if (!sec.is_object()) return out;
    const Json& value = sec.get(key);
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            std::string item = Trim(value.at(i).as_string());
            if (!item.empty()) out.push_back(item);
        }
    } else if (value.is_string() && !value.as_string().empty()) {
        out.push_back(Trim(value.as_string()));
    }
    return out;
}

bool Config::flag(const std::string& section, const std::string& key, bool fallback) const {
    const Json& sec = data.get(section);
    if (!sec.is_object()) return fallback;
    const Json& value = sec.get(key);
    return value.is_null() ? fallback : value.as_bool(fallback);
}

std::string ConfigPath(const std::string& explicit_path) {
    if (!explicit_path.empty()) return ResolvePath(explicit_path);
    std::string from_env = EnvUtf8(L"CODEX_NOTIFY_CONFIG");
    if (!from_env.empty()) return ResolvePath(from_env);
    return PathJoin(ExeDir(), "config.json");
}

Config LoadConfig(const std::string& explicit_path) {
    Config config;
    config.path = ConfigPath(explicit_path);
    config.project_dir = ExeDir();

    Json user = Json::Object();
    std::string error;
    if (FileExists(config.path)) {
        if (!Json::ParseFile(config.path, &user, &error) || !user.is_object()) {
            LogWarn("config", Cat("配置文件解析失败，改用默认值：", config.path, "（", error, "）"));
            user = Json::Object();
        }
    }
    if (NormalizeDeviceAddresses(user) && !SaveConfig(user, config.path)) {
        LogWarn("config", Cat("设备地址已转换为 url，但配置文件未能保存：", config.path));
    }
    config.data = JsonDeepMerge(DefaultConfig(), user);
    if (!FileExists(config.path) && !SaveConfig(config.data, config.path)) {
        LogWarn("config", Cat("首次启动配置文件未能创建：", config.path));
    }

    // 环境变量兜底：方便临时覆盖而不改文件
    std::string webhook = EnvUtf8(L"CODEX_NOTIFY_WEBHOOK");
    if (!webhook.empty()) config.data["feishu"].set("webhook_url", Json::Str(webhook));
    std::string secret = EnvUtf8(L"CODEX_NOTIFY_SECRET");
    if (!secret.empty()) config.data["feishu"].set("secret", Json::Str(secret));

    config.data.set("_config_path", Json::Str(config.path));
    config.data.set("_project_dir", Json::Str(config.project_dir));
    return config;
}

bool SaveConfig(const Json& data, const std::string& path) {
    Json clean = Json::Object();
    for (size_t i = 0; i < data.size(); ++i) {
        const std::string& key = data.key_ref(i);
        if (!key.empty() && key[0] == '_') continue;
        clean.set(key, data.at(i));
    }
    std::string dir = PathDirName(path);
    if (!dir.empty()) EnsureDir(dir);
    return WriteFileAtomic(path, clean.Dump(2));
}

Json PublicConfig(const Json& data) {
    Json safe = Json::Object();
    for (size_t i = 0; i < data.size(); ++i) {
        const std::string& key = data.key_ref(i);
        if (!key.empty() && key[0] == '_') continue;
        safe.set(key, data.at(i));
    }
    Json& feishu = safe["feishu"];
    if (feishu.is_object()) {
        for (const char* key : {"webhook_url", "secret"}) {
            const Json& value = feishu.get(key);
            if (value.is_string() && !value.as_string().empty()) {
                const std::string& text = value.as_string();
                // 和 Python 一样按**码点**算长度
                std::string masked = CodePointCount(text) > 42
                                         ? TruncateCodePoints(text, 38) + "..."
                                         : "***";
                feishu.set(key, Json::Str(masked));
            }
        }
    }
    Json& api = safe["api"];
    if (api.is_object() && api.get("token").is_string() && !api["token"].as_string().empty()) {
        api.set("token", Json::Str("***"));
    }
    return safe;
}

void ConfigStore::Store(std::shared_ptr<Config> config) {
    std::lock_guard<std::mutex> guard(lock_);
    current_ = std::move(config);
    ++generation_;
}

std::shared_ptr<Config> ConfigStore::Current() const {
    std::lock_guard<std::mutex> guard(lock_);
    return current_;
}

unsigned long long ConfigStore::Generation() const {
    std::lock_guard<std::mutex> guard(lock_);
    return generation_;
}

}  // namespace cn
