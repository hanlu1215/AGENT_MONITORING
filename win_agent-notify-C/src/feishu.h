// 飞书自定义机器人：状态样式、签名、卡片构造、发送。
#pragma once

#include <string>
#include <utility>

#include "config.h"
#include "json.h"

namespace cn {

// 归一化状态值：working / waiting_approval / completed / interrupted / idle / info
std::string NormalizeStatus(const std::string& value);
std::string StatusLabel(const std::string& status);   // 中文名
std::string StatusIcon(const std::string& status);    // 圆点 emoji
std::string StatusTemplate(const std::string& status);  // 卡片颜色
std::string AgentLabel(const std::string& agent);     // codex -> Codex / claude -> Claude Code

// 飞书签名：HMAC-SHA256("{timestamp}\n{secret}") 再 base64
std::string GenSign(const std::string& timestamp, const std::string& secret);

// 构造交互式卡片消息体
Json BuildCard(const std::string& title, const std::string& template_name,
               const std::string& body_md, const std::string& footer, bool mention_all);

// 发送任意消息体。返回 (是否成功, 说明)
std::pair<bool, std::string> SendPayload(const Config& cfg, const Json& payload);

// 按状态推一张卡片
std::pair<bool, std::string> FeishuPushStatus(const Config& cfg, const std::string& status,
                                              const std::string& text,
                                              const std::string& thread_name,
                                              const std::string& cwd,
                                              const std::string& title,
                                              const std::string& source,
                                              const std::string& agent);

}  // namespace cn
