// source_include_test.cpp —— 多文件包含展开（INCLUDE / include）的纯逻辑自测
// 用例：透传 / BASIC 与 Lua 指令 / 嵌套 / 循环 / 缺文件 / 跨语言 / 路径穿越 / 超深
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "script/source_include.h"

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_total;                                                         \
        if (!(cond)) {                                                     \
            ++g_fail;                                                      \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

static const char* kDir = "/tmp/kx_include_test";

static void write_file(const std::string& name, const std::string& content) {
    std::ofstream f(std::string(kDir) + "/" + name, std::ios::binary | std::ios::trunc);
    f << content;
}

static bool expand(const std::string& src, const std::string& lang, std::string* out,
                   std::string* err) {
    return kx::expand_source_includes(src, kDir, lang, "main", out, err);
}

int main() {
    if (::system(("rm -rf " + std::string(kDir)).c_str()) != 0) { /* 不存在也继续 */ }
    if (::system(("mkdir -p " + std::string(kDir)).c_str()) != 0) {
        std::printf("[source_include_test] 无法创建测试目录\n");
        return 2;
    }

    std::printf("== 1) BASIC：透传与展开 ==\n");
    {
        std::string out, err;
        CHECK(expand("A = 1\nEND\n", "basic", &out, &err));
        CHECK(out == "A = 1\nEND\n");           // 无指令 → 原样

        write_file("sub.bas", "SUB twice(x)\n  RETURN x * 2\nEND SUB\n");
        CHECK(expand("INCLUDE \"sub.bas\"\nA = twice(21)\nEND\n", "basic", &out, &err));
        CHECK(out.find("INCLUDE sub.bas") != std::string::npos);   // 展开标记
        CHECK(out.find("SUB twice(x)") != std::string::npos);
        CHECK(out.find("A = twice(21)") != std::string::npos);
    }

    std::printf("== 2) BASIC：嵌套 / 注释 / 非指令行 ==\n");
    {
        std::string out, err;
        write_file("inner.bas", "B = 2\n");
        write_file("outer.bas", "INCLUDE \"inner.bas\"\nA = 1\n");
        CHECK(expand("include \"outer.bas\" ' 嵌套\n", "basic", &out, &err));
        CHECK(out.find("B = 2") != std::string::npos && out.find("A = 1") != std::string::npos);

        std::string out2, err2;
        CHECK(expand("INCLUDE \"sub.bas\", 1\n", "basic", &out2, &err2));
        CHECK(out2 == "INCLUDE \"sub.bas\", 1\n");   // 后面还有内容 → 不按指令处理
        CHECK(expand("' INCLUDE \"sub.bas\"\n", "basic", &out2, &err2));
        CHECK(out2 == "' INCLUDE \"sub.bas\"\n");    // 注释行不动
    }

    std::printf("== 3) BASIC：错误路径 ==\n");
    {
        std::string out, err;
        CHECK(!expand("INCLUDE \"missing.bas\"\n", "basic", &out, &err));
        CHECK(err.find("无法读取") != std::string::npos);

        write_file("lib.lua", "-- x\n");
        CHECK(!expand("INCLUDE \"lib.lua\"\n", "basic", &out, &err));   // 跨语言
        CHECK(err.find("同语言") != std::string::npos);

        CHECK(!expand("INCLUDE \"../x.bas\"\n", "basic", &out, &err));  // 路径穿越
        CHECK(!expand("INCLUDE \"a/b.bas\"\n", "basic", &out, &err));

        write_file("a.bas", "INCLUDE \"b.bas\"\n");
        write_file("b.bas", "INCLUDE \"a.bas\"\n");
        CHECK(!expand("INCLUDE \"a.bas\"\n", "basic", &out, &err));     // 循环
        CHECK(err.find("循环") != std::string::npos);
    }

    std::printf("== 4) BASIC：嵌套超过 8 层 ==\n");
    {
        for (int i = 0; i < 10; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "d%d.bas", i);
            std::string next;
            if (i < 9) {
                char n2[32];
                std::snprintf(n2, sizeof(n2), "d%d.bas", i + 1);
                next = std::string("INCLUDE \"") + n2 + "\"\n";
            } else {
                next = "X = 1\n";
            }
            write_file(name, next);
        }
        std::string out, err;
        CHECK(!expand("INCLUDE \"d0.bas\"\n", "basic", &out, &err));
        CHECK(err.find("嵌套") != std::string::npos);
    }

    std::printf("== 5) Lua：include 两种写法 ==\n");
    {
        std::string out, err;
        write_file("sub.lua", "function twice(x) return x * 2 end\n");
        CHECK(expand("include(\"sub.lua\")\nprint(twice(21))\n", "lua", &out, &err));
        CHECK(out.find("function twice") != std::string::npos);
        CHECK(out.find("include sub.lua") != std::string::npos);

        std::string out2, err2;
        CHECK(expand("include \"sub.lua\" -- 无括号\n", "lua", &out2, &err2));
        CHECK(out2.find("function twice") != std::string::npos);

        std::string out3, err3;
        CHECK(!expand("include(\"sub.bas\")\n", "lua", &out3, &err3));   // 跨语言
        CHECK(!expand("include(\"gone.lua\")\n", "lua", &out3, &err3));
    }

    std::printf("[source_include_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
