// script.cpp —— ZBasic 子集：词法 / 语法 / 解释执行
//
// 语言子集（刻意保持小，够用即可；新增能力请在此处补文档）：
//   LET a = 1 / a = 1            赋值
//   PRINT a; "x"; b              输出（末尾 ; 不换行）
//   IF c THEN stmt               单行 IF
//   IF c THEN ... ELSE ... ENDIF 块 IF（支持 END IF / ELSE）
//   WHILE c ... WEND             循环（支持 END WHILE）
//   FOR i = a TO b [STEP s] ... NEXT [i]
//   GOTO 标号 / 标号:           顶层跳转
//   END                          结束脚本（注意：STOP 是设备命令「停止运动」，不是结束）
//   DIM a(10)                    兼容写法（数组自动按需创建）
//   REM ... 与 ' ...             注释；行尾 \ 续行
//   数组：A(i)（一维，下标不检查边界）
//   运算：+ - * / \ MOD ^ = <> < > <= >= AND OR NOT
//   内置函数：ABS INT SGN SQR SIN COS TAN MIN MAX LEN VAL STR
//   设备命令：交给 ScriptHost（见 motion_host.h）
//
// 已知限制（写在文档里，不偷偷藏）：
//   * GOTO/GOSUB 只保证在顶层语句间跳转正确，从循环体内跳出会丢失该层循环状态；
//   * 标签须定义在当前层（顶层或子程序体内）；GOSUB 返回地址栈不跨层；
//   * 数组下标不做边界检查。
#include "script/script.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kx {

// ===========================================================================
// Value
// ===========================================================================
// Value::to_num / to_text / truthy 的定义已内联到 script.h（供 BASIC 与 Lua
// 两个引擎共享，避免任一引擎仅为这几个小工具函数就链接本文件）。

// ===========================================================================
// 词法
// ===========================================================================
enum class TK { NUM, STR, ID, OP, EOL, EOFT };

struct Tok {
    TK          k = TK::EOFT;
    std::string s;
    double      n = 0;
    int         line = 1;
};

static std::string upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

static bool id_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool id_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$' || c == '%';
}

struct Lexer {
    const std::string& src;
    size_t i = 0;
    int    line = 1;
    std::vector<Tok> out;
    std::string err;
    int         err_line = 0;

    explicit Lexer(const std::string& s) : src(s) {}

    bool at_line_start() const { return out.empty() || out.back().k == TK::EOL; }

    bool run() {
        while (i < src.size()) {
            const char c = src[i];
            if (c == '\n') { Tok t; t.k = TK::EOL; t.line = line; out.push_back(t); ++i; ++line; continue; }
            if (c == '\r' || c == ' ' || c == '\t') { ++i; continue; }
            if (c == '\\') {                      // 仅"行尾的 \"算续行；否则是整除运算符
                size_t j = i + 1;
                while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r')) ++j;
                if (j < src.size() && src[j] == '\n') { i = j + 1; ++line; continue; }
            }
            if (c == '\'') { while (i < src.size() && src[i] != '\n') ++i; continue; }

            // 十六进制字面量：$FF / $6060（ZBasic 习惯），&H1F 亦接受
            if (c == '$' && i + 1 < src.size() && std::isxdigit((unsigned char)src[i + 1])) {
                size_t j = i + 1;
                while (j < src.size() && std::isxdigit((unsigned char)src[j])) ++j;
                Tok t; t.k = TK::NUM; t.line = line;
                t.n = (double)std::strtoull(src.c_str() + i + 1, nullptr, 16);
                out.push_back(t);
                i = j;
                continue;
            }
            if (c == '&' && i + 1 < src.size() && (src[i + 1] == 'H' || src[i + 1] == 'h')) {
                size_t j = i + 2;
                while (j < src.size() && std::isxdigit((unsigned char)src[j])) ++j;
                if (j == i + 2) { err = "&H 后缺少十六进制数字"; err_line = line; return false; }
                Tok t; t.k = TK::NUM; t.line = line;
                t.n = (double)std::strtoull(src.c_str() + i + 2, nullptr, 16);
                out.push_back(t);
                i = j;
                continue;
            }

            if (std::isdigit((unsigned char)c) ||
                (c == '.' && i + 1 < src.size() && std::isdigit((unsigned char)src[i + 1]))) {
                const char* start = src.c_str() + i;
                char* end = nullptr;
                const double v = std::strtod(start, &end);
                Tok t; t.k = TK::NUM; t.n = v; t.line = line;
                out.push_back(t);
                i += (size_t)(end - start);
                continue;
            }

            if (c == '"') {
                ++i;
                std::string s;
                bool closed = false;
                while (i < src.size()) {
                    if (src[i] == '"') {
                        if (i + 1 < src.size() && src[i + 1] == '"') { s += '"'; i += 2; continue; }
                        ++i; closed = true; break;
                    }
                    if (src[i] == '\n') break;
                    s += src[i++];
                }
                if (!closed) { err = "字符串缺少右引号"; err_line = line; return false; }
                Tok t; t.k = TK::STR; t.s = s; t.line = line;
                out.push_back(t);
                continue;
            }

            if (id_start(c)) {
                size_t j = i;
                while (j < src.size() && id_char(src[j])) ++j;
                const std::string word = src.substr(i, j - i);
                if (upper(word) == "REM" && at_line_start()) {
                    i = j;
                    while (i < src.size() && src[i] != '\n') ++i;
                    continue;
                }
                Tok t; t.k = TK::ID; t.s = word; t.line = line;
                out.push_back(t);
                i = j;
                continue;
            }

            const char c2 = (i + 1 < src.size()) ? src[i + 1] : '\0';
            std::string op;
            if ((c == '<' && (c2 == '=' || c2 == '>')) || (c == '>' && c2 == '=') ||
                (c == '!' && c2 == '=') || (c == '=' && c2 == '=')) {
                op = std::string(1, c) + c2;
                i += 2;
            } else {
                op = std::string(1, c);
                ++i;
            }
            if (op == "!=") op = "<>";
            if (op == "==") op = "=";
            static const char* known[] = {"+", "-", "*", "/", "\\", "^", "(", ")", ",",
                                          ";", "=", "<", ">", "<=", ">=", "<>", ":", "?", "#"};
            bool ok = false;
            for (const char* k : known) if (op == k) { ok = true; break; }
            if (!ok) { err = "无法识别的字符 '" + op + "'"; err_line = line; return false; }
            Tok t; t.k = TK::OP; t.s = op; t.line = line;
            out.push_back(t);
        }
        Tok t; t.k = TK::EOFT; t.line = line;
        out.push_back(t);
        return true;
    }
};

// ===========================================================================
// AST
// ===========================================================================
struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
    enum Kind { NUM, STR, VAR, BIN, UN, CALL } kind = NUM;
    double             num = 0;
    std::string        str;       // STR 字面量 / VAR 名 / 运算符 / CALL 名
    ExprP              a, b;
    std::vector<ExprP> args;
    int                line = 0;
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Stmt {
    struct Branch { ExprP cond; std::vector<StmtP> body; };   // IF / ELSEIF 各分支

    enum Kind { ASSIGN, ASSIGN_ARR, PRINT, IF, WHILE, FOR, CALL, GOTO, GOSUB, END,
                EXIT, WAIT, CONST_DEF, DIM_DEF, LOCAL_DEF, RETURN, GET, LABEL } kind = LABEL;
    std::string              name;      // 变量名 / 命令名 / 标签 / GET 的目标数组名
    std::string              target;    // GET：接收字节数的结果变量（可为空）
    ExprP                    e1, e2, e3;
    ExprP                    port;      // PRINT #port / GET #port 的端口表达式
    std::vector<StmtP>       body;
    std::vector<StmtP>       els;
    std::vector<Branch>      branches;  // 块式 IF 的全部分支（含 ELSEIF）
    std::vector<std::string> names;     // CONST/DIM 的多名声明
    std::vector<char>        is_arr;    // DIM：1 = 带括号（数组，同时可当 0 结尾字符串）
    std::vector<ExprP>       args;
    ExprP                    axis;              // 命令修饰符：MOVEABS(x) AXIS(0)
    bool                     print_newline = true;
    int                      line = 0;
};

// 子程序（GLOBAL SUB）：参数按值传入；形参与 LOCAL 在调用期间遮蔽同名全局
// 原程序用「全局变量 + SUB 名」通信（get_num 写 numval），因此无需返回值机制
struct SubDef {
    std::string              name;
    std::vector<std::string> params;
    std::vector<std::string> locals;      // 形参 + LOCAL 声明
    std::vector<StmtP>       body;
    std::unordered_map<std::string, size_t> labels;
    int                      def_line = 0;
};

// ===========================================================================
// 语法
// ===========================================================================
struct Parser {
    const std::vector<Tok>& t;
    size_t i = 0;
    std::string err;
    int         err_line = 0;
    std::unordered_map<std::string, SubDef> subs;   // 子程序定义表
    SubDef*     cur_sub = nullptr;                  // 当前正在解析的子程序（收 LOCAL）

    explicit Parser(const std::vector<Tok>& toks) : t(toks) {}

    const Tok& cur() const { return t[i]; }
    const Tok& nxt() const { return t[std::min(i + 1, t.size() - 1)]; }
    bool is_op(const char* s) const { return cur().k == TK::OP && cur().s == s; }
    bool is_id(const char* s) const { return cur().k == TK::ID && upper(cur().s) == s; }

