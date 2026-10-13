// 极简 JSON：解析 + 序列化。
//
// 为什么自己写：整个程序不想依赖任何第三方库（Python 版也是纯标准库）。
// 与 Python json 模块对齐的几个点：
//   · 对象**保序**（读 config.json 再写回去，字段顺序不变）
//   · 字符串按 UTF-8 原样进出（等价 ensure_ascii=False），转义规则同 json.dumps
//   · 区分整数和浮点（version: 1 不能写成 1.0），浮点用最短往返表示（等价 repr）
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cn {

class Json {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };

    Json() = default;
    explicit Json(bool v) : type_(BOOL), bool_(v) {}
    explicit Json(long long v) : type_(NUM), int_(true), inum_(v), num_(double(v)) {}
    explicit Json(double v) : type_(NUM), num_(v) {}
    explicit Json(const char* v) : type_(STR), str_(v ? v : "") {}
    explicit Json(std::string v) : type_(STR), str_(std::move(v)) {}

    static Json Object();
    static Json Array();
    static Json Null() { return Json(); }
    static Json Num(long long v) { return Json(v); }
    static Json Num(int v) { return Json((long long)v); }
    static Json Num(double v) { return Json(v); }
    static Json Str(std::string v) { return Json(std::move(v)); }
    static Json Bool(bool v) { return Json(v); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == NUL; }
    bool is_bool() const { return type_ == BOOL; }
    bool is_num() const { return type_ == NUM; }
    bool is_string() const { return type_ == STR; }
    bool is_array() const { return type_ == ARR; }
    bool is_object() const { return type_ == OBJ; }

    // ---- 读取 ----
    bool as_bool(bool fallback = false) const;
    long long as_int(long long fallback = 0) const;
    double as_double(double fallback = 0.0) const;
    const std::string& as_string() const { return str_; }   // 非字符串时返回空串
    std::string as_string(const std::string& fallback) const {
        return type_ == STR ? str_ : fallback;
    }

    size_t size() const;
    bool empty() const;
    // 数组元素 / 对象第 i 个值
    const Json& at(size_t i) const;
    Json& at(size_t i);
    // 对象取值：不存在时返回一个静态 null
    const Json& get(const std::string& key) const;
    Json& operator[](const std::string& key);            // 不存在则创建（对象成员）
    const char* key_at(size_t i) const;                  // 对象第 i 个键名，非对象返回 ""
    const std::string& key_ref(size_t i) const;

    bool has(const std::string& key) const;
    bool has_str(const std::string& key) const;          // 存在且是非空字符串
    std::string get_str(const std::string& key, const std::string& fallback = "") const;
    long long get_int(const std::string& key, long long fallback = 0) const;
    double get_double(const std::string& key, double fallback = 0.0) const;
    bool get_bool(const std::string& key, bool fallback = false) const;
    const Json& get_or(const std::string& key, const Json& fallback) const;

    // ---- 写入 ----
    Json& set(const std::string& key, Json value);       // 对象：新增/覆盖
    Json& push_back(Json value);                         // 数组
    void remove(const std::string& key);
    void clear() { type_ = NUL; str_.clear(); arr_.clear(); obj_.clear(); }

    // ---- 解析 / 序列化 ----
    static Json Parse(const std::string& text, std::string* error = nullptr);
    static bool ParseFile(const std::string& path, Json* out, std::string* error = nullptr);
    std::string Dump(int indent = 2) const;

    void reset() { *this = Json(); }

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = NUL;
    bool bool_ = false;
    bool int_ = false;      // NUM 时：是否整数（整数不能写成 1.0）
    long long inum_ = 0;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

// 对象的“深合并”：base 被 override 覆盖，两边都是对象时递归合并。
// 对应 Python 的 config._deep_merge。
Json JsonDeepMerge(const Json& base, const Json& override_value);

}  // namespace cn
