#include "feishu.h"

#include <windows.h>

#include <map>

#include "http.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

struct StatusStyle {
    const char* action;     // 动作词（智能体名字拼在前面）
    const char* card;       // 飞书卡片颜色
    const char* icon;       // 标题前的小圆点
    const char* label;      // 中文状态名
};

// 卡片颜色要和 ESP32 板载灯一致：
//   空闲=绿 工作中=黄 等待审批=红 已完成=蓝 已中断=品红 通知=青
const std::map<std::string, StatusStyle>& Styles() {
    static const std::map<std::string, StatusStyle> kStyles = {
        {"working", {"正在工作", "yellow", "🟡", "工作中"}},
        {"waiting_approval", {"等待审批", "red", "🔴", "等待审批"}},
        {"completed", {"任务完成", "blue", "🔵", "已完成"}},
        {"interrupted", {"任务中断", "carmine", "🟣", "已中断"}},
        {"idle", {"空闲", "green", "🟢", "空闲"}},
        {"info", {"通知", "turquoise", "🔔", "通知"}},
    };
    return kStyles;
}

const StatusStyle& StyleOf(const std::string& status) {
    auto found = Styles().find(status);
    if (found != Styles().end()) return found->second;
    return Styles().at("info");
}

const std::map<std::string, std::string>& Aliases() {
    static const std::map<std::string, std::string> kAliases = {
        {"working", "working"},      {"work", "working"},
        {"busy", "working"},         {"running", "working"},
        {"工作中", "working"},        {"工作", "working"},
        {"运行中", "working"},        {"进行中", "working"},
        {"waiting", "waiting_approval"},        {"waiting_approval", "waiting_approval"},
        {"approval", "waiting_approval"},       {"permission", "waiting_approval"},
        {"等待审批", "waiting_approval"},        {"待审批", "waiting_approval"},
        {"等待确认", "waiting_approval"},
        {"completed", "completed"},  {"complete", "completed"},
        {"done", "completed"},       {"finished", "completed"},
        {"已完成", "completed"},      {"完成", "completed"},
        {"interrupted", "interrupted"},         {"aborted", "interrupted"},
        {"failed", "interrupted"},   {"已中断", "interrupted"},
        {"中断", "interrupted"},      {"失败", "interrupted"},
        {"idle", "idle"},            {"空闲", "idle"},
        {"info", "info"},            {"通知", "info"},
        {"消息", "info"},
    };
    return kAliases;
}

const std::map<std::string, std::string>& AgentLabels() {
    static const std::map<std::string, std::string> kLabels = {
        {"codex", "Codex"},      {"claude", "Claude Code"},
        {"claude-code", "Claude Code"}, {"claudecode", "Claude Code"},
        {"cc", "Claude Code"},   {"manual", "手动"},
        {"info", "系统"},
    };
    return kLabels;
}

std::string Footer(const std::string& project = "") {
    std::string stamp = FormatLocal(NowUnix(), "%Y-%m-%d %H:%M:%S");
    std::string out = "Codex Watcher · " + stamp;
    if (!project.empty()) out += " · " + project;
    return out;
}

}  // namespace

std::string NormalizeStatus(const std::string& value) {
    std::string key = ToLowerAscii(Trim(value.empty() ? "info" : value));
    auto found = Aliases().find(key);
    if (found != Aliases().end()) return found->second;
    if (Styles().count(key)) return key;
    return "info";
}

std::string StatusLabel(const std::string& status) {
    return StyleOf(NormalizeStatus(status)).label;
}

std::string StatusIcon(const std::string& status) {
    return StyleOf(NormalizeStatus(status)).icon;
}

std::string StatusTemplate(const std::string& status) {
    return StyleOf(NormalizeStatus(status)).card;
}

std::string AgentLabel(const std::string& agent) {
    std::string key = ToLowerAscii(Trim(agent.empty() ? "codex" : agent));
    auto found = AgentLabels().find(key);
    if (found != AgentLabels().end()) return found->second;
    std::string text = Trim(agent);
    return text.empty() ? "Codex" : text;
}

std::string GenSign(const std::string& timestamp, const std::string& secret) {
    // 飞书的算法：key = "{timestamp}\n{secret}"，待签内容为空
    // （和 Python 版 hmac.new(string_to_sign, digestmod=sha256) 完全一致）
    std::string key = timestamp + "\n" + secret;
    return Base64Encode(HmacSha256(key, ""));
}

