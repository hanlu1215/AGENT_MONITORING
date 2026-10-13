// 字符串工具：内部一律 UTF-8，只在 Win32 的 W 系 API 边界转换成 UTF-16。
#pragma once

#include <string>
#include <type_traits>
#include <vector>

namespace cn {

// ---- 编码转换 ----
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);
// 把非法 UTF-8 序列替换成 U+FFFD（对应 Python 的 errors="replace"）
std::string SanitizeUtf8(const std::string& text);

// 把可能是"别的编码"的文本归一化成 UTF-8：
//   · 本来就是合法 UTF-8 -> 原样返回
//   · 不是 -> 按系统默认代码页（中文 Windows 是 GBK）解一次再转 UTF-8
// 为什么需要：Windows 控制台里的 curl 带中文参数时会按 GBK 发出，
// 直接当 UTF-8 解就会满屏 U+FFFD（界面上就是乱码）。
std::string NormalizeToUtf8(const std::string& text);

// ---- 基本处理（都是 UTF-8 字节层面安全操作） ----
std::string Trim(const std::string& text);
std::string ToLowerAscii(const std::string& text);
bool StartsWith(const std::string& text, const std::string& prefix);
bool EndsWith(const std::string& text, const std::string& suffix);
bool Contains(const std::string& text, const std::string& needle);
std::vector<std::string> Split(const std::string& text, char sep);
std::string Join(const std::vector<std::string>& items, const std::string& sep);
std::string ReplaceAll(std::string text, const std::string& from, const std::string& to);

// ---- 按码点计数与截断（Python 的 len()/s[:n] 都是码点语义，
//      按字节截会把一个汉字撕成半个，必须用这两个） ----
size_t CodePointCount(const std::string& text);
std::string TruncateCodePoints(const std::string& text, size_t limit);
std::string TruncateUtf8Bytes(const std::string& text, size_t limit);   // 按字节且不切碎字符

// Python 的 " ".join(s.split()) —— 压掉所有空白（含 U+3000 全角空格）
std::string CollapseWhitespace(const std::string& text);

// 数字格式化
std::string FormatInt(long long value);
std::string FormatDouble(double value, int decimals);

// 从文本里找第一个 UUID（8-4-4-4-12 十六进制）——会话文件名里的线程 id
std::string FindUuid(const std::string& text);

// 去掉 Windows 上可能出现的孤立代理项（对应 Python 版 push.clean()）
std::string CleanText(const std::string& text);

// ---- 拼接：Cat("a=", 1, " b=", 2.5) ----
inline std::string Cat() { return std::string(); }

inline void CatAppend(std::string& out, const std::string& value) { out += value; }
inline void CatAppend(std::string& out, const char* value) { if (value) out += value; }
inline void CatAppend(std::string& out, const std::wstring& value) { out += WideToUtf8(value); }
inline void CatAppend(std::string& out, const std::vector<std::string>& value) {
    out += "[";
    for (size_t i = 0; i < value.size(); ++i) {
        if (i) out += ", ";
        out += value[i];
    }
    out += "]";
}

template <typename T>
void CatAppend(std::string& out, T value) {
    if constexpr (std::is_same_v<T, bool>) {
        out += value ? "true" : "false";
    } else if constexpr (std::is_integral_v<T>) {
        out += std::to_string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        out += FormatDouble(double(value), 3);
    } else {
        out += std::to_string(value);
    }
}

template <typename T, typename... Rest>
std::string Cat(T&& first, Rest&&... rest) {
    std::string out;
    CatAppend(out, std::forward<T>(first));
    (CatAppend(out, std::forward<Rest>(rest)), ...);
    return out;
}

}  // namespace cn
