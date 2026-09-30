// ecat_axis.cpp —— M2 运动内核实机验证工具
//
// 目的：在真 SV630 上验证 docs/planA/04 的运动内核（真实 Axis + Cia402Axis + EthercatMaster）：
//   ① CiA402 使能序列（0x80 -> 0x06 -> 0x07 -> 0x0F）与逐级状态位等待；
//   ② PP（0x6060=1）点位：0x607A 目标 + bit4(new setpoint) 触发 + bit12 应答 + bit10 到位；
//   ③ 去使能（回 0x06）与位置回读（0x6064 / INC_PER_MM -> mm）；
//   ④ PV（0x6060=3）点动：60FF 走 SDO，验证速度环能持续走；
//   ⑤ 急停：0x6040.bit8(halt) 叠加在 PDO 控制字上，验证随时可停。
//
// 与 tools/ecat_probe.cpp 的区别：probe 恒写 ctrl_word=0（纯观察、绝对安全）；
// 本工具在 --enable 时会**真的使能、必要时真的让电机转动**。
//
// 安全约定：
//   * 不带 --enable 时与 probe 等价（只跑 1ms 循环、只观察）。
//   * --move 默认 5mm @ 5mm/s，且走完先回到"使能瞬间的位置"，再卸力。
//   * --jog 默认 5mm/s 走 2s，然后速度归零减速停机，再卸力。
//   * --estop 在点动加速到速后再发 halt，验证急停能在极短距离内停住。
//   * 任何超时/报警都会立即去使能并打印原因。
//
// 用法：
//   sudo ./ecat_axis                                  # 只观察（安全）
//   sudo ./ecat_axis --enable                         # 仅使能，不走位，结束自动去使能
//   sudo ./ecat_axis --enable --move 5 --speed 5      # 使能 + 走 5mm + 回原位 + 去使能
//   sudo ./ecat_axis --enable --move 5 --rel          # 相对定位（置 bit6）
//   sudo ./ecat_axis --jog 5 --jog-secs 2             # PV 点动 5mm/s 走 2s 后减速停
//   sudo ./ecat_axis --jog 5 --estop                  # PV 点动到速后急停（halt）
//   sudo ./ecat_axis --enable --keep                  # 结束后保持使能（仅供调试）
#include "../src/motion/ethercat_master.h"
#include "../src/motion/axis.h"
#include "../src/motion/rt_util.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using namespace kx;

namespace {

std::atomic<bool> g_run{true};

// PV 点动速度信箱：0x60FF 在档位1/2 未映射 PDO，改由 SDO 线程下发（点动只需一次）。
std::atomic<int32_t> g_vel_req{0};
std::atomic<int32_t> g_vel_sent{INT32_MIN};
double               g_inc_per_mm = 14043.41;   // 仅供 60FF 打印换算

// 未映射 PDO 的偏移哨兵（读前必须判断，否则越界访问 domain）
inline bool off_ok(unsigned int o) { return o != EthercatMaster::kOffsetInvalid; }

// ---------------------------------------------------------------------------
// SDO 参数信箱：6081/6083/6084 只能走 SDO 且阻塞，不能放 RT 线程
// ---------------------------------------------------------------------------
struct ParamBox {
    std::mutex        m;
    AxisParams        p{};
    std::atomic<bool> has_new{false};
    std::atomic<bool> done{false};

