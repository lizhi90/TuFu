// script_test.cpp —— ZBasic 子集脚本引擎自测（纯逻辑，不需要 IgH / 硬件）
//
// 用假 Host 覆盖：赋值/运算/优先级、字符串、PRINT(含尾分号)、IF(单行/块)、
// WHILE、FOR/STEP、GOTO/标签、数组、内置函数、宿主命令、预算与中止、
// 以及编译期/运行期错误。
#include "../src/script/script.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// ---------------------------------------------------------------------------
// 假宿主：记录 PRINT 输出，提供几个确定性命令
// ---------------------------------------------------------------------------
struct FakeHost : ScriptHost {
    std::vector<std::string> out;
    int stop_after = -1;          // >=0 时第 N 次 call 返回 3（模拟中止）
    int calls = 0;
    double delay_ms = -1;         // 最后一次 DELAY/WAIT 的毫秒数
    std::function<void()> on_print;   // 每次输出时回调（用于模拟外部 request_abort）

    // B 层端口 IO：记录 OPEN/PRINT#/PUTCHAR，GET 按队列回传字节
    std::vector<std::string> opened;      // "OPEN 10 TCP_SERVER 4321"
    std::vector<std::string> port_out;    // 每次 PRINT #端口 / PUTCHAR 发出的文本
    std::vector<std::string> get_queue;   // 每次 GET 回传的字节串（FIFO）

    int call(const std::string& name, const std::vector<Value>& args,
             Value* ret, std::string* err) override {
        ++calls;
        if (stop_after >= 0 && calls > stop_after) return 3;   // 请求中止

        if (name == "GETVAL")  { if (ret) *ret = Value::number(42); return 0; }
        if (name == "ADD")    { if (ret) *ret = Value::number(args[0].to_num() + args[1].to_num()); return 0; }
        if (name == "ECHO")   { if (ret) *ret = args.empty() ? Value::text("") : args[0]; return 0; }
        if (name == "FAIL")   { if (err) *err = "故意失败"; return 2; }
        if (name == "NOOP")   { return 0; }
        if (name == "WAITIDLE") { return 0; }   // 模拟「等待轴停止」
        if (name == "DELAY")  { delay_ms = args.empty() ? 0 : args[0].to_num(); return 0; }
        if (name == "OPEN") {
            std::string s = "OPEN " + std::to_string((int)args[0].to_num());
            for (size_t k = 1; k < args.size(); ++k) s += " " + args[k].to_text();
            opened.push_back(s);
            return 0;
        }
        if (name == "PRINT") {          // args = {端口, 文本}
            port_out.push_back(args.size() > 1 ? args[1].to_text() : std::string());
            return 0;
        }
        if (name == "PUTCHAR") {        // args = {端口, 原始字节串}
            port_out.push_back(args.size() > 1 ? args[1].to_text() : std::string());
            return 0;
        }
        if (name == "GET") {            // args = {端口, 数组名, 偏移, 最大字节数}
            std::string bytes = get_queue.empty() ? std::string() : get_queue.front();
            if (!get_queue.empty()) get_queue.erase(get_queue.begin());
            if (ret) *ret = Value::text(bytes);
            return 0;
        }
        return 1;   // 不认识
    }
    void print(const std::string& line) override {
        out.push_back(line);
        if (on_print) on_print();
    }
};

