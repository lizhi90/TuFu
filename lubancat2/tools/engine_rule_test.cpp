// engine_rule_test.cpp —— 「控制器同一时刻只加载一种脚本语言」规则自测
// （纯逻辑，不需要 IgH / 硬件）
//
// 覆盖：
//   * SCRIPT_ENGINE 名字解析：大小写/首尾空白归一，非法值被拒（绝不静默）；
//   * 脚本文件扩展名与引擎的一致性校验（.lua/.bas）；
//   * ScriptEngineSlot：同一时刻只允许装一个引擎，重复/换语言装载被拒，卸载后可换。
#include "script/engine_rule.h"

#include <cstdio>
#include <string>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

int main() {
    std::printf("== 引擎名解析（大小写/空白，非法值明确拒绝）==\n");
    ScriptLanguage l;
    check(parse_script_language("", &l) && l == ScriptLanguage::BASIC, "\"\" -> basic");
    check(parse_script_language("basic", &l) && l == ScriptLanguage::BASIC, "basic -> basic");
    check(parse_script_language("BASIC", &l) && l == ScriptLanguage::BASIC, "BASIC -> basic");
    check(parse_script_language("  Basic ", &l) && l == ScriptLanguage::BASIC, "\"  Basic \" -> basic");
    check(parse_script_language("lua", &l) && l == ScriptLanguage::LUA, "lua -> lua");
    check(parse_script_language("LUA", &l) && l == ScriptLanguage::LUA, "LUA -> lua");
    check(!parse_script_language("python", &l), "python 被拒");
    check(!parse_script_language("lu", &l), "lu 被拒");
    check(!parse_script_language("basic2", &l), "basic2 被拒");
    check(std::string(script_language_name(ScriptLanguage::BASIC)) == "basic" &&
          std::string(script_language_name(ScriptLanguage::LUA)) == "lua", "规范化名 basic/lua");

    std::printf("== 脚本文件语言一致性 ==\n");
    check(script_language_from_file("a.lua") == ScriptLanguage::LUA, "a.lua -> lua");
    check(script_language_from_file("a.bas") == ScriptLanguage::BASIC, "a.bas -> basic");
    check(script_language_from_file("noext") == ScriptLanguage::BASIC, "无扩展名 -> basic");
    std::string e;
    check(script_file_matches_language(ScriptLanguage::LUA, "a.lua", &e), "lua 引擎 + .lua 通过");
    check(script_file_matches_language(ScriptLanguage::BASIC, "a.bas", &e), "basic 引擎 + .bas 通过");
    check(script_file_matches_language(ScriptLanguage::BASIC, "noext", &e), "无扩展名 -> 放行");
    check(!script_file_matches_language(ScriptLanguage::BASIC, "a.lua", &e) && !e.empty(),
          "basic 引擎 + .lua 被拒");
    check(!script_file_matches_language(ScriptLanguage::LUA, "a.bas", &e), "lua 引擎 + .bas 被拒");

    std::printf("== 单引擎槽：同一时刻只能装一个引擎 ==\n");
    {
        ScriptEngineSlot slot;
        std::string err;
        check(!slot.loaded(), "初始未装载");

        check(slot.load(ScriptLanguage::BASIC, nullptr, &err), "装载 basic 成功");
        check(slot.loaded() && slot.language() == ScriptLanguage::BASIC, "记录语言 = basic");
        check(slot.engine() && std::string(slot.engine()->name()) == "basic", "引擎名 = basic");

        check(!slot.load(ScriptLanguage::LUA, nullptr, &err) && !err.empty(),
              "已装 basic 时拒绝再装 lua（互斥）");
        check(slot.language() == ScriptLanguage::BASIC &&
              std::string(slot.engine()->name()) == "basic", "拒绝后仍是原引擎");
        check(!slot.load(ScriptLanguage::BASIC, nullptr, &err), "同语言重复装载也被拒");

        slot.unload();
        check(!slot.loaded(), "卸载后为空");
        check(slot.load(ScriptLanguage::LUA, nullptr, &err), "卸载后可换装 lua");
        check(std::string(slot.engine()->name()) == "lua", "引擎名 = lua");
    }

    if (g_fail) { std::printf("engine_rule_test: %d 项失败\n", g_fail); return 1; }
    std::printf("engine_rule_test: 全部通过\n");
    return 0;
}
