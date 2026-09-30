// axis_test.cpp —— 增量4 闭环逻辑自测（不需要 IgH / 硬件）
//
// 用一个"假驱动器 FakeDrive"模拟 SV630 的 CiA402 行为 + PP/PV 运动，
// 把 Cia402Axis + Axis 整条链路跑起来，验证：
//   1) 使能序列 -> enabled
//   2) PP 绝对定位：bit4 触发 / bit12 应答 / bit10 到位 -> move_done
//   3) PP 相对定位：bit6=1 且终点 = 起点 + 距离
//   4) PV 点动：60FF 有符号速度生效、STOP 后归零
//   5) 定位超时 -> move_error
//   6) 驱动器故障 -> alarm=1 / FAULT
//   7) 运动中重复定位 -> busy（被忽略）
//   8) 轮廓参数 6081/6083/6084 计算与 dirty 标记
//   9) AXISSTATUS 位映射（手册 6.3）：通讯/驱动器故障/随动超限/告警
//  10) 坐标系置零 SET_POS（DATUM(3)）与软件回零 HOME 的偏移语义
#include "../src/motion/axis.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kx;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// ---------------------------------------------------------------------------
// 假驱动器
// ---------------------------------------------------------------------------
class FakeDrive {
public:
    enum St { SWITCH_ON_DISABLED = 0x40, READY = 0x21, SWITCHED_ON = 0x23,
              OP_ENABLED = 0x27, QUICK_STOP = 0x07, FAULT = 0x08 };

    static constexpr double DT = 0.001;   // 1ms

    int     st          = SWITCH_ON_DISABLED;
    int32_t pos         = 0;      // 0x6064
    int32_t demand      = 0;      // 0x6062
    int8_t  mode_disp   = 0;
    bool    fault_flag  = false;
    bool    warning_flag = false; // bit7  告警
    bool    follow_err_flag = false; // bit13 随动误差
    bool    reached     = true;   // bit10
    bool    ack         = false;  // bit12
    bool    freeze      = false;  // true = 电机不转（模拟超时）
    bool    mode_valid  = true;   // v0.8.1: 0x6061 是否上报（SV630 默认档位不上报）
    bool    demand_valid = true;  // v0.8.1: 0x6062 是否上报（未映射时 DPOS 取内核目标）
    int32_t target      = 0;
    int32_t last_raw    = 0;      // 上一次写入的 607A 原始值
    int32_t vel         = 0;
    int32_t pv          = 0;      // 由 test 从 axis.params() 灌入（模拟 SDO）
    bool    bit4_prev   = false;

    AxisPdoIn in() const {
        AxisPdoIn i;
        i.status_word = build_sw();
        i.pos_actual  = pos;
        i.pos_demand  = demand;
        i.pos_demand_valid = demand_valid;
        i.mode_disp   = mode_disp;
        i.mode_disp_valid = mode_valid;
        return i;
    }