    void fail(const std::string& m) {
        if (err.empty()) { err = m; err_line = cur().line; }
    }
    void skip_eol() { while (cur().k == TK::EOL) ++i; }

    // 关键字匹配，支持两词形式（"END IF"）
    bool at_kw(const char* kw) const {
        if (cur().k != TK::ID) return false;
        const std::string u = upper(cur().s);
        const std::string w = kw;
        const size_t sp = w.find(' ');
        if (sp == std::string::npos) return u == w;
        return u == w.substr(0, sp) && nxt().k == TK::ID && upper(nxt().s) == w.substr(sp + 1);
    }
    bool eat_kw(const char* kw) {
        if (!at_kw(kw)) return false;
        i += (std::string(kw).find(' ') == std::string::npos) ? 1 : 2;
        return true;
    }

    // ---------------- 表达式（优先级递增） ----------------
    void parse_expr(ExprP& o) { parse_or(o); }

    static ExprP mk_bin(const std::string& op, ExprP a, ExprP b, int ln) {
        auto n = std::make_unique<Expr>();
        n->kind = Expr::BIN; n->str = op; n->a = std::move(a); n->b = std::move(b); n->line = ln;
        return n;
    }

    // 逻辑/位运算优先级（低 -> 高）：OR < XOR < EQV < AND < 比较
    // 说明：与 ZBasic 一致，AND/OR/NOT/XOR/EQV 均为「按位」运算（布尔场景下与逻辑等价）
    void parse_or(ExprP& o) {
        parse_xor(o);
        while (is_id("OR")) { const int ln = cur().line; ++i; ExprP r; parse_xor(r); o = mk_bin("OR", std::move(o), std::move(r), ln); }
    }
    void parse_xor(ExprP& o) {
        parse_eqv(o);
        while (is_id("XOR")) { const int ln = cur().line; ++i; ExprP r; parse_eqv(r); o = mk_bin("XOR", std::move(o), std::move(r), ln); }
    }
    void parse_eqv(ExprP& o) {
        parse_and(o);
        while (is_id("EQV")) { const int ln = cur().line; ++i; ExprP r; parse_and(r); o = mk_bin("EQV", std::move(o), std::move(r), ln); }
    }
    void parse_and(ExprP& o) {
        parse_cmp(o);
        while (is_id("AND")) { const int ln = cur().line; ++i; ExprP r; parse_cmp(r); o = mk_bin("AND", std::move(o), std::move(r), ln); }
    }
    void parse_cmp(ExprP& o) {
        parse_add(o);
        while (cur().k == TK::OP && (cur().s == "=" || cur().s == "<>" || cur().s == "<" ||
                                     cur().s == ">" || cur().s == "<=" || cur().s == ">=")) {
            const std::string op = cur().s; const int ln = cur().line; ++i;
            ExprP r; parse_add(r); o = mk_bin(op, std::move(o), std::move(r), ln);
        }
    }
    void parse_add(ExprP& o) {
        parse_mul(o);
        while (is_op("+") || is_op("-")) {
            const std::string op = cur().s; const int ln = cur().line; ++i;
            ExprP r; parse_mul(r); o = mk_bin(op, std::move(o), std::move(r), ln);
        }
    }
    void parse_mul(ExprP& o) {
        parse_pow(o);
        while (is_op("*") || is_op("/") || is_op("\\") || is_id("MOD")) {
            const std::string op = (cur().k == TK::ID) ? "MOD" : cur().s;
            const int ln = cur().line; ++i;
            ExprP r; parse_pow(r); o = mk_bin(op, std::move(o), std::move(r), ln);
        }
    }
    void parse_pow(ExprP& o) {                    // 右结合
        parse_unary(o);
        if (is_op("^")) { const int ln = cur().line; ++i; ExprP r; parse_pow(r); o = mk_bin("^", std::move(o), std::move(r), ln); }
    }
    void parse_unary(ExprP& o) {
        if (is_op("-") || is_op("+")) {
            const std::string op = cur().s; const int ln = cur().line; ++i;
            ExprP r; parse_unary(r);
            if (op == "+") { o = std::move(r); return; }
            auto n = std::make_unique<Expr>();
            n->kind = Expr::UN; n->str = "-"; n->a = std::move(r); n->line = ln;
            o = std::move(n);
            return;
        }
        if (is_id("NOT")) {
            const int ln = cur().line; ++i;
            ExprP r; parse_unary(r);
            auto n = std::make_unique<Expr>();
            n->kind = Expr::UN; n->str = "NOT"; n->a = std::move(r); n->line = ln;
            o = std::move(n);
            return;
        }
        if (is_op("*")) {                    // ?*ARR：按字符串打印（ZBasic 的字符串形式）
            const int ln = cur().line; ++i;
            ExprP r; parse_unary(r);
            auto n = std::make_unique<Expr>();
            n->kind = Expr::UN; n->str = "@"; n->a = std::move(r); n->line = ln;
            o = std::move(n);
            return;
        }
        parse_primary(o);
    }
    void parse_arglist(std::vector<ExprP>& out) {
        if (is_op(")")) { ++i; return; }
        while (true) {
            ExprP e; parse_expr(e);
            out.push_back(std::move(e));
            if (is_op(",")) { ++i; continue; }
            if (is_op(")")) { ++i; break; }
            fail("参数表里期望 ',' 或 ')'");
            return;
        }
    }
    void parse_primary(ExprP& o) {
        const Tok& tk = cur();
        if (tk.k == TK::NUM) {
            o = std::make_unique<Expr>();
            o->kind = Expr::NUM; o->num = tk.n; o->line = tk.line; ++i; return;
        }
        if (tk.k == TK::STR) {
            o = std::make_unique<Expr>();
            o->kind = Expr::STR; o->str = tk.s; o->line = tk.line; ++i; return;
        }
        if (tk.k == TK::ID) {
            const std::string name = upper(tk.s);
            const int ln = tk.line;
            ++i;
            o = std::make_unique<Expr>();
            if (is_op("(")) {
                ++i;
                o->kind = Expr::CALL; o->line = ln;
                o->str = name;
                parse_arglist(o->args);
            } else {
                o->kind = Expr::VAR; o->str = name; o->line = ln;
            }
            return;
        }
        if (is_op("(")) {
            ++i;
            parse_expr(o);
            if (!is_op(")")) { fail("表达式缺少 ')'"); return; }
            ++i;
            return;
        }
        fail("表达式里出现意外记号");
        o = std::make_unique<Expr>();
        o->kind = Expr::NUM; o->num = 0; o->line = tk.line;
        if (cur().k != TK::EOFT && cur().k != TK::EOL) ++i;
    }

    // ---------------- 语句 ----------------
    // 收集子程序内的标签（含嵌套块），供 SUB 内的 GOTO 使用
    void collect_sub_labels(const std::vector<StmtP>& blk, std::unordered_map<std::string, size_t>& out) {
        for (size_t k = 0; k < blk.size(); ++k) {
            const Stmt& s = *blk[k];
            if (s.kind == Stmt::LABEL && !s.name.empty()) out[s.name] = k;
            else if (s.kind == Stmt::IF) {
                for (const Stmt::Branch& br : s.branches) collect_sub_labels(br.body, out);
                collect_sub_labels(s.els, out);
            } else if (s.kind == Stmt::WHILE || s.kind == Stmt::FOR) {
                collect_sub_labels(s.body, out);
            }
        }
    }

    // [GLOBAL] SUB name(p1, p2) ... END SUB
    bool parse_sub() {
        const int ln = cur().line;
        if (is_id("GLOBAL")) ++i;
        ++i;                                        // SUB
        if (cur_sub) { fail("不支持嵌套定义子程序"); return false; }
        if (cur().k != TK::ID) { fail("SUB 后需要子程序名"); return false; }
        SubDef d;
        d.name = upper(cur().s);
        d.def_line = ln;
        ++i;
        if (is_op("(")) {
            ++i;
            if (!is_op(")")) {
                while (true) {
                    if (cur().k != TK::ID) { fail("子程序 " + d.name + " 的参数名缺失"); return false; }
                    d.params.push_back(upper(cur().s));
                    ++i;
                    if (is_op(",")) { ++i; continue; }
                    break;
                }
            }
            if (!is_op(")")) { fail("子程序 " + d.name + " 的参数表缺少 ')'"); return false; }
            ++i;
        }
        d.locals = d.params;
        cur_sub = &d;
        std::string stop;
        const bool ok = parse_block(d.body, {"END SUB"}, &stop);
        cur_sub = nullptr;
        if (!ok) return false;
        if (stop != "END SUB") { fail("子程序 " + d.name + " 缺少 END SUB"); return false; }
        eat_kw("END SUB");
        collect_sub_labels(d.body, d.labels);
        if (subs.count(d.name)) { fail("子程序 " + d.name + " 重复定义"); return false; }
        subs.emplace(d.name, std::move(d));
        return true;
    }

