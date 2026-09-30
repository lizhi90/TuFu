# vendor: Lua 5.4.6（第三方，勿改）

- **上游**：https://www.lua.org/ftp/lua-5.4.6.tar.gz
- **版本**：Lua 5.4.6（2023-05-02 发布）
- **许可**：MIT，见同目录 `LICENSE`（原文亦见 `doc/readme.html`）。
- **来源**：官方 `src/` 目录核心 C 源码（lapi/lauxlib/lbaselib/…/lzio 共 33 个 `.c` + 头文件 + `lua.hpp`）。
- **已剔除**：`lua.c`、`luac.c`（含 `main()`，本控制器只需嵌入式库，不需独立解释器/编译器）。
- **用途**：`src/script/lua_engine.cpp` 链接为静态库（CMake 目标 `lua`），供 Lua 脚本引擎使用。

> ⚠ 本目录为**原样 vendor**，除剔除上述两个 `main` 文件外不作任何修改；
> 如需升级，请整目录替换并同步本文档版本号。