    void post(const AxisParams& v) {
        std::lock_guard<std::mutex> lk(m);
        p = v;
        has_new.store(true);
    }
    bool take(AxisParams* out) {
        if (!has_new.load()) return false;
        std::lock_guard<std::mutex> lk(m);
        *out = p;
        has_new.store(false);
        return true;
    }
    void mark_done() { done.store(true); }
    bool consume_done() { return done.exchange(false); }
};

const char* mr_name(MoveResult r) {
    switch (r) {
        case MoveResult::NONE:    return "NONE";
        case MoveResult::RUNNING: return "RUNNING";
        case MoveResult::DONE:    return "DONE";
        case MoveResult::ERROR:   return "ERROR";
    }
    return "?";
}

void sdo_thread(EthercatMaster* ec, ParamBox* box) {
    while (g_run.load()) {
        AxisParams p;
        if (box->take(&p)) {
            uint32_t a1 = 0, a2 = 0, a3 = 0;
            const bool ok1 = ec->sdo_download_u32(0x6081, 0, (uint32_t)p.profile_vel, 32, &a1);
            const bool ok2 = ec->sdo_download_u32(0x6083, 0, (uint32_t)p.profile_acc, 32, &a2);
            const bool ok3 = ec->sdo_download_u32(0x6084, 0, (uint32_t)p.profile_dec, 32, &a3);
            std::printf("[m2][sdo] 6081=%d 6083=%d 6084=%d -> %s%s%s%s\n",
                        p.profile_vel, p.profile_acc, p.profile_dec,
                        (ok1 && ok2 && ok3) ? "全部 OK" : "部分失败",
                        ok1 ? "" : " [6081 失败]", ok2 ? "" : " [6083 失败]",
                        ok3 ? "" : " [6084 失败]");
            if (!ok1) std::printf("[m2][sdo]   6081 abort=0x%08x\n", a1);
            if (!ok2) std::printf("[m2][sdo]   6083 abort=0x%08x\n", a2);
            if (!ok3) std::printf("[m2][sdo]   6084 abort=0x%08x\n", a3);
            box->mark_done();
        }
        // 0x60FF（PV 目标速度）：档位未映射 PDO 时走 SDO；值变化才下发
        const int32_t vreq = g_vel_req.load();
        if (vreq != g_vel_sent.load()) {
            uint32_t ab = 0;
            if (ec->sdo_download_u32(0x60FF, 0, (uint32_t)vreq, 32, &ab)) {
                g_vel_sent.store(vreq);
                std::printf("[m2][sdo] 0x60FF = %d inc/s (%.4f mm/s) OK\n",
                            vreq, (double)vreq / g_inc_per_mm);
            } else {
                std::printf("[m2][sdo] 0x60FF = %d 失败 abort=0x%08x\n", vreq, ab);
                g_vel_sent.store(vreq);   // 避免刷屏；下一相位再触发
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---------------------------------------------------------------------------
struct Opt {
    double   seconds  = 0.0;      // 0 = 自动（使能/走位 20s，观察 10s）
    bool     enable   = false;
    bool     have_move = false;
    double   dist_mm  = 5.0;
    bool     relative = false;
    double   speed    = 5.0;
    double   accel    = 100.0;
    bool     have_jog = false;
    bool     jog_speed_set = false;
    double   jog_speed = 5.0;     // mm/s（可负）
    double   jog_secs  = 2.0;     // 点动持续秒数
    bool     estop     = false;   // 点动到速后急停（halt）
    int      profile  = 1;
    uint32_t dc       = 0x0300;
    uint32_t vid      = 0x00100000;
    uint32_t pid      = 0x000c0112;
    double   inc_per_mm = 14043.41;
    bool     keep     = false;    // 结束后不去使能（调试）
};

void usage(const char* a0) {
    std::printf(
        "用法: %s [选项]\n"
        "  --enable            使能轴（不带则只观察，安全）\n"
        "  --move MM           使能后走 MM 毫米（隐含 --enable），走完回起始位\n"
        "  --rel               相对定位（默认绝对）\n"
        "  --speed MM_S        速度（默认 5 mm/s）\n"
        "  --accel MM_S2       加速度（默认 100 mm/s^2）\n"
        "  --jog MM_S          PV(6060=3) 点动速度（隐含 --enable；需 --profile >=2）\n"
        "  --jog-secs N        点动持续秒数（默认 2）\n"
        "  --estop             点动到速后下发急停 6040.bit8(halt)（隐含 --jog）\n"
        "  --inc-per-mm V      脉冲当量（默认 14043.41）\n"
        "  --profile N         显式 PDO 档位（默认 1；点动自动升到 2）\n"
        "  --dc 0x0300         DC 配置（0 = 关）\n"
        "  --vid/--pid 0x..    SII 匹配用 vendor/product\n"
        "  --keep              结束后保持使能（调试；默认去使能）\n"
        "  --secs N            运行上限秒数（0 = 自动）\n", a0);
}

} // namespace

int main(int argc, char** argv) {
    Opt o;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (!std::strcmp(a, "--enable")) o.enable = true;
        else if (!std::strcmp(a, "--keep"))   o.keep = true;
        else if (!std::strcmp(a, "--rel"))    o.relative = true;
        else if (!std::strcmp(a, "--move")   && i + 1 < argc) { o.have_move = true; o.enable = true; o.dist_mm = std::atof(argv[++i]); }
        else if (!std::strcmp(a, "--jog")    && i + 1 < argc) { o.have_jog = true; o.enable = true; o.jog_speed_set = true; o.jog_speed = std::atof(argv[++i]); }
        else if (!std::strcmp(a, "--jog-secs") && i + 1 < argc) o.jog_secs = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--estop")) { o.estop = true; o.have_jog = true; o.enable = true; }
        else if (!std::strcmp(a, "--speed")  && i + 1 < argc) o.speed = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--accel")  && i + 1 < argc) o.accel = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--inc-per-mm") && i + 1 < argc) o.inc_per_mm = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--profile") && i + 1 < argc) o.profile = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--dc")     && i + 1 < argc) o.dc = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(a, "--vid")    && i + 1 < argc) o.vid = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(a, "--pid")    && i + 1 < argc) o.pid = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(a, "--secs")   && i + 1 < argc) o.seconds = std::atof(argv[++i]);
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "未知参数: %s\n", a); usage(argv[0]); return 2; }
    }

    if (o.estop && !o.jog_speed_set) o.jog_speed = o.speed;   // --estop 未指定速度则复用 --speed
    // PV 点动需要读 0x6061 模式回显来确认已进 PV；档位1 无 6061，自动升到档位2
    if (o.have_jog && o.profile < 2) {
        std::printf("[m2] ⚠ 点动需 0x6061 模式回显，PDO 档位 %d -> 2\n", o.profile);
        o.profile = 2;
    }

    if (o.seconds <= 0.0) o.seconds = o.enable ? 20.0 : 10.0;

    // 绑核 CPU3 + SCHED_FIFO 80（非 root 时仅告警，继续跑）
    rt_setup(3, 80);
    DmaLatencyGuard dma;

    // ---- 主站 ----
    EthercatConfig cfg;
    cfg.master_index     = 0;
    cfg.slave_position[0] = 0;   // 单轴工具：轴 0 对应环上第 0 个从站（其余沿用构造默认）
    cfg.vendor_id        = o.vid;
    cfg.product_code     = o.pid;
    cfg.cycle_ns         = 1000000;
    cfg.dc_assign        = o.dc;
    cfg.dc_shift_ns      = 500000;
    cfg.use_explicit_pdo = true;
    cfg.explicit_profile = o.profile;
    cfg.select_ref_clock = (o.dc != 0);
    cfg.wd_divider       = 0;
    cfg.wd_intervals     = 0;

    EthercatMaster ec;
    AxisPdoOffset off;
    if (!ec.init(cfg, &off)) {
        std::fprintf(stderr, "[m2] EtherCAT 初始化失败（先跑 ecat_probe 排查）\n");
        return 1;
    }
    std::printf("[m2] EtherCAT 就绪（profile=%d DC=%s）\n", o.profile, o.dc ? "开" : "关");

    // ---- 运动内核 ----
    AxisConfig axc;
    axc.inc_per_mm = o.inc_per_mm;
    axc.def_speed  = 10.0;
    axc.def_accel  = 100.0;
    axc.spd_max    = 3276.7;
    axc.tol_mm     = 0.05;
    axc.timeout_ms = 10000;
    axc.mode_wait_ms = 1500;   // PP<->PV 切换留足时间

    Cia402Axis::Config c402c;
    c402c.default_mode = 1;   // PP

    Axis axis;
    axis.init(axc, c402c);

    ParamBox box;
    std::thread th_sdo(sdo_thread, &ec, &box);

    if (o.have_jog)
        std::printf("[m2] 模式=JOG_PV%s，速度=%.3f mm/s，持续=%.2fs，INC_PER_MM=%.5f\n",
                    o.estop ? "+ESTOP" : "", o.jog_speed, o.jog_secs, o.inc_per_mm);
    else
        std::printf("[m2] 模式=%s，目标=%.3f mm @ %.1f mm/s，accel=%.1f mm/s^2，INC_PER_MM=%.5f\n",
                    o.have_move ? (o.relative ? "MOVE_REL" : "MOVE_ABS") : "仅观察/使能",
                    o.dist_mm, o.speed, o.accel, o.inc_per_mm);
    if (!o.enable) std::printf("[m2] 未指定 --enable：ctrl_word 恒 0，仅观察（不会动）\n");

    // ---- 时序状态机 ----
    enum class Phase { OBSERVE, ENABLING, MOVE_OUT, MOVE_BACK,
                       JOG_RUN, JOG_STOP, ESTOP_HOLD, DISABLE, DONE };
    const char* ph_names[] = {"OBSERVE", "ENABLING", "MOVE_OUT", "MOVE_BACK",
                              "JOG_RUN", "JOG_STOP", "ESTOP_HOLD", "DISABLE", "DONE"};

    Phase     ph       = Phase::OBSERVE;
    long long ph_t0    = 0;          // 进入当前相位的时间
    bool      dis_sent = false;
    double    start_mm = 0.0;        // 使能瞬间的实际位置
    bool      params_sent = false;

    bool  ok_enable = false, ok_move = false, ok_back = false;
    bool  ok_jog = false, ok_jog_stop = false, ok_estop = false;
    const char* fail_reason = "";

    // PV 点动上下文
    long long jog_vel_t      = 0;    // 60FF 已下发时刻（0 = 未下发）
    int32_t   jog_start_inc  = 0;    // 下发速度瞬间位置
    bool      jog_moved      = false;
    long long jstop_t        = 0;    // 减速停稳判定参考时刻
    int32_t   jstop_ref      = 0;
    int32_t   halt_inc       = 0;    // 急停下发瞬间位置
    bool      es_have_ref    = false;
    int32_t   es_ref_inc     = 0;
    uint16_t  prev_cw        = 0;    // 上一拍实际写出的控制字（供日志判断 bit8）

    // 急停响应时间实测（halt 下发拍 → 速度首次降到阈值拍）
    long long halt_t_ns      = 0;    // 下发 6040.bit8=1 的时刻
    long long es_stop_t_ns   = 0;    // 速度首次落入阈值的时刻（0 = 未判定）
    int32_t   es_prev_inc    = 0;    // 上一拍位置（差分求速度）
    double    es_v0_mmps     = 0.0;  // halt 瞬间的理论速度（mm/s）
    double    es_v_stop      = 0.0;  // 判定停止时的瞬时速度（mm/s）
    double    es_stop_dist   = 0.0;  // halt 到判定停止的位移（mm）

    const long long t0        = mono_ns();
    long long       wake      = t0 + cfg.cycle_ns;
    const long long period    = cfg.cycle_ns;
    const long long enable_to = 8000LL * 1000000LL;   // 使能等待上限 8s
    const long long move_to   = 15000LL * 1000000LL;  // 单次定位上限 15s
    long long       count = 0, over = 0;
    char last_state[40] = "";
    bool bus_said = false;
    long long bus_since = 0;    // 首次进入 OP 的时刻（使能要等 OP 稳定后再发）

    while (g_run.load()) {
        sleep_until(wake);
        const long long now = mono_ns();
        if (o.dc != 0) ec.set_app_time((uint64_t)now);

        ec.receive();

        EthercatMaster::BusState bs;
        ec.read_state(&bs);
        const bool bus_ok = bs.ok();
        if (bus_ok && !bus_said) {
            std::printf("[m2] 从站进入 OP（AL=0x%02x，t=%.3fs）\n",
                        bs.slave_al, (double)(now - t0) / 1e9);
            bus_said  = true;
            bus_since = now;
        }
        if (!bus_ok && bus_said) {
            std::printf("[m2] ⚠ 从站掉出 OP（AL=0x%02x）\n", bs.slave_al);
            bus_said = false;
        }
        axis.set_bus_ok(bus_ok);

        // ---- 读 TxPDO ----
        AxisPdoIn in;
        in.status_word = off_ok(off.status_word) ? ec.read_u16(off.status_word) : 0;
        in.pos_actual  = off_ok(off.pos_actual)  ? ec.read_s32(off.pos_actual)  : 0;
        in.pos_demand  = off_ok(off.pos_demand)  ? ec.read_s32(off.pos_demand)  : 0;
        in.mode_disp   = off_ok(off.mode_disp)   ? (int8_t)ec.read_u8(off.mode_disp) : 0;

        // ---- CiA402 状态迁移日志（M2 关键证据链）----
        {
            const Cia402State s = cia402_decode(in.status_word);
            const char* sn = cia402_state_name(s);
            if (std::strcmp(sn, last_state) != 0) {
                std::printf("[m2] t=%6.3fs CiA402 -> %-22s sw=0x%04x\n",
                            (double)(now - t0) / 1e9, sn, in.status_word);
                std::snprintf(last_state, sizeof last_state, "%s", sn);
            }
        }

        // ---- 业务：按时序下发命令 ----
        switch (ph) {
        case Phase::OBSERVE:
            // 等从站进 OP 且稳定 500ms 后再使能（PREOP 下写控制字无效、PDO 数据不可信）
            if (o.enable && bus_said && (now - bus_since) >= 500LL * 1000000LL) {
                AxisCmd c; c.op = AxisCmd::ENABLE;
                axis.apply(c);
                std::printf("[m2] 下发 ENABLE（期望 0x1650 -> 0x1631 -> 0x1633 -> 0x1637）\n");
                ph = Phase::ENABLING; ph_t0 = now;
            }
            break;

        case Phase::ENABLING:
            if (axis.enabled()) {
                ok_enable = true;
                start_mm  = axis.status().mpos;
                std::printf("[m2] ✔ 已使能，耗时 %.0f ms，起始位置 %.4f mm，sw=0x%04x\n",
                            (double)(now - ph_t0) / 1e6, start_mm, in.status_word);
                if (o.have_jog) {
                    AxisCmd c; c.op = AxisCmd::JOG; c.speed = o.jog_speed;
                    axis.apply(c);
                    std::printf("[m2] 下发 JOG(PV) %.3f mm/s（等 6061==3 后经 SDO 下发 0x60FF）\n",
                                o.jog_speed);
                    ph = Phase::JOG_RUN; ph_t0 = now;
                } else if (o.have_move) {
                    AxisCmd c;
                    c.op    = o.relative ? AxisCmd::MOVE_REL : AxisCmd::MOVE_ABS;
                    c.pos   = o.relative ? o.dist_mm : (start_mm + o.dist_mm);
                    c.speed = o.speed;
                    c.accel = o.accel;
                    axis.apply(c);
                    std::printf("[m2] 下发 %s -> %.4f mm @ %.1f mm/s\n",
                                o.relative ? "MOVE_REL" : "MOVE_ABS", c.pos, o.speed);
                    ph = Phase::MOVE_OUT; ph_t0 = now;
                } else {
                    ph = Phase::DISABLE; ph_t0 = now;
                }
            } else if ((now - ph_t0) > enable_to) {
                fail_reason = "使能超时（5s 未进 Operation enabled）";
                std::printf("[m2] ✘ %s；cia402=%s\n", fail_reason, axis.cia402().last_error());
                ph = Phase::DISABLE; ph_t0 = now;
            }
            break;

        case Phase::MOVE_OUT: {
            const MoveResult r = axis.move_result();
            if (r == MoveResult::DONE) {
                ok_move = true;
                std::printf("[m2] ✔ 定位完成：mpos=%.4f mm（目标 %.4f），耗时 %.0f ms，"
                            "bit10/bit12=1\n", axis.status().mpos, axis.target_mm(),
                            (double)(now - ph_t0) / 1e6);
                AxisCmd c; c.op = AxisCmd::MOVE_ABS; c.pos = start_mm;
                c.speed = o.speed; c.accel = o.accel;
                axis.apply(c);
                std::printf("[m2] 回起始位 -> %.4f mm\n", start_mm);
                ph = Phase::MOVE_BACK; ph_t0 = now;
            } else if (r == MoveResult::ERROR) {
                fail_reason = "定位失败";
                std::printf("[m2] ✘ %s：%s\n", fail_reason, axis.last_error());
                ph = Phase::DISABLE; ph_t0 = now;
            } else if ((now - ph_t0) > move_to) {
                fail_reason = "定位超时（15s）";
                std::printf("[m2] ✘ %s\n", fail_reason);
                ph = Phase::DISABLE; ph_t0 = now;
            }
            break;
        }

        case Phase::MOVE_BACK: {
            const MoveResult r = axis.move_result();
            if (r == MoveResult::DONE) {
                ok_back = true;
                std::printf("[m2] ✔ 已回起始位：mpos=%.4f mm\n", axis.status().mpos);
                ph = Phase::DISABLE; ph_t0 = now;
            } else if (r == MoveResult::ERROR) {
                fail_reason = "回位失败";
                std::printf("[m2] ✘ %s：%s\n", fail_reason, axis.last_error());
                ph = Phase::DISABLE; ph_t0 = now;
            } else if ((now - ph_t0) > move_to) {
                fail_reason = "回位超时（15s）";
                std::printf("[m2] ✘ %s\n", fail_reason);
                ph = Phase::DISABLE; ph_t0 = now;
            }
            break;
        }

        case Phase::JOG_RUN: {
            if (axis.move_result() == MoveResult::ERROR) {
                fail_reason = "点动异常（PV 切换/内部错误）";
                std::printf("[m2] ✘ %s：%s\n", fail_reason, axis.last_error());
                ph = Phase::DISABLE; ph_t0 = now;
                break;
            }
            // 1) 等驱动器回显 6061==3，确认已切到 PV
            if (in.mode_disp != 3) {
                if ((now - ph_t0) > enable_to) {
                    fail_reason = "切换到 PV(6060=3) 超时";
                    std::printf("[m2] ✘ %s：6061=%d\n", fail_reason, (int)in.mode_disp);
                    ph = Phase::DISABLE; ph_t0 = now;
                }
                break;
            }
            // 2) 首次确认 PV：下发 0x60FF 目标速度（档位未映射 PDO，故走 SDO）
            if (jog_vel_t == 0) {
                int32_t v = (int32_t)std::llround(o.jog_speed * o.inc_per_mm);
                const int32_t vmax = (int32_t)std::llround(axc.spd_max * o.inc_per_mm);
                if (v >  vmax) v =  vmax;
                if (v < -vmax) v = -vmax;
                g_vel_req.store(v);
                jog_vel_t     = now;
                jog_start_inc = in.pos_actual;
                std::printf("[m2] 已进 PV(6061=3)：下发 0x60FF=%d inc/s (%.3f mm/s)\n",
                            v, (double)v / o.inc_per_mm);
                break;
            }
            // 3) 跟踪位移，确认真的在走
            if (!jog_moved &&
                std::llabs((long long)in.pos_actual - jog_start_inc) >
                    (long long)std::llround(0.1 * o.inc_per_mm)) {
                jog_moved = true;
                std::printf("[m2] ✔ 检出运动：mp=%.4f mm（起点 %.4f）\n",
                            axis.status().mpos, (double)jog_start_inc / o.inc_per_mm);
            }
            // 4) 到点（到速后急停）或到时（减速停）
            if (o.estop) {
                if (jog_moved && (now - jog_vel_t) > 500LL * 1000000LL) {
                    ok_jog = true;
                    AxisCmd c; c.op = AxisCmd::STOP;
                    axis.apply(c);
                    // 刻意保留 0x60FF 的 jog 速度不变：只靠 halt(bit8) 停车，
                    // 才是对急停的强验证（若 halt 失效，电机会继续按 60FF 走）。
                    halt_inc   = in.pos_actual;
                    halt_t_ns  = now;
                    es_prev_inc = in.pos_actual;
                    es_v0_mmps = (double)g_vel_req.load() / o.inc_per_mm;
                    std::printf("[m2] 下发急停 STOP（6040.bit8 halt），halt@mp=%.4f mm；"
                                "60FF 保持 %d inc/s 不变\n",
                                axis.status().mpos, g_vel_req.load());
                    ph = Phase::ESTOP_HOLD; ph_t0 = now;
                } else if ((now - jog_vel_t) > 5000LL * 1000000LL) {
                    fail_reason = "点动 5s 未见位移，无法验证急停";
                    std::printf("[m2] ✘ %s\n", fail_reason);
                    ph = Phase::DISABLE; ph_t0 = now;
                }
            } else if ((now - jog_vel_t) >= (long long)(o.jog_secs * 1e9)) {
                ok_jog = jog_moved;
                g_vel_req.store(0);              // 60FF=0，按减速停机
                std::printf("[m2] 点动到时：0x60FF=0，等待减速停止…\n");
                ph = Phase::JOG_STOP; ph_t0 = now;
            }
            break;
        }

        case Phase::JOG_STOP: {
            if (jstop_t == 0) { jstop_ref = in.pos_actual; jstop_t = now; break; }
            if (std::llabs((long long)in.pos_actual - jstop_ref) >
                (long long)std::llround(axc.tol_mm * o.inc_per_mm)) {
                jstop_ref = in.pos_actual;   // 仍在动，刷新参考
                jstop_t   = now;
            } else if ((now - jstop_t) > 500LL * 1000000LL) {
                ok_jog_stop = true;
                std::printf("[m2] ✔ 点动已停稳：mp=%.4f mm，sw=0x%04x\n",
                            axis.status().mpos, in.status_word);
                ph = Phase::DISABLE; ph_t0 = now;
            }
            if ((now - ph_t0) > move_to) {
                fail_reason = "点动减速停止超时（15s）";
                std::printf("[m2] ✘ %s\n", fail_reason);
                ph = Phase::DISABLE; ph_t0 = now;
            }
            break;
        }

        case Phase::ESTOP_HOLD: {
            const long long dt = now - ph_t0;
            // 逐拍差分测速：PDO 未映射 606Ch，只能用 6064h 位置差近似（1ms 周期足够）
            const double v_inst = (double)(in.pos_actual - es_prev_inc) / o.inc_per_mm
                                  * (1e9 / (double)period);
            es_prev_inc = in.pos_actual;
            if (es_stop_t_ns == 0 && (now - halt_t_ns) > 2LL * 1000000LL) {
                const double v_thr = (std::fabs(es_v0_mmps) * 0.02 > 0.5)
                                         ? std::fabs(es_v0_mmps) * 0.02 : 0.5;
                if (std::fabs(v_inst) <= v_thr) {
                    es_stop_t_ns = now;
                    es_v_stop    = v_inst;
                    es_stop_dist = (double)(in.pos_actual - halt_inc) / o.inc_per_mm;
                    std::printf("[m2] 急停响应：halt→|v|<%.2f mm/s 用时 %.1f ms，"
                                "该段位移 %.4f mm（v_inst=%.2f mm/s）\n",
                                v_thr, (double)(es_stop_t_ns - halt_t_ns) / 1e6,
                                es_stop_dist, es_v_stop);
                }
            }
            if (!es_have_ref && dt > 1000LL * 1000000LL) {
                es_ref_inc  = in.pos_actual;   // 1.0s 时刻的位置
                es_have_ref = true;
            }
            if (dt > 1500LL * 1000000LL) {
                const double drift  = std::fabs((double)(in.pos_actual - es_ref_inc)) / o.inc_per_mm;
                const double since  = std::fabs((double)(in.pos_actual - halt_inc)) / o.inc_per_mm;
                ok_estop = jog_moved && (drift <= 0.2);
                std::printf("[m2] %s 急停：halt 后停止距离 %.4f mm；1.0~1.5s 残余漂移 %.4f mm"
                            "（阈值 0.2），cw=0x%04x bit8=%d\n",
                            ok_estop ? "✔" : "✘", since, drift,
                            prev_cw, (prev_cw >> 8) & 1);
                if (es_stop_t_ns != 0) {
                    const double t_ms = (double)(es_stop_t_ns - halt_t_ns) / 1e6;
                    const double dec  = (t_ms > 0.0)
                                            ? std::fabs(es_v0_mmps) / (t_ms / 1000.0) : 0.0;
                    std::printf("[m2]     响应时间 %.1f ms（v0=%.2f mm/s → |v|=%.2f mm/s），"
                                "平均减速度 ≈ %.0f mm/s²\n",
                                t_ms, es_v0_mmps, std::fabs(es_v_stop), dec);
                } else {
                    std::printf("[m2]     响应时间：未在 1.5s 内检出速度降至阈值"
                                "（v0=%.2f mm/s，请核对 605Dh/6084h/609Ah 与机械负载）\n",
                                es_v0_mmps);
                }
                ph = Phase::DISABLE; ph_t0 = now;
            }
            break;
        }

        case Phase::DISABLE:
            g_vel_req.store(0);   // 兜底：确保 PV 目标速度为 0
            if (o.keep) {
                std::printf("[m2] --keep：保持使能，相位结束\n");
                ph = Phase::DONE;
                break;
            }
            if (!dis_sent) {
                AxisCmd c; c.op = AxisCmd::DISABLE;
                axis.apply(c);
                std::printf("[m2] 去使能（回 0x06 shutdown）…\n");
                dis_sent = true;
            }
            if ((now - ph_t0) > 1000LL * 1000000LL) ph = Phase::DONE;
            break;

        case Phase::DONE:
            break;
        }

        // ---- 运动内核 ----
        AxisPdoOut out;
        axis.tick(in, &out);

        // ---- 信号量安全兜底：未使能时强制 ctrl_word=0 ----
        // （axis.tick 已按 CiA402 给控制字；此处仅在"完全不使能"模式保持 0）
        if (!o.enable) out.control_word = 0x0000;
        prev_cw = out.control_word;

        // ---- 写 RxPDO ----
        if (off_ok(off.ctrl_word))  ec.write_u16(off.ctrl_word,  out.control_word);
        if (off_ok(off.target_pos)) ec.write_s32(off.target_pos, out.target_pos);
        if (off_ok(off.mode))       ec.write_u8 (off.mode,       (uint8_t)out.mode);
        if (off_ok(off.target_vel)) ec.write_s32(off.target_vel, out.target_vel);

        // ---- 轮廓参数（6081/6083/6084）交给 SDO 线程 ----
        if (axis.params().dirty && !params_sent) { box.post(axis.params()); params_sent = true; }
        if (box.consume_done()) { axis.params_flushed(); params_sent = false; }

        ec.send();

        ++count;
        if (now - wake > period) ++over;

        // ---- 每 100 拍打印一行 ----
        if (count % 100 == 0) {
            std::printf("[m2] t=%6.3fs %-10s sw=0x%04x en=%d mp=%9.4f dp=%9.4f 6061=%d "
                        "607a=%d 60ff=%d cw=0x%04x mr=%-7s idle=%d\n",
                        (double)(now - t0) / 1e9, ph_names[(int)ph], in.status_word,
                        axis.status().enabled, axis.status().mpos, axis.status().dpos,
                        (int)in.mode_disp, out.target_pos, g_vel_req.load(), out.control_word,
                        mr_name(axis.move_result()), axis.status().idle);
        }
        if ((count % 1000) == 0) std::fflush(stdout);

        // ---- 退出条件 ----
        if (ph == Phase::DONE && (now - ph_t0) > 300LL * 1000000LL) break;
        if ((now - t0) >= (long long)(o.seconds * 1e9)) {
            if (o.enable && !o.keep) { fail_reason = fail_reason[0] ? fail_reason : "到达时间上限，强制退出"; }
            break;
        }

        wake += period;
        if (now > wake) { ++over; wake = now + period; }
    }

    g_run.store(false);
    if (th_sdo.joinable()) th_sdo.join();

    // ---- 收尾：确保卸力 ----
    if (o.enable && !o.keep) {
        std::printf("[m2] 收尾：强制去使能 200 拍\n");
        for (int i = 0; i < 200; ++i) {
            ec.receive();
            EthercatMaster::BusState bs;
            ec.read_state(&bs);
            axis.set_bus_ok(bs.ok());
            AxisPdoIn in;
            in.status_word = off_ok(off.status_word) ? ec.read_u16(off.status_word) : 0;
            in.pos_actual  = off_ok(off.pos_actual)  ? ec.read_s32(off.pos_actual)  : 0;
            in.pos_demand  = off_ok(off.pos_demand)  ? ec.read_s32(off.pos_demand)  : 0;
            in.mode_disp   = off_ok(off.mode_disp)   ? (int8_t)ec.read_u8(off.mode_disp) : 0;
            AxisCmd c; c.op = AxisCmd::DISABLE;
            axis.apply(c);
            AxisPdoOut out;
            axis.tick(in, &out);
            if (off_ok(off.ctrl_word))  ec.write_u16(off.ctrl_word,  out.control_word);
            if (off_ok(off.target_pos)) ec.write_s32(off.target_pos, out.target_pos);
            if (off_ok(off.mode))       ec.write_u8 (off.mode,       (uint8_t)out.mode);
            if (off_ok(off.target_vel)) ec.write_s32(off.target_vel, out.target_vel);
            if (o.dc != 0) ec.set_app_time((uint64_t)mono_ns());
            ec.send();
            sleep_until(mono_ns() + period);
        }
    }

    // ---- 结论 ----
    std::printf("\n===== M2 结论 =====\n");
    std::printf("  周期数    : %lld（滞后 %lld 拍）\n", count, over);
    std::printf("  使能序列  : %s\n", o.enable ? (ok_enable ? "PASS ✔" : "FAIL ✘") : "未执行");
    if (o.have_move) {
        std::printf("  PP 走位   : %s\n", ok_move ? "PASS ✔" : "FAIL ✘");
        std::printf("  回到原位  : %s\n", ok_back ? "PASS ✔" : "FAIL ✘");
    }
    if (o.have_jog) {
        std::printf("  PV 点动   : %s\n", ok_jog ? "PASS ✔" : "FAIL ✘");
        if (o.estop) std::printf("  急停 halt : %s\n", ok_estop ? "PASS ✔" : "FAIL ✘");
        else         std::printf("  减速停止  : %s\n", ok_jog_stop ? "PASS ✔" : "FAIL ✘");
    }
    if (fail_reason[0]) std::printf("  失败原因  : %s\n", fail_reason);
    std::printf("  终态      : sw=0x%04x enabled=%d re=%d\n",
                off_ok(off.status_word) ? ec.read_u16(off.status_word) : 0,
                axis.status().enabled, axis.status().err_code);

    bool pass = ok_enable || !o.enable;
    if (o.have_move) pass = pass && ok_move && ok_back;
    if (o.have_jog)  pass = pass && ok_jog && (o.estop ? ok_estop : ok_jog_stop);
    return pass ? 0 : 1;
}
