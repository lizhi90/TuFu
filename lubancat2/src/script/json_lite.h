// json_lite.h —— 极简 JSON 值 / 解析 / 序列化（零依赖，头文件实现）
//
// 用途：调试通道（见 docs/planA/13）的报文格式是 **JSON-Lines**
//   （一行一个 JSON 对象，UTF-8，'\n' 结束），控制器侧需要一个不引第三方库的
//   JSON 解析/生成实现。第三方 JSON 库（nlohmann 等）体积大且本项目只需这一处，
//   故自带一份**够用且严格**的实现：
//     * 值类型：null / bool / number / string / array / object；
//     * 字符串转义完整（含 \uXXXX 与 UTF-16 代理对 -> UTF-8），确保插件发来的
//       中文源码（script.compile 的 src）不会被截断或乱码；
//     * 数字：输出整数时不留 ".0"，非整数用 %.10g，与 JS JSON.stringify 观感一致；
//     * 解析失败给出中文错误串（进 BAD_REQUEST 回包的 msg，便于现场定位）。
//
// 设计边界：不做 DOM 变更、不做流式解析；对象用**有序数组**存放键值对（保留插入顺序，
// 回包字段顺序稳定，便于日志比对）。同一键重复出现时 find() 返回**第一个**。
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace kx {

// ---------------------------------------------------------------------------
// JSON 值
// ---------------------------------------------------------------------------
struct Json {
    enum Type { NIL = 0, BOOL, NUM, STR, ARR, OBJ };

    Type        type = NIL;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;                                 // STR
    std::vector<Json> arr;                           // ARR
    std::vector<std::pair<std::string, Json>> obj;   // OBJ（保序）

    // ---- 构造 ----
    static Json make_bool(bool v)             { Json j; j.type = BOOL; j.b = v;    return j; }
    static Json make_num(double v)            { Json j; j.type = NUM;  j.num = v;  return j; }
    static Json make_int(long long v)         { return make_num((double)v); }
    static Json make_str(const std::string& s){ Json j; j.type = STR;  j.str = s;  return j; }
    static Json make_arr()                    { Json j; j.type = ARR;  return j; }
    static Json make_obj()                    { Json j; j.type = OBJ;  return j; }

    // ---- 判定 ----
    bool is_nil()  const { return type == NIL; }
    bool is_bool() const { return type == BOOL; }
    bool is_num()  const { return type == NUM; }
    bool is_str()  const { return type == STR; }
    bool is_arr()  const { return type == ARR; }
    bool is_obj()  const { return type == OBJ; }

    // ---- 写入（OBJ/ARR）----
    Json& set(const std::string& k, Json v) { obj.emplace_back(k, std::move(v)); return *this; }
    Json& push(Json v)                      { arr.push_back(std::move(v));       return *this; }

    // ---- 读取 ----
    const Json* find(const std::string& k) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& k) const { return find(k) != nullptr; }

    // 取值（类型不符时返回默认值；解析层不抛异常，调用方自行判类型给 BAD_PARAM）
    std::string as_str(const std::string& def = std::string()) const {
        return type == STR ? str : def;
    }
    double    as_num(double def = 0.0) const { return type == NUM ? num : def; }
    long long as_int(long long def = 0) const {
        if (type != NUM) return def;
        return (long long)num;   // 与 JS 一致：截断取整
    }
    bool as_bool(bool def = false) const { return type == BOOL ? b : def; }
};

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------
inline void json_escape_to(const std::string& s, std::string* out) {
    *out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n";  break;
            case '\r': *out += "\\r";  break;
            case '\t': *out += "\\t";  break;
            case '\b': *out += "\\b";  break;
            case '\f': *out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    *out += buf;
                } else {
                    *out += (char)c;   // UTF-8 原样透传
                }
        }
    }
    *out += '"';
}

inline void json_dump_to(const Json& j, std::string* out) {
    switch (j.type) {
        case Json::NIL:
            *out += "null";
            break;
        case Json::BOOL:
            *out += j.b ? "true" : "false";
            break;
        case Json::NUM: {
            char buf[40];
            if (j.num == (double)(long long)j.num && std::fabs(j.num) < 1e15)
                std::snprintf(buf, sizeof(buf), "%lld", (long long)j.num);
            else
                std::snprintf(buf, sizeof(buf), "%.10g", j.num);
            *out += buf;
            break;
        }
        case Json::STR:
            json_escape_to(j.str, out);
            break;
        case Json::ARR:
            *out += '[';
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) *out += ',';
                json_dump_to(j.arr[i], out);
            }
            *out += ']';
            break;
        case Json::OBJ:
            *out += '{';
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) *out += ',';
                json_escape_to(j.obj[i].first, out);
                *out += ':';
                json_dump_to(j.obj[i].second, out);
            }
            *out += '}';
            break;
    }
}

