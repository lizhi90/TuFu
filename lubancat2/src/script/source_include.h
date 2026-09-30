// source_include.h —— 编译前的多文件包含展开（用户拍板 2026-09-26）
//
// 方案 A 配套：控制器开机只运行一个「主文件」，其它脚本文件作为**子程序**被主文件包含。
// 包含在**编译前**做文本展开（与 ZBasic 工程的“多文件编译到一起”同语义）：
//   BASIC：独占一行的  INCLUDE "sub.bas"（关键字大小写不敏感，可跟 ' 注释）
//   Lua  ：独占一行的  include("sub.lua") 或 include "sub.lua"（可跟 -- 注释）
//
// 规则（不满足一律明确报错，不静默跳过）：
//   * 只能包含 DEBUG_SCRIPT_DIR 内的**纯文件名**（valid_script_file_name，防路径穿越）；
//   * 被包含文件语言必须与主文件一致（.bas ↔ basic / .lua ↔ lua）；
//   * 递归深度 ≤ 8；循环包含报错；
//   * 行号按**展开后的合并源**计算（错误/断点行号对应合并源，见 docs/planA/10 §多文件）。
//
// 本文件不依赖 vscode / ecrt，可被纯逻辑自测直接覆盖。
#pragma once

#include <string>

namespace kx {

// 展开 src 中的 INCLUDE 指令：
//   src   主文件源码
//   dir   脚本目录（被包含文件的根目录，不递归子目录）
//   lang  "basic" | "lua"（主文件语言，决定指令语法与被包含文件的扩展名）
//   from  诊断用来源名（如 "main.bas" / "<compile>"），出现在错误信息里
//   out   展开后的合并源码
//   err   失败原因（含 来源:行号）
bool expand_source_includes(const std::string& src, const std::string& dir,
                            const std::string& lang, const std::string& from,
                            std::string* out, std::string* err);

} // namespace kx
