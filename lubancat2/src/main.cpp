// main.cpp —— 进程入口（增量5）
//
// 职责：
//   1) 读 config/app.conf（AppConfig），命令行可覆盖少量关键项；
//   2) 装配 EthercatMaster（PDO/DC）+ Axis（CiA402 状态机 + 运动语义）；
//   3) 拉起 RT 线程跑 1ms 循环：receive -> 读 TxPDO -> axis.tick -> 写 RxPDO -> send；
//   4) 单独的 SDO 线程下发 6081/6083/6084（ecrt SDO 会阻塞，不能放 RT 线程）；
//   5) 主线程做低频日志（供 ssh 观察），收到 SIGINT/SIGTERM 后有序退出。
//
// 范围：本项目为**控制器开发项目**。与机器人 / 触摸屏 / 称重的**具体协议对接**计划改由
//       脚本程序实现（等控制器本体完成后再开发脚本层）；原生的 4 个协议服务
//       （4321 ASCII / 502 Modbus-TCP / 称重 / 机器人）已从本工程**移除**（见 docs/planA/12）。
//       脚本线程只经 kx::Shared 收发（见 state.h），RT 循环不感知脚本细节。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "common/config.h"
#include "common/state.h"
#include "motion/axis.h"
#include "motion/drive_profile.h"
#include "motion/ethercat_master.h"
#include "motion/interp.h"
#include "motion/rt_util.h"
#include "script/boot_config.h"    // D8：主文件（开机运行）清单
#include "script/port_config.h"    // D9：端口数量上限（.portmax）
#include "script/port_manager.h"   // D9：运行期端口上限（全局）
#include "script/debug_server.h"
#include "script/engine_rule.h"
#include "script/motion_host.h"
#include "script/nvram_store.h"   // NVSET/NVGET：4x 寄存器持久化（.nvram）
#include "script/source_include.h" // 编译前 INCLUDE 展开（多文件）

namespace kx {
namespace {

std::atomic<bool> g_running{true};
// 脚本中止标志（语义：**true = 请求中止**，与 g_running 相反！）。
// MotionHost::aborted() 判「标志为真」，因此不能把「运行中=true」的 g_running 直接传给它，
// 否则脚本一启动就被判中止（2026-09-26 修：开机脚本此前一直秒退为 ABORTED）。
std::atomic<bool> g_abort{false};
// `sys.restart`（D7）的命令失败时的兜底标志：优雅收尾后以失败码退出，
// 让 systemd `Restart=on-failure` 重启（成功的路径由 systemctl restart 直接接管）。
std::atomic<bool> g_restart{false};

void on_signal(int) {
    g_abort.store(true);
    g_running.store(false);
}

// 未映射的 PDO 条目其偏移为 kOffsetInvalid，读写前必须判断，否则越界访问 domain
inline bool off_ok(unsigned int o) { return o != EthercatMaster::kOffsetInvalid; }

// ---------------------------------------------------------------------------
// RT 线程 <-> SDO 线程 的参数信箱（每轴一格）
// 6081/6083/6084 只能走 SDO 且阻塞，因此 RT 侧只投递，SDO 侧落地后回执。
// ---------------------------------------------------------------------------
struct ParamsMailbox {
    struct Slot {
        AxisParams        p{};
        std::atomic<bool> has_new{false};   // RT -> SDO 线程
        std::atomic<bool> done{false};      // SDO 线程 -> RT（本轮已下发）
    };
    std::mutex m;
    Slot       slot[kAxisMax];

