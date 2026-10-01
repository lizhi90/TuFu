// motion_host.cpp —— 脚本设备命令 -> kx::Shared 的翻译层
#include "script/motion_host.h"

#include "modbus/modbus_master.h"
#include "modbus/modbus_server.h"

#include "script/nvram_store.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace kx {
namespace {

constexpr int kNotMine = -1;   // B 层子分发：本分发不认识该命令，交回上层

long long mono_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool arg_num(const std::vector<Value>& a, size_t k, double* out) {
    if (k >= a.size()) return false;
    *out = a[k].to_num();
    return true;
}

// 轴参数名（1 参=读，2 参=写）：ZBasic 里这些既能读也能写
bool is_axis_param(const std::string& n) {
    return n == "SPEED" || n == "ACCEL" || n == "DECEL" ||
           n == "ATYPE" || n == "UNITS" || n == "DRIVE_PROFILE" || n == "AXIS_ADDRESS";
}

double arg_at(const std::vector<Value>& a, size_t k) {
    return k < a.size() ? a[k].to_num() : 0.0;
}

// 总线信息查询：一次取回快照（减少重复加锁）
kx::BusInfo bus_snapshot(Shared& sh) {
    std::lock_guard<std::mutex> lk(sh.mtx);
    return sh.bus;
}

} // namespace

// ---------------------------------------------------------------------------
// 等待原语
// ---------------------------------------------------------------------------
bool MotionHost::sleep_ms(int ms) {
    int left = ms;
    while (left > 0) {
        if (aborted()) return false;
        const int chunk = left > 10 ? 10 : left;   // 10ms 粒度，保证中止响应
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        left -= chunk;
    }
    return !aborted();
}

bool MotionHost::wait_move(int axis, int timeout_ms, std::string* err) {
    // 先确认本命令生效(RUNNING)，再等终态；
    // 未见到 RUNNING 时，短暂忽略上一轮遗留的终态。
    const long long t0 = mono_ms();
    bool running_seen = false;
    const long long stale_grace_ms = 50;

    while (true) {
        const AxisStatus st = sh_->snapshot(axis);
        const long long el = mono_ms() - t0;

        if (st.move_result == 1) {
            running_seen = true;
        } else if (st.move_result == 2) {
            if (running_seen || el >= stale_grace_ms) return true;
        } else if (st.move_result == 3) {
            if (running_seen || el >= stale_grace_ms) {
                *err = "运动内核报告失败";
                return false;
            }
        }

        if (aborted())       { *err = "已中止"; return false; }
        if (el > timeout_ms) { *err = "等待运动超时"; return false; }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
    }
}

