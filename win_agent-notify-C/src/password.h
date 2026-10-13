// 隐私屏幕的解锁密码：sha256(SALT + 明文) 的十六进制。
#pragma once

#include <windows.h>

#include <string>

#include "config.h"

namespace cn {

std::string HashPassword(const std::string& text);
bool CheckPassword(const Config& cfg, const std::string& text);
bool HasPassword(const Config& cfg);

// 弹一个输入框（密码用圆点遮挡）。返回 false 表示用户取消。
bool PromptText(HWND owner, const std::wstring& title, const std::wstring& label,
                bool mask, std::wstring* out);

// 界面上「设置解锁密码…」：两次输入一致才写入 cfg（并返回 true）
bool PromptSetPassword(HWND owner, Config* cfg);

}  // namespace cn