    void post(int axis, const AxisParams& v) {
        if (axis < 0 || axis >= kAxisMax) return;
        std::lock_guard<std::mutex> lk(m);
        slot[axis].p = v;
        slot[axis].has_new.store(true);
    }
    // 取出一格待下发的参数；axis 输出目标轴号
    bool take(int* axis, AxisParams* out) {
        for (int a = 0; a < kAxisMax; ++a) {
            if (!slot[a].has_new.load()) continue;
            std::lock_guard<std::mutex> lk(m);
            *out  = slot[a].p;
            *axis = a;
            slot[a].has_new.store(false);
            return true;
        }
        return false;
    }
    void mark_done(int axis) {
        if (axis >= 0 && axis < kAxisMax) slot[axis].done.store(true);
    }
    bool consume_done(int axis) {
        if (axis < 0 || axis >= kAxisMax) return false;
        return slot[axis].done.exchange(false);
    }
};

// 低频日志用的状态快照（RT 每 ~100ms 刷新一次）
struct SnapData {
    EthercatMaster::BusState bus{};
    int      axis_count = 0;
    double   mpos[kAxisMax]  = {0.0};
    double   dpos[kAxisMax]  = {0.0};
    int      enabled[kAxisMax] = {0}, alarm[kAxisMax] = {0}, idle[kAxisMax] = {0};
    int      bus_ok = 0;
    const char* astate[kAxisMax] = {nullptr};
    long long max_jitter_ns = 0;
    unsigned long long cycles = 0, overruns = 0;
    unsigned long long lost = 0;      // 掉总线次数
};

struct Snap {
    std::mutex m;
    SnapData   d{};
};

// ---------------------------------------------------------------------------
// RT 1ms 循环
// ---------------------------------------------------------------------------
void rt_loop(EthercatMaster& master, Axis* axis, const AppConfig& cfg,
             const AxisPdoOffset* off, Shared& sh, ParamsMailbox& pmb, Snap& snap) {
    // 绑核 + SCHED_FIFO + 锁内存 + 禁深 C-state
    if (!rt_setup(cfg.rt.cpu, cfg.rt.prio)) {
        std::fprintf(stderr, "[rt] rt_setup 未完全成功（继续，但实时性可能不达标）\n");
    } else {
        std::printf("[rt] 绑核 CPU%d，SCHED_FIFO prio=%d\n", cfg.rt.cpu, cfg.rt.prio);
    }
    DmaLatencyGuard dma_guard;

    const long long cycle = (long long)cfg.ecat.cycle_ns;
    long long wake = mono_ns() + cycle;
    long long max_jit = 0;
    unsigned long long cycles = 0, overruns = 0, lost = 0;
    bool was_bus_ok = false;
    unsigned pub_div = 0;
    unsigned flag_div = 0;
    bool bus_hold = false;      // BUSSTOP; 软停总线，SCAN; 解除

    const int n_ax = cfg.axis_count;
    bool params_sent[kAxisMax] = {false};

    // ---- M3：多轴直线插补会话（RT 内部状态；宿主经 Shared::lin 下请求）----
    LinInterp lin;
    unsigned long lin_seq_seen = 0;
    int      lin_n = 0;
    int      lin_ax[kAxisMax] = {0};
    double   lin_start[kAxisMax] = {0};   // 起点（机器 inc）
    double   lin_dist[kAxisMax] = {0};    // 位移（inc）
    uint32_t lin_settle = 0;
    bool     lin_active = false;
    constexpr uint32_t kLinSettleTicks = 100;   // 到点确认 100ms

    while (g_running.load()) {
        // ---- 0) 唤醒点抖动统计 ----
        const long long t_wake = mono_ns();
        const long long jit = t_wake - wake;
        if (jit > max_jit) max_jit = jit;

        // ---- 1) 收 ----
        master.receive();

        EthercatMaster::BusState bs;
        master.read_state(&bs);
        const bool bus_ok = bs.ok();
        if (bus_ok && !was_bus_ok)  std::printf("[rt] EtherCAT 进入 OP（link/slave/AL 正常）\n");
        if (!bus_ok && was_bus_ok) { ++lost; std::fprintf(stderr, "[rt] EtherCAT 掉出 OP\n"); }
        was_bus_ok = bus_ok;

        // 4321 的 SCAN; / BUSSTOP; —— 软停总线 / 恢复（docs/planA/05 §1 ⭐）
        if ((flag_div++ % 10) == 0) {
            std::lock_guard<std::mutex> lk(sh.mtx);
            if (sh.busstop_flag) {
                sh.busstop_flag = 0;
                bus_hold = true;
                std::printf("[rt] BUSSTOP: 软停总线\n");
            }
            if (sh.rescan_flag) {
                sh.rescan_flag = 0;
                bus_hold = false;
                std::printf("[rt] SCAN: 恢复总线\n");
            }
        }
        const bool ax_bus_ok = bus_ok && !bus_hold;

        // ---- 1.5) M3 多轴直线插补：受理新请求 / 推进运行中会话 ----
        {
            std::lock_guard<std::mutex> lk(sh.lin.mtx);
            if (sh.lin.state == 1 && sh.lin.seq != lin_seq_seen) {
                lin_seq_seen = sh.lin.seq;
                // 受理校验：参与轴存在、总线就绪、已使能、非运动中
                bool ok = true;
                const char* why = "";
                for (int i = 0; i < sh.lin.n && ok; ++i) {
                    const int ax = sh.lin.axis[i];
                    if (ax < 0 || ax >= n_ax)              { ok = false; why = "轴号超出本机轴数"; break; }
                    if (!ax_bus_ok || !axis[ax].enabled()) { ok = false; why = "轴未使能或总线未就绪"; break; }
                    if (axis[ax].moving())                 { ok = false; why = "轴运动中"; break; }
                }
                if (!ok) {
                    sh.lin.state = 4;
                    std::snprintf(sh.lin.err, sizeof(sh.lin.err), "%s", why);
                } else {
                    lin_n = sh.lin.n;
                    double dist[kAxisMax] = {0};
                    for (int i = 0; i < lin_n; ++i) {
                        const int ax = sh.lin.axis[i];
                        lin_ax[i]    = ax;
                        lin_start[i] = (double)axis[ax].mpos_inc();
                        lin_dist[i]  = (double)axis[ax].mm_to_inc(sh.lin.target_mm[i]) - lin_start[i];
                        dist[i]      = lin_dist[i];
                        axis[ax].lin_begin();
                    }
                    // 主导轴（|dist| 最大）的当量换算 speed/accel（mm→inc）
                    int dom = 0; double best = 0.0;
                    for (int i = 0; i < lin_n; ++i) {
                        const double a = std::fabs(lin_dist[i]);
                        if (a > best) { best = a; dom = i; }
                    }
                    const double ipm = axis[lin_ax[dom]].status().inc_per_mm;
                    double v_inc = 0, a_inc = 0;
                    if (ipm > 0.0) {
                        v_inc = sh.lin.speed * ipm;
                        a_inc = sh.lin.accel * ipm;
                    }
                    if (v_inc <= 0.0) {
                        for (int i = 0; i < lin_n; ++i) axis[lin_ax[i]].lin_end(false);
                        sh.lin.state = 4;
                        std::snprintf(sh.lin.err, sizeof(sh.lin.err),
                                      "主导轴脉冲当量未设置（先 UNITS）或速度非法");
                    } else {
                        if (a_inc <= 0.0) a_inc = v_inc * 10.0;   // 缺省：0.1s 加速到全速
                        lin.begin(lin_n, dist, v_inc, a_inc);
                        lin_active = true;
                        lin_settle = 0;
                        sh.lin.state = 2;
                        std::printf("[lin] 受理：%d 轴，主导位移 %.1f inc，v=%.0f inc/s a=%.0f inc/s^2，预计 %.3fs\n",
                                    lin_n, best, v_inc, a_inc, lin.total_time());
                    }
                }
            }
        }
        if (lin_active) {
            const double s = lin.step((double)cycle * 1e-9);
            for (int i = 0; i < lin_n; ++i) {
                axis[lin_ax[i]].csp_feed((int32_t)llround(lin_start[i] + lin_dist[i] * s));
            }
            // 中止检测：任一参与轴提前退出 MOVING（故障/掉线/STOP）
            bool aborted = false;
            for (int i = 0; i < lin_n; ++i) {
                if (!lin.done()) {
                    if (!axis[lin_ax[i]].moving()) { aborted = true; break; }
                }
            }
            if (aborted) {
                lin_active = false;
                for (int i = 0; i < lin_n; ++i) axis[lin_ax[i]].lin_end(false);
                std::lock_guard<std::mutex> lk(sh.lin.mtx);
                sh.lin.state = 4;
                std::snprintf(sh.lin.err, sizeof(sh.lin.err), "插补中止（轴故障/急停/掉线）");
                std::fprintf(stderr, "[lin] 插补中止\n");
            } else if (lin.done()) {
                // 到点确认：全部参与轴实际位置进入容差
                bool all_in = true;
                for (int i = 0; i < lin_n; ++i) {
                    Axis& axm = axis[lin_ax[i]];
                    const double tgt = axm.inc_to_mm((int32_t)llround(lin_start[i] + lin_dist[i]));
                    if (std::fabs(axm.status().mpos - tgt) > axm.tol_mm()) { all_in = false; break; }
                }
                if (all_in) {
                    if (++lin_settle >= kLinSettleTicks) {
                        lin_active = false;
                        for (int i = 0; i < lin_n; ++i) axis[lin_ax[i]].lin_end(true);
                        std::lock_guard<std::mutex> lk(sh.lin.mtx);
                        sh.lin.state = 3;
                        std::printf("[lin] 插补完成（%d 轴）\n", lin_n);
                    }
                } else {
                    lin_settle = 0;
                }
            }
        }

        // ---- 2)~7) 逐轴：读 TxPDO -> 命令 -> tick -> 写 RxPDO -> 参数下发 ----
        for (int a = 0; a < n_ax; ++a) {
            const AxisPdoOffset& o = off[a];
            axis[a].set_vel_pdo_mapped(off_ok(o.target_vel));   // 无 60FF → 点动走 PP 跟随
            axis[a].set_bus_ok(ax_bus_ok);

            // 读 TxPDO（未映射对象读 0）
            AxisPdoIn in;
            in.status_word = off_ok(o.status_word) ? master.read_u16(o.status_word) : 0;
            in.pos_actual  = off_ok(o.pos_actual)  ? master.read_s32(o.pos_actual)  : 0;
            in.pos_demand_valid = off_ok(o.pos_demand);
            in.pos_demand  = in.pos_demand_valid ? master.read_s32(o.pos_demand) : 0;
            in.mode_disp_valid = off_ok(o.mode_disp);
            in.mode_disp   = in.mode_disp_valid ? (int8_t)master.read_u8(o.mode_disp) : 0;

            // 取业务命令并执行（本轴）
            AxisCmd cmd = sh.take(a);
            if (cmd.op != AxisCmd::NONE) axis[a].apply(cmd);

            // 运动内核
            AxisPdoOut out;
            axis[a].tick(in, &out);

            // 写 RxPDO（未映射对象跳过）
            if (off_ok(o.ctrl_word))  master.write_u16(o.ctrl_word,  out.control_word);
            if (off_ok(o.target_pos)) master.write_s32(o.target_pos, out.target_pos);
            if (off_ok(o.mode))       master.write_u8 (o.mode,       (uint8_t)out.mode);
            if (off_ok(o.target_vel)) master.write_s32(o.target_vel, out.target_vel);

            // 参数下发（6081/6083/6084，交给 SDO 线程）
            if (axis[a].params().dirty && !params_sent[a]) {
                pmb.post(a, axis[a].params());
                params_sent[a] = true;
            }
            if (pmb.consume_done(a)) {
                axis[a].params_flushed();
                params_sent[a] = false;
            }
        }

        // ---- 8) DC 应用时间 + 发 ----
        if (cfg.ecat.dc_assign != 0) master.set_app_time((uint64_t)mono_ns());
        master.send();

        // ---- 9) 发布共享状态（服务线程读；限频到 ~100ms 一次以减少锁竞争）----
        ++cycles;
        if ((pub_div++ % 100) == 0) {
            AxisStatus st[kAxisMax];
            for (int a = 0; a < n_ax; ++a) {
                st[a] = axis[a].status();
                st[a].move_result = (int)axis[a].move_result();
                st[a].moving      = axis[a].moving() ? 1 : 0;
                st[a].target_mm   = axis[a].target_mm();
            }

            // 总线设备快照（供脚本 NODE_*/ETHERCAT）：~100ms 扫一次，远低于 1ms RT 周期
            BusInfo bi;
            master.scan_bus(cfg.ecat.slave_position, (unsigned)n_ax, &bi);
            {
                std::lock_guard<std::mutex> lk(sh.mtx);
                for (int a = 0; a < n_ax; ++a) sh.axis[a] = st[a];
                sh.bus  = bi;
                ++sh.bus_seq;   // 通知 SLOT_SCAN「有新快照」
            }
            std::lock_guard<std::mutex> lk(snap.m);
            snap.d.bus       = bs;
            snap.d.bus_ok    = bus_ok ? 1 : 0;
            snap.d.axis_count = n_ax;
            for (int a = 0; a < n_ax; ++a) {
                snap.d.mpos[a]    = st[a].mpos;
                snap.d.dpos[a]    = st[a].dpos;
                snap.d.enabled[a] = st[a].enabled;
                snap.d.alarm[a]   = st[a].alarm;
                snap.d.idle[a]    = st[a].idle;
                snap.d.astate[a]  = axis[a].state_name();
            }
            snap.d.max_jitter_ns = max_jit;
            snap.d.cycles      = cycles;
            snap.d.overruns    = overruns;
            snap.d.lost        = lost;
        }

        // ---- 10) 绝对周期唤醒 ----
        wake += cycle;
        const long long now = mono_ns();
        if (now > wake) {          // 落后超过一拍：重新对时，避免追赶风暴
            ++overruns;
            wake = now + cycle;
        }
        sleep_until(wake);
    }

    // 退出前尽力去使能（多发几拍 0x06，让驱动器卸力）
    for (int a = 0; a < n_ax; ++a) {
        AxisCmd dis;
        dis.op = AxisCmd::DISABLE;
        axis[a].apply(dis);
    }
    for (int i = 0; i < 20; ++i) {
        master.receive();
        EthercatMaster::BusState bs;
        master.read_state(&bs);
        for (int a = 0; a < n_ax; ++a) {
            const AxisPdoOffset& o = off[a];
            axis[a].set_vel_pdo_mapped(off_ok(o.target_vel));   // 无 60FF → 点动走 PP 跟随
            axis[a].set_bus_ok(bs.ok());
            AxisPdoIn in;
            in.status_word = off_ok(o.status_word) ? master.read_u16(o.status_word) : 0;
            in.pos_actual  = off_ok(o.pos_actual)  ? master.read_s32(o.pos_actual)  : 0;
            in.pos_demand_valid = off_ok(o.pos_demand);
            in.pos_demand  = in.pos_demand_valid ? master.read_s32(o.pos_demand) : 0;
            in.mode_disp_valid = off_ok(o.mode_disp);
            in.mode_disp   = in.mode_disp_valid ? (int8_t)master.read_u8(o.mode_disp) : 0;
            AxisPdoOut out;
            axis[a].tick(in, &out);
            if (off_ok(o.ctrl_word))  master.write_u16(o.ctrl_word,  out.control_word);
            if (off_ok(o.target_pos)) master.write_s32(o.target_pos, out.target_pos);
            if (off_ok(o.mode))       master.write_u8 (o.mode,       (uint8_t)out.mode);
            if (off_ok(o.target_vel)) master.write_s32(o.target_vel, out.target_vel);
        }
        if (cfg.ecat.dc_assign != 0) master.set_app_time((uint64_t)mono_ns());
        master.send();
        sleep_until(mono_ns() + cycle);
    }
    std::printf("[rt] 已退出，共 %llu 拍，超时 %llu 次，最大抖动 %.1f us\n",
                cycles, overruns, (double)max_jit / 1000.0);
}

// ---------------------------------------------------------------------------
// SDO 线程：下发轮廓速度/加/减速度
// ---------------------------------------------------------------------------
void sdo_loop(EthercatMaster& master, ParamsMailbox& pmb) {
    while (g_running.load()) {
        AxisParams p;
        int axis = -1;
        if (pmb.take(&axis, &p)) {
            uint32_t abort = 0;
            const bool ok1 = master.sdo_download_u32_on(axis, 0x6081, 0, (uint32_t)p.profile_vel, 32, &abort);
            const bool ok2 = master.sdo_download_u32_on(axis, 0x6083, 0, (uint32_t)p.profile_acc, 32, &abort);
            const bool ok3 = master.sdo_download_u32_on(axis, 0x6084, 0, (uint32_t)p.profile_dec, 32, &abort);
            if (!(ok1 && ok2 && ok3)) {
                std::fprintf(stderr,
                             "[sdo] 轴%d 轮廓参数下发失败 (6081=%d 6083=%d 6084=%d)，"
                             "确认 drive 支持这些对象\n",
                             axis, p.profile_vel, p.profile_acc, p.profile_dec);
            }
            pmb.mark_done(axis);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ---------------------------------------------------------------------------
// 脚本线程：可选地在独立线程里跑一个脚本
//   * 引擎由 SCRIPT_ENGINE 选择：basic(BASIC 子集，见 docs/planA/08) | lua(见 09)；
//   * 两引擎共用同一设备宿主 MotionHost，只经 kx::Shared 下发命令，
//     不碰 RT 线程；
//   * 等总线就绪后再执行；收到退出信号时经 host->aborted() 及时中止；
//   * 进程退出时由 g_running 触发中止，脚本不会卡住关机。
// ---------------------------------------------------------------------------
void script_loop(Shared& sh, const AppConfig& cfg, std::atomic<bool>& running,
                 std::atomic<bool>& abort_flag) {
    // 0) 解析开机脚本（用户拍板 2026-09-26，方案 A）：
    //    DEBUG_SCRIPT_DIR/.boot 清单（插件「控制器文件 → 设为主文件」）优先，语言按扩展名；
    //    否则回退旧配置 SCRIPT_FILE + SCRIPT_ENGINE；两者都没有 → 不启动脚本。
    std::string boot_dir = cfg.debug_cfg.script_dir.empty() ? "/userdata/kine-x/scripts"
                                                            : cfg.debug_cfg.script_dir;
    while (boot_dir.size() > 1 && boot_dir.back() == '/') boot_dir.pop_back();

    std::string script_file = cfg.script.file;
    ScriptLanguage lang = ScriptLanguage::BASIC;
    bool from_boot = false;
    {
        const BootInfo bi = read_boot_info(boot_dir);
        if (!bi.name.empty()) {
            if (!bi.valid) {
                std::fprintf(stderr, "[script] 开机主文件不可用：%s\n", bi.reason.c_str());
                return;
            }
            script_file = boot_dir + "/" + bi.name;
            lang        = script_file_is_lua(bi.name) ? ScriptLanguage::LUA : ScriptLanguage::BASIC;
            from_boot   = true;
            std::printf("[script] 开机主文件 = %s（%s）\n", script_file.c_str(),
                        script_language_name(lang));
        }
    }
    if (script_file.empty()) {
        std::fprintf(stderr, "[script] 未配置开机脚本（.boot 为空且 SCRIPT_FILE 未设置），不启动\n");
        return;
    }
    if (!from_boot) {                                     // 旧路径：语言取 SCRIPT_ENGINE
        if (!parse_script_language(cfg.script.engine, &lang)) {
            std::fprintf(stderr, "[script] 未知脚本引擎: %s（可选 basic | lua）\n",
                         cfg.script.engine.c_str());
            return;
        }
        std::string lang_err;
        if (!script_file_matches_language(lang, script_file, &lang_err)) {
            std::fprintf(stderr, "[script] %s\n", lang_err.c_str());
            return;
        }
    }

    // 1) 可选：先等总线上电就绪（让使能/MOVE 有意义）
    if (cfg.script.wait_bus) {
        const int step_ms = 100;
        int waited = 0;
        while (running.load() && waited < cfg.script.wait_bus_ms) {
            if (sh.snapshot().bus_ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
            waited += step_ms;
        }
        if (!running.load()) return;
        if (!sh.snapshot().bus_ok) {
            std::fprintf(stderr, "[script] 等待总线就绪超时(%dms)，不执行脚本\n",
                         cfg.script.wait_bus_ms);
            return;
        }
        std::printf("[script] 总线就绪，开始执行 %s\n", script_file.c_str());
    }

    // 2) 读脚本 + 展开 INCLUDE（多文件：其它脚本作为子程序并入主文件）
    std::ifstream f(script_file);
    if (!f) {
        std::fprintf(stderr, "[script] 打不开脚本文件: %s\n", script_file.c_str());
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    std::string src_expanded;
    {
        const std::string from = script_file.substr(script_file.find_last_of('/') + 1);
        std::string ierr;
        if (!expand_source_includes(ss.str(), boot_dir, script_language_name(lang), from,
                                    &src_expanded, &ierr)) {
            std::fprintf(stderr, "[script] %s\n", ierr.c_str());
            return;
        }
    }

    // 3) 装配宿主 + 引擎
    MotionHost::Config hcfg;
    hcfg.move_timeout_ms  = (int)cfg.axis[0].timeout_ms + 2000;   // 兜底应大于定位超时
    hcfg.enable_timeout_ms= 5000;
    // NVRAM：4x 寄存器持久化（NVSET/NVGET；文件=脚本目录内隐藏文件 .nvram；与 .portmax/.boot 同类）
    kx::NvramStore nvram(boot_dir + "/.nvram");
    {
        std::string nverr;
        nvram.load(&nverr);
        if (!nverr.empty()) std::fprintf(stderr, "[nvram] %s\n", nverr.c_str());
        std::printf("[nvram] 已加载 %d 条持久化记录（%s）\n", nvram.count(), nvram.file().c_str());
    }
    MotionHost host(sh, hcfg);
    // 退出信号 -> 脚本中止；必须传「true=中止」的 g_abort（g_running 语义相反，传错会秒退）
    host.set_abort_flag(&abort_flag);
    host.set_nvram(&nvram);                       // NVSET/NVGET 的目标存储
    host.set_output([](const std::string& s) { std::printf("[script] %s\n", s.c_str()); });

    // 经单引擎槽装载：同一时刻只允许一个引擎（重复装载会被拒绝）
    ScriptEngineSlot slot;
    std::string slot_err;
    if (!slot.load(lang, &host, &slot_err)) {
        std::fprintf(stderr, "[script] %s\n", slot_err.c_str());
        return;
    }
    IScriptEngine* eng = slot.engine();

    std::string err;
    if (!eng->compile(src_expanded, &err)) {
        std::fprintf(stderr, "[script/%s] 编译失败: %s\n", eng->name(), err.c_str());
        return;
    }
    eng->set_trace(cfg.script.trace);

    const IScriptEngine::Status st = eng->run((unsigned long long)cfg.script.max_steps);
    if (st == IScriptEngine::Status::DONE) {
        std::printf("[script/%s] 执行完成（%llu 步，%d 条设备命令）\n",
                    eng->name(), eng->steps(), host.cmds());
    } else {
        std::fprintf(stderr, "[script/%s] 结束: %s（第 %d 行: %s）\n",
                     eng->name(), IScriptEngine::status_name(st),
                     eng->error_line(), eng->error().c_str());
    }
}

// ---------------------------------------------------------------------------
void usage(const char* argv0) {
    std::printf(
        "用法: %s [配置文件] [选项]\n"
        "  配置文件            默认 config/app.conf\n"
        "  --dc <hex|dec>      覆盖 ECAT_DC_ASSIGN（0 = 关 DC）\n"
        "  --explicit          强用代码内显式 PDO 映射\n"
        "  --profile <n>       强制 PDO 档位（0=档案默认；见 --list-profiles）\n"
        "  --profile-name <s>  指定驱动器档案：auto(默认自动识别) | sv630 | generic | ...\n"
        "  --list-profiles     列出所有已注册驱动器档案后退出\n"
        "  --cpu <n>           覆盖 RT 绑核（-1 = 不绑）\n"
        "  --prio <n>          覆盖 SCHED_FIFO 优先级（0 = 不改）\n"
        "  --script <file.bas> 启动后运行该脚本（等价 SCRIPT_FILE；覆盖 SCRIPT_ENABLE=1）\n"
        "  --script-engine <s> 脚本引擎：basic(默认) | lua（等价 SCRIPT_ENGINE）\n"
        "  --script-trace      脚本单步跟踪\n"
        "  --dump              打印最终配置后退出\n"
        "  -h, --help          显示本帮助\n",
        argv0);
}

void profile_sink(const char* line, void* user) {
    (void)user;
    std::printf("[profile] %s\n", line);
}

} // namespace
} // namespace kx

int main(int argc, char** argv) {
    using namespace kx;

    std::string cfg_path = "config/app.conf";
    bool dump_only = false;
    bool list_profiles = false;
    std::string ov_profile_name;
    std::string ov_script;
    std::string ov_script_engine;
    bool ov_script_trace = false;
    // 命令行覆盖（-1 表示未指定）
    long long ov_dc = -1, ov_cpu = -2, ov_prio = -1, ov_profile = -1;
    bool ov_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--dump") { dump_only = true; }
        else if (a == "--list-profiles") { list_profiles = true; }
        else if (a == "--explicit") { ov_explicit = true; }
        else if (a == "--script" && i + 1 < argc) { ov_script = argv[++i]; }
        else if (a == "--script-engine" && i + 1 < argc) { ov_script_engine = argv[++i]; }
        else if (a == "--script-trace") { ov_script_trace = true; }
        else if (a == "--profile" && i + 1 < argc) {
            ov_profile = std::strtoll(argv[++i], nullptr, 0);
            ov_explicit = true;   // 选了档位必然走显式映射
        }
        else if (a == "--profile-name" && i + 1 < argc) { ov_profile_name = argv[++i]; }
        else if (a == "--dc" && i + 1 < argc) { ov_dc = std::strtoll(argv[++i], nullptr, 0); }
        else if (a == "--cpu" && i + 1 < argc) { ov_cpu = std::strtoll(argv[++i], nullptr, 0); }
        else if (a == "--prio" && i + 1 < argc) { ov_prio = std::strtoll(argv[++i], nullptr, 0); }
        else if (!a.empty() && a[0] != '-') { cfg_path = a; }
        else { std::fprintf(stderr, "未知参数: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    AppConfig cfg;
    cfg.load(cfg_path);
    if (ov_dc >= 0)      cfg.ecat.dc_assign = (uint32_t)ov_dc;
    if (ov_cpu != -2)    cfg.rt.cpu = (int)ov_cpu;
    if (ov_prio >= 0)    cfg.rt.prio = (int)ov_prio;
    if (ov_explicit)     cfg.ecat.use_explicit_pdo = true;
    if (ov_profile >= 0) cfg.ecat.explicit_profile = (int)ov_profile;
    if (!ov_profile_name.empty()) cfg.ecat.profile_name = ov_profile_name;
    if (!ov_script.empty()) { cfg.script.file = ov_script; cfg.script.enable = true; }
    if (!ov_script_engine.empty()) {
        // 规则：只允许 basic | lua；合法值规范化为小写，非法值保留原样（script_loop 会明确报错）
        ScriptLanguage lang;
        if (parse_script_language(ov_script_engine, &lang))
            cfg.script.engine = script_language_name(lang);
        else
            cfg.script.engine = ov_script_engine;
    }
    if (ov_script_trace)    cfg.script.trace = true;

    if (list_profiles) {
        std::printf("已注册驱动器档案（共 %zu 份）:\n", ProfileRegistry::instance().size());
        profile_dump_all(profile_sink, nullptr);
        return 0;
    }

    cfg.dump();
    if (dump_only) return 0;

    // ---- D9：端口数量上限（运行期；持久化于脚本目录 `.portmax`，缺省 16）----
    // 在任何 PortManager 构造之前加载（进程级上限，所有宿主/实例共享）。
    {
        std::string pdir = cfg.debug_cfg.script_dir.empty() ? "/userdata/kine-x/scripts"
                                                            : cfg.debug_cfg.script_dir;
        while (pdir.size() > 1 && pdir.back() == '/') pdir.pop_back();
        const int pmax = read_port_max(pdir, PortManager::kDefaultMaxPorts);   // 缺/非法 → 默认 16
        PortManager::set_global_max_ports(pmax, nullptr);
        std::printf("[main] 端口数量上限=%d（%s/.portmax，缺省 %d；插件「修改端口数量」可调）\n",
                    pmax, pdir.c_str(), PortManager::kDefaultMaxPorts);
    }

    // 信号：优雅退出
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- 解析驱动器档案（auto = 交给主站在读到 SII 身份后自动匹配）----
    bool prof_auto = false, prof_unknown = false;
    const DriveProfile* want_profile =
        profile_resolve_name(cfg.ecat.profile_name.c_str(), &prof_auto, &prof_unknown);
    if (prof_unknown) {
        std::fprintf(stderr, "[main] 未知驱动器档案 '%s'（--list-profiles 可查看），退出\n",
                     cfg.ecat.profile_name.c_str());
        return 2;
    }
    if (prof_auto) {
        std::printf("[main] 驱动器档案=auto（按总线 SII 身份自动识别）\n");
    } else if (want_profile) {
        std::printf("[main] 驱动器档案=指定 '%s'\n", want_profile->name);
    }

    // ---- 装配 EtherCAT 主站 ----
    EthercatConfig ecfg;
    ecfg.master_index     = cfg.ecat.master_index;
    ecfg.axis_count       = (unsigned)cfg.axis_count;
    for (int a = 0; a < cfg.axis_count; ++a) ecfg.slave_position[a] = cfg.ecat.slave_position[a];
    ecfg.vendor_id        = cfg.ecat.vendor_id;
    ecfg.product_code     = cfg.ecat.product_code;
    ecfg.cycle_ns         = cfg.ecat.cycle_ns;
    ecfg.dc_assign        = cfg.ecat.dc_assign;
    ecfg.dc_shift_ns      = cfg.ecat.dc_shift_ns;
    ecfg.use_explicit_pdo = cfg.ecat.use_explicit_pdo;
    ecfg.explicit_profile = cfg.ecat.explicit_profile;
    ecfg.wd_divider       = cfg.ecat.wd_divider;
    ecfg.wd_intervals     = cfg.ecat.wd_intervals;
    ecfg.profile          = want_profile;                 // nullptr = 自动识别
    ecfg.profile_explicit = (want_profile != nullptr) || cfg.ecat.profile_force;
    ecfg.auto_identity    = cfg.ecat.auto_identity;

    EthercatMaster master;
    AxisPdoOffset  off0;
    const DriveProfile* used_profile = nullptr;
    if (!master.init(ecfg, &off0, &used_profile)) {
        std::fprintf(stderr, "[main] EtherCAT 初始化失败，退出（先跑 tools/ecat_probe 排查）\n");
        return 1;
    }
    std::printf("[main] EtherCAT 就绪（%u 轴，DC %s，档案=%s）\n",
                (unsigned)cfg.axis_count, cfg.ecat.dc_assign != 0 ? "开" : "关",
                used_profile ? used_profile->name : "?");

    // 采集各轴 PDO 偏移（RT 循环逐轴读写）
    AxisPdoOffset off[kAxisMax];
    for (int a = 0; a < cfg.axis_count; ++a) off[a] = master.pdo_offset(a);

    // ---- 装配运动内核（每轴一套：单位/速度/容差/超时）----
    Cia402Axis::Config c402c;
    c402c.default_mode = 1;   // 上电默认 PP

    Axis axis[kAxisMax];
    for (int a = 0; a < cfg.axis_count; ++a) {
        AxisConfig axc;
        axc.inc_per_mm = cfg.axis[a].inc_per_mm;
        axc.def_speed  = cfg.axis[a].def_speed;
        axc.def_accel  = cfg.axis[a].def_accel;
        axc.spd_max    = cfg.axis[a].spd_max;
        axc.tol_mm     = cfg.axis[a].tol_mm;
        axc.timeout_ms = cfg.axis[a].timeout_ms;
        axis[a].init(axc, c402c);
        axis[a].set_csp_mode(cfg.motion_mode_csp != 0);   // M3：MOTION_MODE 配置初始化
    }
    if (cfg.motion_mode_csp != 0 && cfg.ecat.dc_assign == 0) {
        std::fprintf(stderr,
                     "[cfg] 警告：MOTION_MODE=csp 但 ECAT_DC_ASSIGN=0（未启用 DC 同步）——\n"
                     "       CSP 依赖驱动器按同步周期跟随 0x607A，强烈建议开启 DC（0x0300）。\n");
    }

    Shared        sh;
    sh.axis_count = cfg.axis_count;   // 运行时实际轴数：服务层据此校验轴号
    ParamsMailbox pmb;
    Snap          snap;

    // ---- 拉起线程 ----
    std::thread th_rt(rt_loop, std::ref(master), &axis[0], std::cref(cfg),
                      &off[0], std::ref(sh), std::ref(pmb), std::ref(snap));
    std::thread th_sdo;
    if (cfg.ecat.param_via_sdo) {
        th_sdo = std::thread(sdo_loop, std::ref(master), std::ref(pmb));
    }
    // 开机脚本来源：.boot 主文件（优先）或 SCRIPT_FILE 回退；两者都没有则不启动。
    // 主文件清单失效（被删/混合）时明确告警，并回落 SCRIPT_FILE；调试口随之恢复可用。
    std::string boot_dir = cfg.debug_cfg.script_dir.empty() ? "/userdata/kine-x/scripts"
                                                            : cfg.debug_cfg.script_dir;
    while (boot_dir.size() > 1 && boot_dir.back() == '/') boot_dir.pop_back();
    const BootInfo boot_bi = read_boot_info(boot_dir);
    if (!boot_bi.name.empty() && !boot_bi.valid) {
        std::fprintf(stderr, "[main] 开机主文件不可用：%s（回落 SCRIPT_FILE）\n",
                     boot_bi.reason.c_str());
    }
    const bool boot_usable = !boot_bi.name.empty() && boot_bi.valid;

    std::thread th_script;
    const bool script_on = cfg.script.enable && (boot_usable || !cfg.script.file.empty());
    if (script_on) {
        std::printf("[main] 脚本引擎已启用: engine=%s boot=%s file=%s\n",
                    cfg.script.engine.c_str(),
                    boot_usable ? boot_bi.name.c_str() : "(none)",
                    cfg.script.file.empty() ? "(none)" : cfg.script.file.c_str());
        th_script = std::thread(script_loop, std::ref(sh), std::cref(cfg), std::ref(g_running),
                                std::ref(g_abort));
    } else if (cfg.script.enable) {
        std::printf("[main] SCRIPT_ENABLE=1 但未配置开机脚本（.boot 主文件 / SCRIPT_FILE 均为空），跳过\n");
    }

    // ---- 调试通道（DebugServer，见 docs/planA/13、16）----
    // 与自动脚本**互斥**用引擎：SCRIPT_ENABLE=1 且（设了 .boot 主文件或指定了 SCRIPT_FILE）时，
    // 引擎被自动脚本占用，
    // 此时调试口只提供 D1（看状态 / 发命令），script.* 与 var.* 会明确回 BUSY，不伪装可用。
    // 调试口独占时（allow_script=true）语言**由 DEBUG_SCRIPT_DIR 内现有脚本绑定**
    // （空目录=auto 两种都收；2026-09-25 拍板），cfg.script.engine 仅作兜底初值。
    //
    // v0.8.1：开机脚本占用引擎时，传给 DebugServer 的 engine_hint 必须是**该脚本的真实语言**
    // （D8 主文件按扩展名 / SCRIPT_FILE 回退按 SCRIPT_ENGINE），否则 sys.info.engine 会误报 basic。
    std::string boot_engine_hint = cfg.script.engine;
    if (boot_usable) {
        boot_engine_hint = script_file_is_lua(boot_bi.name) ? "lua" : "basic";
    }
    std::unique_ptr<DebugServer> dbg;
    if (cfg.debug_cfg.enable) {
        dbg = std::make_unique<DebugServer>(sh, cfg.debug_cfg, /*allow_script=*/!script_on,
                                            boot_engine_hint, &g_restart);
        std::string derr;
        if (dbg->listen_now(&derr)) {
            dbg->start();
            std::printf("[main] 调试通道已监听 %s:%d（%s）\n", cfg.debug_cfg.bind.c_str(),
                        dbg->port(),
                        script_on ? "D1；脚本引擎被自动脚本占用，D2/D4/D5 不可用"
                                  : "D1~D8；脚本语言由 DEBUG_SCRIPT_DIR 内现有脚本绑定");
        } else {
            std::printf("[main] 调试通道监听失败(%s:%d): %s\n", cfg.debug_cfg.bind.c_str(),
                        cfg.debug_cfg.port, derr.c_str());
            dbg.reset();
        }
    }

    // ---- 主线程：低频日志 ----
    const int log_ms = cfg.log_ms > 0 ? cfg.log_ms : 3000;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(log_ms));
        if (!g_running.load()) break;

        SnapData s;
        {
            std::lock_guard<std::mutex> lk(snap.m);
            s = snap.d;
        }
        std::printf(
            "[stat] %s | bus=%d link=%d resp=%u AL=0x%02x/0x%02x op=%d lost=%llu | "
            "cyc=%llu over=%llu jit=%.1fus\n",
            s.bus_ok ? "OK " : "BAD", s.bus_ok, s.bus.link_up ? 1 : 0, s.bus.slaves_responding,
            s.bus.master_al, s.bus.slave_al, s.bus.slave_op ? 1 : 0, s.lost,
            s.cycles, s.overruns, (double)s.max_jitter_ns / 1000.0);
        for (int a = 0; a < s.axis_count; ++a) {
            std::printf("[axis%d] %-8s en=%d idle=%d alarm=%d mpos=%.3f dpos=%.3f\n",
                        a, s.astate[a] ? s.astate[a] : "?", s.enabled[a], s.idle[a], s.alarm[a],
                        s.mpos[a], s.dpos[a]);
        }
        std::fflush(stdout);
    }

    std::printf("\n[main] 收到退出信号，正在停止…\n");
    if (dbg) { dbg->stop(); dbg.reset(); }   // 先停调试口：它会叫停在跑的脚本并断开客户端
    if (th_script.joinable()) th_script.join();
    if (th_sdo.joinable())    th_sdo.join();
    if (th_rt.joinable())     th_rt.join();
    const bool restarting = g_restart.load();
    std::printf("[main] 已退出%s\n", restarting ? "（重启兜底：交 systemd 重启）" : "");
    return restarting ? 1 : 0;
}