inline std::string json_dump(const Json& j) {
    std::string s;
    json_dump_to(j, &s);
    return s;
}

// ---------------------------------------------------------------------------
// 解析（递归下降）
// ---------------------------------------------------------------------------
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(Json* out) {
        skip_ws();
        if (!value(out)) return false;
        skip_ws();
        if (i_ != s_.size()) return fail("JSON 尾部有多余字符");
        return true;
    }

    const std::string& error() const { return err_; }

private:
    const std::string& s_;
    size_t             i_ = 0;
    std::string        err_;

    bool fail(const char* m) { if (err_.empty()) err_ = m; return false; }

    void skip_ws() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool value(Json* out) {
        skip_ws();
        if (i_ >= s_.size()) return fail("JSON 意外结束");
        const char c = s_[i_];
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') { out->type = Json::STR; return string(&out->str); }
        if (c == 't' || c == 'f') return literal_bool(out);
        if (c == 'n') return literal_null(out);
        return number(out);
    }

    bool object(Json* out) {
        ++i_;                                  // '{'
        out->type = Json::OBJ;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("JSON 对象缺少键");
            std::string k;
            if (!string(&k)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("JSON 对象缺少 ':'");
            ++i_;
            Json v;
            if (!value(&v)) return false;
            out->obj.emplace_back(std::move(k), std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            return fail("JSON 对象缺少 ',' 或 '}'");
        }
    }

    bool array(Json* out) {
        ++i_;                                  // '['
        out->type = Json::ARR;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            Json v;
            if (!value(&v)) return false;
            out->arr.push_back(std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            return fail("JSON 数组缺少 ',' 或 ']'");
        }
    }

    bool string(std::string* out) {
        ++i_;                                  // 开引号
        out->clear();
        while (i_ < s_.size()) {
            const unsigned char c = (unsigned char)s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { *out += (char)c; continue; }
            if (i_ >= s_.size()) return fail("JSON 转义不完整");
            const char e = s_[i_++];
            switch (e) {
                case '"':  *out += '"';  break;
                case '\\': *out += '\\'; break;
                case '/':  *out += '/';  break;
                case 'n':  *out += '\n'; break;
                case 't':  *out += '\t'; break;
                case 'r':  *out += '\r'; break;
                case 'b':  *out += '\b'; break;
                case 'f':  *out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(&cp)) return false;
                    // UTF-16 代理对：\uD83D\uDE00 -> 单个码点
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        const size_t save = i_;
                        i_ += 2;
                        unsigned lo = 0;
                        if (hex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        else
                            i_ = save;   // 不是低代理：回退，按单码点处理
                    }
                    utf8_append(cp, out);
                    break;
                }
                default: return fail("JSON 未知转义");
            }
        }
        return fail("JSON 字符串未闭合");
    }

    bool hex4(unsigned* out) {
        if (i_ + 4 > s_.size()) return fail("JSON \\u 不完整");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail("JSON \\u 非法十六进制");
        }
        *out = v;
        return true;
    }

    static void utf8_append(unsigned cp, std::string* out) {
        if (cp < 0x80) {
            *out += (char)cp;
        } else if (cp < 0x800) {
            *out += (char)(0xC0 | (cp >> 6));
            *out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += (char)(0xE0 | (cp >> 12));
            *out += (char)(0x80 | ((cp >> 6) & 0x3F));
            *out += (char)(0x80 | (cp & 0x3F));
        } else {
            *out += (char)(0xF0 | (cp >> 18));
            *out += (char)(0x80 | ((cp >> 12) & 0x3F));
            *out += (char)(0x80 | ((cp >> 6) & 0x3F));
            *out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool literal_bool(Json* out) {
        if (s_.compare(i_, 4, "true") == 0)  { i_ += 4; out->type = Json::BOOL; out->b = true;  return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out->type = Json::BOOL; out->b = false; return true; }
        return fail("JSON 非法字面量");
    }

    bool literal_null(Json* out) {
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out->type = Json::NIL; return true; }
        return fail("JSON 非法字面量");
    }

    bool number(Json* out) {
        const size_t b = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool any_digit = false;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (std::isdigit((unsigned char)c)) { any_digit = true; ++i_; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { ++i_; continue; }
            break;
        }
        if (!any_digit) return fail("JSON 非法数字");
        const std::string t = s_.substr(b, i_ - b);
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || (end && *end != '\0')) return fail("JSON 数字解析失败");
        out->type = Json::NUM;
        out->num  = v;
        return true;
    }
};

// 解析一行 JSON；失败时 err 带中文原因（供 BAD_REQUEST 回包）
inline bool json_parse(const std::string& s, Json* out, std::string* err = nullptr) {
    JsonParser p(s);
    if (p.parse(out)) return true;
    if (err) *err = p.error();
    return false;
}

} // namespace kx
