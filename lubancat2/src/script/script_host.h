// script_host.h —— 设备宿主接口（与脚本语言无关）
//
// 设计说明（本次拆分）：
//   * `ScriptHost` 是引擎与设备之间的**唯一接口**（命令名 + 值数组 + 返回值 + 错误），
//     BASIC 与 Lua 引擎都只认它，因此与语言无关；
//   * 原先它寄生在 script.h（BASIC 引擎的头）里，现已独立到本头文件：
//     Lua 引擎 / MotionHost / 测试假宿主都只依赖于此，不再被 BASIC 引擎牵连。
//   * 具体实现见 motion_host.h（真机）/ tools/*_test.cpp（假宿主）。
#pragma once

#include "script_value.h"   // kx::Value（call 的入参/出参）

#include <string>
#include <vector>

namespace kx {

// ---------------------------------------------------------------------------
// 宿主接口：脚本能做的设备动作都在这里
// ---------------------------------------------------------------------------
struct ScriptHost {
    virtual ~ScriptHost() = default;

    // 执行一条设备命令。
    // 返回 0 = 成功；1 = 不认识这个命令（引擎会退化为数组访问）；
    // 2 = 运行错误（填 err）；3 = 请求中止脚本。
    virtual int call(const std::string& name, const std::vector<Value>& args,
                     Value* ret, std::string* err) = 0;

    // PRINT 输出的落点
    virtual void print(const std::string& line) = 0;

    // 引擎每执行若干语句回调一次；返回 true 表示要求中止（例如收到停机请求）
    virtual bool aborted() { return false; }
};

} // namespace kx