bool MotionHost::wait_lin(int timeout_ms, std::string* err) {
    // 等直线插补会话终态（state: 1 请求 → 2 运行 → 3 done / 4 error）
    const long long t0 = mono_ms();
    while (true) {
        int st = 0;
        char ebuf[sizeof(sh_->lin.err)] = {0};
        {
            std::lock_guard<std::mutex> lk(sh_->lin.mtx);
            st = sh_->lin.state;
            if (st == 4) std::snprintf(ebuf, sizeof(ebuf), "%s", sh_->lin.err);
        }
        if (st == 3) return true;
        if (st == 4) { *err = std::string("直线插补失败：") + ebuf; return false; }
        if (aborted())       { *err = "已中止"; return false; }
        if (mono_ms() - t0 > timeout_ms) { *err = "等待插补超时"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
    }
}

bool MotionHost::wait_enabled(int axis, bool want, int timeout_ms, std::string* err) {
    const long long t0 = mono_ms();
    while (true) {
        if ((sh_->snapshot(axis).enabled != 0) == want) return true;
        if (aborted())                        { *err = "已中止"; return false; }
        if (mono_ms() - t0 > timeout_ms)      { *err = want ? "使能超时" : "去使能超时"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
    }
}

// ---------------------------------------------------------------------------
// 命令分发
// ---------------------------------------------------------------------------
// ★命令投递（2026-09-29）：等命令槽空闲再投，避免快速连发被覆盖（如 JOGLEAD+SPEED+JOG）。
//   超时（内核 8ms 未取走，异常情况）→ 计数 + 输出明确错误（不静默丢命令）。
void MotionHost::post_cmd(const AxisCmd& c) {
    if (!sh_) return;
    if (!sh_->post_wait(c, 8)) {
        ++errors_;
        if (out_) out_("[cmd] 命令投递超时（内核未及时取走），已丢弃 op=" + std::to_string((int)c.op));
    }
}

int MotionHost::call(const std::string& name, const std::vector<Value>& args,
                     Value* ret, std::string* err) {
    ++cmds_;
    if (cfg_.echo && out_) out_("[cmd] " + name);

    auto fail = [&](const std::string& m) { ++errors_; if (err) *err = m; return 2; };

    // 多轴系统：实际轴数由 AXIS_COUNT 决定（Shared::axis_count）。指向不存在轴的命令
    // 一律**明确报错**，绝不静默退化成「返回轴 0 数据」（那会让脚本读到错误的位置/状态，
    // 见 docs/planA/08 §8.4）。轴选择（BASE/AXIS、命令后缀 AXIS(n)）在此**真正生效**。
    auto require_axis = [&](int ax, const char* what) -> bool {
        if (axis_valid(ax)) return true;
        fail(std::string(what) + " 轴号 " + std::to_string(ax) +
             " 不存在（本控制器轴数 " + std::to_string(sh_->axis_count) +
             "，有效 0.." + std::to_string(sh_->axis_count - 1) + "）");
        return false;
    };

    // ---- 使能/去使能（可选轴号；缺省当前轴 BASE）----
    if (name == "EN" || name == "ENABLE" || name == "DIS" || name == "DISABLE") {
        const bool en = (name == "EN" || name == "ENABLE");
        const int  ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, en ? "ENABLE" : "DISABLE")) return 2;
        AxisCmd c; c.axis = ax; c.op = en ? AxisCmd::ENABLE : AxisCmd::DISABLE;
        post_cmd(c);
        if (!wait_enabled(ax, en, cfg_.enable_timeout_ms, err)) return 2;
        return 0;
    }
    if (name == "STOP" || name == "RAPIDSTOP") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "STOP")) return 2;
        AxisCmd c; c.axis = ax; c.op = AxisCmd::STOP; post_cmd(c); return 0;
    }

    // ---- 轴选择：BASE n[,n2[,n3...]] / AXIS n（命令后缀 AXIS(n) 也走这里） ----
    // M3：BASE 支持多轴轴表（直线插补参与轴，配合 MOVEABS/MOVE 多目标）；
    //     单轴用法完全向后兼容（base_ = 轴表首元素）。
    if (name == "BASE" || name == "AXIS") {
        if (args.empty()) {
            if (ret) *ret = Value::number(base_);
            return 0;
        }
        const int n = (int)args.size();
        if (n > 8) return fail("BASE 最多 8 轴");
        for (int i = 0; i < n; ++i) {
            const int ax = (int)args[i].to_num();
            if (!require_axis(ax, "AXIS/BASE")) return 2;
            base_list_[i] = ax;
        }
        base_n_ = n;
        base_   = base_list_[0];
        if (ret) *ret = Value::number(base_);
        return 0;
    }

    // ---- M3：MOTION_MODE([m]) —— 运行模式 0=PP（驱动器规划）1=CSP（控制器每拍规划）----
    // 全局（对所有轴生效）；CSP 定位/插补需 DC 同步与已设置脉冲当量。
    if (name == "MOTION_MODE") {
        if (args.empty()) { if (ret) *ret = Value::number(motion_mode_); return 0; }
        const int m = (int)args[0].to_num();
        if (m != 0 && m != 1) return fail("MOTION_MODE 只接受 0(PP) / 1(CSP)");
        for (int a = 0; a < sh_->axis_count; ++a) {
            AxisCmd c; c.axis = a; c.op = AxisCmd::SET_MOTION_MODE; c.pos = (double)m;
            post_cmd(c);
        }
        motion_mode_ = m;
        if (m == 0 && out_) {
            for (int a = 0; a < sh_->axis_count; ++a) {
                if (axis_param("SRAMP", a) > 0.0) {
                    out_("[SRAMP] MOTION_MODE=PP 下 S 曲线不生效（驱动器无 S 参数）；需 S 曲线请用 CSP(1)，或 SRAMP=0");
                    break;
                }
            }
        }
        if (ret) *ret = Value::number(motion_mode_);
        return 0;
    }
    // ---- UNITS(ax[, v])：脉冲当量（inc/mm）----
    // v0.8.1：控制器**不再固定/默认**脉冲当量（不同伺服+机械各不相同）——
    // 写入 → 下发内核 SET_SCALE（运动中拒绝，未使能可设）；读取 → 内核实际生效值（0 = 未设置）。
    if (name == "UNITS") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "UNITS")) return 2;
        if (args.size() >= 2) {
            const double v = args[1].to_num();
            if (!(v > 0.0)) return fail("脉冲当量必须 > 0（inc/mm）");
            if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪，拒绝设置脉冲当量");
            AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_SCALE; c.pos = v;
            post_cmd(c);
            if (!wait_move(ax, 2000, err)) { ++errors_; return 2; }
            params_[name][ax] = v;                  // 兼容记录（读取以内核为准）
            return 0;
        }
        if (ret) *ret = Value::number(sh_->snapshot(ax).inc_per_mm);
        return 0;
    }

    // ---- MBD_STATUS/MBD_LIST：Modbus 主站状态/清单（planA/21；P4）----
    if (name == "MBD_STATUS" || name == "MBD_LIST") {
        if (!master_) return fail("未启用 Modbus 主站（固件 ModbusMaster 未接线）");
        if (name == "MBD_LIST") {
            if (ret) *ret = Value::text(master_->list_text());
            return 0;
        }
        std::string dev;
        if (!args.empty() && args[0].type == Value::STR) dev = args[0].str;
        if (ret) *ret = Value::text(master_->status_text(dev));
        return 0;
    }

    // ---- MBREG_ZONE/MBREG_PUT：按区读写 4x（脚本与固件库存同步；planA/20 P3b）----
    if (name == "MBREG_ZONE" || name == "MBREG_PUT") {
        if (!mb_) return fail("未启用 Modbus 固件库存（ModbusServer 未接线）");
        if (name == "MBREG_ZONE") {
            if (args.size() < 2 || args[0].type != Value::NUM || args[1].type != Value::NUM) {
                return fail("MBREG_ZONE 需要 (start, count)");
            }
            const int st = (int)args[0].num, cnt = (int)args[1].num;
            if (st < 0 || cnt < 1 || st + cnt > 65536 || cnt > 1024) {
                return fail("MBREG_ZONE 参数越界（0<=start、1<=count<=1024、start+count<=65536）");
            }
            if (ret) *ret = Value::text(mb_->read_zone(st, cnt));
            return 0;
        }
        if (args.size() < 2 || args[0].type != Value::NUM || args[1].type != Value::STR) {
            return fail("MBREG_PUT 需要 (start, packed)");
        }
        mb_->write_zone((int)args[0].num, args[1].str);
        return 0;
    }

    // ---- MB_READ/MB_WRITE/MB_LIST：Modbus 组态变量按名访问（planA/20；固件 ModbusServer）----
    if (name == "MB_READ" || name == "MB_WRITE" || name == "MB_LIST") {
        if (!mb_) return fail("未启用 Modbus 组态（固件 ModbusServer 未接线）");
        if (name == "MB_LIST") {
            if (ret) *ret = Value::text(mb_->list_text());
            return 0;
        }
        if (args.empty() || args[0].type != Value::STR) {
            return fail(name + " 需要变量名（字符串；MB_LIST() 可查看）");
        }
        std::string merr;
        if (name == "MB_READ") {
            double v = 0;
            if (mb_->read_name(args[0].str, &v, &merr)) {
                if (ret) *ret = Value::number(v);
                return 0;
            }
            // 主站回退：“设备.点位”（planA/21）
            if (master_) {
                const size_t dot = args[0].str.rfind('.');
                if (dot != std::string::npos) {
                    std::string derr;
                    if (master_->read_point(args[0].str.substr(0, dot),
                                            args[0].str.substr(dot + 1), &v, &derr)) {
                        if (ret) *ret = Value::number(v);
                        return 0;
                    }
                    return fail(derr);
                }
            }
            return fail(merr);
        }
        if (args.size() < 2 || args[1].type != Value::NUM) {
            return fail("MB_WRITE 需要变量名与数值");
        }
        if (mb_->write_name(args[0].str, args[1].num, &merr)) return 0;
        if (master_) {
            const size_t dot = args[0].str.rfind('.');
            if (dot != std::string::npos) {
                std::string derr;
                if (master_->write_point(args[0].str.substr(0, dot),
                                         args[0].str.substr(dot + 1), args[1].num, &derr)) {
                    return 0;
                }
                return fail(derr);
            }
        }
        return fail(merr);
    }

    // ---- REGMAP_GET()：Modbus 寄存器表文本（脚本目录 .mbmap）----
    //   Kine-X 用户寄存器（4x300~999）配置载体：脚本每秒轮询本命令做热加载；
    //   写入由调试口 D11 `mbmap.set` 完成（原子写）；本命令只读。
    if (name == "REGMAP_GET") {
        std::string text;
        if (!regmap_dir_.empty()) {
            std::ifstream f(regmap_dir_ + "/.mbmap", std::ios::binary);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                text = ss.str();
            }
        }
        if (ret) *ret = Value::text(text);
        return 0;
    }

    // ---- JOGLEAD(ax[, s])：点动 PP 跟随前视（秒；实际前视 = s + v²/(2a)）----
    // 写入 → 下发内核 SET_JOG_LEAD（运行时生效，点动中也可改）；读取 → 内核当前值。
    if (name == "JOGLEAD") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "JOGLEAD")) return 2;
        if (args.size() >= 2) {
            const double v = args[1].to_num();
            if (!(v > 0.0)) return fail("点动前视必须 > 0（秒）");
            AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_JOG_LEAD; c.pos = v;
            post_cmd(c);
            params_[name][ax] = v;
            return 0;
        }
        if (ret) *ret = Value::number(sh_->snapshot(ax).jog_lead_s);
        return 0;
    }

    // ---- SRAMP(ax[, ms])：S 曲线时间（0~250ms，0=梯形；**CSP 生效**，ZBasic SRAMP 同名）----
    if (name == "SRAMP") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "SRAMP")) return 2;
        if (args.size() >= 2) {
            const double ms = args[1].to_num();
            if (ms < 0.0 || ms > 250.0) return fail("SRAMP 必须 0~250（毫秒）");
            AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_SRAMP; c.pos = ms / 1000.0;
            post_cmd(c);
            params_[name][ax] = ms;
            if (ms > 0.0 && motion_mode_ == 0 && out_) {
                out_("[SRAMP] 当前 MOTION_MODE=PP，S 曲线不生效（驱动器无 S 参数）；需 S 曲线请 MOTION_MODE(1)（CSP）");
            }
            return 0;
        }
        if (ret) *ret = Value::number(axis_param("SRAMP", ax));
        return 0;
    }

    // ---- FASTDEC(ax[, v])：急停/停机减速度（mm/s²；0=未设置；ZBasic FASTDEC 同名）----
    if (name == "FASTDEC") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "FASTDEC")) return 2;
        if (args.size() >= 2) {
            const double v = args[1].to_num();
            if (v < 0.0) return fail("FASTDEC 必须 >= 0（mm/s²；0=未设置）");
            AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_FASTDEC; c.pos = v;
            post_cmd(c);
            params_[name][ax] = v;
            return 0;
        }
        if (ret) *ret = Value::number(axis_param("FASTDEC", ax));
        return 0;
    }

    // ---- VP_SPEED(ax)：当前运动速度（mm/s，内核实际位置差分估计，只读）----
    if (name == "VP_SPEED") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "VP_SPEED")) return 2;
        if (ret) *ret = Value::number(sh_->snapshot(ax).vel_mm);
        return 0;
    }

    // ---- 轴参数读写（按轴分别记录；缺省当前轴 BASE）----
    if (is_axis_param(name)) {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, name.c_str())) return 2;
        if (args.size() >= 2) {
            params_[name][ax] = args[1].to_num();
            if (name == "SPEED" || name == "ACCEL" || name == "DECEL") {
                // 同步内核缺省：MOVE/JOG 未显式给 speed/accel 时使用（DECEL 亦作 6084）
                AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_DEFAULTS;
                c.speed = axis_param("SPEED", ax);
                c.accel = axis_param("ACCEL", ax);
                c.pos   = axis_param("DECEL", ax);
                post_cmd(c);
            }
            return 0;
        }
        if (ret) *ret = Value::number(axis_param(name, ax));
        return 0;
    }
    // ---- 解除轴分组 = no-op（无控制器的分组语义，仍校验轴号） ----
    if (name == "DISABLE_GROUP") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "DISABLE_GROUP")) return 2;
        return 0;
    }

    // ---- DRIVE_CONTROLWORD(ax)[=v]：CiA402 控制字 6040h ----
    // ⚠脚本里是 `DRIVE_CONTROLWORD(0)=v`（赋值形式），走引擎「设备参数写入」路径
    //   （script.cpp ASSIGN_ARR）：宿主若不认识会**静默退化为数组赋值**。故必须显式处理：
    //   只映射脚本用到的标准序列 128(0x80 清错) / 6(0x06 关机) / 15(0x0F 使能) 到等价内核动作，
    //   其余值**明确报错**，绝不静默（避免脚本任意改控制字与内核使能状态机冲突）。
    if (name == "DRIVE_CONTROLWORD") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, "DRIVE_CONTROLWORD")) return 2;
        if (args.size() < 2) { if (ret) *ret = Value::number(0); return 0; }   // 读：无数据源，恒 0
        const int v = (int)args[1].to_num();
        AxisCmd c; c.axis = ax;
        if      (v == 128) c.op = AxisCmd::FAULT_RESET;   // 0x80 清故障
        else if (v == 6)   c.op = AxisCmd::DISABLE;       // 0x06 关机（去使能）
        else if (v == 15)  c.op = AxisCmd::ENABLE;        // 0x0F 使能
        else return fail("DRIVE_CONTROLWORD 只接受标准序列 128(清错)/6(关机)/15(使能)，收到 " +
                         std::to_string(v) + "（不直接写控制字，避免与内核使能状态机冲突）");
        post_cmd(c);
        return 0;
    }

    // ---- 定位 ----
    // ⚠ZBasic 语义：MOVE/MOVR = 相对(距离)，MOVEABS = 绝对
    // M3：BASE 多轴后（base_n_>1），MOVEABS/MOVE 变为**多轴直线插补**：
    //     MOVEABS(p1,...,pN[, spd[, acc[, wait]]])——p_i 为第 i 轴的绝对目标；
    //     MOVE  为各轴相对距离。spd/acc 作用于主导轴（|位移| 最大）；wait 缺省 1。
    //     单轴（base_n_==1）走原路径（内核按 MOTION_MODE 决定 PP/CSP 规划方式）。
    if (name == "MOVE" || name == "MOVR" || name == "MOVEREL" ||
        name == "MOVEABS" || name == "MOVE_ABS") {
        const bool rel = (name == "MOVE" || name == "MOVR" || name == "MOVEREL");

        // ---- 多轴直线插补（M3）----
        if (base_n_ > 1) {
            const int n = base_n_;
            if ((int)args.size() < n) {
                return fail("多轴 " + name + " 需要 " + std::to_string(n) +
                            " 个目标（BASE 轴表长度），收到 " + std::to_string(args.size()));
            }
            double spd = 0, acc = 0;
            const bool has_spd = arg_num(args, n, &spd) && spd > 0;
            const bool has_acc = arg_num(args, n + 1, &acc) && acc > 0;
            if (!has_spd) spd = axis_param("SPEED", base_);
            if (!(spd > 0)) return fail("多轴定位需要速度（末参 spd 或 SPEED(基准轴)）");

            {
                std::lock_guard<std::mutex> lk(sh_->lin.mtx);
                if (sh_->lin.state == 1 || sh_->lin.state == 2)
                    return fail("上一直线插补尚未结束（state=" + std::to_string(sh_->lin.state) + "）");
                for (int i = 0; i < n; ++i) {
                    const int ax = base_list_[i];
                    if (!require_axis(ax, "MOVE")) return 2;
                    if (!sh_->snapshot(ax).bus_ok) return fail("轴 " + std::to_string(ax) + " 总线未就绪");
                    double p = args[i].to_num();
                    if (rel) p += sh_->snapshot(ax).mpos;      // 相对：当前实际位置 + 距离
                    sh_->lin.axis[i]      = ax;
                    sh_->lin.target_mm[i] = p;
                }
                sh_->lin.n     = n;
                sh_->lin.speed = spd;
                sh_->lin.accel = has_acc ? acc : 0.0;
                ++sh_->lin.seq;
                sh_->lin.state = 1;
                sh_->lin.err[0] = '\0';
            }
            if (!wait_lin(cfg_.move_timeout_ms, err)) { ++errors_; return 2; }
            return 0;
        }

        // ---- 单轴（原路径）----
        const int ax = base_;   // 定位作用于「当前轴」（命令后缀 AXIS(n) 已先设 base_）
        if (!require_axis(ax, "MOVE")) return 2;
        double pos = 0, spd = 0, acc = 0;
        if (!arg_num(args, 0, &pos)) return fail("MOVE 需要目标位置");
        if (!arg_num(args, 1, &spd) || !(spd > 0)) spd = axis_param("SPEED", ax);  // 缺省速度
        if (!arg_num(args, 2, &acc) || !(acc > 0)) {
            acc = axis_param("ACCEL", ax);
            if (!(acc > 0)) acc = axis_param("DECEL", ax);
        }
        if (!(spd > 0)) spd = 0;      // 仍未给 -> 用内核默认
        if (!(acc > 0)) acc = 0;
        if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪");

        // 第 4 参 wait（可选，默认 1）：0/false = **异步**——只下发定位命令立即返回，
        // 由脚本用 IDLE()/BUSY()/MPOS() 自行观察到位（端口服务类长驻脚本用，见 11 §3.1）。
        double wait_arg = 1;
        const bool wait = !(arg_num(args, 3, &wait_arg) && wait_arg == 0);

        AxisCmd c;
        c.axis  = ax;
        c.op    = (name == "MOVE" || name == "MOVR" || name == "MOVEREL")
                      ? AxisCmd::MOVE_REL : AxisCmd::MOVE_ABS;
        c.pos   = pos;
        c.speed = spd;
        c.accel = acc;
        post_cmd(c);
        if (wait && !wait_move(ax, cfg_.move_timeout_ms, err)) { ++errors_; return 2; }
        return 0;
    }

    // ---- 点动（作用于当前轴 BASE；命令后缀 AXIS(n) 已先设 base_）----
    if (name == "JOG" || name == "VJOG") {
        const int ax = base_;
        if (!require_axis(ax, "JOG")) return 2;
        double spd = 0;
        if (!arg_num(args, 0, &spd)) return fail("JOG 需要速度");
        if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪");
        AxisCmd c; c.axis = ax; c.op = AxisCmd::JOG; c.speed = spd;
        post_cmd(c);
        return 0;
    }

    // ---- VMOVE(dir)：连续速度运动（dir>0 正转 / dir<0 反转），速度取 SPEED(当前轴) ----
    // ZBasic 里 VMOVE 是「以当前 SPEED 速度朝 dir 方向持续走」，直到 STOP/CANCEL；
    // 与内核 JOG（点动 = 连续速度直到停止）语义一致。命令后缀 AXIS(n) 由引擎先设 base_。
    if (name == "VMOVE") {
        const int ax = base_;
        if (!require_axis(ax, "VMOVE")) return 2;
        if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪，拒绝速度运动");
        const double dir = args.empty() ? 1.0 : args[0].to_num();
        double spd = axis_param("SPEED", ax);
        if (!(spd > 0)) spd = 1.0;                       // 未设 SPEED 时给最小正速度
        AxisCmd c; c.axis = ax; c.op = AxisCmd::JOG; c.speed = (dir < 0 ? -spd : spd);
        post_cmd(c);
        return 0;
    }

    // ---- CANCEL(mode)：取消运动（停止当前定位/点动）。落到内核 STOP ----
    // mode 仅作为「取消类型」占位，一律停止当前运动；轴号取 base_（后缀 AXIS(n) 已先设 base_）。
    if (name == "CANCEL") {
        const int ax = base_;
        if (!require_axis(ax, "CANCEL")) return 2;
        AxisCmd c; c.axis = ax; c.op = AxisCmd::STOP;
        post_cmd(c);
        return 0;
    }

    // ---- 回零：软件回零（绝对定位到约定零点 AxisConfig::home_mm，默认 0）----
    // 绝对编码器系统下即"回到机械零点"；驱动器内部回零（6060=6 + 0x6098 方式）为独立后续项（见 08 §8.4）。
    if (name == "HOME") {
        const int ax = base_;
        if (!require_axis(ax, "HOME")) return 2;
        if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪，无法回零");
        AxisCmd c; c.axis = ax; c.op = AxisCmd::HOME;
        post_cmd(c);
        if (!wait_move(ax, cfg_.move_timeout_ms, err)) { ++errors_; return 2; }
        if (ret) *ret = Value::number(0);
        return 0;
    }

    // ---- DATUM：回零/清错。手册语义：DATUM(0)=清除控制器所有轴错误（不改坐标）；DATUM(3) AXIS(n)=置零 ----
    // 见 docs/planA/04-运动内核设计.md §1.2：控制器侧"清错误"由 cia402 的 6040=0x80(fault reset) 承担，
    // 故 DATUM(0) 下发内核 AxisCmd::FAULT_RESET（真实触发 0x80 脉冲），而非仅"显式接受"。
    // DATUM(3)：当前位置置零 —— 由内核 AxisCmd::SET_POS 做坐标系偏移（不动电机），见 docs/planA/08 §8.4。
    if (name == "DATUM") {
        const int ax   = base_;   // 作用轴取当前轴（DATUM(3) AXIS(n) 由后缀先设 base_）
        if (!require_axis(ax, "DATUM")) return 2;
        const int mode = args.empty() ? 0 : (int)args[0].to_num();
        if (mode == 0) {                                  // 清除控制器/驱动器错误
            AxisCmd c; c.axis = ax; c.op = AxisCmd::FAULT_RESET;
            post_cmd(c);
            if (ret) *ret = Value::number(0);
            return 0;
        }
        if (mode == 3) {                       // 当前位置置零
            if (!sh_->snapshot(ax).bus_ok) return fail("总线未就绪，无法置零");
            AxisCmd c; c.axis = ax; c.op = AxisCmd::SET_POS; c.pos = 0.0;
            post_cmd(c);
            if (!wait_move(ax, cfg_.move_timeout_ms, err)) { ++errors_; return 2; }
            if (ret) *ret = Value::number(0);
            return 0;
        }
        return fail("DATUM 模式 " + std::to_string(mode) + " 暂未实现（置零用 DATUM(3)，回零用 HOME）");
    }

    // ---- TICKS：毫秒计时器（与 BASIC 的 TICKS 系统变量同语义：倒数，两次差值=经过毫秒）----
    // Lua 侧无 os.time/os.clock（沙箱禁用），长驻脚本用它做心跳/超时/周期调度（见 11 §2/§3.1）。
    if (name == "TICKS") {
        if (ret) *ret = Value::number(-(double)mono_ms());
        return 0;
    }

    // ---- 等待 ----
    if (name == "DELAY" || name == "SLEEP" || name == "WAIT") {
        double ms = 0;
        if (!arg_num(args, 0, &ms)) return fail("DELAY 需要毫秒数");
        if (ms < 0) ms = 0;
        if (!sleep_ms((int)ms)) { if (err) *err = "已中止"; return 3; }
        return 0;
    }
    // ---- WAIT IDLE：等指定轴到位（引擎在 WAIT/WA 语句里调用）；作用于当前轴 BASE ----
    if (name == "WAITIDLE") {
        const int ax = base_;
        if (!require_axis(ax, "WAIT IDLE")) return 2;
        const long long t0 = mono_ms();
        while (sh_->snapshot(ax).idle == 0) {
            if (aborted())                             { if (err) *err = "已中止"; return 3; }
            if (mono_ms() - t0 > cfg_.move_timeout_ms) { ++errors_; if (err) *err = "WAIT IDLE 超时"; return 2; }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
        }
        return 0;
    }

    // ---- 状态查询（可选轴号；缺省当前轴 BASE；越界轴号明确报错，不静默回读别的轴）----
    if (name == "POS" || name == "MPOS" || name == "DPOS" ||
        name == "IDLE" || name == "ISIDLE" || name == "AXISSTATUS" ||
        name == "BUS" || name == "BUSOK" || name == "BUSY" ||
        name == "ENABLED" || name == "ALARM") {
        const int ax = args.empty() ? base_ : (int)args[0].to_num();
        if (!require_axis(ax, name.c_str())) return 2;
        const AxisStatus st = sh_->snapshot(ax);
        if (name == "POS" || name == "MPOS") { if (ret) *ret = Value::number(st.mpos); return 0; }
        if (name == "DPOS")                  { if (ret) *ret = Value::number(st.dpos); return 0; }
        // ZBasic 的 IDLE()：-1 = 运动结束，0 = 运动中（脚本按 <>0 判「到位」）
        if (name == "IDLE")                  { if (ret) *ret = Value::number(st.idle ? -1 : 0); return 0; }
        if (name == "ISIDLE")                { if (ret) *ret = Value::number(st.idle); return 0; }
        // 轴状态字：按 ZBasic 手册 6.3 位表由内核组装（bit2 通讯 / bit3 驱动器故障 /
        // bit8 随动超限 / bit22 告警输入），脚本用 `AXISSTATUS(n) and MV_ERRMASK` 判报警
        if (name == "AXISSTATUS")            { if (ret) *ret = Value::number(st.axis_status); return 0; }
        if (name == "BUS" || name == "BUSOK"){ if (ret) *ret = Value::number(st.bus_ok); return 0; }
        if (name == "BUSY")                  { if (ret) *ret = Value::number(st.moving); return 0; }
        if (name == "ENABLED")               { if (ret) *ret = Value::number(st.enabled); return 0; }
        if (name == "ALARM")                 { if (ret) *ret = Value::number(st.alarm); return 0; }
    }

    // ---- 总线软控（SLOT_* / SCAN / BUSSTOP）----
    // 返回值写入 ret（→ 引擎的 RETURN 系统变量），供脚本 `IF RETURN THEN` 判断成败。
    // SLOT_SCAN(0)：触发重扫，等 RT 发布一次新快照，返回扫到的从站数（0=没扫到）。
    // 等「快照发布序号变化」而非「node_count>0」，保证读到的是本次扫描后的结果，
    // 也保证「扫不到设备」能正确返回 0（走脚本的 ELSE 失败分支）。
    if (name == "SLOT_SCAN" || name == "SCAN") {
        unsigned long before = 0;
        {
            std::lock_guard<std::mutex> lk(sh_->mtx);
            before = sh_->bus_seq;
            sh_->rescan_flag = 1;
        }
        const long long t0 = mono_ms();
        int count = -1;                                             // -1 = 新快照尚未发布
        while (true) {
            {
                std::lock_guard<std::mutex> lk(sh_->mtx);
                if (sh_->bus_seq != before) count = sh_->bus.node_count;
            }
            if (count >= 0) break;
            if (aborted()) { if (err) *err = "已中止"; return 3; }
            if (mono_ms() - t0 >= cfg_.scan_timeout_ms) {
                count = bus_snapshot(*sh_).node_count;              // 超时：退回当前快照
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
        }
        if (ret) *ret = Value::number(count);
        return 0;
    }
    // SLOT_START(0)：总线开启。等总线进入 OP（bus_ok）且已有从站，返回 1=成功 / 0=失败。
    if (name == "SLOT_START") {
        { std::lock_guard<std::mutex> lk(sh_->mtx); sh_->rescan_flag = 1; }   // 解除 BUSSTOP 软停
        const long long t0 = mono_ms();
        bool ok = false;
        while (true) {
            ok = sh_->snapshot().bus_ok != 0 && bus_snapshot(*sh_).node_count > 0;
            if (ok) break;
            if (aborted())            { if (err) *err = "已中止"; return 3; }
            if (mono_ms() - t0 >= cfg_.start_timeout_ms) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms > 0 ? cfg_.poll_ms : 1));
        }
        if (ret) *ret = Value::number(ok ? 1 : 0);
        return 0;
    }
    // SLOT_STOP(0) / BUSSTOP：软停总线（同 4321 的 BUSSTOP;）
    if (name == "SLOT_STOP" || name == "BUSSTOP") {
        { std::lock_guard<std::mutex> lk(sh_->mtx); sh_->busstop_flag = 1; }
        if (ret) *ret = Value::number(1);
        return 0;
    }

    // ---- B 层：端口 / Modbus 寄存器 / 脚本任务 / 总线设备信息 ----
    // 每个子分发「认识」时返回 0/2/3；不认识返回 kNotMine 交回上层（最终退化为数组访问）
    if (int rc = call_port(name, args, ret, err);   rc != kNotMine) return rc;
    if (int rc = call_modbus(name, args, ret, err); rc != kNotMine) return rc;
    if (int rc = call_nvram(name, args, ret, err); rc != kNotMine) return rc;
    if (int rc = call_task(name, args, ret, err);   rc != kNotMine) return rc;
    if (int rc = call_bus(name, args, ret, err);    rc != kNotMine) return rc;

    // ---- SDO_WRITE：直接发 EtherCAT SDO ----
    // 脚本宿主不碰 ecrt（见 motion_host.h 顶部），无 SDO 通道；**明确报错**而非静默退化。
    // （遗留脚本里它只出现在 `SCAN_EVENT(in(7))` 恒 0 的调试分支，本控制器无 DI 故永不进入。）
    // 转为「要做」的条件见 docs/planA/08 §8.4。
    if (name == "SDO_WRITE") {
        return fail("SDO_WRITE 在脚本层不可用：需 EtherCAT 主站通道，脚本宿主不碰 ecrt"
                    "（见 docs/planA/08 §8.4）");
    }

    return 1;   // 不认识 -> 引擎退还为数组访问
}