    // 直到命中 stop_kws（不消费）或文件结束
    bool parse_block(std::vector<StmtP>& out, const std::vector<const char*>& stop_kws,
                     std::string* stopped_by) {
        while (true) {
            skip_eol();
            if (cur().k == TK::EOFT) { if (stopped_by) *stopped_by = "EOF"; return err.empty(); }
            for (const char* kw : stop_kws)
                if (at_kw(kw)) { if (stopped_by) *stopped_by = kw; return true; }

            // 子程序定义：SUB name(...) ... END SUB（不进当前语句流）
            if (is_id("SUB") || (is_id("GLOBAL") && nxt().k == TK::ID && upper(nxt().s) == "SUB")) {
                if (!parse_sub()) return false;
                continue;
            }

            // 顶层标签 IDENT ':'
            if (cur().k == TK::ID && nxt().k == TK::OP && nxt().s == ":") {
                auto s = std::make_unique<Stmt>();
                s->kind = Stmt::LABEL; s->name = upper(cur().s); s->line = cur().line;
                i += 2;
                out.push_back(std::move(s));
                continue;
            }
            if (is_op(":")) { ++i; continue; }

            StmtP s = parse_stmt();
            if (!err.empty()) return false;
            out.push_back(std::move(s));

            if (cur().k == TK::EOL) { skip_eol(); continue; }
            if (cur().k == TK::EOFT) { if (stopped_by) *stopped_by = "EOF"; return true; }
            if (is_op(":")) { ++i; continue; }
            if (cur().k == TK::ID) {
                bool stop = false;
                for (const char* kw : stop_kws) if (at_kw(kw)) stop = true;
                if (stop) { if (stopped_by) *stopped_by = upper(cur().s); return true; }
            }
            fail("语句后有多余内容（是否漏了换行？）");
            return false;
        }
    }

    // 命令修饰符：MOVEABS(x) AXIS(0)（目前只认 AXIS）
    void parse_cmd_modifiers(Stmt& s) {
        while (is_id("AXIS") && nxt().k == TK::OP && nxt().s == "(") {
            i += 2;                                  // AXIS (
            ExprP ax; parse_expr(ax);
            if (!is_op(")")) { fail("AXIS(...) 缺少 ')'"); return; }
            ++i;
            s.axis = std::move(ax);
        }
    }

    // 带 # 端口号的命令：CMD #port, args... → CALL CMD(port, args...)
    StmtP parse_port_cmd(StmtP s, const char* cmd) {
        s->kind = Stmt::CALL;
        s->name = cmd;
        ++i;                                        // 命令名
        if (is_op("#")) ++i;
        if (cur().k == TK::EOL || cur().k == TK::EOFT) { fail(std::string(cmd) + " 需要 #端口号"); return s; }
        ExprP p0; parse_expr(p0); s->args.push_back(std::move(p0));
        while (is_op(",")) { ++i; ExprP a; parse_expr(a); s->args.push_back(std::move(a)); }
        return s;
    }

    // OPEN #port, "TYPE", ...（端口号用 # 前缀，语法糖，等价于普通表达式）
    StmtP parse_open(StmtP s) { return parse_port_cmd(std::move(s), "OPEN"); }

    // GET #port, ARRAY [, 字节数] —— 非阻塞接收；字节写入数组并以 0 结尾
    StmtP parse_get(StmtP s, const std::string& target) {
        s->kind = Stmt::GET;
        s->target = target;
        ++i;                                        // GET
        if (is_op("#")) ++i;
        ExprP p0; parse_expr(p0); s->port = std::move(p0);
        if (!is_op(",")) { fail("GET 语法：GET #端口, 数组, 字节数"); return s; }
        ++i;
        if (cur().k != TK::ID) { fail("GET 的第二个参数应为数组名"); return s; }
        s->name = upper(cur().s);
        ++i;
        if (is_op("(")) {                           // 可带起始下标：GET #p, BUF(偏移), n
            ++i;
            parse_expr(s->e1);
            if (!is_op(")")) { fail("GET 数组下标缺少 ')'"); return s; }
            ++i;
        }
        if (is_op(",")) { ++i; parse_expr(s->e2); }  // 最大字节数（可省）
        return s;
    }

