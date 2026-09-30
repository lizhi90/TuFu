// motion_host_test.cpp —— 脚本设备宿主 -> kx::Shared 的绑定自测（不需要 IgH / 硬件）
//
// 用一个「假 RT 线程」消费 AxisCmd 并刷新 AxisStatus，验证：
//   EN/DIS 使能等待、MOVE 阻塞到到位并回读 POS、JOG/BUSY、DELAY、
//   以及总线未就绪时 MOVE 明确报错（不静默）。
#include "../src/script/lua_engine.h"      // LuaEngine（验证 TICKS 在 Lua 侧可用）
#include "../src/script/motion_host.h"
#include "../src/script/script.h"          // ScriptEngine（本测试用 BASIC 引擎跑脚本）

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 假 RT 线程：消费命令 + 推进定位
// ---------------------------------------------------------------------------
struct FakeRt {
    Shared sh;
    std::atomic<bool> run{true};
    std::atomic<bool> scan_ok{true};   // false = 模拟「扫不到任何设备」
    std::atomic<int>  fault_resets{0}; // DRIVE_CONTROLWORD(0)=128 / DATUM(0) 触发的清故障次数
    std::atomic<bool> hold_move{false}; // true = 定位保持运动（不自动到位），测异步/阻塞语义用
    std::atomic<bool> lin_consume{true}; // M3：false = 不消费 LIN 会话（模拟受理失败/卡住）
    std::atomic<int>    defaults_seen{0};  // ★SET_DEFAULTS 次数（连发不丢命令回归用）
    std::atomic<int>    joglead_seen{0};   // ★SET_JOG_LEAD 次数
    std::atomic<double> joglead_val{0.0};  // 最近一次 JOGLEAD 值
    std::thread th;