    void step(const AxisPdoOut& o) {
        // --- 清故障 ---
        if (o.control_word & 0x0080) { fault_flag = false; st = SWITCH_ON_DISABLED; }

        // --- CiA402 状态迁移 ---
        const uint16_t c = (uint16_t)(o.control_word & 0x000F);
        if (!fault_flag) {
            switch (st) {
            case SWITCH_ON_DISABLED:
                if (c == 0x06 || c == 0x07 || c == 0x0F) st = READY;
                break;
            case READY:
                if (c == 0x07 || c == 0x0F) st = SWITCHED_ON;
                else if (!(c & 0x02)) st = SWITCH_ON_DISABLED;
                break;
            case SWITCHED_ON:
                if (c == 0x0F) st = OP_ENABLED;
                else if (!(c & 0x02)) st = SWITCH_ON_DISABLED;
                break;
            case OP_ENABLED:
                if (!(c & 0x02)) st = SWITCH_ON_DISABLED;
                else if (!(c & 0x04)) st = QUICK_STOP;
                else if (c == 0x07) st = SWITCHED_ON;
                break;
            case QUICK_STOP:
                if (!(c & 0x02)) st = SWITCH_ON_DISABLED;
                else if (c == 0x0F) st = OP_ENABLED;
                break;
            default: break;
            }
        }
        if (fault_flag) st = FAULT;

        // --- 模式回显 ---
        mode_disp = o.mode;

        const bool halt = (o.control_word & 0x0100) != 0;

        if (st == OP_ENABLED && !halt) {
            if (o.mode == 1) {                                  // PP
                const bool b4  = (o.control_word & 0x0010) != 0;
                const bool rel = (o.control_word & 0x0040) != 0;   // bit6 相对定位
                if (b4 && (!bit4_prev || o.target_pos != last_raw)) {
                    // 相对定位：驱动器把 607A 当作“距离”，叠加到当前目标上
                    target   = rel ? (demand + o.target_pos) : o.target_pos;
                    last_raw = o.target_pos;
                    reached  = false;
                    ack      = true;
                }
                bit4_prev = b4;
                if (!b4 && !reached) ack = false;
                if (!reached) {
                    if (freeze || pv <= 0) {
                        // 不动
                    } else {
                        int32_t step_inc = (int32_t)((double)pv * DT);
                        if (step_inc < 1) step_inc = 1;
                        int32_t diff = target - demand;
                        if (std::abs(diff) <= step_inc) demand = target;
                        else demand += (diff > 0 ? step_inc : -step_inc);
                        if (demand == target) reached = true;
                    }
                }
            } else if (o.mode == 3) {                           // PV
                vel = o.target_vel;
                if (!freeze) {
                    double d = (double)vel * DT;
                    demand += (int32_t)(d >= 0 ? std::floor(d) : std::ceil(d));
                } else {
                    demand += 0;
                }
                target  = demand;
                reached = (vel == 0);
                ack     = false;
                bit4_prev = false;
            } else if (o.mode == 8) {                           // CSP：理想跟随（每拍对齐目标）
                demand  = o.target_pos;
                target  = demand;
                reached = true;
                ack     = false;
                bit4_prev = false;
            }
        } else {
            bit4_prev = false;
            if (halt && st == OP_ENABLED) reached = true;
        }

        pos = demand;   // 忽略跟随滞后，简化
    }

private:
    uint16_t build_sw() const {
        uint16_t sw = 0x0200;   // bit9 remote = 1
        sw |= 0x0020;           // bit5 quick-stop ok = 1
        switch (st) {
        case SWITCH_ON_DISABLED: sw |= 0x0040; break;
        case READY:              sw |= 0x0001; break;
        case SWITCHED_ON:        sw |= 0x0003; break;
        case OP_ENABLED:         sw |= 0x0007; break;
        case QUICK_STOP:         sw |= 0x0001 | 0x0002; break;
        case FAULT:              sw |= 0x0008; break;
        }
        if (st == OP_ENABLED || st == SWITCHED_ON || st == READY) sw |= 0x0010;  // bit4 voltage
        if (fault_flag) sw |= 0x0008;
        if (warning_flag)    sw |= 0x0080;   // bit7
        if (follow_err_flag) sw |= 0x2000;   // bit13
        if (reached)    sw |= 0x0400;   // bit10
        if (ack)        sw |= 0x1000;   // bit12
        return sw;
    }
};

// ---------------------------------------------------------------------------
struct Rig {
    FakeDrive   drv;
    Axis        ax;
    AxisPdoOut  out{};
    bool        saw_bit4 = false;
    bool        saw_bit5 = false;   // PP 立即更新（bit5）
    bool        saw_bit6 = false;

    void init(double timeout_ms = 10000.0, uint32_t settle_ms = 200, double inc = 14043.41) {
        AxisConfig c;
        c.inc_per_mm = inc;    // v0.8.1：当量由使用方给出（控制器无默认；传 0 = 未设置）
        c.def_speed  = 100.0;
        c.def_accel  = 500.0;
        c.spd_max    = 3276.7;
        c.tol_mm     = 0.05;
        c.timeout_ms = (uint32_t)timeout_ms;
        c.settle_ms  = settle_ms;
        c.mode_wait_ms = 200;

        Cia402Axis::Config cc;
        cc.stage_timeout_ms = 500;
        cc.reset_pulse_ms   = 20;
        cc.default_mode     = 1;
        ax.init(c, cc);
        ax.set_bus_ok(true);
    }

    void cycle() {
        AxisPdoIn in = drv.in();
        ax.tick(in, &out);
        if (out.control_word & CW_NEW_SETPOINT) saw_bit4 = true;
        if (out.control_word & CW_CHANGE_SET_IMM) saw_bit5 = true;
        if (out.control_word & CW_CHANGE_SET)   saw_bit6 = true;
        drv.pv = ax.params().profile_vel;   // 模拟 6081 经 SDO 下发
        drv.step(out);
    }