    // RUNTASK n, 子程序名：第二个参数是「任务名」而非数值，转成字符串常量交给宿主
    void fixup_task_name(Stmt& s) {
        if (s.name != "RUNTASK" || s.args.size() != 2) return;
        const Expr* a1 = s.args[1].get();
        if (a1 && a1->kind == Expr::VAR) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::STR; e->str = a1->str; e->line = a1->line;
            s.args[1] = std::move(e);
        }
    }

    StmtP parse_stmt() {
        auto s = std::make_unique<Stmt>();
        s->line = cur().line;
        if (cur().k == TK::OP && cur().s == "?") { ++i; return parse_print(std::move(s)); }
        if (cur().k != TK::ID) { fail("语句应以命令或变量名开头"); return s; }

        if (is_id("PRINT")) { ++i; return parse_print(std::move(s)); }

        // ---- 端口 IO：OPEN #p, ... / GET #p, arr, n（仅当紧随 '#' 才按关键字处理） ----
        if (is_id("OPEN") && nxt().k == TK::OP && nxt().s == "#") return parse_open(std::move(s));
        if (is_id("GET")  && nxt().k == TK::OP && nxt().s == "#") return parse_get(std::move(s), "");
        if (is_id("PUTCHAR") && nxt().k == TK::OP && nxt().s == "#") return parse_port_cmd(std::move(s), "PUTCHAR");

        // ---- 声明：GLOBAL CONST / GLOBAL DIM（GLOBAL SUB 见子程序章节） ----
        if (is_id("GLOBAL")) {
            ++i;
            if (!is_id("CONST") && !is_id("DIM")) {
                fail("GLOBAL 后只支持 CONST / DIM");
                return s;
            }
        }
        if (is_id("CONST")) {                                  // CONST N = 值[, M = 值]
            ++i;
            s->kind = Stmt::CONST_DEF;
            while (true) {
                if (cur().k != TK::ID) { fail("CONST 后需要常量名"); return s; }
                s->names.push_back(upper(cur().s));
                ++i;
                if (!is_op("=")) { fail("常量名后需要 '='"); return s; }
                ++i;
                ExprP e; parse_expr(e);
                s->args.push_back(std::move(e));
                if (is_op(",")) { ++i; continue; }
                break;
            }
            return s;
        }
        if (is_id("DIM")) {                                    // DIM a, b(10), c（登记；不设容量上限）
            ++i;
            s->kind = Stmt::DIM_DEF;
            while (true) {
                if (cur().k != TK::ID) { fail("DIM 后需要变量名"); return s; }
                s->names.push_back(upper(cur().s));
                ++i;
                char arr = 0;
                if (is_op("(")) {
                    ++i; arr = 1;
                    ExprP dim; parse_expr(dim);
                    if (!is_op(")")) { fail("DIM 的 ')' 缺失"); return s; }
                    ++i;
                }
                s->is_arr.push_back(arr);
                if (is_op(",")) { ++i; continue; }
                break;
            }
            return s;
        }

        // ---- WAIT IDLE（等轴停止）/ WAIT UNTIL 条件 / WAIT ms（延时，同 ZBasic）----
        // WA / WAIT 后跟 '=' 时按普通赋值处理，避免与变量名冲突
        if ((is_id("WAIT") || is_id("WA")) && !(nxt().k == TK::OP && nxt().s == "=")) {
            ++i;
            s->kind = Stmt::WAIT;
            if (is_id("IDLE")) { ++i; s->name = "IDLE"; return s; }
            if (is_id("UNTIL")) { ++i; s->name = "UNTIL"; parse_expr(s->e1); return s; }
            s->name = "DELAY";                                 // WAIT 1000 / WA 1000 = 延时 1000ms
            parse_expr(s->e1);
            return s;
        }

        // ---- EXIT FOR / EXIT WHILE ----
        if (is_id("EXIT")) {
            ++i;
            s->kind = Stmt::EXIT;
            if (cur().k != TK::ID) { fail("EXIT 后需要 FOR / WHILE"); return s; }
            s->name = upper(cur().s);
            ++i;
            if (s->name != "FOR" && s->name != "WHILE" && s->name != "SUB") {
                fail("EXIT 只支持 FOR / WHILE / SUB");
                return s;
            }
            return s;
        }

        // ---- LOCAL a, b（子程序内局部变量） ----
        if (is_id("LOCAL")) {
            ++i;
            s->kind = Stmt::LOCAL_DEF;
            while (true) {
                if (cur().k != TK::ID) { fail("LOCAL 后需要变量名"); return s; }
                const std::string n = upper(cur().s);
                s->names.push_back(n);
                if (cur_sub) cur_sub->locals.push_back(n);
                ++i;
                if (is_op(",")) { ++i; continue; }
                break;
            }
            return s;
        }

        // ---- RETURN（提前结束子程序）----
        if (is_id("RETURN")) {
            ++i;
            s->kind = Stmt::RETURN;
            if (cur().k != TK::EOL && cur().k != TK::EOFT && !is_op(":")) {
                parse_expr(s->e1);      // 容错：忽略返回值表达式（原程序用全局变量通信）
            }
            return s;
        }

        if (is_id("IF")) {
            ++i;
            s->kind = Stmt::IF;
            ExprP cond; parse_expr(cond);
            if (!eat_kw("THEN")) { fail("IF 后缺少 THEN"); return s; }
            if (cur().k != TK::EOL) {                      // 单行式 IF c THEN stmt
                Stmt::Branch br; br.cond = std::move(cond);
                br.body.push_back(parse_stmt());
                s->branches.push_back(std::move(br));
                return s;
            }
            // 块式：IF / ELSEIF... / ELSE / ENDIF
            static const std::vector<const char*> kIfStop =
                {"ELSEIF", "ELSE IF", "ELSE", "ENDIF", "END IF"};
            s->branches.push_back(Stmt::Branch{std::move(cond), {}});
            skip_eol();
            std::string stop;
            if (!parse_block(s->branches.back().body, kIfStop, &stop)) return s;
            while (stop == "ELSEIF" || stop == "ELSE IF") {
                eat_kw(stop.c_str());
                ExprP c2; parse_expr(c2);
                if (!eat_kw("THEN")) { fail("ELSEIF 后缺少 THEN"); return s; }
                if (cur().k != TK::EOL) {
                    fail("ELSEIF 后应换行（ELSEIF 只支持块式；单行分支请用单行 IF）");
                    return s;
                }
                s->branches.push_back(Stmt::Branch{std::move(c2), {}});
                skip_eol();
                std::string st2;
                if (!parse_block(s->branches.back().body, kIfStop, &st2)) return s;
                stop = st2;
            }
            if (stop == "ELSE") {
                eat_kw("ELSE");
                std::string stop2;
                if (!parse_block(s->els, {"ENDIF", "END IF"}, &stop2)) return s;
                if (stop2 == "EOF") { fail("IF 块缺少 ENDIF"); return s; }
                eat_kw(stop2.c_str());
            } else {
                if (stop == "EOF") { fail("IF 块缺少 ENDIF"); return s; }
                eat_kw(stop.c_str());
            }
            return s;
        }

        if (is_id("WHILE")) {
            ++i;
            s->kind = Stmt::WHILE;
            parse_expr(s->e1);
            if (cur().k != TK::EOL && !is_op(":")) { fail("WHILE 条件后应换行"); return s; }
            skip_eol();
            std::string stop;
            if (!parse_block(s->body, {"WEND", "END WHILE"}, &stop)) return s;
            if (stop == "EOF") { fail("WHILE 缺少 WEND"); return s; }
            eat_kw(stop.c_str());
            return s;
        }

        if (is_id("FOR")) {
            ++i;
            s->kind = Stmt::FOR;
            if (cur().k != TK::ID) { fail("FOR 后需要循环变量"); return s; }
            s->name = upper(cur().s);
            ++i;
            if (!is_op("=")) { fail("FOR 变量后需要 '='"); return s; }
            ++i;
            parse_expr(s->e1);
            if (!is_id("TO")) { fail("FOR 缺少 TO"); return s; }
            ++i;
            parse_expr(s->e2);
            if (is_id("STEP")) { ++i; parse_expr(s->e3); }
            if (cur().k != TK::EOL && !is_op(":")) { fail("FOR 头部后应换行"); return s; }
            skip_eol();
            std::string stop;
            if (!parse_block(s->body, {"NEXT"}, &stop)) return s;
            if (stop == "EOF") { fail("FOR 缺少 NEXT"); return s; }
            eat_kw("NEXT");
            if (cur().k == TK::ID && upper(cur().s) == s->name) ++i;   // NEXT i
            return s;
        }

        if (is_id("GOTO")) {
            ++i;
            if (cur().k != TK::ID) { fail("GOTO 后需要标号"); return s; }
            s->kind = Stmt::GOTO;
            s->name = upper(cur().s);
            ++i;
            return s;
        }
        if (is_id("GOSUB")) {
            ++i;
            if (cur().k != TK::ID) { fail("GOSUB 后需要标号"); return s; }
            s->kind = Stmt::GOSUB;
            s->name = upper(cur().s);
            ++i;
            return s;
        }
        // 只认 END 作为结束语句；STOP 是设备命令「停止运动」，不是结束脚本
        if (is_id("END")) { ++i; s->kind = Stmt::END; return s; }
        if (is_id("LET")) { ++i; }

        // 赋值 / 数组赋值 / 命令调用
        if (cur().k != TK::ID) { fail("期望变量名或命令名"); return s; }
        const std::string name = upper(cur().s);
        const int ln = cur().line;
        ++i;

        if (is_op("(")) {
            ++i;
            std::vector<ExprP> a;
            bool had_comma = false;
            if (!is_op(")")) {
                while (true) {
                    ExprP e; parse_expr(e);
                    a.push_back(std::move(e));
                    if (is_op(",")) { ++i; had_comma = true; continue; }
                    break;
                }
            }
            if (!is_op(")")) { fail("参数表缺少 ')'"); return s; }
            ++i;
            if (a.size() == 1 && !had_comma && is_op("=")) {   // 数组赋值 A(i) = ...
                ++i;
                s->kind = Stmt::ASSIGN_ARR; s->name = name; s->e1 = std::move(a[0]);
                parse_expr(s->e2);
                return s;
            }
            s->kind = Stmt::CALL; s->name = name; s->line = ln;   // 命令 / 函数调用（可为 0 参）
            s->args = std::move(a);
            fixup_task_name(*s);                                  // RUNTASK n, 子程序名
            parse_cmd_modifiers(*s);                              // MOVEABS(x) AXIS(0)
            return s;
        }

        if (is_op("=")) {
            // rxnum = GET #port, ARRAY, n
            const Tok& t2 = t[std::min(i + 2, t.size() - 1)];
            if (nxt().k == TK::ID && upper(nxt().s) == "GET" &&
                t2.k == TK::OP && t2.s == "#") {
                ++i;                                     // =
                return parse_get(std::move(s), name);
            }
            ++i;
            s->kind = Stmt::ASSIGN; s->name = name;
            s->e1 = std::make_unique<Expr>();
            s->e1->kind = Expr::NUM; s->e1->num = 0; s->e1->line = ln;
            parse_expr(s->e2);
            return s;
        }

        // 无括号命令：NAME arg1, arg2  （MOVE 100, 200）
        s->kind = Stmt::CALL; s->name = name; s->line = ln;
        if (cur().k == TK::EOL || cur().k == TK::EOFT || is_op(":") ||
            cur().k == TK::ID) {
            // 后面没有参数（下一行或下一语句）
        } else {
            while (true) {
                ExprP a; parse_expr(a);
                s->args.push_back(std::move(a));
                if (is_op(",")) { ++i; continue; }
                break;
            }
        }
        fixup_task_name(*s);                              // RUNTASK n, 子程序名
        parse_cmd_modifiers(*s);                          // MOVEABS x AXIS(0)
        return s;
    }

    StmtP parse_print(StmtP s) {
        s->kind = Stmt::PRINT;
        if (is_op("#")) {                 // PRINT #端口, 输出项...
            ++i;
            parse_expr(s->port);
            if (is_op(",")) ++i;
            while (is_op(",")) ++i;       // 容错：端口后多余逗号
        }
        while (is_op(",")) ++i;           // ?,,：前导空输出项（只分隔、不输出）
        if (cur().k == TK::EOL || cur().k == TK::EOFT || is_op(":")) {
            s->print_newline = true;
            return s;
        }
        bool last_semi = false;
        while (true) {
            // ZBasic 风格：输出项可相邻书写（"文本"变量），逐项直接拼接
            const size_t before = i;
            ExprP e; parse_expr(e);
            s->args.push_back(std::move(e));
            if (i == before && cur().k != TK::EOL && cur().k != TK::EOFT) ++i;   // 防死循环
            if (is_op(";"))       { ++i; last_semi = true;  }
            else if (is_op(","))  { ++i; last_semi = false; while (is_op(",")) ++i; }  // 连续逗号=空项
            else                  { last_semi = false; }
            if (cur().k == TK::EOL || cur().k == TK::EOFT || is_op(":")) break;
        }
        s->print_newline = !last_semi;
        return s;
    }
};

