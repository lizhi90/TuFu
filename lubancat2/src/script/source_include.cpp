#include "script/source_include.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "script/boot_config.h"   // valid_script_file_name / script_lang_of_name

namespace kx {

namespace {

constexpr int kMaxDepth = 8;

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool ieq_prefix(const std::string& s, size_t pos, const char* kw, size_t* end) {
    const size_t n = std::char_traits<char>::length(kw);
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char)s[pos + i]) != (unsigned char)kw[i]) return false;
    }
    *end = pos + n;
    return true;
}

// 解析一行是否是指令；是指令则输出被包含文件名
bool parse_directive(const std::string& raw, const std::string& lang, std::string* name) {
    const std::string line = trim(raw);
    size_t p = 0;
    if (lang == "basic") {
        size_t after_kw = 0;
        if (!ieq_prefix(line, p, "include", &after_kw)) return false;
        p = after_kw;
        if (p < line.size() && !std::isspace((unsigned char)line[p]) && line[p] != '"') {
            return false;   // 形如 INCLUDEX 的标识符
        }
    } else {
        size_t after_kw = 0;
        if (!ieq_prefix(line, p, "include", &after_kw)) return false;
        p = after_kw;
        if (p < line.size() && !std::isspace((unsigned char)line[p]) && line[p] != '(' && line[p] != '"') {
            return false;
        }
    }
    while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
    if (lang == "lua" && p < line.size() && line[p] == '(') {
        ++p;
        while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
    }
    if (p >= line.size() || line[p] != '"') return false;
    const size_t q1 = p;
    const size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    *name = line.substr(q1 + 1, q2 - q1 - 1);
    size_t rest = q2 + 1;
    while (rest < line.size() && std::isspace((unsigned char)line[rest])) ++rest;
    if (lang == "lua" && rest < line.size() && line[rest] == ')') {
        ++rest;
        while (rest < line.size() && std::isspace((unsigned char)line[rest])) ++rest;
    }
    if (rest >= line.size()) return true;                       // 行尾
    if (lang == "basic" && line[rest] == '\'') return true;     // BASIC 注释
    if (lang == "lua" && line.compare(rest, 2, "--") == 0) return true;
    return false;                                               // 后面还有别的内容 → 不按指令处理
}

struct Expander {
    std::string dir;
    std::string lang;
    std::string out;
    std::string err;
    std::vector<std::string> stack;   // 当前展开链（文件名）

    bool expand(const std::string& src, const std::string& from) {
        std::istringstream in(src);
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            std::string name;
            if (parse_directive(line, lang, &name)) {
                if (!expand_file(name, from, lineno)) return false;
            } else {
                out += line;
                out += '\n';
            }
        }
        return true;
    }

    bool expand_file(const std::string& name, const std::string& from, int lineno) {
        const std::string where = from + ":" + std::to_string(lineno);
        if (!valid_script_file_name(name)) {
            err = where + ": INCLUDE 只允许纯文件名（字母/数字/._-，不以 . 开头）：" + name;
            return false;
        }
        const std::string lang_of = script_lang_of_name(name);
        if (lang_of.empty() || lang_of != lang) {
            err = where + ": 被包含文件必须是同语言脚本（当前 " + lang + "）：" + name;
            return false;
        }
        if (std::find(stack.begin(), stack.end(), name) != stack.end()) {
            std::string chain;
            for (const auto& s : stack) chain += s + " -> ";
            chain += name;
            err = where + ": 循环 INCLUDE：" + chain;
            return false;
        }
        if ((int)stack.size() >= kMaxDepth) {
            err = where + ": INCLUDE 嵌套超过 " + std::to_string(kMaxDepth) + " 层：" + name;
            return false;
        }
        std::ifstream f(join(dir, name), std::ios::binary);
        if (!f) {
            err = where + ": 无法读取被包含文件：" + name;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        out += (lang == "lua") ? "-- ===== include " : "' ===== INCLUDE ";
        out += name;
        out += " =====\n";
        stack.push_back(name);
        const bool ok = expand(ss.str(), name);
        stack.pop_back();
        return ok;
    }
};

} // namespace

bool expand_source_includes(const std::string& src, const std::string& dir,
                            const std::string& lang, const std::string& from,
                            std::string* out, std::string* err) {
    Expander ex;
    ex.dir  = dir;
    ex.lang = lang;
    if (!ex.expand(src, from)) {
        if (err) *err = ex.err;
        return false;
    }
    if (out) *out = ex.out;
    return true;
}

} // namespace kx