// ---------------------------------------------------------------------------
// B 层子分发：端口通道（真实 TCP，见 port_manager.h）
// ---------------------------------------------------------------------------
int MotionHost::call_port(const std::string& name, const std::vector<Value>& args,
                          Value* ret, std::string* err) {
    auto fail = [&](const std::string& m) { ++errors_; if (err) *err = m; return 2; };

    if (name == "OPEN") {                                  // OPEN #n, 类型[, 端口[, IP]]
        if (args.size() < 2) return fail("OPEN 需要 #通道, 类型[, 端口[, IP]]");
        const int         port = (int)arg_at(args, 0);
        const std::string type = args[1].to_text();
        const double      pnum = arg_at(args, 2);
        const std::string ip   = args.size() >= 4 ? args[3].to_text() : std::string();
        std::string perr;
        if (!ports_.open(port, type, pnum, ip, &perr)) return fail(perr);
        return 0;
    }
    if (name == "CLOSE") { ports_.close_port((int)arg_at(args, 0)); return 0; }

    // PORT_STATUS(ch[, idx])：idx 省略=任一客户端（旧语义）；idx>=0 指定客户端（多客户端，2026-09-28）
    if (name == "PORT_STATUS") {
        const int ch  = (int)arg_at(args, 0);
        const int idx = args.size() >= 2 ? (int)arg_at(args, 1) : -1;
        if (ret) *ret = Value::number(ports_.status(ch, idx) ? 1 : 0);
        return 0;
    }
    if (name == "PORT_CLIENTS") { if (ret) *ret = Value::number(ports_.clients((int)arg_at(args, 0))); return 0; }
    if (name == "PORT_TARGET") { if (ret) *ret = Value::text(ports_.target((int)arg_at(args, 0))); return 0; }
    if (name == "PORT_MAX")    { if (ret) *ret = Value::number(PortManager::global_max_ports() - 1); return 0; }
    if (name == "PORT_INFO") {                             // D10：给端口打标签（通讯状态面板）
        const int idx = (int)arg_at(args, 0);
        const std::string tag  = (args.size() >= 2 && args[1].type == Value::STR) ? args[1].str : std::string();
        const std::string role = (args.size() >= 3 && args[2].type == Value::STR) ? args[2].str : std::string();
        std::string perr;
        if (!PortManager::set_tag(idx, tag, role, &perr)) {
            ++errors_;
            if (err) *err = perr;
            return 2;
        }
        return 0;
    }

    if (name == "PRINT" || name == "PUTCHAR") {            // PRINT #n,文本[, idx] / PUTCHAR #n,字节[, idx]
        if (args.size() < 2) return fail(name + " #通道 需要数据");
        const int         port  = (int)arg_at(args, 0);
        const std::string bytes = args[1].to_text();       // 字节切片已由引擎求值为字符串
        const int         idx   = args.size() >= 3 ? (int)arg_at(args, 2) : 0;   // 多客户端（2026-09-28）
        if (ports_.kind(port) == PortManager::NONE)
            return fail("端口 " + std::to_string(port) + " 未打开（先用 OPEN）");
        std::string perr;
        // 已 OPEN 但未连接/瞬时错误：与 ZBasic 一致地静默丢弃（脚本用 PORT_STATUS 守卫并重连）
        (void)ports_.send(port, bytes, &perr, idx);
        return 0;
    }

    if (name == "GET") {                                   // args: [通道, 数组名, 起点, 最大字节数[, idx]]
        if (args.size() < 2) return fail("GET 需要 #通道, 数组[, 字节数]");
        const int    port = (int)arg_at(args, 0);
        const size_t max  = args.size() >= 4 ? (size_t)arg_at(args, 3) : 0;
        const int    idx  = args.size() >= 5 ? (int)arg_at(args, 4) : 0;         // 多客户端（2026-09-28）
        std::string  bytes, perr;
        const int n = ports_.recv(port, &bytes, max, &perr, idx);
        if (n < 0) return fail(perr);
        if (ret) *ret = Value::text(bytes);                // 引擎据字节串写入数组并补 0 结尾
        return 0;
    }

    return kNotMine;
}

