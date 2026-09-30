// lua_engine_test.cpp —— Lua 脚本引擎自测（假 Host，纯逻辑，不需要 IgH / 硬件）
//
// 覆盖（对齐 docs/planA/09 任务 L-18）：
//   * 同名命令调用与返回值 / 同名同义（MOVEABS 参数=距离、SPEED 读写）；
//   * 值桥接：number / string / boolean / nil、Lua 表运算；
//   * 错误行号归一；
//   * 步数预算（死循环）与中止（request_abort / 宿主 aborted / 命令返回 3）；
//   * 沙箱：os/io/debug/package/load/dofile/require 均不可用；
//   * 绝不静默：命令失败、未绑定命令、编译错误都要明确报错；
//   * 端口显式函数 PORT_PRINT / PORT_GET（表 1 基）；
//   * 变量内省 get/set/list_vars 与 labels() 的 API 名清单。
#include "../src/script/lua_engine.h"
#include "../src/script/script_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// ---------------------------------------------------------------------------
// 假宿主：确定性命令，便于断言
// ---------------------------------------------------------------------------
struct FakeHost : ScriptHost {
    int         calls = 0;
    bool        abort_now = false;
    double      last_speed = -1;
    double      last_moveabs_dist = -1;
    std::string last_print;

    int call(const std::string& name, const std::vector<Value>& args,
             Value* ret, std::string* err) override {
        ++calls;
        if (name == "POS")     { if (ret) *ret = Value::number(12.5); return 0; }
        if (name == "ENABLE")  { if (ret) *ret = Value::number(1); return 0; }
        if (name == "STOP")    { if (ret) *ret = Value::number(1); return 0; }
        if (name == "MOVEABS") {                       // ⚠同名同义：参数是绝对位置(第1参)/速度(第2参)
            last_moveabs_dist = args.empty() ? 0 : args[0].to_num();
            if (ret) *ret = Value::number(1);
            return 0;
        }
        if (name == "SPEED") {                         // 读(1参)/写(2参)
            last_speed = args.size() >= 2 ? args[1].to_num() : (args.empty() ? 0 : args[0].to_num());
            if (ret) *ret = Value::number(last_speed);
            return 0;
        }
        if (name == "BOOM")    { if (err) *err = "命令 BOOM 失败（故意）"; return 2; }
        if (name == "ABORTME") { return 3; }           // 请求中止
        if (name == "PRINT" || name == "PUTCHAR") {
            last_print = args.size() > 1 ? args[1].to_text() : std::string();
            return 0;
        }
        if (name == "GET") {                           // 回传 4 字节：'A','B',0,'C'
            if (ret) *ret = Value::text(std::string("AB\0C", 4));
            return 0;
        }
        return 1;   // 不认识
    }
    void print(const std::string&) override {}
    bool aborted() override { return abort_now; }
};