// ===========================================================================
// 解释执行
// ===========================================================================
namespace {

// F_BREAK = EXIT 跳出本层循环；F_SUB_RET = RETURN/EXIT SUB 结束本层子程序
enum Flow { F_NORMAL = 0, F_JUMP = 1, F_STOP = 2, F_BREAK = 3, F_SUB_RET = 4, F_RETURN = 5 };

// 内置函数：返回 true 表示已处理
bool eval_builtin(const std::string& name, const std::vector<Value>& a, Value* ret,
                  std::string* err) {
    auto need = [&](size_t n) { return a.size() >= n; };
    auto num = [&](size_t k) { return a[k].to_num(); };

    if (name == "ABS"  && need(1)) { *ret = Value::number(std::fabs(num(0))); return true; }
    if (name == "INT"  && need(1)) { *ret = Value::number(std::floor(num(0))); return true; }
    if (name == "SGN"  && need(1)) { *ret = Value::number(num(0) > 0 ? 1 : (num(0) < 0 ? -1 : 0)); return true; }
    if (name == "SQR") {
        if (!need(1)) { *err = "SQR 需要 1 个参数"; return true; }
        if (num(0) < 0) { *err = "SQR 参数为负"; return true; }
        *ret = Value::number(std::sqrt(num(0))); return true;
    }
    if (name == "SIN"  && need(1)) { *ret = Value::number(std::sin(num(0))); return true; }
    if (name == "COS"  && need(1)) { *ret = Value::number(std::cos(num(0))); return true; }
    if (name == "TAN"  && need(1)) { *ret = Value::number(std::tan(num(0))); return true; }
    if (name == "MIN"  && need(1)) {
        double v = num(0);
        for (size_t k = 1; k < a.size(); ++k) v = std::min(v, num(k));
        *ret = Value::number(v); return true;
    }
    if (name == "MAX"  && need(1)) {
        double v = num(0);
        for (size_t k = 1; k < a.size(); ++k) v = std::max(v, num(k));
        *ret = Value::number(v); return true;
    }
    if ((name == "LEN" || name == "STRLEN") && need(1)) { *ret = Value::number((double)a[0].to_text().size()); return true; }
    if (name == "VAL"  && need(1)) { *ret = Value::number(a[0].to_num()); return true; }
    if (name == "STR"  && need(1)) { *ret = Value::text(a[0].to_text()); return true; }
    if (name == "STRFIND" && need(2)) {                          // 返回 0 起下标；未命中 = -1
        const std::string hay = a[0].to_text();
        const std::string ndl = a[1].to_text();
        long long from = (a.size() > 2) ? (long long)num(2) : 0;
        if (from < 0) from = 0;
        if (ndl.empty() || from > (long long)hay.size()) { *ret = Value::number(-1); return true; }
        const size_t p = hay.find(ndl, (size_t)from);
        *ret = Value::number(p == std::string::npos ? -1 : (double)p);
        return true;
    }
    if (name == "STRCOMP" && need(2)) {                          // 相等 = 0（与原程序用法一致）
        const int c = a[0].to_text().compare(a[1].to_text());
        *ret = Value::number(c < 0 ? -1 : (c > 0 ? 1 : 0));
        return true;
    }
    if (name == "TOSTR" && need(1)) {                            // TOSTR(值[, 总宽, 小数位])
        const double v = num(0);
        const int w = (a.size() > 1) ? (int)num(1) : 0;
        const int d = (a.size() > 2) ? (int)num(2) : 0;
        char buf[160];
        if (w > 0) std::snprintf(buf, sizeof(buf), "%*.*f", w, d < 0 ? 0 : d, v);
        else       std::snprintf(buf, sizeof(buf), "%.*f", d < 0 ? 0 : d, v);
        *ret = Value::text(buf);
        return true;
    }
    if (name == "HEX" && need(1)) {                              // 大写十六进制，字节对齐（HEX(9)="09"）
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llX", (unsigned long long)(long long)num(0));
        std::string s = buf;
        if (s.size() % 2) s = "0" + s;
        *ret = Value::text(s);
        return true;
    }
    if (name == "CHR" && need(1)) { *ret = Value::text(std::string(1, (char)((long long)num(0) & 0xFF))); return true; }
    if (name == "ASC" && need(1)) {
        const std::string s = a[0].to_text();
        *ret = Value::number(s.empty() ? 0 : (double)(unsigned char)s[0]);
        return true;
    }
    if (name == "ATAN" && need(1)) { *ret = Value::number(std::atan(num(0))); return true; }
    return false;
}

} // namespace

struct ScriptEngine::Impl {
    ScriptHost*                   host = nullptr;
    std::vector<StmtP>            program;
    std::unordered_map<std::string, size_t> labels;
    std::map<std::string, Value>  vars;
    std::unordered_map<std::string, Value> consts;       // GLOBAL CONST 常量表（编译期收集）
    std::unordered_set<std::string>        array_names;  // DIM x(n)：数组，亦可当 0 结尾字符串
    std::string                   error;
    int                           error_line = 0;
    ScriptEngine::Status          status = ScriptEngine::Status::READY;
    unsigned long long            steps = 0;
    unsigned long long            budget = 0;       // 0 = 不限（run() 会传默认值）
    bool                          budget_hit = false;
    std::atomic<bool>             abort_req{false}; // 跨线程置位（request_abort / D5 挂起唤醒）
    bool                          trace_on  = false;
    size_t                        pending_goto = 0;
    std::string                   pending_goto_name;   // GOTO/GOSUB 目标名（由当前层标签表解析）
    bool                          pending_gosub = false; // true=本次跳转来自 GOSUB（需压栈返回地址）
    std::unordered_map<std::string, SubDef> subs;      // 子程序定义表
    int                           sub_depth = 0;       // 子程序递归深度（防爆栈）
    bool                          stop_req  = false;   // END 已执行
    std::string                   pline;            // PRINT 未换行时的挂起输出

    // ---- D5：断点 / 暂停 / 单步（协作式，语句边界生效；docs/planA/13 §5.2）----
    // dbg_mtx 保护以下全部字段；会话线程（request_* / paused / current_line）与
    // worker 线程（dbg_gate）都经它访问。挂起等待用 dbg_cv，abort 可随时打断。
    mutable std::mutex            dbg_mtx;
    std::condition_variable       dbg_cv;
    std::set<int>                 breakpoints;      // 断点行号集合
    bool                          pause_req   = false; // 外部请求暂停（下一条语句边界生效）
    bool                          step_req    = false; // 暂停中收到单步：恢复后走一条再停
    bool                          resume_req  = false; // 恢复信号（唤醒 dbg_cv）
    bool                          paused_flag = false; // 当前挂起中
    int                           cur_line    = 0;     // 当前执行行（current_line()）

    // D5 调试闸门：每条语句执行前调用。命中断点 / 暂停请求时挂起，直到
    // resume / step / abort。返回 false = 已被中止，调用方应立即返回。
    bool dbg_gate(int line) {
        {
            std::lock_guard<std::mutex> lk(dbg_mtx);
            cur_line = line;
            if (!pause_req && breakpoints.count(line) == 0) return true;   // 快速路径
            pause_req   = false;
            paused_flag = true;
        }
        std::unique_lock<std::mutex> lk(dbg_mtx);
        dbg_cv.wait(lk, [&] { return resume_req || abort_req.load(); });
        const bool stepping = step_req;
        step_req = resume_req = false;
        paused_flag = false;
        if (stepping) pause_req = true;    // 单步：本条执行完后，下一条语句边界再停
        return !abort_req.load();
    }

    // RETURN 系统变量：最近一次设备命令/函数的返回值。ZBasic 里 `SLOT_SCAN(0)` 后
    // 用 `IF RETURN THEN` 判成败，即读此值（非 0 = 真）。
    Value                         last_cmd_ret_;

    // TICKS：可赋值的「倒数」毫秒计数器（1 tick≈1ms）。
    // 赋值 N 后每过 1ms 自动减 1，故「先取值减后取值」= 已流逝 ms（t0 - TICKS）。
    long long                     tick_anchor_ms  = 0;
    double                        tick_anchor_val = 0.0;

    static long long wall_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    double ticks_now() const { return tick_anchor_val - (double)(wall_ms() - tick_anchor_ms); }
    void   set_ticks(double v) { tick_anchor_ms = wall_ms(); tick_anchor_val = v; }
    void   reset_ticks()       { set_ticks(0.0); }

    void seterr(const std::string& m, int line) {
        if (error.empty()) { error = m; error_line = line; }
    }

    // PRINT 末尾带 ';' 时不换行：先攒着，直到需要换行或脚本结束
    void flush_print() {
        if (!pline.empty() && host) { host->print(pline); pline.clear(); }
    }

    // 数组名当字符串读：元素 0..首个 0 之间的字节拼成字符串（ZBasic「数组即字符串」）
    // 这是原程序的核心数据模型：STRFIND(CMDSTR,",")、VAL(PSTR)、STRLEN(PSTR) 都依赖它
    std::string array_text(const std::string& n) {
        std::string s;
        for (long long k = 0; s.size() < 65536; ++k) {
            auto it = vars.find(n + "[" + std::to_string(k) + "]");
            const long long code = (it == vars.end()) ? 0 : (long long)it->second.to_num();
            if (code == 0) break;                 // 0 = 字符串结束
            s.push_back((char)(unsigned char)(code & 0xFF));
        }
        return s;
    }

    // 数组原始字节（不因 0 截断）：从 start 起取 count 个元素，低 8 位拼成字节串
    // 供「数组切片」BUF(起点,长度)、CRC16(数组,起点,数量)、PUTCHAR #p, BUF(0,n) 消费
    std::string array_bytes(const std::string& n, size_t start, size_t count) {
        std::string s;
        s.reserve(count);
        for (size_t k = 0; k < count && k < 65536; ++k) {
            auto it = vars.find(n + "[" + std::to_string(start + k) + "]");
            const long long code = (it == vars.end()) ? 0 : (long long)it->second.to_num();
            s.push_back((char)(unsigned char)(code & 0xFF));
        }
        return s;
    }

