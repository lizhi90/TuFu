/* lua_runner.c —— 极简 Lua 运行器（vendor Lua 5.4），供离线脚本自测用
 *
 * 用法:  lua_runner <脚本.lua> [args...]
 * 说明:  仅打开标准库并执行脚本；命令行参数写入全局 ARGV 表（ARGV[1] 起），
 *        便于测试脚本定位被测文件（如 tools/port_script_selftest.lua）。
 *        本目标不依赖 IgH/ecrt，任何机器都能构建（见 CMakeLists）。
 */
#include <stdio.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <script.lua> [args...]\n", argv[0]);
        return 2;
    }
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    lua_newtable(L);
    for (int i = 2; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "ARGV");

    int rc = 0;
    if (luaL_loadfile(L, argv[1]) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[lua_runner] 错误: %s\n", lua_tostring(L, -1));
        rc = 1;
    }
    lua_close(L);
    return rc;
}