static bool compile_ok(LuaEngine& e, const char* src) {
    std::string err;
    if (!e.compile(src, &err)) {
        std::printf("    [编译失败] %s\n", err.c_str());
        return false;
    }
    return true;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// ---------------------------------------------------------------------------
// 1) 同名命令调用 + 返回值 + 同名同义
// ---------------------------------------------------------------------------
static void test_cmd_calls() {
    std::printf("== 1) 同名命令调用 / 返回值 / 同名同义 ==\n");
    FakeHost h;
    LuaEngine e(&h);
    const char* src =
        "ENABLE()\n"
        "MOVEABS(10, 200)\n"
        "p = POS(0)\n"           // 命令返回值 -> 全局变量
        "SPEED(0, 300)\n"
        "local s = 0\n"
        "for i = 1, 300 do s = s + i end\n";   // 保证 VM 指令数超过 hook 粒度
    if (!compile_ok(e, src)) { check(false, "编译"); return; }
    check(e.run() == LuaEngine::Status::DONE, "脚本正常结束");
    check(h.calls == 4, "4 条命令都下发到宿主");
    check(e.get_var("p").to_num() == 12.5, "POS 返回值回填 Lua 变量");
    check(h.last_moveabs_dist == 10.0, "MOVEABS 第 1 参 = 位置（同义）");
    check(h.last_speed == 300.0, "SPEED 第 2 参 = 写入值（同义）");
    check(e.steps() > 0, "steps() 统计到 VM 指令数");
    check(e.name() == std::string("lua"), "name() == \"lua\"");
}

// ---------------------------------------------------------------------------
// 2) 值桥接 / 表 / ww.call · ww.ret
// ---------------------------------------------------------------------------
static void test_values_and_ww() {
    std::printf("== 2) 值桥接 / 表 / ww.call · ww.ret ==\n");
    FakeHost h;
    LuaEngine e(&h);
    const char* src =
        "local t = {}\n"
        "for i = 1, 5 do t[i] = i * i end\n"
        "sum = 0\n"
        "for i = 1, 5 do sum = sum + t[i] end\n"
        "msg = \"a\" .. \"b\"\n"
        "flag = (sum == 55)\n"
        "ww.call(\"POS\", 0)\n"
        "r = ww.ret()\n";
    if (!compile_ok(e, src)) { check(false, "编译"); return; }
    check(e.run() == LuaEngine::Status::DONE, "脚本正常结束");
    check(e.get_var("sum").to_num() == 55, "表 + 循环求和 = 55");
    check(e.get_var("msg").to_text() == "ab", "字符串拼接");
    check(e.get_var("flag").to_num() == 1, "布尔 true -> 1");
    check(e.get_var("r").to_num() == 12.5, "ww.call/ww.ret 取回命令返回值");
    check(e.get_var("nosuchvar").is_nil(), "未赋值变量 -> nil（Lua 原生语义）");
}

// ---------------------------------------------------------------------------
// 3) 错误行号归一 / 编译错误
// ---------------------------------------------------------------------------
static void test_error_line() {
    std::printf("== 3) 错误行号 / 编译错误 ==\n");
    FakeHost h;
    {
        LuaEngine e(&h);
        if (!compile_ok(e, "local x = 1\nx = nosuchtable.field\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::RUNTIME_ERROR, "运行错误 -> RUNTIME_ERROR");
        check(e.error_line() == 2, "error_line() 归一到第 2 行");
        check(!e.error().empty(), "error() 有明确文本");
    }
    {
        LuaEngine e(&h);
        std::string err;
        check(!e.compile("local x = (\n", &err), "语法错误 -> compile 返回 false");
        check(e.status() == LuaEngine::Status::COMPILE_ERROR, "状态 = COMPILE_ERROR");
        check(e.error_line() > 0, "编译错误带行号");
    }
}

// ---------------------------------------------------------------------------
// 4) 预算 / 中止
// ---------------------------------------------------------------------------
static void test_budget_and_abort() {
    std::printf("== 4) 步数预算 / 中止 ==\n");
    {
        FakeHost h;
        LuaEngine e(&h);
        if (!compile_ok(e, "while true do end\n")) { check(false, "编译"); return; }
        check(e.run(2000) == LuaEngine::Status::BUDGET_EXCEEDED, "死循环被预算叫停");
    }
    {
        FakeHost h;
        LuaEngine e(&h);
        // ABORTME 是测试专属命令、不在命令表内，故走通用转调入口 ww.call
        if (!compile_ok(e, "ww.call(\"ABORTME\")\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::ABORTED, "命令返回 3 -> ABORTED");
    }
    {
        FakeHost h;
        LuaEngine e(&h);
        if (!compile_ok(e, "local i = 0\nwhile true do i = i + 1 end\n")) { check(false, "编译"); return; }
        e.request_abort();
        check(e.run() == LuaEngine::Status::ABORTED, "request_abort 生效");
    }
    {
        FakeHost h;
        h.abort_now = true;                          // 宿主已请求停机
        LuaEngine e(&h);
        if (!compile_ok(e, "local i = 0\nwhile true do i = i + 1 end\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::ABORTED, "宿主 aborted() -> ABORTED");
    }
}

// ---------------------------------------------------------------------------
// 4b) D5（v0.8.0）：行级断点 / 暂停 / 单步 / 挂起中中止唤醒
// ---------------------------------------------------------------------------
static void test_d5_breakpoints() {
    std::printf("== 4b) D5 行级断点 / 单步 / 中止唤醒 ==\n");
    FakeHost h;
    LuaEngine e(&h);
    e.enable_line_hooks(true);
    if (!compile_ok(e, "a = 0\nfor i = 1, 1000000 do\na = a + 1\nend\n")) {
        check(false, "D5: 编译循环脚本");
        return;
    }
    e.add_breakpoint(3);

    std::atomic<int> st{-1};
    std::thread th([&] {
        st.store(static_cast<int>(e.run(0)));
    });
    auto wait_paused = [&](bool want) {
        for (int i = 0; i < 300 && e.paused() != want; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return e.paused();
    };
    check(wait_paused(true), "D5: 命中断点后挂起（paused()=true）");
    check(e.current_line() == 3, "D5: current_line=3");
    check(e.breakpoints().size() == 1 && e.breakpoints()[0] == 3, "D5: breakpoints() 回读");

    e.request_step();
    check(wait_paused(true) || wait_paused(true), "D5: 单步后再次挂起");
    check(e.current_line() >= 2, "D5: 单步落在循环体内（行号 >= 2）");

    e.request_abort();                 // 挂起中中止：必须唤醒 worker 才能退出
    th.join();
    check(st.load() == static_cast<int>(LuaEngine::Status::ABORTED), "D5: 挂起中 request_abort -> ABORTED");
    check(!e.paused(), "D5: 结束后 paused()=false");
    e.clear_breakpoints();
    check(e.breakpoints().empty(), "D5: clear_breakpoints");

    // 恢复路径：断点 → 清断点 → resume → DONE（不清断点会在循环里反复命中，join 永不返回）
    e.add_breakpoint(3);
    std::atomic<int> st2{-1};
    std::thread th2([&] {
        st2.store(static_cast<int>(e.run(0)));
    });
    check(wait_paused(true), "D5: 二次运行命中断点");
    e.clear_breakpoints();
    e.resume();
    th2.join();
    check(st2.load() == static_cast<int>(LuaEngine::Status::DONE), "D5: 清断点 + resume 后正常运行到 DONE");
}

// ---------------------------------------------------------------------------
// 5) 沙箱：越权入口一律不可用
// ---------------------------------------------------------------------------
static void test_sandbox() {
    std::printf("== 5) 沙箱白名单 ==\n");
    FakeHost h;
    LuaEngine e(&h);
    const char* src =
        "has_os     = (os ~= nil)\n"
        "has_io     = (io ~= nil)\n"
        "has_debug  = (debug ~= nil)\n"
        "has_pkg    = (package ~= nil)\n"
        "has_load   = (load ~= nil)\n"
        "has_dofile = (dofile ~= nil)\n"
        "has_require= (require ~= nil)\n"
        "has_print  = (PRINT ~= nil)\n"          // 语法糖名不注册（改 PORT_PRINT）
        "has_port   = (PORT_PRINT ~= nil)\n";
    if (!compile_ok(e, src)) { check(false, "编译"); return; }
    check(e.run() == LuaEngine::Status::DONE, "沙箱探测脚本正常结束");
    for (const char* n : {"has_os", "has_io", "has_debug", "has_pkg",
                          "has_load", "has_dofile", "has_require", "has_print"}) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s == 0（不可用）", n);
        check(e.get_var(n).to_num() == 0, buf);
    }
    check(e.get_var("has_port").to_num() == 1, "PORT_PRINT 已注册");
    {
        LuaEngine e2(&h);
        if (!compile_ok(e2, "os.exit(1)\n")) { check(false, "编译"); return; }
        check(e2.run() == LuaEngine::Status::RUNTIME_ERROR, "os.exit 调用即明确报错");
    }
}

// ---------------------------------------------------------------------------
// 6) 绝不静默：命令失败 / 未绑定命令
// ---------------------------------------------------------------------------
static void test_fail_loud() {
    std::printf("== 6) 绝不静默 ==\n");
    FakeHost h;
    {
        LuaEngine e(&h);
        // BOOM 是测试专属命令（宿主返回 2=运行错误），走通用转调入口 ww.call
        if (!compile_ok(e, "ww.call(\"BOOM\")\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::RUNTIME_ERROR, "命令失败 -> RUNTIME_ERROR");
        check(e.error().find("BOOM") != std::string::npos, "错误文本含命令名（不静默）");
    }
    {
        LuaEngine e(&h);
        if (!compile_ok(e, "NOSUCHCMD()\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::RUNTIME_ERROR, "未注册的全局命令名 -> 明确报错");
    }
    {
        LuaEngine e(&h);
        if (!compile_ok(e, "ww.call(\"NOPE\")\n")) { check(false, "编译"); return; }
        check(e.run() == LuaEngine::Status::RUNTIME_ERROR, "ww.call 未绑定命令 -> 明确报错");
        check(e.error().find("NOPE") != std::string::npos, "错误文本含未绑定命令名");
    }
}

// ---------------------------------------------------------------------------
// 7) 端口显式函数（表 1 基）
// ---------------------------------------------------------------------------
static void test_port_funcs() {
    std::printf("== 7) PORT_PRINT / PORT_GET ==\n");
    FakeHost h;
    LuaEngine e(&h);
    const char* src =
        "local t = {}\n"
        "n = PORT_GET(1, t)\n"
        "b1 = t[1]\n"
        "b4 = t[4]\n"
        "PORT_PRINT(2, \"hello\")\n";
    if (!compile_ok(e, src)) { check(false, "编译"); return; }
    check(e.run() == LuaEngine::Status::DONE, "端口脚本正常结束");
    check(e.get_var("n").to_num() == 4, "PORT_GET 返回字节数");
    check(e.get_var("b1").to_num() == 'A', "t[1] = 'A'（1 基）");
    check(e.get_var("b4").to_num() == 'C', "t[4] = 'C'（含 0 字节也填入）");
    check(h.last_print == "hello", "PORT_PRINT 整包发送内容正确");
}

// ---------------------------------------------------------------------------
// 8) 变量内省 / labels
// ---------------------------------------------------------------------------
static void test_introspection() {
    std::printf("== 8) 变量内省 / labels ==\n");
    FakeHost h;
    LuaEngine e(&h);
    if (!compile_ok(e, "sum = 55\nname = \"ww\"\n")) { check(false, "编译"); return; }
    check(e.run() == LuaEngine::Status::DONE, "脚本正常结束");

    const auto vars = e.list_vars();
    check(has(vars, "sum = 55"), "list_vars 含数值变量");
    check(has(vars, "name = \"ww\""), "list_vars 含字符串变量");
    check(!has(vars, "ww = table"), "函数/表不作为变量列出");

    e.set_var("preset", Value::number(7));
    check(e.get_var("preset").to_num() == 7, "set_var/get_var 往返");

    e.clear_vars();
    check(e.get_var("sum").is_nil(), "clear_vars 清除脚本变量");
    check(e.get_var("preset").is_nil(), "clear_vars 清除预置变量");

    // 已注册 API（函数/表）无法用 get_var 表示（Value 无函数类型），改用脚本实证其仍在 _G
    if (compile_ok(e, "kept = (type(POS) == 'function') and (type(ww) == 'table')\n")) {
        e.run();
        check(e.get_var("kept").to_num() == 1, "clear_vars 保留已注册 API");
    }

    const auto labs = e.labels();
    check(has(labs, "MOVEABS") && has(labs, "ww"), "labels() 返回已注册 API 名清单");

    e.set_trace(true);
    check(e.trace(), "set_trace(true) 生效");
}

// ---------------------------------------------------------------------------
int main() {
    std::printf("==== lua_engine_test ====\n");
    test_cmd_calls();
    test_values_and_ww();
    test_error_line();
    test_budget_and_abort();
    test_d5_breakpoints();
    test_sandbox();
    test_fail_loud();
    test_port_funcs();
    test_introspection();
    std::printf("==== %s（失败 %d）====\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