    // Modbus CRC16（初值 $FFFF，多项式 $A001 / 反射式）
    static unsigned crc16_modbus(const std::string& d) {
        unsigned crc = 0xFFFF;
        for (size_t k = 0; k < d.size(); ++k) {
            crc ^= (unsigned char)d[k];
            for (int b = 0; b < 8; ++b)
                crc = (crc & 1) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1);
        }
        return crc & 0xFFFFu;
    }

    Value& var_ref(const std::string& n) {
        auto it = vars.find(n);
        if (it == vars.end()) {
            Value v;                       // 未赋值变量默认 0
            if (n.size() > 0 && n[0] == '$') v = Value::text("");
            else                             v = Value::number(0);
            it = vars.emplace(n, v).first;
        }
        return it->second;
    }

    // ---- 表达式 ----
    Value eval(const Expr& e) {
        switch (e.kind) {
            case Expr::NUM: return Value::number(e.num);
            case Expr::STR: return Value::text(e.str);
            case Expr::VAR: {
                if (e.str == "RETURN") return last_cmd_ret_;                // 最近一次命令的返回值
                if (e.str == "TICKS") return Value::number(ticks_now());   // 倒计时计数器
                if (array_names.count(e.str)) return Value::text(array_text(e.str));  // 数组当字符串
                auto c = consts.find(e.str);
                if (c != consts.end()) return c->second;                   // 常量优先
                auto it = vars.find(e.str);
                if (it != vars.end()) return it->second;
                return (e.str[0] == '$') ? Value::text("") : Value::number(0);
            }
            case Expr::UN: {
                const Value v = eval(*e.a);
                // NOT 按位取反（与 ZBasic 一致，NOT 0 = -1）；逻辑取反请写 (x = 0)
                if (e.str == "NOT") return Value::number((double)~(long long)v.to_num());
                if (e.str == "@")   return Value::text(v.to_text());   // ?*X：强制按字符串打印
                return Value::number(-v.to_num());
            }
            case Expr::BIN: return eval_bin(e);
            case Expr::CALL: return eval_call(e);
        }
        return Value::number(0);
    }

    Value eval_bin(const Expr& e) {
        const std::string& op = e.str;
        // AND/OR/XOR/EQV 一律按位（ZBasic 一致）：布尔条件里 1 AND 1 = 1，与逻辑等价
        if (op == "AND" || op == "OR" || op == "XOR" || op == "EQV") {
            const long long a = (long long)eval(*e.a).to_num();
            const long long b = (long long)eval(*e.b).to_num();
            long long r = 0;
            if      (op == "AND") r = a & b;
            else if (op == "OR")  r = a | b;
            else if (op == "XOR") r = a ^ b;
            else                  r = ~(a ^ b);   // EQV = 按位同或
            return Value::number((double)r);
        }

        const Value lv = eval(*e.a);
        const Value rv = eval(*e.b);

        // 比较：两边都是字符串时按字典序
        if (op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            double r = 0;
            if (lv.type == Value::STR && rv.type == Value::STR) {
                const int c = lv.str.compare(rv.str);
                if (op == "=")  r = c == 0;
                else if (op == "<>") r = c != 0;
                else if (op == "<")  r = c < 0;
                else if (op == ">")  r = c > 0;
                else if (op == "<=") r = c <= 0;
                else                 r = c >= 0;
            } else {
                const double a = lv.to_num(), b = rv.to_num();
                if (op == "=")  r = a == b;
                else if (op == "<>") r = a != b;
                else if (op == "<")  r = a < b;
                else if (op == ">")  r = a > b;
                else if (op == "<=") r = a <= b;
                else                 r = a >= b;
            }
            return Value::number(r);
        }

        // 字符串拼接
        if (op == "+" && (lv.type == Value::STR || rv.type == Value::STR))
            return Value::text(lv.to_text() + rv.to_text());

        const double a = lv.to_num();
        const double b = rv.to_num();
        if (op == "+") return Value::number(a + b);
        if (op == "-") return Value::number(a - b);
        if (op == "*") return Value::number(a * b);
        if (op == "^") return Value::number(std::pow(a, b));
        if (op == "/") {
            if (b == 0.0) { seterr("除数为 0", e.line); return Value::number(0); }
            return Value::number(a / b);
        }
        if (op == "\\") {
            if (b == 0.0) { seterr("整除除数为 0", e.line); return Value::number(0); }
            return Value::number(std::floor(a / b));
        }
        if (op == "MOD") {
            if (b == 0.0) { seterr("MOD 除数为 0", e.line); return Value::number(0); }
            return Value::number(std::fmod(a, b));
        }
        seterr("未知运算符 " + op, e.line);
        return Value::number(0);
    }

    Value eval_call(const Expr& e) {
        // 数组切片：BUF(起始, 长度) —— 取原始字节拼成字符串（不因 0 截断）
        if (e.args.size() == 2 && array_names.count(e.str)) {
            const long long st = (long long)eval(*e.args[0]).to_num();
            const long long ln = (long long)eval(*e.args[1]).to_num();
            return Value::text(array_bytes(e.str, (size_t)(st < 0 ? 0 : st), (size_t)(ln < 0 ? 0 : ln)));
        }
        // 内置 CRC16(数组, 起点, 数量)：返回按「发送顺序」打包的值（高 8 位 = 先发的低字节）
        if (e.str == "CRC16" && e.args.size() >= 1) {
            std::string bytes;
            if (e.args[0]->kind == Expr::VAR && array_names.count(e.args[0]->str)) {
                const long long st = (e.args.size() >= 2) ? (long long)eval(*e.args[1]).to_num() : 0;
                const long long ln = (e.args.size() >= 3) ? (long long)eval(*e.args[2]).to_num()
                                                          : (long long)array_text(e.args[0]->str).size();
                bytes = array_bytes(e.args[0]->str, (size_t)(st < 0 ? 0 : st), (size_t)(ln < 0 ? 0 : ln));
            } else {
                bytes = eval(*e.args[0]).to_text();
                if (e.args.size() >= 3) {
                    const long long st = (long long)eval(*e.args[1]).to_num();
                    const long long ln = (long long)eval(*e.args[2]).to_num();
                    if (st > 0 || (size_t)(st + ln) <= bytes.size())
                        bytes = bytes.substr((size_t)(st < 0 ? 0 : st), (size_t)(ln < 0 ? 0 : ln));
                }
            }
            const unsigned crc = crc16_modbus(bytes);
            const unsigned packed = ((crc & 0xFFu) << 8) | ((crc >> 8) & 0xFFu);   // 与控制器一致
            return Value::number((double)packed);
        }

        std::vector<Value> args;
        args.reserve(e.args.size());
        for (const ExprP& a : e.args) args.push_back(eval(*a));

        Value r;
        std::string berr;
        if (eval_builtin(e.str, args, &r, &berr)) {
            if (!berr.empty()) { seterr(berr + "（" + e.str + "）", e.line); return Value::number(0); }
            return r;
        }

        if (!host) { seterr("没有宿主，无法执行命令 " + e.str, e.line); return Value::number(0); }

        std::string herr;
        Value ret;
        const int rc = host->call(e.str, args, &ret, &herr);
        if (rc == 0) { last_cmd_ret_ = ret; return ret; }   // 记入 RETURN 系统变量
        if (rc == 3) { abort_req = true; return Value::number(0); }
        if (rc == 1) {
            // 不是命令 -> 退化为数组元素（一维，下标不做边界检查）
            if (args.size() == 1)
                return var_ref(e.str + "[" + args[0].to_text() + "]");
            seterr("未知命令/函数 " + e.str + "（参数 " + std::to_string(args.size()) + " 个）", e.line);
            return Value::number(0);
        }
        seterr("命令 " + e.str + " 失败: " + (herr.empty() ? std::string("宿主返回错误") : herr), e.line);
        return Value::number(0);
    }

    void exec_assign(const Stmt& s, const std::string& key) {
        if (consts.count(key)) { seterr("常量 " + key + " 只读，不能赋值（GLOBAL CONST）", s.line); return; }
        const Value v = eval(*s.e2);
        if (key == "TICKS") { set_ticks(v.to_num()); return; }   // 给 TICKS 赋值 = 重装倒计数初值
        var_ref(key) = v;
    }

    // 编译期收集 GLOBAL CONST（含嵌套块），供表达式求值
    void collect_consts(const std::vector<StmtP>& blk) {
        for (const StmtP& s : blk) {
            if (s->kind == Stmt::CONST_DEF) {
                for (size_t k = 0; k < s->names.size() && k < s->args.size(); ++k)
                    consts[s->names[k]] = eval(*s->args[k]);
            } else if (s->kind == Stmt::IF) {
                for (const Stmt::Branch& br : s->branches) collect_consts(br.body);
                collect_consts(s->els);
            } else if (s->kind == Stmt::WHILE || s->kind == Stmt::FOR) {
                collect_consts(s->body);
            }
        }
    }

    // ---- 语句 ----
    void exec_stmt(const Stmt& s, Flow* flow) {
        ++steps;
        if (budget != 0 && steps > budget) { budget_hit = true; return; }
        // 每 1024 条语句给宿主一次叫停的机会（长循环也能被中止）
        if ((steps & 0x3FFULL) == 0 && host && host->aborted()) { abort_req = true; return; }
        // D5：断点命中 / 暂停 / 单步在此挂起（语句边界；被中止时直接退出）
        if (!dbg_gate(s.line)) return;
        if (trace_on && host) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[trace] L%d %s", s.line,
                          s.kind == Stmt::CALL ? s.name.c_str() :
                          s.kind == Stmt::LABEL ? "NOP" : "stmt");
            host->print(buf);
        }
        switch (s.kind) {
            case Stmt::LABEL: break;

            case Stmt::CONST_DEF: break;                   // 编译期已收集
            case Stmt::DIM_DEF: {
                for (size_t k = 0; k < s.names.size(); ++k) {
                    const std::string& n = s.names[k];
                    if (k < s.is_arr.size() && s.is_arr[k]) { array_names.insert(n); continue; }
                    if (vars.count(n) || array_names.count(n)) continue;
                    vars[n] = (!n.empty() && n.back() == '$') ? Value::text("") : Value::number(0);
                }
                break;
            }
            case Stmt::WAIT: {
                if (s.name == "IDLE") {                    // 等轴停止：交宿主轮询（带超时/中止）
                    if (!host) { seterr("没有宿主，无法执行 WAIT IDLE", s.line); return; }
                    Value r; std::string herr;
                    const int rc = host->call("WAITIDLE", {}, &r, &herr);
                    if (rc == 1) { seterr("宿主不支持 WAIT IDLE（等待轴停止）", s.line); return; }
                    if (rc == 2) { seterr("WAIT IDLE 失败: " + herr, s.line); return; }
                    if (rc == 3) abort_req = true;
                    break;
                }
                if (s.name == "DELAY") {                   // WAIT ms / WA ms：延时（交宿主，可被中止打断）
                    if (!host) { seterr("没有宿主，无法执行 WAIT 延时", s.line); return; }
                    double ms = eval(*s.e1).to_num();
                    if (ms < 0) ms = 0;
                    Value r; std::string herr;
                    const int rc = host->call("DELAY", {Value::number(ms)}, &r, &herr);
                    if (rc == 3) { abort_req = true; break; }
                    if (rc != 0) { seterr("WAIT/DELAY 失败: " + herr, s.line); return; }
                    break;
                }
                while (!eval(*s.e1).truthy()) {            // WAIT UNTIL/条件：引擎内轮询
                    ++steps;
                    if (budget != 0 && steps > budget) { budget_hit = true; return; }
                    if ((steps & 0x3FFULL) == 0 && host && host->aborted()) { abort_req = true; return; }
                    if (!error.empty()) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                break;
            }
            // EXIT FOR/WHILE 跳出本层循环；EXIT SUB 结束本层子程序
            case Stmt::EXIT: *flow = (s.name == "SUB") ? F_SUB_RET : F_BREAK; break;

            case Stmt::ASSIGN:     exec_assign(s, s.name); break;
            case Stmt::ASSIGN_ARR: {
                const Value idx = eval(*s.e1);
                // 非 DIM 数组名：先试「设备参数写入」（SPEED(0)=100 / ATYPE(0)=65 …），
                // 宿主不认识(1)时再退回普通一维数组元素，保持既有行为
                if (!array_names.count(s.name) && host) {
                    const Value val = eval(*s.e2);
                    Value r; std::string herr;
                    const int rc = host->call(s.name, {idx, val}, &r, &herr);
                    if (rc == 0) break;
                    if (rc == 3) { abort_req = true; break; }
                    if (rc == 2) { seterr("命令 " + s.name + " 失败: " + herr, s.line); return; }
                    var_ref(s.name + "[" + idx.to_text() + "]") = val;   // rc == 1
                    break;
                }
                exec_assign(s, s.name + "[" + idx.to_text() + "]");
                break;
            }
            case Stmt::PRINT: {
                if (s.port) {                                  // PRINT #端口, ...：一次性整包发出（不附加换行）
                    if (!host) { seterr("没有宿主，无法执行 PRINT #端口", s.line); return; }
                    std::string text;
                    for (size_t k = 0; k < s.args.size(); ++k) text += eval(*s.args[k]).to_text();
                    Value r; std::string herr;
                    const int rc = host->call("PRINT", {eval(*s.port), Value::text(text)}, &r, &herr);
                    if (rc == 1) { seterr("宿主不支持 PRINT #端口", s.line); return; }
                    if (rc == 2) { seterr("PRINT #端口 失败: " + herr, s.line); return; }
                    if (rc == 3) abort_req = true;
                    break;
                }
                for (size_t k = 0; k < s.args.size(); ++k)
                    pline += eval(*s.args[k]).to_text();
                if (s.print_newline) flush_print();
                break;
            }
            case Stmt::GET: {                                  // GET #端口, 数组[(偏移)] [, 字节数]：非阻塞接收
                if (!host) { seterr("没有宿主，无法执行 GET", s.line); return; }
                const size_t base = s.e1 ? (size_t)(eval(*s.e1).to_num() < 0 ? 0 : eval(*s.e1).to_num()) : 0;
                std::vector<Value> args;
                args.push_back(s.port ? eval(*s.port) : Value::number(0));
                args.push_back(Value::text(s.name));           // 目标数组名（供宿主参考/日志）
                args.push_back(Value::number((double)base));   // 写入起始偏移
                if (s.e2) args.push_back(eval(*s.e2));         // 最大字节数（可省）
                Value r; std::string herr;
                const int rc = host->call("GET", args, &r, &herr);
                if (rc == 1) { seterr("宿主不支持 GET（端口接收）", s.line); return; }
                if (rc == 2) { seterr("GET 失败: " + herr, s.line); return; }
                if (rc == 3) { abort_req = true; break; }
                size_t n = 0;
                if (r.type == Value::STR) {                    // 宿主回传字节串：引擎写入数组并补 0 结尾
                    n = r.str.size();
                    for (size_t k = 0; k < n; ++k)
                        var_ref(s.name + "[" + std::to_string(base + k) + "]") =
                            Value::number((double)(unsigned char)r.str[k]);
                    var_ref(s.name + "[" + std::to_string(base + n) + "]") = Value::number(0);
                    array_names.insert(s.name);
                } else {
                    n = (size_t)(r.to_num() < 0 ? 0 : r.to_num());   // 宿主自行写入数组，仅回传长度
                    if (n > 0) array_names.insert(s.name);
                }
                if (!s.target.empty()) var_ref(s.target) = Value::number((double)n);
                break;
            }
            case Stmt::IF: {
                bool hit = false;
                for (const Stmt::Branch& br : s.branches) {
                    if (eval(*br.cond).truthy()) { exec_block(br.body, flow); hit = true; break; }
                }
                if (!hit) exec_block(s.els, flow);
                break;
            }
            case Stmt::WHILE: {
                while (eval(*s.e1).truthy()) {
                    exec_block(s.body, flow);
                    if (*flow == F_BREAK) { *flow = F_NORMAL; break; }         // EXIT WHILE
                    if (*flow != F_NORMAL || budget_hit || abort_req || !error.empty()) return;
                }
                break;
            }
            case Stmt::FOR: {
                const double start = eval(*s.e1).to_num();
                const double end   = eval(*s.e2).to_num();
                const double step  = s.e3 ? eval(*s.e3).to_num() : 1.0;
                if (step == 0.0) { seterr("FOR STEP 为 0", s.line); return; }
                double v = start;
                while (step > 0 ? (v <= end) : (v >= end)) {
                    var_ref(s.name) = Value::number(v);
                    exec_block(s.body, flow);
                    if (*flow == F_BREAK) { *flow = F_NORMAL; break; }         // EXIT FOR
                    if (*flow != F_NORMAL || budget_hit || abort_req || !error.empty()) return;
                    v += step;
                }
                break;
            }
            case Stmt::CALL: {
                std::vector<Value> args;
                args.reserve(s.args.size());
                for (const ExprP& a : s.args) args.push_back(eval(*a));
                auto sub = subs.find(s.name);                          // 1) 子程序
                if (sub != subs.end()) { call_sub(sub->second, args, s.line); break; }
                Value r;
                std::string berr;
                if (eval_builtin(s.name, args, &r, &berr)) break;      // 2) 语句位置调用内置函数：忽略结果
                if (!host) { seterr("没有宿主，无法执行命令 " + s.name, s.line); return; }
                if (s.axis) {                                          // 修饰符 AXIS(n)：等效先 BASE(n)
                    Value ar; std::string aerr;
                    const int arc = host->call("AXIS", {eval(*s.axis)}, &ar, &aerr);
                    if (arc == 3) { abort_req = true; break; }
                }
                std::string herr;
                const int rc = host->call(s.name, args, &r, &herr);
                if (rc == 0) last_cmd_ret_ = r;                        // 记入 RETURN 系统变量
                if (rc == 1) { seterr("未知命令 " + s.name, s.line); return; }
                if (rc == 2) { seterr("命令 " + s.name + " 失败: " + herr, s.line); return; }
                if (rc == 3) { abort_req = true; break; }
                break;
            }
            case Stmt::GOTO: {
                pending_goto_name = s.name;      // 由 exec_program 在「当前层」解析并跳转
                pending_gosub     = false;
                *flow = F_JUMP;
                break;
            }
            case Stmt::GOSUB: {                  // 跳转并在当前层压入返回地址（RETURN 回到下一条）
                pending_goto_name = s.name;
                pending_gosub     = true;
                *flow = F_JUMP;
                break;
            }
            case Stmt::END:       *flow = F_STOP;    break;
            case Stmt::LOCAL_DEF: break;             // 作用域由 call_sub 统一管理
            // RETURN：GOSUB 返回时由 exec_program 弹栈回到下一条；否则提前结束本层子程序
            case Stmt::RETURN:    *flow = F_RETURN;  break;
        }
    }

    void exec_block(const std::vector<StmtP>& blk, Flow* flow) {
        for (const StmtP& st : blk) {
            exec_stmt(*st, flow);
            if (*flow != F_NORMAL || budget_hit || abort_req || !error.empty()) return;
        }
    }

    // 执行一段语句序列（顶层或子程序体）；标签在当前层内解析
    void exec_program(const std::vector<StmtP>& prog,
                      const std::unordered_map<std::string, size_t>& lbls) {
        std::vector<size_t> gosub_stack;   // 本层的 GOSUB 返回地址栈（不跨层）
        size_t pc = 0;
        while (pc < prog.size()) {
            if (abort_req) return;
            Flow flow = F_NORMAL;
            exec_stmt(*prog[pc], &flow);
            if (budget_hit || abort_req || stop_req || !error.empty()) return;
            if (flow == F_SUB_RET) return;                 // EXIT SUB：无条件结束本层
            if (flow == F_STOP) { stop_req = true; return; }   // END：结束整个脚本
            if (flow == F_RETURN) {
                if (!gosub_stack.empty()) {                // GOSUB 返回：回到调用点的下一条
                    pc = gosub_stack.back();
                    gosub_stack.pop_back();
                    continue;
                }
                if (sub_depth > 0) return;                 // 子程序内提前返回（等价 EXIT SUB）
                seterr("RETURN 出现在子程序/GOSUB 之外", prog[pc]->line);
                return;
            }
            if (flow == F_BREAK) { seterr("EXIT 出现在循环之外", prog[pc]->line); return; }
            if (flow == F_JUMP) {
                auto it = lbls.find(pending_goto_name);
                if (it == lbls.end()) {
                    seterr("未定义的标号 " + pending_goto_name, prog[pc]->line);
                    return;
                }
                if (pending_gosub) gosub_stack.push_back(pc + 1);   // GOSUB：记录返回地址
                pc = it->second;
                continue;
            }
            ++pc;
        }
    }

    // 调用子程序：形参按值传入；形参 + LOCAL 在调用期间遮蔽同名全局变量
    // （原程序用全局变量与 SUB 名通信，如 get_num 写 numval）
    void call_sub(const SubDef& d, const std::vector<Value>& args, int line) {
        if (sub_depth >= 32) { seterr("子程序递归层数过深（>32）: " + d.name, line); return; }
        if (args.size() != d.params.size()) {
            seterr("子程序 " + d.name + " 需要 " + std::to_string(d.params.size()) +
                   " 个参数，实到 " + std::to_string(args.size()) + " 个", line);
            return;
        }
        struct Saved { std::string n; Value v; bool had; };
        std::vector<Saved> saved;
        saved.reserve(d.locals.size());
        for (const std::string& n : d.locals) {
            auto it = vars.find(n);
            if (it != vars.end()) { saved.push_back(Saved{n, it->second, true}); vars.erase(it); }
            else                  { saved.push_back(Saved{n, Value(),   false}); }
        }
        for (size_t k = 0; k < d.params.size(); ++k) vars[d.params[k]] = args[k];

        ++sub_depth;
        exec_program(d.body, d.labels);
        --sub_depth;

        for (const Saved& sv : saved) {
            if (sv.had) vars[sv.n] = sv.v;
            else        vars.erase(sv.n);
        }
    }
};

} // namespace kx

