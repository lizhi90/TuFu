// lua_engine.h —— Lua 脚本引擎（标准 Lua 5.4 + 本项目自定义 API）
//
// 定位（见 docs/planA/09）：
//   * 与 BASIC 引擎（script.h）**并行、二选一（互斥）**，共用同一设备宿主 ScriptHost；
//   * 语言：标准 Lua 5.4（vendor 于 third_party/lua/），仅保留白名单标准库（沙箱）；
//   * API：**扁平全局 + 与 BASIC 同名（同名同义）**，少量辅助入口挂在 `ww` 命名空间
//     （`ww.call` 通用转调 / `ww.ret` 读最近命令返回值）；命令名清单来自
//     `motion_command_names()`，由本项目自研（非正运动的 `kx` 前缀）。
//   * 安全：不碰 EtherCAT / RT 线程；指令级预算 + 中止检查双保险（lua_sethook）；
//     未绑定命令、越权操作、非法参数**一律明确报错**（绝不静默）。
//
// 预算语义差异：BASIC 的 max_steps 是**语句级**，本引擎是**VM 指令级**（按 hook 周期近似
// 统计），数值不可直接比较；两者复用同一配置键 SCRIPT_MAX_STEPS（见 09 §3.6）。
//
// 确定性：GC 采用分代模式（LUA_GCGEN），但 GC 停顿本质不确定 —— 本引擎**不承诺硬实时**，
// 只运行于独立**非 RT** 线程；运动时序由 RT 线程 + 参数信箱保证（见 09 §5 / 任务 L-11）。
#pragma once

#include "engine.h"

#include <memory>
#include <string>
#include <vector>

namespace kx {

class LuaEngine : public IScriptEngine {
public:
    explicit LuaEngine(ScriptHost* host = nullptr);
    ~LuaEngine() override;
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // 内部状态（定义在 lua_engine.cpp）。声明为 public 以便 .cpp 中的
    // hook / 自由函数通过 lua_getextraspace 取用；字段本身对外不承诺稳定。
    struct Impl;

    const char* name() const override { return "lua"; }

    void set_host(ScriptHost* host) override;

    // 编译；失败时 err 带行号与原因。可重复调用（会丢弃上一份 chunk）
    bool compile(const std::string& src, std::string* err = nullptr) override;

    // 运行。max_steps 为 VM 指令级预算（默认 5e6，与 BASIC 共用配置但语义不同）
    Status run(unsigned long long max_steps = 5000000ULL) override;

    Status              status() const override;
    const std::string&  error() const override;
    int                 error_line() const override;
    unsigned long long  steps() const override;

    void request_abort() override;

    Value                    get_var(const std::string& name) const override;
    void                     set_var(const std::string& name, const Value& v) override;
    std::vector<std::string> list_vars() const override;
    void                     clear_vars() override;

    void set_trace(bool on) override;
    bool trace() const override;

    // Lua 无标签概念：返回**已注册 API/命令名清单**（调试软件做符号列表用）
    std::vector<std::string> labels() const override;

    // ---- D5：断点 / 暂停 / 单步（v0.8.0；行级 hook，语义对齐 BASIC 的语句级 dbg_gate）----
    void add_breakpoint(int line) override;
    void remove_breakpoint(int line) override;
    void clear_breakpoints() override;
    std::vector<int> breakpoints() const override;
    void request_pause() override;
    void request_step() override;
    void resume() override;
    bool paused() const override;
    int  current_line() const override;
    void enable_line_hooks(bool on) override;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace kx