// ---------------------------------------------------------------------------
// B 层子分发：Modbus 4x 寄存器（脚本侧使用 sh_->mb_regs 镜像）
// ---------------------------------------------------------------------------
int MotionHost::call_modbus(const std::string& name, const std::vector<Value>& args,
                            Value* ret, std::string* err) {
    if (name != "MODBUS_REG" && name != "MODBUS_IEEE") return kNotMine;
    auto fail = [&](const std::string& m) { ++errors_; if (err) *err = m; return 2; };

    const int idx = (int)arg_at(args, 0);
    if (idx < 0 || idx >= kModbusRegCount)
        return fail("Modbus 寄存器下标越界（0.." + std::to_string(kModbusRegCount - 1) + "）");

    sh_->mb_mark_used(idx);                                // v0.8.6：读/写都算「控制器用到的寄存器」

    std::lock_guard<std::recursive_mutex> lk(sh_->mb_mtx);

    if (name == "MODBUS_REG") {                            // 16 位：读(1参)/写(2参)
        if (args.size() >= 2) { sh_->mb_regs[idx] = (uint16_t)(int16_t)(int)args[1].to_num(); return 0; }
        if (ret) *ret = Value::number((double)(int16_t)sh_->mb_regs[idx]);
        return 0;
    }

    // MODBUS_IEEE：32 位浮点，占 n/n+1 个字；字序低字在前（与触摸屏 502 实测一致，2026-09-26 定案）
    if (idx + 1 >= kModbusRegCount) return fail("MODBUS_IEEE 需要 n/n+1 两个字（下标 <= 254）");
    sh_->mb_mark_used(idx + 1);                            // 高字也算用到（v0.8.6）
    if (args.size() >= 2) {
        const float f = (float)args[1].to_num();
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        sh_->mb_regs[idx]     = (uint16_t)(bits & 0xFFFFu);
        sh_->mb_regs[idx + 1] = (uint16_t)(bits >> 16);
        return 0;
    }
    const uint32_t bits = ((uint32_t)sh_->mb_regs[idx + 1] << 16) | (uint32_t)sh_->mb_regs[idx];
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    if (ret) *ret = Value::number((double)f);
    return 0;
}