// ===========================================================================
// 公共 API
// ===========================================================================
namespace kx {

ScriptEngine::ScriptEngine(ScriptHost* host) : p_(new Impl()) { p_->host = host; }
ScriptEngine::~ScriptEngine() = default;
void ScriptEngine::set_host(ScriptHost* host) { p_->host = host; }

void ScriptEngine::request_abort() {
    // 持 dbg_mtx 置位并唤醒：脚本可能正挂在 D5 调试闸门（dbg_cv）上，
    // 不 notify 会永远睡死（wait 谓词只在被唤醒时才复查）。
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->abort_req = true;
    p_->dbg_cv.notify_all();
}

bool ScriptEngine::compile(const std::string& src, std::string* err) {
    p_->program.clear();
    p_->labels.clear();
    p_->error.clear();
    p_->error_line = 0;
    p_->steps = 0;
    p_->budget_hit = false;
    p_->abort_req = false;
    p_->pending_goto = 0;
    p_->pline.clear();
    p_->status = Status::READY;

    Lexer lx(src);
    if (!lx.run()) {
        p_->error = lx.err;
        p_->error_line = lx.err_line;
        p_->status = Status::COMPILE_ERROR;
        if (err) *err = "第 " + std::to_string(lx.err_line) + " 行: " + lx.err;
        return false;
    }

    Parser ps(lx.out);
    std::string stop;
    ps.parse_block(p_->program, {}, &stop);
    if (!ps.err.empty()) {
        p_->error = ps.err;
        p_->error_line = ps.err_line;
        p_->status = Status::COMPILE_ERROR;
        if (err) *err = "第 " + std::to_string(ps.err_line) + " 行: " + ps.err;
        return false;
    }

    // 常量（编译期收集）/ 标号 -> 顶层语句下标
    p_->subs = std::move(ps.subs);
    p_->consts.clear();
    p_->array_names.clear();
    p_->collect_consts(p_->program);
    for (const auto& kv : p_->subs) p_->collect_consts(kv.second.body);   // 子程序内的 CONST 也生效
    for (size_t k = 0; k < p_->program.size(); ++k) {
        const Stmt& s = *p_->program[k];
        if (s.kind == Stmt::LABEL && !s.name.empty()) p_->labels[s.name] = k;
    }
    return true;
}

ScriptEngine::Status ScriptEngine::run(unsigned long long max_steps) {
    if (p_->status == Status::COMPILE_ERROR) return p_->status;
    p_->error.clear();
    p_->error_line = 0;
    p_->steps = 0;
    p_->budget_hit = false;
    p_->budget = max_steps;
    p_->pline.clear();
    {
        std::lock_guard<std::mutex> lk(p_->dbg_mtx);   // 新一轮运行从干净调试态开始
        p_->pause_req = false;
        p_->step_req = false;
        p_->resume_req = false;
        p_->paused_flag = false;
    }
    p_->reset_ticks();
    p_->last_cmd_ret_ = Value::number(0);   // RETURN 系统变量每轮从 0 起
    p_->status = Status::READY;

    // 终止收尾：刷出挂起输出，并清掉本次的中止请求（run 开始前预置的 request_abort 会被此循环消费）
    auto finish = [&](Status st) { p_->flush_print(); p_->abort_req = false; p_->status = st; return st; };

    p_->stop_req = false;
    p_->exec_program(p_->program, p_->labels);
    if (p_->budget_hit)     return finish(Status::BUDGET_EXCEEDED);
    if (p_->abort_req)      return finish(Status::ABORTED);
    if (!p_->error.empty()) return finish(Status::RUNTIME_ERROR);
    return finish(Status::DONE);
}

ScriptEngine::Status ScriptEngine::status() const { return p_->status; }
const std::string& ScriptEngine::error() const { return p_->error; }
int ScriptEngine::error_line() const { return p_->error_line; }
unsigned long long ScriptEngine::steps() const { return p_->steps; }

Value ScriptEngine::get_var(const std::string& name) const {
    auto it = p_->vars.find(upper(name));
    return it == p_->vars.end() ? Value() : it->second;
}

void ScriptEngine::set_var(const std::string& name, const Value& v) {
    p_->vars[upper(name)] = v;
}

std::vector<std::string> ScriptEngine::list_vars() const {
    std::vector<std::string> out;
    for (const auto& kv : p_->vars)
        out.push_back(kv.first + " = " + kv.second.to_text());
    return out;
}

void ScriptEngine::clear_vars() { p_->vars.clear(); }

void ScriptEngine::set_trace(bool on) { p_->trace_on = on; }
bool ScriptEngine::trace() const { return p_->trace_on; }

std::vector<std::string> ScriptEngine::labels() const {
    std::vector<std::string> out;
    for (const auto& kv : p_->labels) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- D5：断点 / 暂停 / 单步（协作式，语句边界生效）----
void ScriptEngine::add_breakpoint(int line) {
    if (line <= 0) return;
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->breakpoints.insert(line);
}

void ScriptEngine::remove_breakpoint(int line) {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->breakpoints.erase(line);
}

void ScriptEngine::clear_breakpoints() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->breakpoints.clear();
}

std::vector<int> ScriptEngine::breakpoints() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return std::vector<int>(p_->breakpoints.begin(), p_->breakpoints.end());
}

void ScriptEngine::request_pause() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    p_->pause_req = true;              // 运行中 → 下一条语句边界生效；未运行 → run() 开始时清除
}

void ScriptEngine::request_step() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    if (!p_->paused_flag) return;      // 仅暂停中有效
    p_->step_req   = true;
    p_->resume_req = true;
    p_->dbg_cv.notify_all();
}

void ScriptEngine::resume() {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    if (!p_->paused_flag) return;
    p_->step_req   = false;
    p_->resume_req = true;
    p_->dbg_cv.notify_all();
}

bool ScriptEngine::paused() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return p_->paused_flag;
}

int ScriptEngine::current_line() const {
    std::lock_guard<std::mutex> lk(p_->dbg_mtx);
    return p_->cur_line;
}

// 状态名统一由 IScriptEngine::status_name 提供（见 engine.h），此文件不再单独定义。

} // namespace kx
