// script.h —— ZBasic 子集脚本引擎（操作本控制器的 BASIC 语言）
//
// 目标：把「本设备的 BASIC 脚本语言」以最小可用子集先落地，成为调试软件的脚本执行后端：
//   * 语言：BASIC 风格（行式），关键字/变量名大小写不敏感；
//   * 结构：LET/赋值、PRINT(与 ? 简写)、IF/ELSEIF/ELSE(单行与块)、WHILE/WEND、
//           FOR/NEXT/STEP、GOTO/GOSUB/标签、END(结束)、EXIT FOR/WHILE/SUB、
//           GLOBAL SUB/END SUB 子程序(参数按值、LOCAL 局部变量、RETURN 提前返回)、
//           WAIT IDLE/UNTIL/ms(与 WA 简写)、REM/'(注释)、GLOBAL CONST/DIM、
//           一维数组 A(i)（数组亦可当「0 结尾字符串」整体读写）；
//           RETURN 系统变量：保存最近一次设备命令/函数的返回值，`IF RETURN THEN` 判其非 0；
//   * 端口 IO：OPEN #端口,…、PRINT #端口,…(整包发送，不附加换行)、
//           GET #端口,数组[(偏移)][,字节数]（非阻塞接收，可 rx=GET… 取回字节数）、
//           PUTCHAR #端口,数组(起点,长度)（按原始字节发送，含 0 字节）；
//           注意：STOP 是设备命令「停止运动」，不是结束语句（用 END 结束）。
//   * 运算：+ - * / \ MOD ^、= <> < > <= >=、AND OR NOT XOR EQV(均为按位)、
//           一元负号、括号、$FF/&H 十六进制字面量、TICKS(倒计时计数器，差值取已流逝 ms)；
//   * 内置函数：ABS INT SGN SQR SIN COS TAN ATAN MIN MAX LEN VAL STR、
//           STRLEN STRFIND STRCOMP TOSTR HEX CHR ASC、CRC16(数组[,起点,数量])；
//   * 设备命令：由 ScriptHost 提供（MOVE/MOVEABS/JOG/HOME/STOP/DELAY/POS/MODBUS_REG...），
//     具体绑定见 motion_host.h（经 kx::Shared 驱动本控制器）。
//
// 设计边界（刻意为之）：
//   * 引擎是「解释执行」+ 语句级步数预算，绝不让脚本无限占用线程；
//   * 不碰 EtherCAT / RT 线程：所有设备动作都通过 ScriptHost 间接下发（与 4321 服务同一条路）；
//   * 纯逻辑可单测（tools/script_test.cpp 用假 Host），不需要 IgH 与硬件。
#pragma once

#include "engine.h"        // kx::IScriptEngine（语言无关接口）
#include "script_host.h"   // kx::ScriptHost（设备宿主接口）
#include "script_value.h"  // kx::Value（共享值类型）

#include <memory>
#include <string>
#include <vector>

namespace kx {

// 说明：本头文件只保留 BASIC 引擎实现 `ScriptEngine`。
//       共享的 `Value` / `ScriptHost` 已拆分到 script_value.h / script_host.h，
//       Lua 引擎等无需再 include 本头文件（见各文件顶部说明）。

// ---------------------------------------------------------------------------
// 脚本引擎（BASIC / ZBasic 子集）—— 实现语言无关接口 IScriptEngine
// ---------------------------------------------------------------------------
class ScriptEngine : public IScriptEngine {
public:
    // 兼容旧用法：ScriptEngine::Status 即 IScriptEngine::Status
    using Status = IScriptEngine::Status;

    explicit ScriptEngine(ScriptHost* host = nullptr);
    ~ScriptEngine() override;
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    const char* name() const override { return "basic"; }

    void set_host(ScriptHost* host) override;

    // 编译；失败时 err 带行号与原因
    bool compile(const std::string& src, std::string* err = nullptr) override;

    // 运行。max_steps 为语句级预算（默认 5e6）；返回最终状态
    Status run(unsigned long long max_steps = 5000000ULL) override;

    Status              status() const override;
    const std::string&  error() const override;
    int                 error_line() const override;
    unsigned long long  steps() const override;

    // 外部请求中止（线程安全，仅置位）
    void request_abort() override;

    // 变量（调试软件可查看/预置）
    Value                          get_var(const std::string& name) const override;
    void                           set_var(const std::string& name, const Value& v) override;
    std::vector<std::string>        list_vars() const override;    // "NAME = 3"
    void                           clear_vars() override;

    // 单步跟踪（写入 host->print）
    void set_trace(bool on) override;
    bool trace() const override;

    // 脚本里出现过的标签（调试软件做断点/跳转列表用）
    std::vector<std::string> labels() const override;

    // ---- D5：断点 / 暂停 / 单步（协作式，语句边界生效；见 engine.h 说明）----
    void add_breakpoint(int line) override;
    void remove_breakpoint(int line) override;
    void clear_breakpoints() override;
    std::vector<int> breakpoints() const override;
    void request_pause() override;
    void request_step() override;
    void resume() override;
    bool paused() const override;
    int  current_line() const override;

    // 状态名：静态便捷入口（实现见 IScriptEngine::status_name）
    static const char* status_name(Status s) { return IScriptEngine::status_name(s); }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace kx
