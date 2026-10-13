#include "util/str.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <winreg.h>

namespace cn {
namespace {

bool IsAsciiSpace(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

// 解码一个 UTF-8 码点；返回消耗的字节数，0 表示非法
size_t DecodeUtf8(const std::string& text, size_t pos, unsigned* cp) {
    unsigned char ch = (unsigned char)text[pos];
    if (ch < 0x80) {
        *cp = ch;
        return 1;
    }
    size_t need = 0;
    unsigned value = 0;
    if ((ch & 0xE0) == 0xC0) {
        need = 1;
        value = ch & 0x1F;
    } else if ((ch & 0xF0) == 0xE0) {
        need = 2;
        value = ch & 0x0F;
    } else if ((ch & 0xF8) == 0xF0) {
        need = 3;
        value = ch & 0x07;
    } else {
        return 0;
    }
    if (pos + need >= text.size()) return 0;   // 续字节不够
    for (size_t i = 1; i <= need; ++i) {
        unsigned char next = (unsigned char)text[pos + i];
        if ((next & 0xC0) != 0x80) return 0;
        value = (value << 6) | (next & 0x3F);
    }
    *cp = value;
    return need + 1;
}

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(char(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// Python str.split() 认的空白：ASCII 空白 + U+0085/00A0/1680/2000-200A/2028/2029/202F/205F/3000
bool IsPythonSpace(unsigned cp) {
    if (cp < 0x80) return IsAsciiSpace((unsigned char)cp);
    switch (cp) {
        case 0x0085: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return cp >= 0x2000 && cp <= 0x200A;
    }
}

}  // namespace

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(size_t(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), need);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                   nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(size_t(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), need, nullptr,
                        nullptr);
    return out;
}

std::string SanitizeUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned cp = 0;
        size_t used = DecodeUtf8(text, pos, &cp);
        if (used == 0) {
            AppendUtf8(out, 0xFFFD);
            ++pos;
            continue;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {   // 代理项在 UTF-8 里非法
            AppendUtf8(out, 0xFFFD);
        } else {
            out.append(text, pos, used);
        }
        pos += used;
    }
    return out;
}

// 系统真正的 ANSI 代码页（中文 Windows 是 936）。
// 只取一次，之后记着用。
UINT SystemAnsiCodePage() {
    static UINT cached = 0;
    if (cached) return cached;

    // 从注册表读，别用 GetACP()/GetLocaleInfo：本程序的 manifest 里写了
    // activeCodePage=UTF-8，那会把这两个也带成 65001，等于没转。
    wchar_t buffer[32] = L"";
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", L"ACP",
                     RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
        cached = (UINT)wcstoul(buffer, nullptr, 10);
    }
    // 注册表也没有（或者系统本来就用 UTF-8）：那就没什么可转的，
    // 走 UTF-8 这条路即可，非法字节交给 SanitizeUtf8 变 U+FFFD。
    if (cached == 0 || cached == 65001) cached = CP_UTF8;
    return cached;
}

std::string NormalizeToUtf8(const std::string& text) {
    // 先看是不是合法 UTF-8：是就别动它（避免把本来就是 UTF-8 的文本误转）
    {
        size_t pos = 0;
        bool valid = true;
        while (pos < text.size()) {
            unsigned cp = 0;
            size_t used = DecodeUtf8(text, pos, &cp);
            if (used == 0) {
                valid = false;
                break;
            }
            pos += used;
        }
        if (valid) return text;
    }
    // 注意：不能用 CP_ACP —— 本程序的 manifest 里写了 activeCodePage=UTF-8，
    // 那会把 CP_ACP 重定向成 UTF-8，等于没转。要去问真正的系统代码页（中文是 936）。
    UINT code_page = SystemAnsiCodePage();
    if (code_page == CP_UTF8) return SanitizeUtf8(text);
    int need = MultiByteToWideChar(code_page, 0, text.data(), int(text.size()), nullptr, 0);
    if (need <= 0) return SanitizeUtf8(text);
    std::wstring wide(size_t(need), L'\0');
    MultiByteToWideChar(code_page, 0, text.data(), int(text.size()), wide.data(), need);
    return WideToUtf8(wide);
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsAsciiSpace((unsigned char)text[begin])) ++begin;
    while (end > begin && IsAsciiSpace((unsigned char)text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string ToLowerAscii(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    }
    return out;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Contains(const std::string& text, const std::string& needle) {
    return !needle.empty() && text.find(needle) != std::string::npos;
}

std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t begin = 0;
    while (true) {
        size_t at = text.find(sep, begin);
        if (at == std::string::npos) {
            out.push_back(text.substr(begin));
            return out;
        }
        out.push_back(text.substr(begin, at - begin));
        begin = at + 1;
    }
}

std::string Join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

size_t CodePointCount(const std::string& text) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned cp = 0;
        size_t used = DecodeUtf8(text, pos, &cp);
        pos += used ? used : 1;
        ++count;
    }
    return count;
}

std::string TruncateCodePoints(const std::string& text, size_t limit) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < text.size() && count < limit) {
        unsigned cp = 0;
        size_t used = DecodeUtf8(text, pos, &cp);
        pos += used ? used : 1;
        ++count;
    }
    return text.substr(0, pos);
}

std::string TruncateUtf8Bytes(const std::string& text, size_t limit) {
    if (text.size() <= limit) return text;
    size_t pos = limit;
    while (pos > 0 && ((unsigned char)text[pos] & 0xC0) == 0x80) --pos;   // 回退到字符边界
    return text.substr(0, pos);
}

std::string CollapseWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_space = true;   // 开头也当空白，等于 Python 的 split 会丢掉前导空白
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned cp = 0;
        size_t used = DecodeUtf8(text, pos, &cp);
        if (used == 0) {
            out.push_back(text[pos]);
            in_space = false;
            ++pos;
            continue;
        }
        if (IsPythonSpace(cp)) {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.append(text, pos, used);
            in_space = false;
        }
        pos += used;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string FormatInt(long long value) { return std::to_string(value); }

std::string FormatDouble(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

std::string FindUuid(const std::string& text) {
    auto is_hex = [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    };
    const size_t lengths[5] = {8, 4, 4, 4, 12};
    for (size_t start = 0; start + 36 <= text.size(); ++start) {
        size_t pos = start;
        bool ok = true;
        for (int part = 0; part < 5 && ok; ++part) {
            for (size_t i = 0; i < lengths[part]; ++i) {
                if (pos >= text.size() || !is_hex(text[pos])) {
                    ok = false;
                    break;
                }
                ++pos;
            }
            if (part < 4) {
                if (pos >= text.size() || text[pos] != '-') {
                    ok = false;
                } else {
                    ++pos;
                }
            }
        }
        if (ok) return text.substr(start, 36);
    }
    return std::string();
}

std::string CleanText(const std::string& text) {
    // Windows 上偶尔混进来的孤立代理项（U+D800~U+DFFF 编码成 UTF-8 后是 ED A0.. 开头）
    return SanitizeUtf8(text);
}

}  // namespace cn