    void run(int n) { for (int i = 0; i < n; ++i) cycle(); }

    // 跑到 move_result 不再是 RUNNING，返回跑了多少拍
    int run_until_settled(int max_cycles) {
        for (int i = 0; i < max_cycles; ++i) {
            cycle();
            if (ax.move_result() != MoveResult::RUNNING) return i + 1;
        }
        return max_cycles;
    }
};

static void send(Rig& r, AxisCmd::Op op, double pos = 0, double spd = 0) {
    AxisCmd c;
    c.op = op; c.pos = pos; c.speed = spd;
    r.ax.apply(c);
}

// ---------------------------------------------------------------------------
static void test_enable() {
    std::printf("== 1) 使能与单位换算 ==\n");
    Rig r; r.init();

    check(std::fabs(r.ax.inc_to_mm(r.ax.mm_to_inc(100.0)) - 100.0) < 1e-6, "mm->inc->mm 往返一致");
    check(r.ax.mm_to_inc(1.0) == 14043, "1mm = 14043 inc (四舍五入)");

    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "ENABLE 后进入使能");
    check(r.ax.state_name()[0] == 'R', "状态为 READY");
    check(r.ax.status().bus_ok == 1, "bus_ok=1");
    check(r.ax.status().mpos == 0.0, "初始位置 0mm");

    send(r, AxisCmd::DISABLE);
    r.run(30);
    check(!r.ax.enabled(), "DISABLE 后去使能");
}

static void test_move_abs() {
    std::printf("== 2) PP 绝对定位 ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "已使能");

    send(r, AxisCmd::MOVE_ABS, 100.0, 100.0);   // 100mm @100mm/s -> 约 1s
    check(r.ax.move_result() == MoveResult::RUNNING, "命令后结果=RUNNING");

    int n = r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "move_done");
    check(r.saw_bit4, "PP 触发位 bit4 曾置 1");
    check(!r.saw_bit6, "绝对定位未置 bit6");
    check(std::fabs(r.ax.status().mpos - 100.0) <= 0.05, "MPOS 到位 (<=0.05mm)");
    check(r.ax.status().idle == 1, "idle=1");
    std::printf("      耗时 %d 拍，MPOS=%.4f mm，target=%.4f mm\n",
                n, r.ax.status().mpos, r.ax.target_mm());
}

// v0.8.1 回归：SV630 档位1 不含 0x6061 —— 未映射时不得做模式门控（否则 PP 永远超时不动）
static void test_move_without_mode_disp() {
    std::printf("== 2b) PP 定位：档案未映射 0x6061（不做模式门控） ==\n");
    Rig r; r.init();
    r.drv.mode_valid = false;
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "已使能");

    send(r, AxisCmd::MOVE_ABS, 50.0, 100.0);
    const int n = r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "未映射 6061 也能正常定位（不再模式超时）");
    check(std::fabs(r.ax.status().mpos - 50.0) <= 0.05, "MPOS 到位 (<=0.05mm)");
    // 6062 也未映射：DPOS 应回落为内核目标位置（而非 0）
    r.drv.demand_valid = false;
    send(r, AxisCmd::MOVE_ABS, 20.0, 100.0);
    r.run_until_settled(6000);
    check(std::fabs(r.ax.status().dpos - 20.0) <= 0.05, "未映射 6062 时 DPOS=目标位置（不回 0）");
    std::printf("      耗时 %d 拍，MPOS=%.4f mm\n", n, r.ax.status().mpos);
}

