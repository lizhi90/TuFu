// boot_config_test.cpp —— D8 主文件清单（.boot）与目录语言扫描的纯逻辑自测
// 用例：文件名白名单 / 目录语言状态 / 清单读写（含原子写与清除）
//       / 有效性校验（不存在、扩展名非法、目录混合）
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "script/boot_config.h"

using namespace kx;

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

static const char* kDir = "/tmp/kx_boot_test";

static void write_file(const std::string& name, const std::string& content) {
    std::ofstream f(std::string(kDir) + "/" + name, std::ios::binary | std::ios::trunc);
    f << content;
}

int main() {
    if (::system(("rm -rf " + std::string(kDir)).c_str()) != 0) { /* 不存在也继续 */ }
    if (::system(("mkdir -p " + std::string(kDir)).c_str()) != 0) {
        std::printf("[boot_config_test] 无法创建测试目录\n");
        return 2;
    }

    std::printf("== 1) 文件名白名单 ==\n");
    CHECK(valid_script_file_name("main.bas"));
    CHECK(valid_script_file_name("sub_1-lib.lua"));
    CHECK(!valid_script_file_name(""));
    CHECK(!valid_script_file_name(".boot"));
    CHECK(!valid_script_file_name("../x.bas"));
    CHECK(!valid_script_file_name("a/b.bas"));
    CHECK(!valid_script_file_name("bad name.bas"));

    std::printf("== 2) 语言判定与目录扫描 ==\n");
    CHECK(script_lang_of_name("a.bas") == "basic" && script_lang_of_name("b.lua") == "lua");
    CHECK(script_lang_of_name("c.txt").empty());
    CHECK(scan_script_dir_lang(kDir) == ScriptDirLang::NONE);
    write_file("sub.bas", "' sub\n");
    CHECK(scan_script_dir_lang(kDir) == ScriptDirLang::BASIC);
    write_file("lib.lua", "-- lib\n");
    CHECK(scan_script_dir_lang(kDir) == ScriptDirLang::MIXED);
    CHECK(::system(("rm -f " + std::string(kDir) + "/lib.lua").c_str()) == 0);

    std::printf("== 3) 清单读写与有效性 ==\n");
    CHECK(read_boot_name(kDir).empty());
    BootInfo none = read_boot_info(kDir);
    CHECK(none.name.empty() && none.valid);

    // 文件不存在 → 可以写入清单，但校验为 invalid（读时明确原因）
    std::string err;
    CHECK(write_boot_name(kDir, "missing.bas", &err));
    BootInfo miss = read_boot_info(kDir);
    CHECK(miss.name == "missing.bas" && !miss.valid && !miss.reason.empty());

    write_file("main.bas", "' main\n");
    CHECK(write_boot_name(kDir, "main.bas", &err));
    BootInfo ok = read_boot_info(kDir);
    CHECK(ok.name == "main.bas" && ok.valid);
    CHECK(read_boot_name(kDir) == "main.bas");

    // 扩展名不可判语言 → invalid
    write_file("plain.txt", "x\n");
    CHECK(write_boot_name(kDir, "plain.txt", &err));
    BootInfo bad_ext = read_boot_info(kDir);
    CHECK(bad_ext.name == "plain.txt" && !bad_ext.valid);

    // 目录混合 → invalid（语言不唯一）
    write_file("lib.lua", "-- lib\n");
    CHECK(write_boot_name(kDir, "main.bas", &err));
    BootInfo mixed = read_boot_info(kDir);
    CHECK(mixed.name == "main.bas" && !mixed.valid && mixed.reason.find("同时存在") != std::string::npos);
    CHECK(::system(("rm -f " + std::string(kDir) + "/lib.lua " + kDir + "/plain.txt").c_str()) == 0);

    // 清除
    CHECK(clear_boot_name(kDir, &err));
    CHECK(read_boot_name(kDir).empty());
    CHECK(clear_boot_name(kDir, &err));   // 幂等

    std::printf("[boot_config_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