Json BuildCard(const std::string& title, const std::string& template_name,
               const std::string& body_md, const std::string& footer, bool mention_all) {
    Json elements = Json::Array();
    if (mention_all) {
        Json div = Json::Object();
        div.set("tag", Json::Str("div"));
        Json text = Json::Object();
        text.set("tag", Json::Str("lark_md"));
        text.set("content", Json::Str("<at id=all></at>"));
        div.set("text", text);
        elements.push_back(div);
    }
    if (!body_md.empty()) {
        Json div = Json::Object();
        div.set("tag", Json::Str("div"));
        Json text = Json::Object();
        text.set("tag", Json::Str("lark_md"));
        text.set("content", Json::Str(body_md));
        div.set("text", text);
        elements.push_back(div);
    }
    if (!footer.empty()) {
        Json hr = Json::Object();
        hr.set("tag", Json::Str("hr"));
        elements.push_back(hr);
        Json note = Json::Object();
        note.set("tag", Json::Str("note"));
        Json note_elements = Json::Array();
        Json plain = Json::Object();
        plain.set("tag", Json::Str("plain_text"));
        plain.set("content", Json::Str(footer));
        note_elements.push_back(plain);
        note.set("elements", note_elements);
        elements.push_back(note);
    }
    if (elements.empty()) {
        Json div = Json::Object();
        div.set("tag", Json::Str("div"));
        Json text = Json::Object();
        text.set("tag", Json::Str("lark_md"));
        text.set("content", Json::Str("（无内容）"));
        div.set("text", text);
        elements.push_back(div);
    }

    Json config = Json::Object();
    config.set("wide_screen_mode", Json::Bool(true));
    config.set("enable_forward", Json::Bool(true));
    Json header_title = Json::Object();
    header_title.set("tag", Json::Str("plain_text"));
    header_title.set("content", Json::Str(title));
    Json header = Json::Object();
    header.set("title", header_title);
    header.set("template", Json::Str(template_name));
    Json card = Json::Object();
    card.set("config", config);
    card.set("header", header);
    card.set("elements", elements);

    Json payload = Json::Object();
    payload.set("msg_type", Json::Str("interactive"));
    payload.set("card", card);
    return payload;
}

std::pair<bool, std::string> SendPayload(const Config& cfg, const Json& payload) {
    std::string url = Trim(cfg.str("feishu", "webhook_url"));
    std::string secret = Trim(cfg.str("feishu", "secret"));
    double timeout = cfg.num("feishu", "timeout_sec", 10.0);
    int retries = int(cfg.integer("feishu", "retries", 0));
    bool dry = cfg.data.get_bool("dry_run", false);

    if (!cfg.flag("feishu", "enabled", true)) return {false, "飞书通道已关闭"};
    if (dry || url.empty()) {
        std::string reason = dry ? "dry-run" : "未配置 webhook_url";
        LogInfo("feishu", Cat("[", reason, "] ", TruncateUtf8Bytes(payload.Dump(0), 800)));
        return {false, reason};
    }

    Json body = payload;
    if (!secret.empty()) {
        std::string stamp = std::to_string((long long)NowUnix());
        body.set("timestamp", Json::Str(stamp));
        body.set("sign", Json::Str(GenSign(stamp, secret)));
    }

    std::string last_error;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        HttpResult result = HttpPostJson(url, body.Dump(0), "application/json; charset=utf-8",
                                         timeout);
        if (result.ok) {
            Json parsed = Json::Parse(result.body);
            long long code = parsed.is_object() ? parsed.get_int("code", 0)
                                                : parsed.get_int("StatusCode", 0);
            if (code == 0) return {true, "ok"};
            last_error = Cat("飞书返回 code=", code, " msg=",
                             parsed.is_object() ? parsed.get_str("msg")
                                                : TruncateUtf8Bytes(result.body, 200));
        } else {
            last_error = result.error;
            if (result.status >= 400) {
                last_error = Cat("HTTP ", result.status, ": ",
                                 TruncateUtf8Bytes(result.body, 200));
            }
        }
        if (attempt < retries) Sleep(DWORD(1000 * (1 + attempt)));
    }
    LogError("feishu", Cat("推送失败: ", last_error));
    return {false, last_error};
}

std::pair<bool, std::string> FeishuPushStatus(const Config& cfg, const std::string& status,
                                              const std::string& text,
                                              const std::string& thread_name,
                                              const std::string& cwd,
                                              const std::string& title,
                                              const std::string& source,
                                              const std::string& agent) {
    std::string normalized = NormalizeStatus(status);
    const StatusStyle& style = StyleOf(normalized);
    std::string name = AgentLabel(agent);
    std::string card_title = title.empty() ? (style.icon + std::string(" ") + name + " " +
                                              style.action)
                                           : title;

    std::vector<std::string> lines;
    lines.push_back("**智能体**：" + name);
    if (!thread_name.empty()) lines.push_back("**任务**：" + thread_name);
    if (!cwd.empty() && cfg.flag("notify", "include_cwd", true)) {
        lines.push_back("**目录**：" + cwd);
    }
    lines.push_back("**状态**：" + std::string(style.label));
    if (!text.empty()) {
        long long limit = cfg.integer("notify", "max_summary_chars", 500);
        std::string summary = CollapseWhitespace(text);
        if (limit > 0 && CodePointCount(summary) > (size_t)limit) {
            summary = TruncateCodePoints(summary, size_t(limit > 0 ? limit - 1 : 0)) + "…";
        }
        lines.push_back("");
        lines.push_back(summary);
    }
    if (!source.empty()) {
        lines.push_back("");
        lines.push_back("_来源：" + source + "_");
    }

    bool mention_all = cfg.flag("feishu", "mention_all_on_waiting", false) &&
                       normalized == "waiting_approval";
    Json card = BuildCard(card_title, style.card, Join(lines, "\n"), Footer(), mention_all);
    return SendPayload(cfg, card);
}

}  // namespace cn