// v0.8.1：控制器不设脉冲当量默认值；未设置时运动命令明确拒绝，运行时由 UNITS/SET_SCALE 设置
static void test_scale_unset_and_runtime() {
    std::printf("== 2c) 脉冲当量：无默认（未设置拒绝运动 / 运行时设置） ==\n");
    Rig r; r.init(10000.0, 200, 0.0);      // 未设置当量
    send(r, AxisCmd::ENABLE);
    r.run(50);
    send(r, AxisCmd::MOVE_ABS, 10.0, 50.0);
    r.run(5);
    check(r.ax.move_result() == MoveResult::ERROR, "未设置当量：定位被明确拒绝");
    check(!r.ax.scale_set(), "scale_set() = false");

    AxisCmd s; s.op = AxisCmd::SET_SCALE; s.pos = 1000.0;   // 空闲可设
    r.ax.apply(s); r.run(2);
    check(r.ax.scale_set() && std::fabs(r.ax.status().inc_per_mm - 1000.0) < 1e-9,
          "SET_SCALE 生效并发布到状态（inc_per_mm=1000）");

    send(r, AxisCmd::MOVE_ABS, 10.0, 50.0);
    check(r.ax.move_result() == MoveResult::RUNNING, "设置后可下发定位");
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "定位完成（按 1000 inc/mm 换算）");
    check(std::abs(r.ax.mm_to_inc(10.0) - 10000) <= 1, "mm_to_inc 按新当量换算");

    send(r, AxisCmd::JOG, 0.0, 10.0); r.run(20);
    AxisCmd s2; s2.op = AxisCmd::SET_SCALE; s2.pos = 2000.0;
    r.ax.apply(s2); r.run(2);
    check(r.ax.move_result() == MoveResult::ERROR, "运动中改当量被拒绝");
    check(std::fabs(r.ax.status().inc_per_mm - 1000.0) < 1e-9, "拒绝后当量保持不变");
    send(r, AxisCmd::STOP);
}

static void test_move_rel() {
    std::printf("== 3) PP 相对定位 ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);

    send(r, AxisCmd::MOVE_ABS, 50.0, 200.0);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "先绝对定位到 50mm");
    double p1 = r.ax.status().mpos;

    r.ax.clear_result();
    r.saw_bit6 = false;
    send(r, AxisCmd::MOVE_REL, 25.0, 200.0);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "相对定位 move_done");
    check(r.saw_bit6, "相对定位置了 bit6");
    check(std::fabs(r.ax.status().mpos - (p1 + 25.0)) <= 0.05, "终点 = 起点 + 距离");
    check(std::fabs(r.ax.target_mm() - (p1 + 25.0)) <= 1e-6, "target_mm 记录正确");
}

