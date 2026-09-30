// engine.h —— 语言无关的脚本引擎接口（供调试软件/主程序「只对接口编程」）
//
// 背景：本项目并存两套脚本引擎，且**互为二选一**（互斥，同一时刻只跑一个引擎线程）：
//   * BASIC 引擎：src/script/script.{h,cpp}（ZBasic 子集，见 docs/planA/08）；
//   * Lua 引擎  ：src/script/lua_engine.{h,cpp}（标准 Lua 5.4，见 docs/planA/09）。
//
// 两者共享**同一个设备宿主接口** `ScriptHost`（只认「命令名 + 值数组 + 返回值 + 错误」，
// 与调用语言无关），因此设备逻辑只写一份（MotionHost）。本文件抽出引擎的公共操作面，
// 让上层（main.cpp 的 script_loop、调试软件）无需关心底下是哪种语言。
//
// 设计边界：
//   * 本接口只描述「编译 / 运行 / 状态 / 错误 / 变量 / 跟踪 / 标签」，不含语言细节；
//   * 值类型复用 `kx::Value`（NUM/STR/NIL）——两引擎的共同数据面；
//   * 不改动 BASIC 引擎的行为：`ScriptEngine` 实现本接口后语义不变（见 08 的回归基线）。
#pragma once

#include <string>
#include <vector>

namespace kx {

// 完整定义在 script.h；本接口只按值/引用「声明」使用，故前置声明即可。
struct Value;
struct ScriptHost;

// ---------------------------------------------------------------------------
// 脚本引擎统一接口
// ---------------------------------------------------------------------------
class IScriptEngine {
public:
    enum class Status {
        READY = 0,          // 已编译，尚未运行 / 运行中
        DONE,               // 正常跑完（BASIC: END/语句耗尽；Lua: 脚本正常返回）
        COMPILE_ERROR,      // 语法错误
        RUNTIME_ERROR,      // 运行期错误（未定义标签/除零/命令报错…）
        ABORTED,            // 被 request_abort() 或 host->aborted() 中止
        BUDGET_EXCEEDED,    // 超出步数预算（防死循环）
    };

    virtual ~IScriptEngine() = default;

    // 引擎标识（"basic" / "lua"），用于日志与调试软件分流
    virtual const char* name() const = 0;

    // 设备宿主（可为空；运行前必须设置）
    virtual void set_host(ScriptHost* host) = 0;

    // 编译；失败时 err 带行号与原因
    virtual bool compile(const std::string& src, std::string* err = nullptr) = 0;

    // 运行。max_steps 为预算（BASIC: 语句级；Lua: VM 指令级，见 09 §3.6）；返回最终状态
    virtual Status run(unsigned long long max_steps = 5000000ULL) = 0;

    virtual Status              status() const = 0;
    virtual const std::string&  error() const = 0;
    virtual int                 error_line() const = 0;
    virtual unsigned long long  steps() const = 0;

    // 外部请求中止（线程安全，仅置位）
    virtual void request_abort() = 0;

    // 变量（调试软件可查看/预置）
    virtual Value                    get_var(const std::string& name) const = 0;
    virtual void                     set_var(const std::string& name, const Value& v) = 0;
    virtual std::vector<std::string> list_vars() const = 0;   // "NAME = 3"
    virtual void                     clear_vars() = 0;

    // 单步跟踪（写入 host->print）
    virtual void set_trace(bool on) = 0;
    virtual bool trace() const = 0;

    // 调试用的位置列表（BASIC: 标签；Lua: 函数/行号表）
    virtual std::vector<std::string> labels() const = 0;

    // ---- D5：断点 / 暂停 / 单步（docs/planA/13 §5.2）----
    // 默认「不支持」（空实现）：未覆写的引擎（如 Lua）走默认，上层据此明确报
    // NOT_SUPPORTED（降级不伪装）。BASIC 引擎（script.h）覆写全部方法。
    // 语义：协作式，仅在**语句边界**生效；线程安全（会话线程调，worker 线程消费）。
    virtual void add_breakpoint(int line)    { (void)line; }
    virtual void remove_breakpoint(int line) { (void)line; }
    virtual void clear_breakpoints()         {}
    virtual std::vector<int> breakpoints() const { return {}; }

    // request_pause：运行中 → 下一条语句边界挂起；未运行 → 无效。
    // request_step：仅暂停中有效 → 恢复并执行一条语句，再于下一条语句边界挂起。
    // resume：暂停中 → 恢复运行。
    virtual void request_pause() {}
    virtual void request_step()  {}
    virtual void resume()        {}

    // 挂起态查询（供 script.status / PAUSED 事件）
    virtual bool paused() const       { return false; }
    virtual int  current_line() const { return 0; }

    // 现场启用「行级 hook」（D5）：调试通道装载的引擎会打开以支持断点/暂停/单步；
    // 开机生产脚本不打开（零额外开销）。默认空实现 = 该引擎无需/不区分（BASIC 恒语句级）。
    virtual void enable_line_hooks(bool on) { (void)on; }

    static const char* status_name(Status s) {
        switch (s) {
            case Status::READY:           return "READY";
            case Status::DONE:            return "DONE";
            case Status::COMPILE_ERROR:   return "COMPILE_ERROR";
            case Status::RUNTIME_ERROR:   return "RUNTIME_ERROR";
            case Status::ABORTED:         return "ABORTED";
            case Status::BUDGET_EXCEEDED: return "BUDGET_EXCEEDED";
        }
        return "?";
    }
};

} // namespace kx