// ---------------------------------------------------------------------------
// B 层子分发：持久化存储（NVSET/NVGET → 脚本目录 .nvram；见 nvram_store.h）
// ---------------------------------------------------------------------------
int MotionHost::call_nvram(const std::string& name, const std::vector<Value>& args,
                           Value* ret, std::string* err) {
    if (name != "NVSET" && name != "NVGET") return kNotMine;
    auto fail = [&](const std::string& m) { ++errors_; if (err) *err = m; return 2; };
    if (!nv_) return fail("NVRAM 未启用（宿主未挂载持久化存储）");
    if (args.empty()) return fail(name + " 需要寄存器号参数（0..255）");
    const int reg = (int)arg_at(args, 0);
    if (reg < 0 || reg >= NvramStore::kRegs)
        return fail("NVRAM 寄存器号越界（0.." + std::to_string(NvramStore::kRegs - 1) + "）");
    if (name == "NVGET") {
        if (ret) *ret = Value::number((double)nv_->get(reg));
        return 0;
    }
    if (args.size() < 2) return fail("NVSET 需要 寄存器号, 值");
    const long v = (long)arg_at(args, 1);
    if (v < -32768 || v > 65535) return fail("NVSET 值越界（-32768..65535）");
    std::string werr;
    if (!nv_->set(reg, (uint16_t)(v & 0xFFFF), &werr))
        return fail(werr.empty() ? std::string("NVRAM 写入失败") : werr);
    return 0;
}

