#include "util/pinyin.h"

#include <cstdio>

namespace cn {
namespace {

// 解码一个 UTF-8 码点，返回消耗的字节数（0 = 非法）
size_t Decode(const std::string& text, size_t pos, unsigned* code) {
    unsigned char ch = (unsigned char)text[pos];
    if (ch < 0x80) {
        *code = ch;
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
    if (pos + need >= text.size()) return 0;
    for (size_t i = 1; i <= need; ++i) {
        unsigned char next = (unsigned char)text[pos + i];
        if ((next & 0xC0) != 0x80) return 0;
        value = (value << 6) | (next & 0x3F);
    }
    *code = value;
    return need + 1;
}

bool IsHanzi(unsigned code) {
    return (code >= 0x4E00 && code <= 0x9FFF) ||      // 基本区
           (code >= 0x3400 && code <= 0x4DBF) ||      // 扩展 A
           (code >= 0xF900 && code <= 0xFAFF);        // 兼容汉字
}

void AppendWithDash(std::string* out, const std::string& piece) {
    if (piece.empty()) return;
    if (!out->empty() && out->back() != '-') out->push_back('-');
    *out += piece;
}

}  // namespace

std::string PinyinSlug(const std::string& text, size_t max_length) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        unsigned code = 0;
        size_t used = Decode(text, pos, &code);
        if (used == 0) {
            ++pos;
            continue;
        }
        pos += used;

        if ((code >= 'a' && code <= 'z') || (code >= '0' && code <= '9')) {
            out.push_back(char(code));
        } else if (code >= 'A' && code <= 'Z') {
            out.push_back(char(code - 'A' + 'a'));
        } else if (IsHanzi(code)) {
            const char* syllable = LookupPinyin(code);
            if (syllable) {
                AppendWithDash(&out, syllable);
            } else {
                // 表里没有：用码点兜底，保证不同的字一定落在不同的文件名上
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "u%04x", code);
                AppendWithDash(&out, buffer);
            }
        } else if (code == '-') {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        } else {
            // 空格、标点、其它符号都当分隔符
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > max_length) {
        out.resize(max_length);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out;
}

}  // namespace cn