static ScriptEngine::Status run_src(FakeHost& h, const char* src,
                                    unsigned long long budget = 5000000ULL) {
    ScriptEngine e(&h);
    std::string err;
    if (!e.compile(src, &err)) {
        std::printf("    [编译失败] %s\n", err.c_str());
        return ScriptEngine::Status::COMPILE_ERROR;
    }
    return e.run(budget);
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

static int idx_of(const std::vector<std::string>& v, const std::string& s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
static void test_expr_assign_print() {
    std::printf("== 1) 赋值 / 运算 / PRINT ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "a = 2 + 3 * 4\n"
        "b = (2 + 3) * 4\n"
        "PRINT a\n"
        "PRINT b\n"
        "PRINT 10 / 4\n"
        "PRINT 10 \\ 4\n"
        "PRINT 10 MOD 4\n"
        "PRINT 2 ^ 10\n"
        "PRINT -3 + 1\n"
        "PRINT 1 + 2 > 2 AND 3 <> 4\n"
        "? 99\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "14"), "2+3*4=14");
    check(has(h.out, "20"), "(2+3)*4=20");
    check(has(h.out, "2.5"), "10/4=2.5");
    check(has(h.out, "2"), "10\\4=2");
    check(has(h.out, "2"), "10 MOD 4=2");
    check(has(h.out, "1024"), "2^10=1024");
    check(has(h.out, "-2"), "-3+1=-2");
    check(has(h.out, "1"), "比较+AND");
    check(has(h.out, "99"), "? 打印快捷写法");
}

static void test_strings_and_print_semicolon() {
    std::printf("== 2) 字符串 / PRINT 尾分号 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "s$ = \"AB\" + \"CD\"\n"
        "PRINT s$\n"
        "PRINT \"x=\"; 7;\n"          // 尾分号 -> 不换行
        "PRINT \"!\"\n"               // 与上一行拼成一行
        "PRINT LEN(s$)\n"
        "PRINT VAL(\"12.5\")\n"
        "PRINT STR(3) + \"z\"\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "ABCD"), "字符串拼接");
    check(has(h.out, "x=7!"), "尾分号不换行，与下一行合并");
    check(has(h.out, "4"), "LEN=4");
    check(has(h.out, "12.5"), "VAL");
    check(has(h.out, "3z"), "STR 拼接");
}

static void test_if() {
    std::printf("== 3) IF 单行 / 块 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "a = 5\n"
        "IF a > 3 THEN PRINT \"big\"\n"
        "IF a > 10 THEN\n"
        "  PRINT \"no\"\n"
        "ELSE\n"
        "  PRINT \"small\"\n"
        "ENDIF\n"
        "IF a = 5 THEN\n"
        "  PRINT \"five\"\n"
        "END IF\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "big"), "单行 IF");
    check(has(h.out, "small"), "IF/ELSE 之 else 分支");
    check(has(h.out, "five"), "IF/END IF 块");
    check(!has(h.out, "no"), "else 分支未误执行");
}

static void test_loops() {
    std::printf("== 4) WHILE / FOR ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "i = 0\n"
        "s = 0\n"
        "WHILE i < 5\n"
        "  s = s + i\n"
        "  i = i + 1\n"
        "WEND\n"
        "PRINT s\n"
        "t = 0\n"
        "FOR k = 1 TO 10 STEP 2\n"
        "  t = t + k\n"
        "NEXT k\n"
        "PRINT t\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "10"), "WHILE 累加 0..4=10");
    check(has(h.out, "25"), "FOR STEP 2 累加 1+3+5+7+9=25");
}

static void test_goto_and_array() {
    std::printf("== 5) GOTO / 标签 / 数组 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "n = 0\n"
        "A(0) = 10\n"
        "A(1) = 20\n"
        "LOOP:\n"
        "n = n + 1\n"
        "IF n <= 2 THEN GOTO LOOP\n"
        "PRINT n\n"
        "PRINT A(0) + A(1)\n"
        "GOTO DONE\n"
        "PRINT \"skip\"\n"
        "DONE:\n"
        "PRINT \"end\"\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "3"), "GOTO 循环 n=3");
    check(has(h.out, "30"), "数组 A(0)+A(1)=30");
    check(has(h.out, "end"), "跳过中间语句");
    check(!has(h.out, "skip"), "GOTO 后未执行中间语句");
}

// ---------------------------------------------------------------------------
// C 层：GOSUB / RETURN（ZBasic 惯用的「子过程」写法）
// ---------------------------------------------------------------------------
static void test_gosub() {
    std::printf("== 5b) GOSUB / RETURN ==\n");
    // 基本：GOSUB 执行子过程后 RETURN 回到调用点的下一条
    {
        FakeHost h;
        const auto st = run_src(h,
            "n = 0\n"
            "GOSUB ADDONE\n"
            "PRINT \"after=\" + STR(n)\n"
            "GOSUB ADDONE\n"
            "PRINT \"after2=\" + STR(n)\n"
            "END\n"
            "ADDONE:\n"
            "  n = n + 1\n"
            "  RETURN\n");
        check(st == ScriptEngine::Status::DONE, "正常结束");
        check(has(h.out, "after=1"), "GOSUB 返回后继续执行下一条");
        check(has(h.out, "after2=2"), "GOSUB 可被多次调用");
    }
    // 嵌套 GOSUB：返回地址按栈序回退（A->B->A->顶层）
    {
        FakeHost h;
        const auto st = run_src(h,
            "GOSUB A\n"
            "PRINT \"done\"\n"
            "END\n"
            "A:\n"
            "  PRINT \"A1\"\n"
            "  GOSUB B\n"
            "  PRINT \"A2\"\n"
            "  RETURN\n"
            "B:\n"
            "  PRINT \"B\"\n"
            "  RETURN\n");
        check(st == ScriptEngine::Status::DONE, "正常结束");
        check(idx_of(h.out, "A1") >= 0 && idx_of(h.out, "A1") < idx_of(h.out, "B") &&
              idx_of(h.out, "B") < idx_of(h.out, "A2") && idx_of(h.out, "A2") < idx_of(h.out, "done"),
              "嵌套 GOSUB 按 A1->B->A2->done 的栈序返回");
    }
    // GOSUB 段内的 RETURN 不结束脚本；RETURN 出现在 GOSUB/子程序之外 -> 运行错误
    {
        FakeHost h;
        check(run_src(h, "PRINT 1\nRETURN\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "顶层 RETURN -> 运行错误（不静默）");
    }
    {
        FakeHost h;
        check(run_src(h, "GOSUB NOWHERE\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "GOSUB 未定义标号 -> 运行错误");
    }
}

static void test_host_cmds_and_builtins() {
    std::printf("== 6) 宿主命令 / 内置函数 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "PRINT GETVAL()\n"
        "PRINT ADD(3, 4)\n"
        "PRINT ECHO(\"hi\")\n"
        "PRINT ABS(-7)\n"
        "PRINT INT(3.9)\n"
        "PRINT SGN(-2)\n"
        "PRINT SQR(16)\n"
        "PRINT MIN(3, 1, 2)\n"
        "PRINT MAX(3, 1, 2)\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "42"), "GETVAL()");
    check(has(h.out, "7"), "ADD");
    check(has(h.out, "hi"), "ECHO");
    check(has(h.out, "7"), "ABS");
    check(has(h.out, "3"), "INT");
    check(has(h.out, "-1"), "SGN");
    check(has(h.out, "4"), "SQR");
    check(has(h.out, "1"), "MIN");
    check(has(h.out, "3"), "MAX");
}

static void test_errors() {
    std::printf("== 7) 错误处理 ==\n");
    {
        FakeHost h;
        ScriptEngine e(&h);
        std::string err;
        check(!e.compile("IF 1 THEN\nPRINT 1\n", &err), "缺 ENDIF -> 编译失败");
        check(e.status() == ScriptEngine::Status::COMPILE_ERROR, "状态=COMPILE_ERROR");
        check(err.find("ENDIF") != std::string::npos, "错误信息提到 ENDIF");
    }
    {
        FakeHost h;
        check(run_src(h, "PRINT 1 / 0\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "除零 -> 运行错误");
    }
    {
        FakeHost h;
        check(run_src(h, "GOTO NOWHERE\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "未定义标号 -> 运行错误");
    }
    {
        FakeHost h;
        check(run_src(h, "FAIL()\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "宿主报错 -> 运行错误");
    }
}

static void test_budget_and_abort() {
    std::printf("== 8) 预算 / 中止 ==\n");
    {
        FakeHost h;
        const auto st = run_src(h, "i = 0\nWHILE 1\n  i = i + 1\nWEND\n", 10000ULL);
        check(st == ScriptEngine::Status::BUDGET_EXCEEDED, "死循环被预算叫停");
    }
    {
        FakeHost h;
        h.stop_after = 2;   // 第 3 次命令调用返回 3
        const auto st = run_src(h,
            "FOR k = 1 TO 100\n"
            "  NOOP()\n"
            "NEXT k\n");
        check(st == ScriptEngine::Status::ABORTED, "宿主请求中止 -> ABORTED");
    }
    {
        // request_abort 在运行中被外部置位（这里用输出回调模拟外部线程叫停）
        FakeHost h;
        ScriptEngine e(&h);
        std::string err;
        check(e.compile("PRINT 1\nPRINT 2\nPRINT 3\n", &err), "多语句编译通过");
        h.on_print = [&] { e.request_abort(); };
        check(e.run() == ScriptEngine::Status::ABORTED, "request_abort 生效");
        check(e.steps() > 0, "中止前确实执行了语句");
    }
    {
        // run() 之前预置 request_abort -> 立即中止（不执行任何语句）
        FakeHost h;
        ScriptEngine e(&h);
        std::string err;
        check(e.compile("WHILE 1\n  i = i + 1\nWEND\n", &err), "长循环编译通过");
        e.request_abort();
        check(e.run(100000000ULL) == ScriptEngine::Status::ABORTED, "预置 abort 立即生效");
    }
}

static void test_vars_and_labels_introspection() {
    std::printf("== 9) 变量 / 标签自省 ==\n");
    FakeHost h;
    ScriptEngine e(&h);
    std::string err;
    check(e.compile("START:\nY = X + 9\nEND\n", &err), "编译通过");
    e.set_var("x", Value::number(5));
    check(e.run() == ScriptEngine::Status::DONE, "运行完成");
    check(e.get_var("Y").to_num() == 14, "预置变量参与运算");
    const std::vector<std::string> lbs = e.labels();
    check(lbs.size() == 1 && lbs[0] == "START", "标签列表含 START");
    e.clear_vars();
    check(e.list_vars().empty(), "清空变量后列表为空");
}

// ---------------------------------------------------------------------------
// A1：ELSEIF / 按位运算（与 ZBasic 一致）
// ---------------------------------------------------------------------------
static void test_a1_elseif_and_bitwise() {
    std::printf("== 10) A1: ELSEIF / 按位运算 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "a = 5\n"
        "IF a = 1 THEN\n"
        "  PRINT \"one\"\n"
        "ELSEIF a = 3 THEN\n"
        "  PRINT \"three\"\n"
        "ELSEIF a = 5 THEN\n"
        "  PRINT \"five\"\n"
        "ELSE\n"
        "  PRINT \"other\"\n"
        "ENDIF\n"
        "PRINT 6 OR 1\n"
        "PRINT 6 XOR 3\n"
        "PRINT 6 AND 3\n"
        "PRINT NOT 0\n"
        "PRINT (12 AND 10)\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "five"), "ELSEIF 命中第 3 支");
    check(!has(h.out, "one") && !has(h.out, "three") && !has(h.out, "other"), "其它分支未误执行");
    check(has(h.out, "7"), "6 OR 1 = 7（按位）");
    check(has(h.out, "5"), "6 XOR 3 = 5");
    check(has(h.out, "2"), "6 AND 3 = 2（按位，而非逻辑 1）");
    check(has(h.out, "-1"), "NOT 0 = -1（按位取反）");
    check(has(h.out, "8"), "12 AND 10 = 8（掩码场景）");
}

// ---------------------------------------------------------------------------
// A1：GLOBAL CONST / GLOBAL DIM / $hex
// ---------------------------------------------------------------------------
static void test_a1_const_dim_hex() {
    std::printf("== 11) A1: 常量 / DIM / 十六进制 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "GLOBAL CONST N = 3, MASK = $FF\n"
        "GLOBAL DIM t, buf(8)\n"
        "PRINT N\n"
        "PRINT MASK\n"
        "PRINT MASK AND $0F\n"
        "PRINT $6060\n"
        "t = 1 + N\n"
        "PRINT t\n"
        "PRINT buf(3)\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "3"), "常量 N = 3");
    check(has(h.out, "255"), "$FF = 255");
    check(has(h.out, "15"), "$FF AND $0F = 15");
    check(has(h.out, "24672"), "$6060 = 24672");
    check(has(h.out, "4"), "DIM 后变量可正常使用");
    check(has(h.out, "0"), "DIM 数组元素默认 0");
}

// ---------------------------------------------------------------------------
// A1：EXIT FOR/WHILE 与 WAIT
// ---------------------------------------------------------------------------
static void test_a1_exit_and_wait() {
    std::printf("== 12) A1: EXIT FOR/WHILE 与 WAIT ==\n");
    {
        FakeHost h;
        const auto st = run_src(h,
            "FOR k = 1 TO 10\n"
            "  IF k = 4 THEN EXIT FOR\n"
            "NEXT k\n"
            "PRINT k\n"
            "i = 0\n"
            "WHILE 1\n"
            "  i = i + 1\n"
            "  IF i >= 3 THEN EXIT WHILE\n"
            "WEND\n"
            "PRINT i\n"
            "WAIT UNTIL 1 = 1\n"
            "WAIT IDLE\n"
            "PRINT \"waited\"\n");
        check(st == ScriptEngine::Status::DONE, "正常结束");
        check(has(h.out, "4"), "EXIT FOR 在 k=4 跳出");
        check(has(h.out, "3"), "EXIT WHILE 在 i=3 跳出");
        check(has(h.out, "waited"), "WAIT UNTIL 通过 + WAIT IDLE 交宿主");
    }
    {
        FakeHost h;   // 条件永不成立 -> 被步数预算叫停（不会挂死）
        check(run_src(h, "WAIT UNTIL 0\n", 300ULL) == ScriptEngine::Status::BUDGET_EXCEEDED,
              "WAIT 死等被预算叫停");
    }
}

// ---------------------------------------------------------------------------
// A1：字符串 / 格式化内置函数
// ---------------------------------------------------------------------------
static void test_a1_builtins() {
    std::printf("== 13) A1: 字符串/格式化内置函数 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "PRINT STRLEN(\"abcd\")\n"
        "PRINT STRCOMP(\"AB\", \"AB\")\n"
        "PRINT STRCOMP(\"AB\", \"AC\")\n"
        "PRINT HEX(9)\n"
        "PRINT HEX(255)\n"
        "PRINT CHR(65)\n"
        "PRINT ASC(\"A\")\n"
        "PRINT \"[\" + TOSTR(3.14159, 8, 2) + \"]\"\n"
        "PRINT INT(ATAN(1) * 4 * 100)\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "4"), "STRLEN(\"abcd\") = 4");
    check(has(h.out, "0"), "STRCOMP 相等 = 0");
    check(has(h.out, "-1"), "STRCOMP 小于 = -1");
    check(has(h.out, "09"), "HEX(9) = 09（字节对齐）");
    check(has(h.out, "FF"), "HEX(255) = FF");
    check(has(h.out, "A"), "CHR(65) = A");
    check(has(h.out, "65"), "ASC(\"A\") = 65");
    check(has(h.out, "[    3.14]"), "TOSTR 宽度 8 / 2 位小数");
    check(has(h.out, "314"), "ATAN 可用");
}

// ---------------------------------------------------------------------------
// A1：TICKS 倒计时（差值方向）
// ---------------------------------------------------------------------------
static void test_a1_ticks() {
    std::printf("== 14) A1: TICKS 倒计时 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "t0 = TICKS\n"
        "x = 0\n"
        "FOR i = 1 TO 20000\n"
        "  x = x + 1\n"
        "NEXT i\n"
        "d = t0 - TICKS\n"
        "ok = 0\n"
        "IF d >= 0 THEN ok = ok + 1\n"
        "IF (TICKS - t0) <= 0 THEN ok = ok + 1\n"
        "IF d < 60000 THEN ok = ok + 1\n"
        "PRINT ok\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "3"), "t0 - TICKS 为正且写反恒 <= 0（与手册一致）");
}

// ---------------------------------------------------------------------------
// A1：常量只读
// ---------------------------------------------------------------------------
static void test_a1_const_readonly() {
    std::printf("== 15) A1: 常量只读 ==\n");
    FakeHost h;
    check(run_src(h, "GLOBAL CONST C = 1\nC = 2\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "给常量赋值 -> 运行错误（不静默）");
}

// ---------------------------------------------------------------------------
// A3：数组即 0 结尾字符串（原程序 CMDSTR/PSTR/get_num 的核心数据模型）
// ---------------------------------------------------------------------------
static void test_a3_array_as_string() {
    std::printf("== 16) A3: 数组即字符串 / ? 简写 / WA 延时 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "GLOBAL DIM CMDSTR(64), PSTR(64)\n"
        "GLOBAL DIM f0\n"
        "CMDSTR(0) = 65\n"          // 'A'
        "CMDSTR(1) = 66\n"          // 'B'
        "CMDSTR(2) = 44\n"          // ','
        "CMDSTR(3) = 67\n"          // 'C'
        "CMDSTR(4) = 68\n"          // 'D'
        "CMDSTR(5) = 0\n"           // 0 结尾
        "?\"str=\"CMDSTR\n"         // ? 简写 + 数组当字符串整体输出
        "PRINT STRLEN(CMDSTR)\n"
        "f0 = STRFIND(CMDSTR, \",\")\n"
        "PRINT f0\n"
        "PRINT STRFIND(CMDSTR, \",\", 0)\n"
        "PRINT STRFIND(CMDSTR, \"Z\")\n"
        "PRINT STRFIND(CMDSTR, \",\", 4)\n"
        "FOR k = 0 TO 1\n"          // 取第 2 个字段 "CD" 写进 PSTR
        "  PSTR(k) = CMDSTR(f0 + 1 + k)\n"
        "NEXT k\n"
        "PSTR(2) = 0\n"
        "?\"field=\"PSTR\n"
        "PSTR(0) = 49\n"            // "12.5"
        "PSTR(1) = 50\n"
        "PSTR(2) = 46\n"
        "PSTR(3) = 53\n"
        "PSTR(4) = 0\n"
        "PRINT VAL(PSTR)\n"
        "WA 250\n"
        "DELAY 30\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "str=AB,CD"), "? 简写 + 数组当字符串整体输出");
    check(has(h.out, "5"), "STRLEN 读到 0 结尾 = 5");
    check(has(h.out, "2"), "STRFIND 返回 0 起下标 2");
    check(has(h.out, "-1"), "未命中 = -1");
    check(has(h.out, "field=CD"), "逐字节赋值后整体读回");
    check(has(h.out, "12.5"), "VAL 解析小数");
    check(h.delay_ms == 30, "WA 250 / DELAY 30 都交宿主延时（最后 = 30）");
}

// ---------------------------------------------------------------------------
// A2：子程序 SUB/END SUB —— 参数按值、结果经全局变量、LOCAL 遮蔽
// ---------------------------------------------------------------------------
static void test_a2_sub_basic() {
    std::printf("== 17) A2: 子程序参数 + 全局通信 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "GLOBAL DIM numval\n"
        "SUB get_num(k, base)\n"          // 原程序风格：结果写全局 numval
        "  numval = numval * base + k\n"
        "END SUB\n"
        "numval = 0\n"
        "get_num(1, 10)\n"
        "get_num(2, 10)\n"
        "get_num(3, 10)\n"
        "PRINT numval\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "123"), "get_num 经全局 numval 拼出 123");
}

static void test_a2_local_shadow() {
    std::printf("== 18) A2: LOCAL 遮蔽全局 + RETURN 提前返回 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "GLOBAL DIM x\n"
        "x = 100\n"
        "SUB bump(n)\n"
        "  LOCAL x\n"
        "  x = n * 2\n"
        "  PRINT x\n"
        "  RETURN\n"
        "  PRINT \"never\"\n"
        "END SUB\n"
        "bump(5)\n"
        "PRINT x\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "10"), "LOCAL x 内部 = 10");
    check(has(h.out, "100"), "全局 x 未被改动 = 100");
    check(!has(h.out, "never"), "RETURN 之后的语句未执行");
}

static void test_a2_exit_sub_and_forward_ref() {
    std::printf("== 19) A2: EXIT SUB / 前向引用 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "poke(7)\n"                       // 前向引用：SUB 定义在调用之后
        "early(1)\n"
        "early(-1)\n"
        "SUB poke(v)\n"
        "  PRINT v + 1\n"
        "END SUB\n"
        "SUB early(t)\n"
        "  IF t > 0 THEN\n"
        "    PRINT \"pos\"\n"
        "    EXIT SUB\n"
        "  ENDIF\n"
        "  PRINT \"nonpos\"\n"
        "END SUB\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "8"), "前向引用：后定义的 SUB 可被先调用");
    check(has(h.out, "pos") && has(h.out, "nonpos"), "EXIT SUB 中途返回，另一分支照常走完");
}

static void test_a2_sub_labels_and_nesting() {
    std::printf("== 20) A2: 子程序内 GOTO/标签 + 嵌套调用 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "SUB countup(n)\n"
        "  LOCAL i, s\n"
        "  s = 0\n"
        "  i = 0\n"
        "TOP:\n"
        "  i = i + 1\n"
        "  s = s + i\n"
        "  IF i < n THEN GOTO TOP\n"
        "  PRINT s\n"
        "END SUB\n"
        "SUB inner(v)\n"
        "  LOCAL r\n"
        "  r = v * 3\n"
        "  PRINT r\n"
        "END SUB\n"
        "SUB outer(v)\n"
        "  LOCAL q\n"
        "  q = v + 1\n"
        "  inner(q)\n"
        "END SUB\n"                        // 同名标签只在子程序层内解析
        "countup(4)\n"
        "outer(4)\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "10"), "子程序内 GOTO 循环 1+2+3+4=10");
    check(has(h.out, "15"), "嵌套调用 outer->inner = (4+1)*3");
}

static void test_a2_errors() {
    std::printf("== 21) A2: 子程序错误处理 ==\n");
    {
        FakeHost h;
        check(run_src(h, "SUB f(a)\nPRINT a\n") == ScriptEngine::Status::COMPILE_ERROR,
              "缺 END SUB -> 编译失败");
    }
    {
        FakeHost h;
        check(run_src(h, "SUB f(a)\n  PRINT a\nEND SUB\nf(1, 2)\n") ==
              ScriptEngine::Status::RUNTIME_ERROR,
              "实参数目不符 -> 运行错误");
    }
    {
        FakeHost h;
        check(run_src(h, "nosuch(1)\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "既非子程序也非命令 -> 运行错误");
    }
    {
        FakeHost h;   // 递归过深 -> 报错退出（不爆栈）
        check(run_src(h,
            "SUB rec(n)\n"
            "  LOCAL m\n"
            "  m = n + 1\n"
            "  rec(m)\n"
            "END SUB\n"
            "rec(0)\n") == ScriptEngine::Status::RUNTIME_ERROR,
            "无限递归被 32 层深度上限拦住");
    }
}

// ---------------------------------------------------------------------------
static void test_b1_port_io() {
    std::printf("== 22) B1: 端口 IO（OPEN / PRINT# / GET# / PUTCHAR） ==\n");
    FakeHost h;
    h.get_queue.push_back("AB,CD");     // 第一次 GET：5 字节
    h.get_queue.push_back("PQ");        // 第二次 GET：2 字节
    const auto st = run_src(h,
        "DIM BUF(16)\n"
        "DIM RX(8)\n"
        "OPEN #10, \"TCP_SERVER\", 4321\n"
        "PRINT #10, \"HELLO/n\"\n"
        "rxnum = GET #10, BUF, 16\n"
        "PRINT rxnum\n"
        "PRINT BUF\n"
        "n2 = GET #10, RX(2), 2\n"      // 带起始偏移
        "PRINT n2\n"
        "PRINT RX(2)\n"
        "PRINT RX(3)\n"
        "PRINT RX(4)\n"                 // 结尾补的 0
        "PUTCHAR #10, BUF(0, 3)\n");    // 数组切片 = 原始字节发送
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(h.opened.size() == 1 && h.opened[0] == "OPEN 10 TCP_SERVER 4321", "OPEN #10 参数透传");
    check(h.port_out.size() == 2 && h.port_out[0] == "HELLO/n", "PRINT #10 原样发送（不附加换行）");
    check(h.port_out.size() == 2 && h.port_out[1] == "AB,", "PUTCHAR #10, BUF(0,3) 发 3 字节");
    check(has(h.out, "5"), "GET 返回字节数（目标变量 rxnum）");
    check(has(h.out, "AB,CD"), "GET 写入数组后数组即字符串");
    check(has(h.out, "2"), "带偏移的 GET 也返回字节数");
    check(has(h.out, "80"), "RX(2) = 'P'");
    check(has(h.out, "81"), "RX(3) = 'Q'");
    check(has(h.out, "0"), "GET 后自动补 0 结尾");
}

static void test_b1_slice_crc16() {
    std::printf("== 23) B1: 数组切片 / CRC16 / ?* 字符串打印 ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "DIM T(8)\n"
        "T(0)=1 : T(1)=3 : T(2)=0 : T(3)=0 : T(4)=0 : T(5)=4\n"
        "crc = CRC16(T, 0, 6)\n"
        "PRINT crc\n"
        "DIM S(4)\n"
        "S(0)=72 : S(1)=105 : S(2)=0\n"
        "?*S\n"                          // ?* = 按字符串打印
        "?,, \"tail\"\n");               // 前导空输出项
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(has(h.out, "17417"), "CRC16 按发送顺序打包（0x4409）");
    check(has(h.out, "Hi"), "?* 数组按字符串打印");
    check(has(h.out, "tail"), "?,, 前导空输出项容错");
}

// ---------------------------------------------------------------------------
// RETURN 系统变量：保存最近一次设备命令/函数的返回值（IF RETURN THEN）
// ---------------------------------------------------------------------------
static void test_return_system_var() {
    std::printf("== 24) RETURN 系统变量（命令返回值） ==\n");
    FakeHost h;
    const auto st = run_src(h,
        "IF RETURN THEN PRINT \"bad_init\"\n"      // 初值 0 -> 假
        "PRINT GETVAL()\n"                          // 42
        "IF RETURN THEN PRINT \"ret_true\"\n"       // 真
        "NOOP()\n"                                  // 返回 0 -> 假
        "IF RETURN THEN\n"
        "  PRINT \"bad_noop\"\n"
        "ELSE\n"
        "  PRINT \"ret_false\"\n"
        "ENDIF\n"
        "x = GETVAL()\n"                            // 表达式位置调用也记入 RETURN
        "IF RETURN THEN PRINT \"ret_expr_true\"\n");
    check(st == ScriptEngine::Status::DONE, "正常结束");
    check(!has(h.out, "bad_init"), "初始 RETURN 为假（0）");
    check(has(h.out, "ret_true"), "GETVAL()=42 后 RETURN 为真");
    check(has(h.out, "ret_false") && !has(h.out, "bad_noop"), "NOOP 返回 0 后 RETURN 为假");
    check(has(h.out, "ret_expr_true"), "表达式位置的函数调用也更新 RETURN");
}

int main() {
    std::printf("==== script 引擎自测 ====\n");
    test_expr_assign_print();
    test_strings_and_print_semicolon();
    test_if();
    test_loops();
    test_goto_and_array();
    test_gosub();
    test_host_cmds_and_builtins();
    test_errors();
    test_budget_and_abort();
    test_vars_and_labels_introspection();
    test_a1_elseif_and_bitwise();
    test_a1_const_dim_hex();
    test_a1_exit_and_wait();
    test_a1_builtins();
    test_a1_ticks();
    test_a1_const_readonly();
    test_a3_array_as_string();
    test_a2_sub_basic();
    test_a2_local_shadow();
    test_a2_exit_sub_and_forward_ref();
    test_a2_sub_labels_and_nesting();
    test_a2_errors();
    test_b1_port_io();
    test_b1_slice_crc16();
    test_return_system_var();

    std::printf("==== %s（失败 %d）====\n", g_fail == 0 ? "ALL PASS" : "HAS FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