// ---------------------------------------------------------------------------
// B 层子分发：脚本任务登记（RUNTASK/STOPTASK/PROC_STATUS）
// 单线程解释器：只记「登记 + 状态」，不并发执行任务体（见 docs/planA/08 §7）
// ---------------------------------------------------------------------------
int MotionHost::call_task(const std::string& name, const std::vector<Value>& args,
                          Value* ret, std::string* err) {
    if (name == "RUNTASK" || name == "STOPTASK") {
        const int slot = (int)arg_at(args, 0);
        if (slot < 0 || slot >= kScriptTaskMax) {
            ++errors_;
            if (err) *err = "任务号越界（0.." + std::to_string(kScriptTaskMax - 1) + "）";
            return 2;
        }
        std::lock_guard<std::mutex> lk(sh_->mtx);
        if (name == "RUNTASK") {
            sh_->task[slot].running = 1;
            sh_->task[slot].name    = args.size() >= 2 ? args[1].to_text() : std::string();
        } else {
            sh_->task[slot].running = 0;
            sh_->task[slot].name.clear();
        }
        return 0;
    }

    if (name == "PROC_STATUS") {
        const int slot = (int)arg_at(args, 0);
        int run = 0;
        if (slot >= 0 && slot < kScriptTaskMax) {
            std::lock_guard<std::mutex> lk(sh_->mtx);
            run = sh_->task[slot].running;
        }
        if (ret) *ret = Value::number(run ? 1 : 0);
        return 0;
    }

    return kNotMine;
}

