#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/str.h"

namespace cn {
namespace {

const Json& NullRef() {
    static const Json kNull;
    return kNull;
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

// 把浮点写成最短往返形式；整数值补 ".0"（和 Python repr(float) 一致）
std::string FormatDouble(double value) {
    if (std::isnan(value) || std::isinf(value)) return "null";   // JSON 没有 NaN/Inf
    char buf[40];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    std::string text(buf, result.ptr);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

void DumpString(std::string& out, const std::string& text) {
    out.push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back(char(ch));   // UTF-8 原样输出（等价 ensure_ascii=False）
                }
        }
    }
    out.push_back('"');
}

class Parser {
public:
    Parser(const std::string& text, std::string* error) : text_(text), error_(error) {}

    bool Run(Json* out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        if (pos_ != text_.size()) return Fail("trailing content");
        return true;
    }

private:
    bool Fail(const char* what) {
        if (error_ && error_->empty()) {
            *error_ = std::string(what) + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    void SkipWs() {
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Literal(const char* word) {
        size_t len = std::strlen(word);
        if (text_.compare(pos_, len, word) != 0) return false;
        pos_ += len;
        return true;
    }

    bool ParseValue(Json* out) {
        if (pos_ >= text_.size()) return Fail("unexpected end");
        char ch = text_[pos_];
        switch (ch) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string text;
                if (!ParseString(&text)) return false;
                *out = Json::Str(std::move(text));
                return true;
            }
            case 't':
                if (!Literal("true")) return Fail("bad literal");
                *out = Json::Bool(true);
                return true;
            case 'f':
                if (!Literal("false")) return Fail("bad literal");
                *out = Json::Bool(false);
                return true;
            case 'n':
                if (!Literal("null")) return Fail("bad literal");
                *out = Json();
                return true;
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(Json* out) {
        ++pos_;   // '{'
        Json object = Json::Object();
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            *out = std::move(object);
            return true;
        }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            SkipWs();
            Json value;
            if (!ParseValue(&value)) return false;
            object.set(key, std::move(value));
            SkipWs();
            if (pos_ >= text_.size()) return Fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                *out = std::move(object);
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Json* out) {
        ++pos_;   // '['
        Json array = Json::Array();
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            *out = std::move(array);
            return true;
        }
        while (true) {
            SkipWs();
            Json value;
            if (!ParseValue(&value)) return false;
            array.push_back(std::move(value));
            SkipWs();
            if (pos_ >= text_.size()) return Fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                *out = std::move(array);
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseString(std::string* out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected string");
        ++pos_;
        out->clear();
        while (true) {
            if (pos_ >= text_.size()) return Fail("unterminated string");
            unsigned char ch = (unsigned char)text_[pos_++];
            if (ch == '"') return true;
            if (ch != '\\') {
                out->push_back(char(ch));
                continue;
            }
            if (pos_ >= text_.size()) return Fail("bad escape");
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ParseHex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // 高代理：后面必须跟一个低代理，否则按 U+FFFD 处理
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            size_t save = pos_;
                            pos_ += 2;
                            unsigned low = 0;
                            if (!ParseHex4(&low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save;
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;   // 孤立低代理
                    }
                    AppendUtf8(*out, cp);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
    }

    bool ParseHex4(unsigned* out) {
        if (pos_ + 4 > text_.size()) return Fail("bad \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            char ch = text_[pos_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= unsigned(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= unsigned(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= unsigned(ch - 'A' + 10);
            } else {
                return Fail("bad \\u escape");
            }
        }
        *out = value;
        return true;
    }

    bool ParseNumber(Json* out) {
        size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool is_float = false;
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if (ch >= '0' && ch <= '9') {
                ++pos_;
            } else if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                is_float = is_float || ch == '.' || ch == 'e' || ch == 'E';
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) return Fail("bad number");
        std::string text = text_.substr(start, pos_ - start);
        if (!is_float) {
            errno = 0;
            char* end = nullptr;
            long long value = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                *out = Json::Num(value);
                return true;
            }
            // 超出 long long：退化成浮点
        }
        errno = 0;
        char* end = nullptr;
        double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || (end && *end != '\0')) return Fail("bad number");
        *out = Json::Num(value);
        return true;
    }

    const std::string& text_;
    std::string* error_;
    size_t pos_ = 0;
};

}  // namespace

Json Json::Object() {
    Json value;
    value.type_ = OBJ;
    return value;
}

Json Json::Array() {
    Json value;
    value.type_ = ARR;
    return value;
}

bool Json::as_bool(bool fallback) const {
    if (type_ == BOOL) return bool_;
    if (type_ == NUM) return num_ != 0.0;
    if (type_ == STR) return !str_.empty() && str_ != "false" && str_ != "0";
    return fallback;
}

long long Json::as_int(long long fallback) const {
    if (type_ == NUM) return int_ ? inum_ : (long long)num_;
    if (type_ == BOOL) return bool_ ? 1 : 0;
    if (type_ == STR) {
        try {
            return std::stoll(str_);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

double Json::as_double(double fallback) const {
    if (type_ == NUM) return int_ ? double(inum_) : num_;
    if (type_ == BOOL) return bool_ ? 1.0 : 0.0;
    if (type_ == STR) {
        try {
            return std::stod(str_);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

size_t Json::size() const {
    if (type_ == ARR) return arr_.size();
    if (type_ == OBJ) return obj_.size();
    if (type_ == STR) return str_.size();
    return 0;
}

bool Json::empty() const { return size() == 0; }

const Json& Json::at(size_t index) const {
    if (type_ == ARR) {
        if (index < arr_.size()) return arr_[index];
    } else if (type_ == OBJ) {
        if (index < obj_.size()) return obj_[index].second;
    }
    return NullRef();
}

Json& Json::at(size_t index) {
    if (type_ == ARR) {
        if (index < arr_.size()) return arr_[index];
        arr_.resize(index + 1);
        return arr_[index];
    }
    if (type_ != OBJ) {
        type_ = OBJ;
        obj_.clear();
    }
    while (obj_.size() <= index) obj_.emplace_back(std::string(), Json());
    return obj_[index].second;
}

const Json& Json::get(const std::string& key) const {
    if (type_ == OBJ) {
        for (const auto& item : obj_) {
            if (item.first == key) return item.second;
        }
    }
    return NullRef();
}

Json& Json::operator[](const std::string& key) {
    if (type_ != OBJ) {
        type_ = OBJ;
        obj_.clear();
    }
    for (auto& item : obj_) {
        if (item.first == key) return item.second;
    }
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

const char* Json::key_at(size_t index) const {
    if (type_ == OBJ && index < obj_.size()) return obj_[index].first.c_str();
    return "";
}

const std::string& Json::key_ref(size_t index) const {
    static const std::string kEmpty;
    if (type_ == OBJ && index < obj_.size()) return obj_[index].first;
    return kEmpty;
}

bool Json::has(const std::string& key) const {
    if (type_ != OBJ) return false;
    for (const auto& item : obj_) {
        if (item.first == key) return true;
    }
    return false;
}

bool Json::has_str(const std::string& key) const {
    const Json& value = get(key);
    return value.is_string() && !value.str_.empty();
}

std::string Json::get_str(const std::string& key, const std::string& fallback) const {
    const Json& value = get(key);
    return value.is_string() ? value.str_ : fallback;
}

long long Json::get_int(const std::string& key, long long fallback) const {
    const Json& value = get(key);
    return value.is_null() ? fallback : value.as_int(fallback);
}

double Json::get_double(const std::string& key, double fallback) const {
    const Json& value = get(key);
    return value.is_null() ? fallback : value.as_double(fallback);
}

bool Json::get_bool(const std::string& key, bool fallback) const {
    const Json& value = get(key);
    return value.is_null() ? fallback : value.as_bool(fallback);
}

const Json& Json::get_or(const std::string& key, const Json& fallback) const {
    const Json& value = get(key);
    return value.is_null() ? fallback : value;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != OBJ) {
        type_ = OBJ;
        obj_.clear();
    }
    for (auto& item : obj_) {
        if (item.first == key) {
            item.second = std::move(value);
            return *this;
        }
    }
    obj_.emplace_back(key, std::move(value));
    return *this;
}

Json& Json::push_back(Json value) {
    if (type_ != ARR) {
        type_ = ARR;
        arr_.clear();
    }
    arr_.push_back(std::move(value));
    return *this;
}

void Json::remove(const std::string& key) {
    if (type_ != OBJ) return;
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) {
            obj_.erase(obj_.begin() + long(i));
            return;
        }
    }
}

Json Json::Parse(const std::string& text, std::string* error) {
    Json out;
    Parser parser(text, error);
    if (!parser.Run(&out)) return Json();
    return out;
}

bool Json::ParseFile(const std::string& path, Json* out, std::string* error) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);   // 容忍 BOM（我们自己写文件时不写 BOM）
    }
    Json parsed = Parse(text, error);
    if (error && !error->empty()) return false;
    *out = std::move(parsed);
    return true;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    const std::string pad = indent > 0 ? std::string(size_t(indent) * size_t(depth + 1), ' ') : "";
    const std::string pad_end = indent > 0 ? std::string(size_t(indent) * size_t(depth), ' ') : "";
    const char* newline = indent > 0 ? "\n" : "";
    switch (type_) {
        case NUL: out += "null"; break;
        case BOOL: out += bool_ ? "true" : "false"; break;
        case NUM: out += int_ ? std::to_string(inum_) : FormatDouble(num_); break;
        case STR: DumpString(out, str_); break;
        case ARR:
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            out += newline;
            for (size_t i = 0; i < arr_.size(); ++i) {
                out += pad;
                arr_[i].dump_to(out, indent, depth + 1);
                if (i + 1 < arr_.size()) out += ",";
                out += newline;
            }
            out += pad_end;
            out += "]";
            break;
        case OBJ:
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            out += newline;
            for (size_t i = 0; i < obj_.size(); ++i) {
                out += pad;
                DumpString(out, obj_[i].first);
                out += ":";
                if (indent > 0) out += " ";
                obj_[i].second.dump_to(out, indent, depth + 1);
                if (i + 1 < obj_.size()) out += ",";
                out += newline;
            }
            out += pad_end;
            out += "}";
            break;
    }
}

std::string Json::Dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

Json JsonDeepMerge(const Json& base, const Json& override_value) {
    if (!base.is_object() || !override_value.is_object()) {
        return override_value.is_null() && !base.is_null() ? base : override_value;
    }
    Json out = base;
    for (size_t i = 0; i < override_value.size(); ++i) {
        const std::string& key = override_value.key_ref(i);
        const Json& value = override_value.at(i);
        const Json& mine = out.get(key);
        if (value.is_object() && mine.is_object()) {
            out.set(key, JsonDeepMerge(mine, value));
        } else {
            out.set(key, value);
        }
    }
    return out;
}

}  // namespace cn