static void test_jog() {
    std::printf("== 4) PV 点动 ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);

    send(r, AxisCmd::JOG, 0.0, 50.0);       // 正向 50mm/s
    r.run(200);                             // 0.2s -> 约 10mm
    double p = r.ax.status().mpos;
    check(p > 5.0 && p < 15.0, "正向点动位置推进约 10mm");
    check((r.out.control_word & CW_HALT) == 0, "点动未置 halt");
    check(std::fabs(r.ax.status().dpos - p) <= 0.2, "点动时 DPOS 跟随推进（面板可见）");

    send(r, AxisCmd::JOG, 0.0, -30.0);      // 反向
    r.run(200);
    check(r.ax.status().mpos < p, "反向点动使位置回退");

    send(r, AxisCmd::STOP);
    r.run(5);
    check(r.out.target_vel == 0, "STOP 后 60FF = 0");
    check(r.ax.state_name()[0] == 'S', "状态 STOPPED");
}

// v0.8.1 回归：RxPDO 无 0x60FF（SV630 默认档位）时，点动走 PP 目标跟随
static void test_jog_pp_follow() {
    std::printf("== 4b) 点动（60FF 未映射 → PP 目标跟随） ==\n");
    Rig r; r.init();
    r.ax.set_vel_pdo_mapped(false);
    send(r, AxisCmd::ENABLE);
    r.run(50);

    // 固件无默认前视：未设置时 PP 点动明确拒绝（与 UNITS 同纪律）
    send(r, AxisCmd::JOG, 0.0, 50.0);
    r.run(20);
    check(r.ax.state_name()[0] != 'J' || std::fabs(r.ax.status().mpos) < 0.01,
          "点动前视未设置 → 不进入点动（拒绝）");
    send(r, AxisCmd::STOP);
    r.run(5);

    send(r, AxisCmd::SET_JOG_LEAD, 0.5);     // 脚本 JOGLEAD(0, 0.5) 等效
    send(r, AxisCmd::JOG, 0.0, 50.0);        // 正向 50mm/s
    r.run(200);                              // 0.2s ≈ 10mm
    const double p = r.ax.status().mpos;
    std::printf("      0.2s 后 MPOS=%.4f mm\n", p);
    check(p > 5.0 && p < 15.0, "PP 跟随点动推进约 10mm");
    check((r.out.control_word & CW_HALT) == 0, "点动未置 halt");
    check(std::fabs(r.ax.status().dpos - p) <= 0.2, "点动时 DPOS 跟随（6062 回显）");
    {
        // 2026-09-27：驱动器目标前视 = 0.5s 行程 + 减速距离 v²/(2a)：50mm/s、a=500 → 25 + 2.5 = 27.5mm
        const double lead_mm =
            (double)(r.out.target_pos - (int32_t)llround(p * 14043.41)) / 14043.41;
        std::printf("      驱动器目标前视=%.2f mm\n", lead_mm);
        check(lead_mm > 20.0 && lead_mm < 35.0, "点动前视 ≥0.5s+减速距离（消除追目标式抖动）");
        check(r.saw_bit5, "PP 新目标脉冲同时置 bit5（立即更新，SV630 单点模式会顿）");
    }
    send(r, AxisCmd::JOG, 0.0, -30.0);       // 反向
    r.run(200);
    check(r.ax.status().mpos < p, "反向点动使位置回退");

    // JOGLEAD 运行时可调：0.2s → 前视 = 0.2·50 + 2.5 = 12.5mm
    send(r, AxisCmd::SET_JOG_LEAD, 0.2);
    send(r, AxisCmd::JOG, 0.0, 50.0);
    r.run(100);
    {
        const double p2 = r.ax.status().mpos;
        const double lead2 = (double)(r.out.target_pos - (int32_t)llround(p2 * 14043.41)) / 14043.41;
        std::printf("      JOGLEAD=0.2s 前视=%.2f mm\n", lead2);
        check(lead2 > 10.0 && lead2 < 15.0, "JOGLEAD 运行时设置生效（0.2s → 12.5mm）");
    }
    send(r, AxisCmd::SET_JOG_LEAD, 0.5);   // 复位默认，避免影响后续用例


    send(r, AxisCmd::STOP);
    r.run(5);
    check(r.out.target_vel == 0, "STOP 后 60FF = 0");
    check(r.ax.state_name()[0] == 'S', "状态 STOPPED");
    check(std::fabs(r.ax.status().dpos - r.ax.status().mpos) <= 0.05, "停止后 DPOS 对齐实际位置");
}


// 2026-09-27：SRAMP（S 曲线）/ FASTDEC（停机减速度）/ VP_SPEED（速度读回）/ 缺省参数下发
static void test_sramp_fastdec_vel() {
    std::printf("== 14b) SRAMP / FASTDEC / VP_SPEED / SET_DEFAULTS ==\n");

    // ---- 梯形 vs S 曲线（同距离），VP_SPEED 中途采样 ----
    {
        Rig r; r.init();
        r.ax.set_csp_mode(true);
        send(r, AxisCmd::ENABLE);
        r.run(50);

        send(r, AxisCmd::MOVE_ABS, 50.0, 100.0);       // 梯形（SRAMP=0）
        const int n_trap = r.run_until_settled(20000);
        check(r.ax.move_result() == MoveResult::DONE && n_trap < 20000, "梯形 CSP 定位完成");

        send(r, AxisCmd::SET_SRAMP, 0.1);              // S 曲线 100ms
        check(std::fabs(r.ax.sramp() - 0.1) < 1e-9, "SRAMP 设置生效（100ms）");
        send(r, AxisCmd::MOVE_ABS, 100.0, 100.0);      // 同距离 50mm
        r.run(250);                                     // ≈巡航段
        const double v_mid = r.ax.status().vel_mm;
        std::printf("      巡航 vel_mm=%.2f mm/s\n", v_mid);
        check(std::fabs(v_mid - 100.0) < 20.0, "VP_SPEED：巡航速度 ≈100mm/s");
        const int n_rest = r.run_until_settled(20000);
        check(r.ax.move_result() == MoveResult::DONE, "S 曲线 CSP 定位完成");
        check(std::fabs(r.ax.status().mpos - 100.0) <= 0.05, "S 曲线到位 MPOS=100");
        const int n_s = 250 + (n_rest >= 20000 ? 0 : n_rest);
        std::printf("      梯形=%d 拍，S 曲线=%d 拍\n", n_trap, n_s);
        check(n_s > n_trap, "S 曲线用时 > 梯形（jerk 平滑）");
        check(n_s < n_trap + 400, "S 曲线延时 ≤ 2×SRAMP 上限（200ms）");
        r.run(30);
        check(std::fabs(r.ax.status().vel_mm) < 2.0, "就位后 vel_mm ≈0");
        send(r, AxisCmd::SET_SRAMP, 0.0);
    }

    // ---- FASTDEC：CSP STOP 按减速度减速（非瞬时停）----
    {
        Rig r; r.init();
        r.ax.set_csp_mode(true);
        send(r, AxisCmd::ENABLE);
        r.run(50);
        send(r, AxisCmd::SET_FASTDEC, 500.0);
        check(std::fabs(r.ax.fastdec() - 500.0) < 1e-9, "FASTDEC 设置生效（500mm/s²）");
        send(r, AxisCmd::MOVE_ABS, 200.0, 100.0);
        r.run(200);                                     // 进入巡航
        const double p_stop = r.ax.status().mpos;
        send(r, AxisCmd::STOP);
        r.run(40);
        const double p_mid = r.ax.status().mpos;
        std::printf("      STOP 后 40ms：%.3f → %.3f mm\n", p_stop, p_mid);
        check(p_mid > p_stop + 0.5, "FASTDEC：STOP 后仍在减速前行（非瞬时停）");
        int n = 0;
        while (n < 4000 && r.ax.state_name()[0] != 'R') { r.cycle(); ++n; }
        check(r.ax.state_name()[0] == 'R', "FASTDEC 停机完成 → READY");
        check(r.ax.move_result() != MoveResult::DONE, "停机不报 move_done（保持 NONE）");
        check(std::fabs(r.ax.status().vel_mm) < 2.0, "停机完成后 vel_mm ≈0");
        const double d_total = r.ax.status().mpos - p_stop;
        std::printf("      停机距离≈%.3f mm（理论 v²/2a=10mm）\n", d_total);
        check(d_total > 6.0 && d_total < 14.0, "停机距离量级 ≈ v²/(2a)");
    }

    // ---- SET_DEFAULTS：缺省速度/加减速下发内核（MOVE 不给参时使用）----
    {
        Rig r; r.init();
        r.ax.set_csp_mode(true);
        send(r, AxisCmd::ENABLE);
        r.run(50);
        AxisCmd c; c.axis = 0; c.op = AxisCmd::SET_DEFAULTS;
        c.speed = 80.0; c.accel = 800.0; c.pos = 600.0;
        r.ax.apply(c);
        r.run(5);
        send(r, AxisCmd::MOVE_ABS, 20.0, 0.0);          // 速度=0 → 用缺省
        r.run(5);
        const AxisParams& pp = r.ax.params();
        check(pp.profile_vel == (int32_t)llround(80.0 * 14043.41), "SET_DEFAULTS：缺省速度 80mm/s 生效");
        check(pp.profile_acc == (int32_t)llround(800.0 * 14043.41), "SET_DEFAULTS：缺省加速度 800 生效");
        check(pp.profile_dec == (int32_t)llround(600.0 * 14043.41), "SET_DEFAULTS：减速度 600（6084）生效");
        send(r, AxisCmd::STOP);
        r.run(5);
    }
}

static void test_timeout() {
    std::printf("== 5) 定位超时 ==\n");
    Rig r; r.init(300.0);      // 300ms 超时
    send(r, AxisCmd::ENABLE);
    r.run(50);
    r.drv.freeze = true;       // 电机不转
    send(r, AxisCmd::MOVE_ABS, 100.0, 100.0);
    int n = r.run_until_settled(3000);
    check(r.ax.move_result() == MoveResult::ERROR, "超时 -> move_error");
    check(r.ax.status().idle == 0, "未到位 idle=0");
    std::printf("      超时用时 %d 拍，错误：%s\n", n, r.ax.last_error());
}

static void test_fault() {
    std::printf("== 6) 驱动器故障 ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "先使能");

    r.drv.fault_flag = true;   // 驱动器报警
    r.run(5);
    check(r.ax.status().alarm == 1, "alarm=1");
    check(std::strcmp(r.ax.state_name(), "FAULT") == 0, "状态 FAULT");
    check(!r.ax.enabled(), "报警后不再使能");

    // 清故障恢复
    r.drv.fault_flag = false;
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "清故障后重新使能成功");
    check(r.ax.status().alarm == 0, "alarm 归 0");
}

static void test_busy() {
    std::printf("== 7) 运动中重复定位 -> busy ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    send(r, AxisCmd::MOVE_ABS, 300.0, 100.0);
    r.run(50);
    check(r.ax.moving(), "正在运动");
    check(r.ax.move_result() == MoveResult::RUNNING, "结果 RUNNING");

    send(r, AxisCmd::MOVE_ABS, 10.0, 100.0);   // 应被忽略（服务层回 busy）
    check(r.ax.target_mm() == 300.0, "重复命令被忽略，目标未改");
    check(r.ax.move_result() == MoveResult::RUNNING, "原运动未被打断");

    int n = r.run_until_settled(6000);
    check(n > 0 && r.ax.move_result() == MoveResult::DONE, "原运动继续完成");
    check(std::fabs(r.ax.status().mpos - 300.0) <= 0.05, "终点仍是第一个目标 300mm");
}

static void test_params() {
    std::printf("== 8) 轮廓参数（6081/6083/6084）==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(!r.ax.params().dirty, "空闲时 dirty=0");

    send(r, AxisCmd::MOVE_ABS, 10.0, 120.0);
    const AxisParams& p = r.ax.params();
    check(p.dirty, "定位后 dirty=1（需要下发）");
    check(p.profile_vel == (int32_t)llround(120.0 * 14043.41), "6081 = 120mm/s 换算");
    check(p.profile_acc == (int32_t)llround(500.0 * 14043.41), "6083 = 默认加速度换算");
    check(p.profile_dec == p.profile_acc, "6084 = 6083");

    r.ax.params_flushed();
    check(!r.ax.params().dirty, "params_flushed() 清 dirty");

    // 速度上限保护
    Rig r2; r2.init();
    send(r2, AxisCmd::ENABLE);
    r2.run(50);
    send(r2, AxisCmd::MOVE_ABS, 10.0, 99999.0);
    check(r2.ax.params().profile_vel == (int32_t)llround(3276.7 * 14043.41), "超速被钳到 MV_SPD_MAX");
}

// AXISSTATUS 位映射（ZBasic 手册 6.3）；脚本 MV_ERRMASK = 6575932
static void test_axisstatus() {
    std::printf("== 9) AXISSTATUS 位映射（手册 6.3）==\n");
    const unsigned int kErrMask = 6575932u;   // 脚本 EtherCAT_SocketServer.bas:102

    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.status().axis_status == 0, "正常使能后 axis_status=0");

    r.drv.fault_flag = true;                  // 驱动器报警
    r.run(3);
    check((r.ax.status().axis_status & 0x8u) != 0, "驱动器故障 -> bit3(8)");
    check((r.ax.status().axis_status & kErrMask) != 0, "与脚本 MV_ERRMASK 求与非 0（脚本判 move_error）");

    r.drv.fault_flag = false;                 // 清故障
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check((r.ax.status().axis_status & 0x8u) == 0, "清故障后 bit3 归 0");

    r.drv.warning_flag = true;                // 6041.bit7 告警
    r.run(3);
    check((r.ax.status().axis_status & 0x400000u) != 0, "6041.bit7 -> bit22(4194304)");
    r.drv.warning_flag = false;

    r.drv.follow_err_flag = true;             // 6041.bit13 随动误差
    r.run(3);
    check((r.ax.status().axis_status & 0x100u) != 0, "6041.bit13 -> bit8(256)");
    r.drv.follow_err_flag = false;

    r.ax.set_bus_ok(false);                   // 总线断开
    r.run(3);
    check((r.ax.status().axis_status & 0x4u) != 0, "总线断 -> bit2(4) 通讯出错");
    check((r.ax.status().axis_status & kErrMask) != 0, "总线断亦命中 MV_ERRMASK");
}

// 坐标系置零（SET_POS / DATUM(3)）与软件回零（HOME）：验证偏移只作用于用户坐标
static void test_setpos_home() {
    std::printf("== 10) 坐标系置零 SET_POS / 软件回零 HOME ==\n");
    Rig r; r.init();
    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "已使能");

    send(r, AxisCmd::MOVE_ABS, 100.0, 200.0);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "先绝对定位到 100mm");
    check(std::fabs(r.ax.status().mpos - 100.0) <= 0.05, "MPOS=100mm");

    // DATUM(3)：把当前位置置为用户坐标 0（坐标系偏移，电机不动）
    send(r, AxisCmd::SET_POS, 0.0);
    r.run(1);
    check(r.ax.move_result() == MoveResult::DONE, "SET_POS 瞬时完成 -> DONE");
    check(std::fabs(r.ax.status().mpos) <= 1e-6, "置零后 MPOS=0（用户坐标）");
    check(std::fabs(r.ax.status().dpos) <= 1e-6, "置零后 DPOS=0");

    // 置零后：相对定位距离不受偏移影响
    r.ax.clear_result();
    send(r, AxisCmd::MOVE_REL, 10.0, 100.0);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "置零后相对定位 move_done");
    check(std::fabs(r.ax.status().mpos - 10.0) <= 0.05, "置零后 MOVE 10 -> MPOS=10");

    // 置零后：绝对定位按用户坐标
    r.ax.clear_result();
    send(r, AxisCmd::MOVE_ABS, 30.0, 100.0);
    r.run_until_settled(6000);
    check(std::fabs(r.ax.status().mpos - 30.0) <= 0.05, "置零后 MOVEABS 30 -> MPOS=30");

    // 软件回零：绝对定位回用户坐标 0
    r.ax.clear_result();
    send(r, AxisCmd::HOME);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "HOME move_done");
    check(std::fabs(r.ax.status().mpos) <= 0.05, "HOME 回到用户坐标 0");
}

