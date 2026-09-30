// script_value.h —— 脚本值（两套引擎共享的公共数据面，与语言无关）
//
// 设计说明（本次拆分）：
//   * `Value` 是 BASIC 与 Lua 引擎**共同的数据面**（NUM/STR/NIL），
//     原先寄生在 script.h（里面还放着 BASIC 的 ScriptEngine 类），
//     导致 Lua 引擎为了拿一个 Value 不得不 include「BASIC 的头」。
//   * 现独立到本头文件：任何需要 Value 的模块（含 Lua 引擎）都只依赖于此，
//     不再与 BASIC 引擎耦合。
//   * 只放**纯值类型**，不引入任何引擎实现或第三方依赖。
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kx {

// ---------------------------------------------------------------------------
// 值：数字或字符串
// ---------------------------------------------------------------------------
struct Value {
    enum Type { NIL = 0, NUM, STR } type = NIL;
    double      num = 0.0;
    std::string str;

    static Value number(double v) { Value x; x.type = NUM; x.num = v; return x; }
    static Value text(const std::string& s) { Value x; x.type = STR; x.str = s; return x; }

    // 定义内联在头里：Value 是 BASIC / Lua 两个引擎共享的基础值类型，
    // 放在这里可避免任一引擎仅为这几个小工具函数就被迫链接对方实现。
    double to_num() const {
        if (type == NUM) return num;
        if (type == STR) {
            const char* p = str.c_str();
            while (*p && std::isspace((unsigned char)*p)) ++p;
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            return (end == p) ? 0.0 : v;
        }
        return 0.0;
    }

    std::string to_text() const {
        if (type == STR) return str;
        if (type == NIL) return "";
        char buf[64];
        if (num == std::floor(num) && std::fabs(num) < 1e15)
            std::snprintf(buf, sizeof(buf), "%lld", (long long)num);
        else
            std::snprintf(buf, sizeof(buf), "%g", num);
        return buf;
    }

    bool truthy() const { return type == STR ? !str.empty() : to_num() != 0.0; }
    bool is_nil() const { return type == NIL; }
};

} // namespace kx