    void start() {
        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.axis[0].bus_ok = 1;
        }
        th = std::thread([this] { loop(); });
    }
    void stop() { run.store(false); if (th.joinable()) th.join(); }

    void loop() {
        bool     moving = false;
        int      tick = 0;
        double   target = 0.0;
        bool     enabled = false;
        int      lin_ticks = -1;      // M3：LIN 会话模拟（>=0 表示运行中）

        while (run.load()) {
            // M3：模拟 RT 受理/推进 LIN 会话（50ms 后置 done；lin_consume=false 时跳过）
            if (lin_consume.load()) {
                std::lock_guard<std::mutex> lk(sh.lin.mtx);
                if (sh.lin.state == 1) { sh.lin.state = 2; lin_ticks = 0; }
                else if (sh.lin.state == 2) {
                    if (++lin_ticks >= 50) { sh.lin.state = 3; lin_ticks = -1; }
                }
            }
            // 响应 SLOT_SCAN/SLOT_START 的重扫请求：模拟 RT 的扫描结果发布
            {
                std::lock_guard<std::mutex> lk(sh.mtx);
                if (sh.rescan_flag) {
                    sh.rescan_flag = 0;
                    BusInfo bi;
                    if (scan_ok.load()) {
                        bi.node_count         = 1;
                        bi.node[0].axis_count = 1;
                        bi.node[0].status     = 8;   // OP
                    }
                    sh.bus = bi;
                    ++sh.bus_seq;                // 模拟 RT 发布序号递增
                }
            }

            AxisCmd c = sh.take(0);
            switch (c.op) {
                case AxisCmd::ENABLE:  enabled = true;  break;
                case AxisCmd::DISABLE: enabled = false; moving = false; break;
                case AxisCmd::MOVE_ABS:
                case AxisCmd::MOVE_REL:
                    moving = true; tick = 0;
                    target = (c.op == AxisCmd::MOVE_REL) ? target + c.pos : c.pos;
                    break;
                case AxisCmd::JOG:     moving = true; tick = 0; break;
                case AxisCmd::STOP:    moving = false; break;
                case AxisCmd::SET_POS: moving = false; tick = 6; target = c.pos; break;  // 瞬时置零并改坐标
                case AxisCmd::SET_SCALE: {                                               // v0.8.1：运行时脉冲当量
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    sh.axis[0].inc_per_mm = c.pos;
                    sh.axis[0].move_result = 2;                                          // DONE
                    break;
                }
                case AxisCmd::HOME:    moving = true; tick = 0; target = 0.0; break;     // 软件回零到 0
                case AxisCmd::SET_JOG_LEAD: ++joglead_seen; joglead_val.store(c.pos); break;  // ★记录
                case AxisCmd::SET_DEFAULTS: ++defaults_seen; break;                             // ★记录
                case AxisCmd::FAULT_RESET: ++fault_resets; break;                        // 清故障：仅计数
                case AxisCmd::NONE:    break;
            }

            // 5 拍后到位；hold_move=true 时保持运动（等测试放行），用于验证 wait 参数
            if (moving && ++tick > 5 && !hold_move.load()) moving = false;

            {
                std::lock_guard<std::mutex> lk(sh.mtx);
                sh.axis[0].enabled = enabled ? 1 : 0;
                sh.axis[0].moving  = moving ? 1 : 0;
                sh.axis[0].mpos    = moving ? sh.axis[0].mpos : target;
                sh.axis[0].dpos    = target;
                sh.axis[0].idle    = moving ? 0 : 1;
                sh.axis[0].move_result = moving ? 1 : (tick > 5 ? 2 : 0);   // 1=RUNNING 2=DONE
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

static ScriptEngine::Status run(MotionHost& host, const char* src) {
    ScriptEngine e(&host);
    std::string err;
    if (!e.compile(src, &err)) { std::printf("    [编译失败] %s\n", err.c_str()); return e.status(); }
    const auto st = e.run();
    if (st != ScriptEngine::Status::DONE)
        std::printf("    [运行非正常] %s 行%d %s\n",
                    ScriptEngine::status_name(st), e.error_line(), e.error().c_str());
    return st;
}

int main() {
    std::printf("==== motion_host 绑定自测 ====\n");

    FakeRt rt;
    rt.start();

    MotionHost::Config cfg;
    cfg.poll_ms = 1;
    cfg.move_timeout_ms = 2000;
    cfg.enable_timeout_ms = 1000;
    cfg.scan_timeout_ms = 300;
    cfg.start_timeout_ms = 300;

    MotionHost host(rt.sh, cfg);
    std::vector<std::string> out;
    host.set_output([&](const std::string& s) { out.push_back(s); });

    std::printf("== 1) 使能 / 定位 / 状态 ==\n");
    const auto st1 = run(host,
        "EN\n"
        "PRINT ENABLED()\n"
        "MOVEABS 10\n"
        "PRINT POS()\n"
        "PRINT BUSY()\n"
        "MOVEABS -5\n"
        "PRINT POS()\n"
        "DIS\n"
        "PRINT ENABLED()\n");
    check(st1 == ScriptEngine::Status::DONE, "脚本正常结束");
    check(has(out, "1"), "EN 后 ENABLED()=1");
    check(has(out, "10"), "MOVEABS 10 后 POS()=10");
    check(has(out, "0"), "到位后 BUSY()=0");
    check(has(out, "-5"), "MOVEABS -5 后 POS()=-5（绝对定位）");
    check(!out.empty() && out.back() == "0", "DIS 后 ENABLED()=0");

    std::printf("== 2) 相对定位 / 点动 / 等待 ==\n");
    out.clear();
    const auto st2 = run(host,
        "EN\n"
        "MOVEABS 0\n"
        "MOVE 25\n"                  // ZBasic：MOVE = 相对（距离）
        "PRINT POS()\n"
        "MOVR 5\n"
        "PRINT POS()\n"
        "JOG 100\n"
        "DELAY 10\n"
        "PRINT BUSY()\n"
        "STOP\n"
        "DELAY 10\n"
        "PRINT BUS()\n");
    check(st2 == ScriptEngine::Status::DONE, "脚本正常结束");
    check(has(out, "25"), "MOVE 25 后 POS()=25（相对）");
    check(has(out, "30"), "MOVR 5 后 POS()=30（相对累加）");
    check(has(out, "1"), "JOG 后 BUSY()=1");
    check(has(out, "1"), "BUS()=1");

    std::printf("== 3) 轴参数 / AXIS 修饰符 / IDLE 语义 ==\n");
    out.clear();
    const auto st3 = run(host,
        "EN\n"
        "SPEED(0) = 50\n"
        "ACCEL(0) = 200\n"
        "DECEL(0) = 200\n"
        "ATYPE(0) = 65\n"
        "UNITS(0) = 14043.41\n"
        "PRINT SPEED(0)\n"
        "MOVEABS(7) AXIS(0)\n"       // 命令修饰符 AXIS(n)
        "PRINT POS()\n"
        "PRINT IDLE(0)\n"            // -1 = 到位
        "PRINT SPEED(0)\n");
    check(st3 == ScriptEngine::Status::DONE, "脚本正常结束");
    check(has(out, "50"), "SPEED(0) 写入后读回 50");
    check(has(out, "7"), "MOVEABS(7) AXIS(0) 走到 7");
    check(has(out, "-1"), "IDLE(0)=-1 表示运动结束（ZBasic 语义）");
    check(has(out, "50"), "运动后 SPEED(0) 未被改写");

    std::printf("== 3b) 单轴系统：非 0 轴明确报错（不静默回读轴 0 数据） ==\n");
    check(run(host, "AXIS(1)\n")           == ScriptEngine::Status::RUNTIME_ERROR, "AXIS(1) 明确报错");
    check(run(host, "PRINT POS(1)\n")      == ScriptEngine::Status::RUNTIME_ERROR, "POS(1) 明确报错");
    check(run(host, "PRINT DPOS(2)\n")     == ScriptEngine::Status::RUNTIME_ERROR, "DPOS(2) 明确报错");
    check(run(host, "PRINT AXISSTATUS(2)\n") == ScriptEngine::Status::RUNTIME_ERROR, "AXISSTATUS(2) 明确报错");
    check(run(host, "SPEED(3) = 10\n")     == ScriptEngine::Status::RUNTIME_ERROR, "SPEED(3) 写入明确报错");
    check(run(host, "PRINT POS(0)\n")      == ScriptEngine::Status::DONE,         "POS(0) 正常（轴 0 可用）");

    std::printf("== 4) HOME 软件回零 / 总线未就绪 ==\n");
    out.clear();
    const auto st4 = run(host, "HOME\nPRINT POS()\n");
    check(st4 == ScriptEngine::Status::DONE, "HOME 软件回零正常结束");
    check(has(out, "0"), "HOME 回零到 0");

    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 0;
    }
    out.clear();
    const auto st5 = run(host, "MOVEABS 1\n");
    check(st5 == ScriptEngine::Status::RUNTIME_ERROR, "总线未就绪时 MOVEABS 报错");

    std::printf("== 5) B 层：Modbus 寄存器 / 任务登记 / 总线信息 ==\n");
    out.clear();
    const auto st6 = run(host,
        "MODBUS_REG(4) = 1234\n"
        "PRINT MODBUS_REG(4)\n"
        "MODBUS_IEEE(10) = 3.5\n"
        "PRINT MODBUS_IEEE(10)\n"
        "RUNTASK 1, dummy_task\n"
        "PRINT PROC_STATUS(1)\n"
        "STOPTASK 1\n"
        "PRINT PROC_STATUS(1)\n"
        "PRINT NODE_COUNT(0)\n"
        "PRINT SCAN_EVENT(0)\n");
    check(st6 == ScriptEngine::Status::DONE, "脚本正常结束");
    check(has(out, "1234"), "MODBUS_REG 读写一致");
    check(has(out, "3.5"), "MODBUS_IEEE 读写一致");
    check(has(out, "1"), "RUNTASK 后 PROC_STATUS(1)=1");
    check(rt.sh.mb_regs[4] == 1234, "mb_regs[4]=1234（脚本侧寄存器镜像）");
    {
        const uint32_t bits = ((uint32_t)rt.sh.mb_regs[11] << 16) | (uint32_t)rt.sh.mb_regs[10];
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        check(std::fabs(f - 3.5f) < 1e-6f, "mb_regs[10..11]=3.5f（低字在前）");
    }
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        check(rt.sh.task[1].running == 0, "STOPTASK 后 task[1] 已停止");
    }
    check(!out.empty() && out.back() == "0", "NODE_COUNT/SCAN_EVENT 未扫描时返回 0");

    std::printf("== 5b) B 层：总线信息来自 Shared::bus 快照 ==\n");
    {
        // 模拟总线层（main.cpp 的 scan_bus）填好的扫描结果
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.bus.node_count         = 2;
        rt.sh.bus.node[0].axis_count = 1;    // 轴从站（本项目单轴）
        rt.sh.bus.node[0].status     = 8;    // OP(0x08)
        rt.sh.bus.node[0].io_base    = 10;
        rt.sh.bus.node[0].in_count   = 16;
        rt.sh.bus.node[0].out_count  = 8;
        rt.sh.bus.node[0].aio_base   = 20;
        rt.sh.bus.node[0].ad_count   = 4;
        rt.sh.bus.node[0].da_count   = 2;
        rt.sh.bus.node[1].axis_count = 0;    // 非轴从站
        rt.sh.bus.node[1].status     = 4;    // SAFEOP(0x04)
    }
    out.clear();
    const auto st7 = run(host,
        "PRINT \"NODE_COUNT=\" + STR(NODE_COUNT(0))\n"
        "PRINT \"AXES0=\" + STR(NODE_AXIS_COUNT(0,0))\n"
        "PRINT \"AXES1=\" + STR(NODE_AXIS_COUNT(0,1))\n"
        "PRINT \"ST0=\" + STR(NODE_STATUS(0,0))\n"
        "PRINT \"IO0=\" + STR(NODE_IO(0,0))\n"
        "PRINT \"AIO0=\" + STR(NODE_AIO(0,0))\n"
        "PRINT \"IN0=\" + STR(NODE_INFO(0,0,10))\n"
        "PRINT \"OUT0=\" + STR(NODE_INFO(0,0,11))\n"
        "PRINT \"AD0=\" + STR(NODE_INFO(10,0,12))\n"
        "PRINT \"DA0=\" + STR(NODE_INFO(10,0,13))\n"
        "?*ETHERCAT(0)\n");
    check(st7 == ScriptEngine::Status::DONE, "脚本正常结束（总线信息）");
    check(has(out, "NODE_COUNT=2"), "NODE_COUNT 取自 Shared::bus 快照");
    check(has(out, "AXES0=1"), "NODE_AXIS_COUNT(0,0)=1（轴从站）");
    check(has(out, "AXES1=0"), "NODE_AXIS_COUNT(0,1)=0（非轴从站）");
    check(has(out, "ST0=8"), "NODE_STATUS 取自快照（OP=0x08）");
    check(has(out, "IO0=10"), "NODE_IO 取自快照");
    check(has(out, "AIO0=20"), "NODE_AIO 取自快照");
    check(has(out, "IN0=16") && has(out, "OUT0=8"), "NODE_INFO IN/OUT 个数");
    check(has(out, "AD0=4") && has(out, "DA0=2"), "NODE_INFO AD/DA 个数");
    check(has(out, "node0:axis=1,status=8,io=10"), "?*ETHERCAT(ii) 诊断串");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);   // 复位，避免影响后续用例
        rt.sh.bus = BusInfo{};
    }

    std::printf("== 5c) B 层：SLOT_SCAN/SLOT_START/SLOT_STOP 与 RETURN ====\n");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 1;      // 上一节曾置 0；恢复总线就绪
        rt.sh.bus = BusInfo{};
    }
    out.clear();
    const auto st8 = run(host,
        "SLOT_SCAN 0\n"
        "IF RETURN THEN\n"
        "  PRINT \"SCAN_OK \" + STR(NODE_COUNT(0))\n"
        "ELSE\n"
        "  PRINT \"SCAN_FAIL\"\n"
        "ENDIF\n"
        "SLOT_START 0\n"
        "IF RETURN THEN PRINT \"START_OK\"\n"
        "SLOT_STOP 0\n"
        "IF RETURN THEN PRINT \"STOP_OK\"\n");
    check(st8 == ScriptEngine::Status::DONE, "脚本正常结束（SLOT_*）");
    check(has(out, "SCAN_OK 1"), "SLOT_SCAN 成功：RETURN=从站数，NODE_COUNT 可读");
    check(!has(out, "SCAN_FAIL"), "SLOT_SCAN 未误报失败");
    check(has(out, "START_OK"), "SLOT_START 成功（bus_ok 且已有从站）→ RETURN 为真");
    check(has(out, "STOP_OK"), "SLOT_STOP → RETURN 为真");

    rt.scan_ok.store(false);        // 模拟扫不到设备
    out.clear();
    const auto st9 = run(host,
        "SLOT_SCAN 0\n"
        "IF RETURN THEN\n"
        "  PRINT \"BAD_SCAN\"\n"
        "ELSE\n"
        "  PRINT \"SCAN_FAIL\"\n"
        "ENDIF\n");
    check(st9 == ScriptEngine::Status::DONE, "扫描失败不报运行错误（走 ELSE 分支）");
    check(has(out, "SCAN_FAIL") && !has(out, "BAD_SCAN"), "未扫到设备时 RETURN=0");
    rt.scan_ok.store(true);

    std::printf("== 5d) DATUM：0=清错（下发 FAULT_RESET），3=置零；其余模式明确报错 ====\n");
    check(run(host, "DATUM 0\n") == ScriptEngine::Status::DONE, "DATUM(0) 清控制器错误：下发 FAULT_RESET");
    out.clear();
    check(run(host, "DATUM 3\nPRINT POS()\n") == ScriptEngine::Status::DONE, "DATUM(3) 当前位置置零：接受并下发");
    check(has(out, "0"), "DATUM(3) 后 POS()=0（坐标系已置零）");
    check(run(host, "DATUM 1\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "DATUM(1) 未定案模式：明确报错（不静默退化）");

    std::printf("== 5f) 定位第 4 参 wait=0：异步下发立即返回（端口服务脚本用） ====\n");
    {
        auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        rt.hold_move.store(true);
        out.clear();
        const long long t0 = now_ms();
        const auto stx = run(host, "SPEED(0)=10\nMOVEABS(5, 10, 100, 0)\nDELAY 10\nPRINT BUSY()\n");
        const long long el = now_ms() - t0;
        check(stx == ScriptEngine::Status::DONE, "wait=0 脚本正常结束（不等到位）");
        check(el < 100, "wait=0 立即返回（定位被 hold 也未阻塞）");
        check(has(out, "1"), "异步定位后 BUSY()=1（运动已下发）");

        // 对照：wait=1 在 hold 期间阻塞不返回；放行后才结束
        std::atomic<bool> done{false};
        std::thread th([&] {
            (void)run(host, "MOVEABS(0, 10, 100, 1)\n");
            done.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(!done.load(), "wait=1 阻塞等待到位（hold 期间未返回）");
        rt.hold_move.store(false);      // 放行：5 拍后到位
        const long long t1 = now_ms();
        while (!done.load() && now_ms() - t1 < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(done.load(), "放行后阻塞定位返回");
        th.join();
    }

    std::printf("== 5g) TICKS：Lua 侧毫秒倒数计时（差值=经过时间） ====\n");
    {
        LuaEngine lua(&host);
        std::string err;
        check(lua.compile("A = TICKS()", &err), "Lua 编译 TICKS 调用");
        check(lua.run(0) == IScriptEngine::Status::DONE, "Lua 运行 TICKS 调用");
        const double a = lua.get_var("A").to_num();
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
        check(lua.compile("B = TICKS()", &err), "Lua 再次编译 TICKS 调用");
        check(lua.run(0) == IScriptEngine::Status::DONE, "Lua 再次运行 TICKS 调用");
        const double b = lua.get_var("B").to_num();
        check(b < a, "TICKS 单调递减（倒数计时，同 BASIC 语义）");
        check((a - b) >= 8.0, "两次 TICKS 差值 ≈ 经过毫秒（>=8ms）");
    }

    std::printf("== 5h) UNITS：脉冲当量由脚本运行时设置（控制器无默认） ====\n");
    {
        // 写：UNITS(0, 1234.5) → 下发内核 SET_SCALE；读：返回内核值
        out.clear();
        const int err0 = host.errors();
        const auto st = run(host, "UNITS(0, 1234.5)\nPRINT UNITS(0)\n");
        check(st == ScriptEngine::Status::DONE, "UNITS 写/读脚本正常结束");
        check(has(out, "1234.5") || has(out, "1234.50"), "UNITS 读取返回内核生效值（1234.5）");
        check(host.errors() == err0, "UNITS 写入无新增错误");

        // 非法值（0）→ 明确报错（不静默）
        out.clear();
        const auto st2 = run(host, "UNITS(0, 0)\n");
        check(st2 == ScriptEngine::Status::RUNTIME_ERROR, "UNITS(0,0) 明确报错");
    }

    std::printf("== 5i) SRAMP / FASTDEC / VP_SPEED（速度曲线 API，2026-09-27） ====\n");
    {
        // SRAMP：0~250 合法；越界明确报错；读回原值（ms）
        out.clear();
        const int err0 = host.errors();
        check(run(host, "SRAMP(0, 100)\nPRINT SRAMP(0)\n") == ScriptEngine::Status::DONE,
              "SRAMP(0,100) 写入/回读正常");
        check(has(out, "100"), "SRAMP 读回 100（ms）");
        check(host.errors() == err0, "SRAMP 写入无新增错误");
        check(run(host, "SRAMP(0, 300)\n") == ScriptEngine::Status::RUNTIME_ERROR,
              "SRAMP 越界（300ms）明确报错");

        // FASTDEC：>=0；读回（mm/s²）
        out.clear();
        check(run(host, "FASTDEC(0, 800)\nPRINT FASTDEC(0)\n") == ScriptEngine::Status::DONE,
              "FASTDEC(0,800) 写入/回读正常");
        check(has(out, "800"), "FASTDEC 读回 800");

        // VP_SPEED：只读速度（内核快照 vel_mm）
        {
            std::lock_guard<std::mutex> lk(rt.sh.mtx);
            rt.sh.axis[0].vel_mm = 42.5;
        }
        out.clear();
        check(run(host, "PRINT VP_SPEED(0)\n") == ScriptEngine::Status::DONE, "VP_SPEED 读取正常");
        check(has(out, "42.5"), "VP_SPEED 读回快照值 42.5");

        // SPEED/ACCEL/DECEL 写值同步内核缺省（SET_DEFAULTS 下发）
        check(run(host, "SPEED(0, 66)\nACCEL(0, 700)\nDECEL(0, 650)\n") ==
                  ScriptEngine::Status::DONE, "SPEED/ACCEL/DECEL 写入正常");
    }

    std::printf("== 5e) AXISSTATUS 位映射回读（脚本用 MV_ERRMASK 判报警） ====\n");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].axis_status = 0x8u | 0x400000u;   // 驱动器故障 + 告警输入
    }
    out.clear();
    check(run(host,
        "PRINT \"AS=\" + STR(AXISSTATUS(0))\n"
        "IF (AXISSTATUS(0) AND 6575932) <> 0 THEN PRINT \"ALARM\"\n") == ScriptEngine::Status::DONE,
        "AXISSTATUS 读回不报错");
    check(has(out, "AS=4194312"), "AXISSTATUS = 0x400008 原样回读");
    check(has(out, "ALARM"), "与脚本 MV_ERRMASK(6575932) 求与非 0");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].axis_status = 0;                  // 复位
    }
    out.clear();
    check(run(host,
        "IF (AXISSTATUS(0) AND 6575932) <> 0 THEN\n"
        "  PRINT \"ALARM\"\n"
        "ELSE\n"
        "  PRINT \"OK\"\n"
        "ENDIF\n") == ScriptEngine::Status::DONE,
        "无错误时 AXISSTATUS 走 ELSE");
    check(has(out, "OK") && !has(out, "ALARM"), "无错误 -> MV_ERRMASK 求与 = 0");

    std::printf("== 5f) VMOVE / CANCEL / DRIVE_CONTROLWORD / SDO_WRITE（绝不静默） ====\n");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // 等此前命令（含 5d 的 DATUM 0）被假 RT 消费完
    const int fr0 = rt.fault_resets.load();
    out.clear();
    check(run(host,
        "SPEED(0) = 30\n"
        "VMOVE(1) AXIS(0)\n"             // 连续正转 -> 内核 JOG(+30)
        "DELAY 10\n"                     // 命令单槽，须给 RT 消费时间（同遗留脚本的 delay(10)）
        "VMOVE(-1) AXIS(0)\n"            // 连续反转 -> 内核 JOG(-30)
        "DELAY 10\n"
        "CANCEL(2)\n"                    // 取消 -> 内核 STOP
        "DELAY 10\n"
        "DRIVE_CONTROLWORD(0) = 128\n"   // 0x80 清错 -> FAULT_RESET（旧版会静默退化为数组赋值）
        "DELAY 10\n"
        "DRIVE_CONTROLWORD(0) = 6\n"     // 0x06 关机 -> 去使能
        "DELAY 10\n"
        "DRIVE_CONTROLWORD(0) = 15\n"    // 0x0F 使能
        "DELAY 10\n")
        == ScriptEngine::Status::DONE, "VMOVE/CANCEL/DRIVE_CONTROLWORD 标准序列正常结束");
    int fr1 = fr0;                                   // 假 RT 异步消费命令：轮询等待 FAULT_RESET 生效
    for (int i = 0; i < 200 && fr1 == fr0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        fr1 = rt.fault_resets.load();
    }
    check(fr1 >= fr0 + 1,
          "DRIVE_CONTROLWORD(0)=128 真实触发 FAULT_RESET（不再静默退化为数组赋值）");

    check(run(host, "DRIVE_CONTROLWORD(0) = 99\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "DRIVE_CONTROLWORD 非标准值明确报错");
    check(run(host, "DRIVE_CONTROLWORD(1) = 128\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "DRIVE_CONTROLWORD 非 0 轴明确报错");
    check(run(host, "SDO_WRITE (0,0,$6060,0,2,0)\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "SDO_WRITE 明确报错（脚本宿主无 ecrt 通道）");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 0;
    }
    check(run(host, "VMOVE(1) AXIS(0)\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "总线未就绪时 VMOVE 报错");
    {
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 1;      // 复位，避免影响后续用例
    }

    std::printf("== 6) B 层：越界 / 未打开通道（明确报错，不静默） ==\n");
    check(run(host, "MODBUS_REG(300) = 1\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "MODBUS_REG 下标越界明确报错");
    check(run(host, "RUNTASK 99, x\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "RUNTASK 任务号越界明确报错");
    out.clear();
    check(run(host, "PRINT PORT_STATUS(10)\n") == ScriptEngine::Status::DONE,
          "未打开通道 PORT_STATUS 不报错（返回 0）");
    check(has(out, "0"), "PORT_STATUS(未打开)=0");

    // ---- M3：多轴直线插补请求 + MOTION_MODE ----
    std::printf("== 7) M3：BASE 多轴 + MOVEABS 多目标（LIN 请求） ==\n");
    {
        rt.sh.axis_count = 2;    // 扩到两轴
        std::lock_guard<std::mutex> lk(rt.sh.mtx);
        rt.sh.axis[0].bus_ok = 1;
        rt.sh.axis[1].bus_ok = 1;
        rt.sh.axis[1].enabled = 1;
        rt.sh.axis[0].enabled = 1;
    }
    out.clear();
    // MOTION_MODE：全局切换（SET_MOTION_MODE post 到所有轴；FakeRt 只消费轴 0，轴 1 命令留存可断言）
    check(run(host, "MOTION_MODE 1\n") == ScriptEngine::Status::DONE, "MOTION_MODE 1 执行成功");
    check(rt.sh.axis_cmd[1].op == AxisCmd::SET_MOTION_MODE, "SET_MOTION_MODE 已 post 到轴 1");
    check(rt.sh.axis_cmd[1].pos == 1.0, "SET_MOTION_MODE pos=1 (CSP)");
    run(host, "MOTION_MODE 0\n");

    // BASE 多轴 + 多目标 MOVEABS（wait=0 异步，FakeRt 不消费 LIN 会话）
    out.clear();
    check(run(host, "BASE 0,1\nMOVEABS 10,20,100\n") == ScriptEngine::Status::DONE,
          "BASE 0,1 + 多目标 MOVEABS 阻塞到完成");
    {
        std::lock_guard<std::mutex> lk(rt.sh.lin.mtx);
        check(rt.sh.lin.state == 3, "LinReq 终态=3（done）");
        check(rt.sh.lin.seq == 1, "LinReq seq=1");
        check(rt.sh.lin.n == 2, "LinReq 轴数=2");
        check(rt.sh.lin.axis[0] == 0 && rt.sh.lin.axis[1] == 1, "LinReq 轴表 0,1");
        check(std::fabs(rt.sh.lin.target_mm[0] - 10.0) < 1e-9 &&
              std::fabs(rt.sh.lin.target_mm[1] - 20.0) < 1e-9, "LinReq 目标 10/20");
        check(std::fabs(rt.sh.lin.speed - 100.0) < 1e-9, "LinReq 主导速度=100");
    }
    // 第二次请求（seq 递增）
    check(run(host, "MOVEABS 1,2,50\n") == ScriptEngine::Status::DONE, "第二次插补（seq=2）");
    {
        std::lock_guard<std::mutex> lk(rt.sh.lin.mtx);
        check(rt.sh.lin.seq == 2 && rt.sh.lin.state == 3, "第二次 seq=2 state=3");
    }
    // 失败路径：暂停 FakeRt 的 LIN 消费 → 发请求（state=1）→ 手动置 4（模拟受理失败）→ 宿主报错
    rt.lin_consume.store(false);
    // 后台线程：200ms 后把 state 置 4（模拟 RT 受理校验失败）
    std::thread([&rt] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lk(rt.sh.lin.mtx);
        rt.sh.lin.state = 4;
        std::snprintf(rt.sh.lin.err, sizeof(rt.sh.lin.err), "轴未使能（模拟）");
    }).detach();
    check(run(host, "MOVEABS 3,4,50\n") == ScriptEngine::Status::RUNTIME_ERROR,
          "受理失败（state=4）→ 宿主报错");
    rt.lin_consume.store(true);
    {
        std::lock_guard<std::mutex> lk(rt.sh.lin.mtx);
        rt.sh.lin.state = 0;      // 复位
    }
    // BASE 回单轴
    check(run(host, "BASE 0\n") == ScriptEngine::Status::DONE, "BASE 0 回单轴");

    // ★post_wait：快速连发不丢命令（2026-09-29；旧「覆盖式」槽位只会留下最后一条）
    {
        const int d0 = rt.defaults_seen.load(), jl0 = rt.joglead_seen.load();
        check(run(host, "JOGLEAD 0, 0.5\nSPEED 0, 100\nJOG 0, 100\n") == ScriptEngine::Status::DONE,
              "连发 JOGLEAD+SPEED+JOG 脚本执行成功");
        // 等内核消费
        for (int i = 0; i < 100 && (rt.joglead_seen.load() == jl0 || rt.defaults_seen.load() == d0); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(rt.joglead_seen.load() > jl0, "连发不丢命令：SET_JOG_LEAD 已到达内核");
        check(rt.defaults_seen.load() > d0, "连发不丢命令：SET_DEFAULTS 已到达内核");
        check(rt.joglead_val.load() == 0.5, "JOGLEAD 值正确（0.5）");
    }

    rt.stop();
    std::printf("==== %s（失败 %d）====\n", g_fail == 0 ? "ALL PASS" : "HAS FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