// ---------------------------------------------------------------------------
// M3：CSP 定位（6060=8，控制器每拍规划 607A；完成=规划完+实际进容差）
static void test_csp_move() {
    std::printf("== 14) M3 CSP 绝对定位 ==\n");
    Rig r; r.init();
    r.ax.set_csp_mode(true);
    check(r.ax.csp_mode(), "csp_mode 已开");

    send(r, AxisCmd::ENABLE);
    r.run(50);
    check(r.ax.enabled(), "已使能");

    send(r, AxisCmd::MOVE_ABS, 50.0, 100.0);
    check(r.ax.move_result() == MoveResult::RUNNING, "CSP 命令后 RUNNING");

    // 前几拍：模式应为 8（CSP），目标位置应逐步推进（不是一次性终值）
    double first_tp = 0, prev_tp = 0;
    bool mode8_seen = false, tp_advanced = false;
    for (int i = 0; i < 20; ++i) {
        r.cycle();
        if ((int)r.out.mode == 8) mode8_seen = true;
        if (i == 0)      first_tp = r.out.target_pos;
        if (i > 0 && r.out.target_pos > prev_tp) tp_advanced = true;
        prev_tp = r.out.target_pos;
    }
    check(mode8_seen, "6060 输出 = 8 (CSP)");
    check(first_tp != 14043 * 50, "607A 首拍不是规划终值（每拍渐进）");
    check(tp_advanced, "607A 每拍推进");

    int n = r.run_until_settled(12000);
    check(n < 12000, "CSP 定位在超时内结束");
    check(r.ax.move_result() == MoveResult::DONE, "CSP move_done");
    check(std::fabs(r.ax.status().mpos - 50.0) <= 0.05, "CSP 到位 MPOS=50");
    // DPOS 应跟随规划（收尾时 = 目标）
    check(std::fabs(r.ax.status().dpos - 50.0) <= 0.1, "CSP DPOS 收敛到目标");

    // 模式切回 PP 后定位仍正常（回到旧路径）
    r.ax.set_csp_mode(false);
    r.ax.clear_result();
    send(r, AxisCmd::MOVE_ABS, 10.0, 100.0);
    r.run_until_settled(6000);
    check(r.ax.move_result() == MoveResult::DONE, "切回 PP 后定位 move_done");
    check(std::fabs(r.ax.status().mpos - 10.0) <= 0.05, "切回 PP 后到位 MPOS=10");
}

