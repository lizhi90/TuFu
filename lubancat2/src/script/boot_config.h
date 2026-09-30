// boot_config.h —— 「主文件（开机运行）」设置与目录语言扫描（用户拍板 2026-09-26）
//
// 背景（方案 A）：控制器脚本目录可存多个 `.bas`/`.lua`，但**开机只运行一个主文件**；
// 其它文件由主文件用 INCLUDE 作为子程序调用（见 source_include.h）。主文件在插件的
// 「控制器文件」列表里点选，设置存放于脚本目录内的隐藏清单 `DEBUG_SCRIPT_DIR/.boot`：
//   * 单行文件名；隐藏文件被 valid_script_file_name 排除 → 不进 file.list、不影响语言绑定；
//   * 由调试通道 `boot.get/set/clear` 维护（D8），`main.cpp` 的脚本线程启动时读取；
//   * 修改后**下次启动生效**（插件可配合 D7「重启控制器」立即应用）。
//
// 本文件不依赖 vscode / ecrt，可被纯逻辑自测直接覆盖。
#pragma once

#include <string>

namespace kx {

// 脚本目录内的语言状态（只统计 .bas/.lua 普通文件；`.boot` 等隐藏文件不计）
enum class ScriptDirLang { NONE, BASIC, LUA, MIXED };

// 纯文件名校验（与 D6 白名单一致）：字母/数字/._-，不以 . 开头，长度 1..128
bool valid_script_file_name(const std::string& n);

// 扫描目录语言状态（目录不存在/读不到 → NONE）
ScriptDirLang scan_script_dir_lang(const std::string& dir);

// `.lua` -> "lua"，`.bas` -> "basic"，其它 -> ""
std::string script_lang_of_name(const std::string& name);

// 清单读取结果
struct BootInfo {
    std::string name;          // 清单记录（空 = 未设置主文件）
    bool        valid = true;  // 该记录当前能否作为开机主文件运行
    std::string reason;        // valid=false 的原因（人话，直接可展示）
};

// 读清单（只看 `.boot`，不校验文件是否存在）
std::string read_boot_name(const std::string& dir);

// 读清单 + 校验：文件存在、扩展名可判语言、目录非混合
BootInfo read_boot_info(const std::string& dir);

// 写清单（先写 `.boot.tmp` 再 rename，避免半截文件）；name 由调用方先做业务校验
bool write_boot_name(const std::string& dir, const std::string& name, std::string* err);

// 清空清单（删除 `.boot`；不存在视为成功）
bool clear_boot_name(const std::string& dir, std::string* err);

} // namespace kx