// ---------------------------------------------------------------------------
// B 层子分发：总线设备信息（SCAN_EVENT/NODE_*/ETHERCAT/ECUSTOM/ETH_MODE）
// 数据来自 kx::Shared::bus 快照；未扫描（node_count==0）时 NODE_* 返回 0
// ---------------------------------------------------------------------------
int MotionHost::call_bus(const std::string& name, const std::vector<Value>& args,
                         Value* ret, std::string* err) {
    (void)err;
    const bool is_node = name == "NODE_COUNT" || name == "NODE_AXIS_COUNT" ||
                         name == "NODE_STATUS" || name == "NODE_IO" ||
                         name == "NODE_AIO"   || name == "NODE_INFO";
    if (!is_node && name != "SCAN_EVENT" && name != "ETHERCAT" &&
        name != "ECUSTOM" && name != "ETH_MODE") return kNotMine;

    const BusInfo bus = bus_snapshot(*sh_);

    if (name == "SCAN_EVENT") { if (ret) *ret = Value::number(0); return 0; }   // 本控制器无 IO 事件源
    if (name == "NODE_COUNT") { if (ret) *ret = Value::number(bus.node_count); return 0; }
    if (name == "ETH_MODE")   { if (ret) *ret = Value::number(0); return 0; }   // 0 = 传统模式
    if (name == "ECUSTOM")    { if (ret) *ret = Value::text(ports_.target((int)arg_at(args, 0))); return 0; }

    if (name == "ETHERCAT") {                              // 诊断字符串（?*ETHERCAT[(ii)]）
        std::string s;
        if (args.empty()) {
            for (int k = 0; k < bus.node_count && k < kBusNodeMax; ++k)
                s += "node" + std::to_string(k) +
                     ":axis=" + std::to_string(bus.node[k].axis_count) +
                     ",status=" + std::to_string(bus.node[k].status) + "\n";
        } else {
            const int k = (int)arg_at(args, 0);
            if (k >= 0 && k < bus.node_count && k < kBusNodeMax)
                s = "node" + std::to_string(k) +
                    ":axis=" + std::to_string(bus.node[k].axis_count) +
                    ",status=" + std::to_string(bus.node[k].status) +
                    ",io=" + std::to_string(bus.node[k].io_base);
        }
        if (ret) *ret = Value::text(s);
        return 0;
    }

    // NODE_*：args[0]=网口号（单网口，忽略），args[1]=节点号
    const int node = (int)arg_at(args, 1);
    const BusNodeInfo* n = (node >= 0 && node < bus.node_count && node < kBusNodeMax)
                               ? &bus.node[node] : nullptr;
    int v = 0;
    if (n) {
        if (name == "NODE_AXIS_COUNT") v = n->axis_count;
        else if (name == "NODE_STATUS") v = n->status;
        else if (name == "NODE_IO")     v = n->io_base;
        else if (name == "NODE_AIO")    v = n->aio_base;
        else if (name == "NODE_INFO") {
            switch ((int)arg_at(args, 2)) {
                case 10: v = n->in_count;  break;          // IN 个数
                case 11: v = n->out_count; break;          // OUT 个数
                case 12: v = n->ad_count;  break;          // AD 个数
                case 13: v = n->da_count;  break;          // DA 个数
                default: v = 0;            break;
            }
        }
    }
    if (ret) *ret = Value::number(v);
    return 0;
}

void MotionHost::print(const std::string& line) {
    if (out_) out_(line);
    else      std::printf("%s\n", line.c_str());
}

bool MotionHost::aborted() { return abort_ && abort_->load(); }

} // namespace kx