// M3：CSP 模式下 STOP 立即对齐实际位置
static void test_csp_stop() {
    std::printf("== 15) M3 CSP 运动中 STOP ==\n");
    Rig r; r.init();
    r.ax.set_csp_mode(true);
    send(r, AxisCmd::ENABLE);
    r.run(50);
    send(r, AxisCmd::MOVE_ABS, 100.0, 50.0);   // 慢速长行程
    r.run(300);                                 // 走 300ms
    send(r, AxisCmd::STOP);
    r.run(10);
    check(r.ax.state_name()[0] == 'S', "STOP 后进入 STOPPED");
    // DPOS 已对齐实际位置
    check(std::fabs(r.ax.status().dpos - r.ax.status().mpos) <= 0.05, "STOP 后 DPOS 对齐 MPOS");
}

int main() {
    std::printf("---- axis 闭源自测（含假驱动器）----\n");
    test_enable();
    test_move_abs();
    test_move_without_mode_disp();
    test_scale_unset_and_runtime();
    test_move_rel();
    test_jog();
    test_jog_pp_follow();
    test_sramp_fastdec_vel();
    test_timeout();
    test_fault();
    test_busy();
    test_params();
    test_axisstatus();
    test_setpos_home();
    test_csp_move();
    test_csp_stop();
    std::printf("---- %s (失败项 %d) ----\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
