// axis.cpp —— 单轴运动内核实现
#include "axis.h"

#include <cmath>
#include <cstdio>

namespace kx {

static constexpr double INC_LIMIT = 2147483000.0;

// ---------------------------------------------------------------------------
void Axis::init(const AxisConfig& cfg, const Cia402Axis::Config& c402cfg) {
    cfg_        = cfg;
    st_         = AxisStatus{};
    pa_         = AxisParams{};
    state_      = State::BOOT;
    result_     = MoveResult::NONE;
    err_text_   = "";
    bus_ok_     = false;
    want_mode_  = 0;
    mode_valid_ = false;
    relative_   = false;
    ack_seen_   = false;
    target_mm_  = 0.0;
    target_inc_ = 0;
    vel_inc_    = 0;
    move_ticks_ = 0;
    settle_ticks_ = 0;
    mode_ticks_ = 0;
    stop_req_   = false;
    pend_       = Pend{};
    in_pos_actual_ = in_pos_demand_ = 0;
    in_mode_disp_ = 0;
    pos_offset_inc_ = 0;
    sub_        = Sub::NONE;
    traj_       = Trapezoid{};
    lin_target_inc_ = 0;
    c402_.init(c402cfg);
}

int32_t Axis::mm_to_inc(double mm) const {
    if (!scale_set()) return pos_offset_inc_;          // 未设置当量：零位移（apply 已拒绝运动命令）
    double v = mm * cfg_.inc_per_mm + (double)pos_offset_inc_;
    if (v >  INC_LIMIT) v =  INC_LIMIT;
    if (v < -INC_LIMIT) v = -INC_LIMIT;
    return (int32_t)llround(v);
}

int32_t Axis::dist_to_inc(double mm) const {
    if (!scale_set()) return 0;
    double v = mm * cfg_.inc_per_mm;
    if (v >  INC_LIMIT) v =  INC_LIMIT;
    if (v < -INC_LIMIT) v = -INC_LIMIT;
    return (int32_t)llround(v);
}

double Axis::inc_to_mm(int32_t inc) const {
    if (!(cfg_.inc_per_mm > 0.0)) return 0.0;
    return (double)(inc - pos_offset_inc_) / cfg_.inc_per_mm;
}

void Axis::set_position(double mm) {
    // 令当前实际位置在当前用户坐标系下等于 mm：偏移 = 机器 inc - 期望 inc
    const int32_t want = (int32_t)llround(mm * cfg_.inc_per_mm);
    pos_offset_inc_ = in_pos_actual_ - want;
}

int32_t Axis::clamp_speed_inc(double mm_per_s) const {
    double v = mm_per_s;
    if (v >  cfg_.spd_max) v =  cfg_.spd_max;
    if (v < -cfg_.spd_max) v = -cfg_.spd_max;
    double inc = v * cfg_.inc_per_mm;
    if (inc >  INC_LIMIT) inc =  INC_LIMIT;
    if (inc < -INC_LIMIT) inc = -INC_LIMIT;
    return (int32_t)llround(inc);
}

const char* Axis::state_name() const {
    switch (state_) {
        case State::BOOT:     return "BOOT";
        case State::DISABLED: return "DISABLED";
        case State::ENABLING: return "ENABLING";
        case State::READY:    return "READY";
        case State::MOVING:   return "MOVING";
        case State::JOG:      return "JOG";
        case State::STOPPED:  return "STOPPED";
        case State::FAULT:    return "FAULT";
    }
    return "?";
}

void Axis::fail(const char* text) { err_text_ = text; }

void Axis::set_bus_ok(bool ok) {
    if (bus_ok_ && !ok) {
        // 掉总线：清使能意图，避免恢复后自动带电
        c402_.request_disable();
        if (state_ == State::MOVING || state_ == State::JOG) result_ = MoveResult::ERROR;
        err_text_ = "EtherCAT 总线掉线";
    }
    bus_ok_    = ok;
    st_.bus_ok = ok ? 1 : 0;
}

void Axis::convert_input(const AxisPdoIn& in) {
    in_pos_actual_ = in.pos_actual;
    in_pos_demand_ = in.pos_demand;
    in_pos_demand_known_ = in.pos_demand_valid;
    in_mode_disp_  = in.mode_disp_valid ? (int)in.mode_disp : -1;
}

// ---------------------------------------------------------------------------
// 命令入口
// ---------------------------------------------------------------------------
void Axis::apply(const AxisCmd& c) {
    // v0.8.1：脉冲当量由脚本 UNITS 运行时设置，控制器不设默认——
    // 未设置时禁止一切需要 mm<->inc 换算的命令（明确报错，不按错比例静默执行）。
    if (!scale_set() &&
        (c.op == AxisCmd::MOVE_ABS || c.op == AxisCmd::MOVE_REL || c.op == AxisCmd::JOG ||
         c.op == AxisCmd::HOME || c.op == AxisCmd::SET_POS)) {
        fail("未设置脉冲当量：请先在脚本中执行 UNITS(轴号, 值)");
        result_ = MoveResult::ERROR;
        return;
    }
    switch (c.op) {
    case AxisCmd::NONE:
        break;

    case AxisCmd::ENABLE:
        c402_.request_enable();
        if (state_ == State::BOOT || state_ == State::DISABLED) state_ = State::ENABLING;
        break;

    case AxisCmd::DISABLE:
        c402_.request_disable();
        pend_ = Pend{};
        state_ = State::DISABLED;
        break;

    case AxisCmd::MOVE_ABS:
    case AxisCmd::MOVE_REL: {
        const bool rel = (c.op == AxisCmd::MOVE_REL);
        if (!bus_ok_) { fail("总线未就绪，拒绝定位命令"); result_ = MoveResult::ERROR; break; }
        if (!c402_.enabled()) {
            pend_ = Pend{true, false, rel, c.pos, c.speed, c.accel};
            c402_.request_enable();
            state_ = State::ENABLING;
            result_ = MoveResult::RUNNING;
            break;
        }
        if (moving()) { fail("运动中，忽略新定位命令"); break; }
        start_move(rel, c.pos, c.speed, c.accel);
        break;
    }

    case AxisCmd::SET_JOG_LEAD:
        set_jog_lead(c.pos);
        break;
    case AxisCmd::SET_SRAMP:
        set_sramp(c.pos);            // 秒（0~0.25；0=梯形）
        break;
    case AxisCmd::SET_FASTDEC:
        set_fastdec(c.pos);          // mm/s²（0=未设置）
        break;
    case AxisCmd::SET_DEFAULTS:
        set_defaults(c.speed, c.accel, c.pos);
        break;
    case AxisCmd::JOG:
        if (!bus_ok_) { fail("总线未就绪，拒绝点动命令"); result_ = MoveResult::ERROR; break; }
        if (!c402_.enabled()) {
            pend_ = Pend{true, true, false, 0.0, c.speed, 0.0};
            c402_.request_enable();
            state_ = State::ENABLING;
            break;
        }
        start_jog(c.speed);
        break;

    case AxisCmd::STOP:
        stop_motion();
        break;

    case AxisCmd::SET_SCALE: {   // 运行时设置脉冲当量（inc/mm）；运动中拒绝
        if (moving() || state_ == State::MOVING || state_ == State::JOG) {
            fail("运动中不可修改脉冲当量");
            result_ = MoveResult::ERROR;
            break;
        }
        if (!(c.pos > 0.0) || c.pos > 1e9) {
            fail("脉冲当量必须 > 0（inc/mm）");
            result_ = MoveResult::ERROR;
            break;
        }
        cfg_.inc_per_mm = c.pos;
        target_mm_      = inc_to_mm(in_pos_actual_);   // 用户坐标随新当量重算
        vel_inc_        = 0;
        err_text_       = "";
        result_         = MoveResult::DONE;
        break;
    }

    case AxisCmd::FAULT_RESET:  // 清故障：向驱动器发 0x80（6040.bit7）复位脉冲（见 cia402）
        c402_.pulse_fault_reset();
        err_text_ = "";
        result_   = MoveResult::DONE;   // 瞬时完成，供宿主 wait_move 等到终态
        break;

    case AxisCmd::SET_POS:      // 坐标系置零/置位：不动电机，只改用户坐标基准
        if (moving()) { fail("运动中，忽略置零命令"); result_ = MoveResult::ERROR; break; }
        set_position(c.pos);
        err_text_ = "";
        result_   = MoveResult::DONE;      // 瞬时完成，供宿主 wait_move 等到终态
        break;

    case AxisCmd::SET_MOTION_MODE: {   // M3：PP(0)/CSP(1) 运行模式切换；运动中拒绝
        if (moving()) {
            fail("运动中不可切换 PP/CSP 运行模式");
            result_ = MoveResult::ERROR;
            break;
        }
        csp_mode_ = (c.pos != 0.0);
        err_text_ = "";
        result_   = MoveResult::DONE;
        break;
    }

    case AxisCmd::HOME: {       // 软件回零：以 cfg_.home_mm 为终点做一次绝对定位
        if (!bus_ok_) { fail("总线未就绪，拒绝回零命令"); result_ = MoveResult::ERROR; break; }
        const double hspd = (c.speed > 0.0) ? c.speed : cfg_.home_speed;
        const double hacc = (c.accel > 0.0) ? c.accel : 0.0;
        if (!c402_.enabled()) {
            pend_ = Pend{true, false, false, cfg_.home_mm, hspd, hacc};
            c402_.request_enable();
            state_  = State::ENABLING;
            result_ = MoveResult::RUNNING;
            break;
        }
        if (moving()) { fail("运动中，忽略回零命令"); break; }
        start_move(false, cfg_.home_mm, hspd, hacc);
        break;
    }
    }
}

void Axis::run_pending() {
    if (!pend_.active) return;
    Pend p = pend_;
    pend_ = Pend{};
    if (p.jog) start_jog(p.speed);
    else       start_move(p.relative, p.pos, p.speed, p.accel);
}

void Axis::start_move(bool relative, double pos_mm, double speed, double accel) {
    c402_.clear_quick_stop();
    stop_req_ = false;

    const double spd = (speed > 0.0) ? speed : cfg_.def_speed;
    const double acc = (accel > 0.0) ? accel : cfg_.def_accel;

    // 轮廓参数（6081/6083/6084）——由上层走 SDO/PDO 下发
    pa_.profile_vel = clamp_speed_inc(spd);
    int32_t a_inc   = (int32_t)llround(acc * cfg_.inc_per_mm);
    if (a_inc <= 0) a_inc = 1;
    pa_.profile_acc = a_inc;
    {
        const double dec = (cfg_.def_decel > 0.0) ? cfg_.def_decel : acc;
        int32_t d_inc = (int32_t)llround(dec * cfg_.inc_per_mm);
        pa_.profile_dec = (d_inc > 0) ? d_inc : a_inc;
    }
    pa_.dirty       = true;

    relative_   = relative;
    // 相对定位：607A 写“距离”（不含坐标系偏移）并置 bit6；绝对定位：607A 写用户坐标对应的机器 inc
    target_inc_ = relative ? dist_to_inc(pos_mm) : mm_to_inc(pos_mm);
    target_mm_  = relative ? (inc_to_mm(in_pos_actual_) + pos_mm) : pos_mm;
    ack_seen_   = false;
    move_ticks_ = 0;
    settle_ticks_ = 0;
    mode_ticks_ = 0;
    vel_inc_    = 0;
    result_     = MoveResult::RUNNING;
    state_      = State::MOVING;
    err_text_   = "";

    if (csp_mode_) {
        // ---- M3 CSP：控制器侧梯型规划，每拍输出 0x607A（PDO 槽位与 PP 共用）----
        const int32_t end_inc = relative ? (in_pos_actual_ + dist_to_inc(pos_mm))
                                         : mm_to_inc(pos_mm);
        traj_.begin((double)in_pos_actual_, (double)end_inc,
                    (double)pa_.profile_vel, (double)pa_.profile_acc, sramp_s_);
        lin_target_inc_ = in_pos_actual_;   // 首拍前保持实际位置，避免跳变
        sub_       = Sub::SELF;
        want_mode_ = 8;                     // CSP
        mode_valid_ = true;
    } else {
        // ---- PP：驱动器内部规划（原路径）----
        sub_       = Sub::NONE;
        want_mode_  = 1;              // PP
        mode_valid_ = true;
    }
}

void Axis::start_jog(double speed_mm) {
    c402_.clear_quick_stop();
    stop_req_ = false;

    double spd = speed_mm;
    if (spd == 0.0) spd = cfg_.def_speed;

    pa_.profile_vel = clamp_speed_inc(std::fabs(spd));
    int32_t a_inc   = (int32_t)llround(cfg_.def_accel * cfg_.inc_per_mm);
    if (a_inc <= 0) a_inc = 1;
    pa_.profile_acc = a_inc;
    pa_.profile_dec = a_inc;
    pa_.dirty       = true;

    vel_inc_    = clamp_speed_inc(spd);
    want_mode_  = 3;              // PV
    mode_valid_ = true;
    ack_seen_   = false;
    mode_ticks_ = 0;
    move_ticks_ = 0;
    settle_ticks_ = 0;
    jog_pulse_  = 0;                  // v0.8.1：PP 跟随点动的 bit4 脉冲计数
    result_     = MoveResult::NONE;   // 点动无 move_done/error
    state_      = State::JOG;
    err_text_   = "";
}

void Axis::stop_motion() {
    const bool csp = (sub_ == Sub::SELF);
    const double v_est = vel_est_mm_;
    // ★FASTDEC（脚本可设）：CSP 模式下按设定减速度规划停机（不再瞬时对齐）；
    //   未设置时保持旧行为（halt / 规划器瞬时对齐）。
    if (csp && fastdec_mm_ > 0.0 && state_ == State::MOVING && std::fabs(v_est) > 1.0 &&
        cfg_.inc_per_mm > 0.0) {
        // 解析停机斜坡：从当前速度 v0 按 a 减速到 0（距离 v0²/2a），由 CSP 每拍输出目标；
        // 不能用 Trapezoid 重规划（那是 0→L 的起步规划，会先“从 0 加速”，速度突降）。
        stopping_   = true;
        stop_t_     = 0.0;
        stop_v0_    = std::fabs(v_est) * cfg_.inc_per_mm;   // inc/s
        stop_a_     = fastdec_mm_ * cfg_.inc_per_mm;        // inc/s²
        stop_p0_    = in_pos_actual_;
        stop_dir_   = (v_est >= 0.0) ? 1 : -1;
        stop_req_   = true;
        move_ticks_ = 0;
        c402_.clear_quick_stop();         // 不用 halt（否则驱动器瞬间停）；减速由规划目标产生
        ack_seen_   = false;
        vel_inc_    = 0;
        return;                            // 保持 MOVING/sub_=SELF
    }
    if (!csp && fastdec_mm_ > 0.0) {
        // PP：改 6084 减速后走 halt（605Dh=1 → 6084 斜坡 + 位置锁定）
        int32_t d_inc = (int32_t)llround(fastdec_mm_ * cfg_.inc_per_mm);
        if (d_inc > 0) { pa_.profile_dec = d_inc; pa_.dirty = true; }
    }
    c402_.request_quick_stop();   // 6040.bit8 halt
    stop_req_   = true;
    // 停止后 DPOS 对齐实际位置（点动/定位被急停打断时，指令位置不应停在“追赶目标”上）
    target_mm_  = inc_to_mm(in_pos_actual_);
    ack_seen_   = false;
    vel_inc_    = 0;
    if (sub_ == Sub::SELF) traj_.abort_at((double)in_pos_actual_);   // CSP：规划器立即对齐
    sub_        = Sub::NONE;
    if (state_ == State::MOVING) result_ = MoveResult::NONE;
    if (state_ == State::MOVING || state_ == State::JOG) state_ = State::STOPPED;
}

// ---- M3：直线插补参与（rt 层在受理会话时调用）----
void Axis::lin_end(bool ok) {
    sub_   = Sub::NONE;
    state_ = State::READY;
    if (ok) {
        result_ = MoveResult::DONE;
    } else if (result_ != MoveResult::ERROR) {
        result_   = MoveResult::ERROR;
        err_text_ = "直线插补中止";
    }
}

void Axis::lin_begin() {
    c402_.clear_quick_stop();
    stop_req_       = false;
    ack_seen_       = false;
    move_ticks_     = 0;
    settle_ticks_   = 0;
    mode_ticks_     = 0;
    vel_inc_        = 0;
    lin_target_inc_ = in_pos_actual_;   // 首拍目标 = 当前实际位置（避免跳变）
    target_mm_      = inc_to_mm(in_pos_actual_);
    sub_            = Sub::LIN;
    want_mode_      = 8;                // CSP
    mode_valid_     = true;
    result_         = MoveResult::RUNNING;
    state_          = State::MOVING;
    err_text_       = "";
}

// ---------------------------------------------------------------------------
// RT 周期
// ---------------------------------------------------------------------------
void Axis::tick(const AxisPdoIn& in, AxisPdoOut* out) {
    convert_input(in);

    // 速度估计（VP_SPEED / 监控/曲线用）：对实际位置差分 + 一阶平滑（τ≈4 拍）
    {
        const int32_t dp = in_pos_actual_ - last_pos_inc_;
        last_pos_inc_ = in_pos_actual_;
        double v_raw = 0.0;
        if (cfg_.inc_per_mm > 0.0) v_raw = ((double)dp / cfg_.inc_per_mm) * 1000.0;   // inc/ms → mm/s
        vel_est_mm_ += 0.25 * (v_raw - vel_est_mm_);
        st_.vel_mm = vel_est_mm_;
    }

    st_.bus_ok = bus_ok_ ? 1 : 0;
    st_.inc_per_mm = cfg_.inc_per_mm;
    st_.axis_status = 0;                      // 本拍重算（手册 6.3 位表）
    st_.mpos      = inc_to_mm(in_pos_actual_);
    st_.jog_lead_s = jog_lead_s_;
    // DPOS：6062 已映射用其回显；未映射（如 SV630 档位1）回落为内核目标位置
    st_.dpos   = in_pos_demand_known_ ? inc_to_mm(in_pos_demand_) : target_mm_;

    out->control_word = 0;
    out->target_pos   = target_inc_;
    out->target_vel   = 0;
    // 未指定模式前回显驱动器当前模式，避免上电即改写 6060
    out->mode         = mode_valid_ ? want_mode_ : (in_mode_disp_ >= 0 ? (uint8_t)in_mode_disp_ : (uint8_t)0);   // 未知时写 0（不改模式）

    // ---- 0) 总线未就绪：安全化 ----
    if (!bus_ok_) {
        c402_.request_disable();
        (void)c402_.tick(in.status_word, in.mode_disp);
        if (state_ == State::MOVING || state_ == State::JOG) result_ = MoveResult::ERROR;
        state_     = State::BOOT;
        st_.enabled = 0;
        st_.idle    = 0;
        st_.alarm   = 1;
        st_.err_code = 1;
        st_.axis_status |= kAxisStComm;       // bit2: 与远程轴通讯出错
        return;
    }

    // ---- 1) CiA402 使能时序 ----
    uint16_t cw = c402_.tick(in.status_word, in.mode_disp);
    cst_        = c402_.status();
    const bool enabled = c402_.enabled();

    // ---- 2) 报警：驱动器故障 / 使能时序失败 ----
    if (cst_.fault || c402_.enable_failed()) {
        if (state_ == State::MOVING || state_ == State::JOG) result_ = MoveResult::ERROR;
        state_    = State::FAULT;
        err_text_ = c402_.enable_failed() ? c402_.last_error() : "驱动器报警 (6041.bit3=1)";
        out->control_word = cw;
        st_.enabled  = 0;
        st_.idle     = 0;
        st_.alarm    = 1;
        st_.err_code = cst_.fault ? 1 : 2;
        st_.axis_status |= kAxisStDriveFault;             // bit3: 远程驱动器报错
        if (cst_.warning) st_.axis_status |= kAxisStAlarmIn;   // bit22: 告警输入
        return;
    }
    st_.alarm    = 0;
    st_.err_code = 0;
    if (cst_.warning)         st_.axis_status |= kAxisStAlarmIn;    // bit22: 告警输入
    if (cst_.following_error) st_.axis_status |= kAxisStFollowErr;  // bit8: 随动误差超限出错
    st_.enabled  = enabled ? 1 : 0;

    // ---- 3) 未使能：等 cia402 时序完成 ----
    if (!enabled) {
        if (state_ == State::MOVING || state_ == State::JOG) {
            result_   = MoveResult::ERROR;
            err_text_ = "运动中被去使能";
        }
        state_        = State::DISABLED;
        ack_seen_     = false;
        move_ticks_   = 0;
        settle_ticks_ = 0;
        out->control_word = cw;
        st_.idle = 0;
        return;
    }

    // ---- 4) 已使能 ----
    if (state_ == State::BOOT || state_ == State::DISABLED || state_ == State::ENABLING)
        state_ = State::READY;

    // 4.1 执行使能前挂起的命令
    run_pending();

    // 4.2 运动状态机
    switch (state_) {
    case State::MOVING: {
        // ---- M3：CSP 单轴定位（sub_==SELF）----
        if (sub_ == Sub::SELF) {
            want_mode_  = 8;   // CSP
            mode_valid_ = true;
            out->mode   = want_mode_;

            // 等模式回显（6061）；档案未映射 6061 时不做门控（同 PP 修复语义）
            if (in_mode_disp_ >= 0 && in_mode_disp_ != want_mode_) {
                if (++mode_ticks_ > cfg_.mode_wait_ms) {
                    fail("切换到 CSP 模式超时 (6061 != 8)");
                    result_ = MoveResult::ERROR;
                    state_  = State::READY;
                }
                break;
            }
            mode_ticks_ = 0;

            // 定位超时
            if (++move_ticks_ > cfg_.timeout_ms) {
                fail("CSP 定位超时");
                result_ = MoveResult::ERROR;
                state_  = State::READY;
                break;
            }

            // FASTDEC 停机斜坡（解析）：v = v0 − a·t，位置 = p0 + dir·(v0·t − ½a·t²)
            if (stopping_) {
                stop_t_ += 0.001;
                double v = stop_v0_ - stop_a_ * stop_t_;
                double p;
                if (v <= 0.0) {                     // 减速到 0：停在 v0²/(2a) 处，交回 READY 位置锁定
                    const double d = (stop_v0_ * stop_v0_) / (2.0 * stop_a_);
                    p = (double)stop_p0_ + stop_dir_ * d;
                    out->target_pos = (int32_t)llround(p);
                    target_inc_ = out->target_pos;
                    target_mm_  = inc_to_mm(target_inc_);
                    stopping_ = false;
                    state_    = State::READY;
                    result_   = MoveResult::NONE;
                    sub_      = Sub::NONE;
                    break;
                }
                p = (double)stop_p0_ + stop_dir_ * (stop_v0_ * stop_t_ - 0.5 * stop_a_ * stop_t_ * stop_t_);
                out->target_pos = (int32_t)llround(p);
                target_inc_ = out->target_pos;
                target_mm_  = inc_to_mm(target_inc_);
                break;                              // 停机段不推进 traj_
            }

            // 每拍输出规划位置（607A 槽位与 PP 共用）
            out->target_pos = (int32_t)llround(traj_.step(0.001));
            target_inc_ = out->target_pos;
            target_mm_  = inc_to_mm(target_inc_);

            // 完成判定：规划完成 且 实际位置进入容差（CSP 下不用 6041.bit12/bit10）
            if (traj_.done() &&
                std::fabs(inc_to_mm(in_pos_actual_) - target_mm_) <= cfg_.tol_mm) {
                if (++settle_ticks_ > cfg_.settle_ms) {
                    if (stopping_) {
                        stopping_ = false;               // FASTDEC 减速停机：完成转 READY，不报 move_done
                        result_   = MoveResult::NONE;
                    } else {
                        result_ = MoveResult::DONE;
                    }
                    settle_ticks_ = 0;
                    state_  = State::READY;
                    sub_    = Sub::NONE;
                }
            } else {
                settle_ticks_ = 0;
            }
            break;
        }

        // ---- M3：直线插补参与轴（sub_==LIN；目标由 rt 层 csp_feed 注入，完成由组判定）----
        if (sub_ == Sub::LIN) {
            want_mode_  = 8;   // CSP
            mode_valid_ = true;
            out->mode   = want_mode_;

            if (in_mode_disp_ >= 0 && in_mode_disp_ != want_mode_) {
                if (++mode_ticks_ > cfg_.mode_wait_ms) {
                    fail("切换到 CSP 模式超时 (6061 != 8)");
                    result_ = MoveResult::ERROR;   // 组会检测到并中止整个插补
                    state_  = State::READY;
                    sub_    = Sub::NONE;
                }
                break;
            }
            mode_ticks_ = 0;

            out->target_pos = lin_target_inc_;
            target_inc_ = lin_target_inc_;
            target_mm_  = inc_to_mm(target_inc_);
            break;
        }

        // ---- PP（原路径，sub_==NONE）----
        want_mode_  = 1;   // PP
        mode_valid_ = true;
        out->mode   = want_mode_;

        // 等模式回显（6061）；档案未映射 6061（in_mode_disp_<0）时不做门控
        if (in_mode_disp_ >= 0 && in_mode_disp_ != want_mode_) {
            if (++mode_ticks_ > cfg_.mode_wait_ms) {
                fail("切换到 PP 模式超时 (6061 != 1)");
                result_ = MoveResult::ERROR;
                state_  = State::READY;
            }
            break;
        }
        mode_ticks_ = 0;

        // 定位超时
        if (++move_ticks_ > cfg_.timeout_ms) {
            fail("定位超时");
            result_ = MoveResult::ERROR;
            state_  = State::READY;
            break;
        }

        // 触发/应答握手：bit4 保持到驱动器用 bit12 应答
        if (!ack_seen_) {
            cw |= CW_NEW_SETPOINT;                 // bit4 新目标点
            if (relative_) cw |= CW_CHANGE_SET;    // bit6 相对定位
            if (cst_.setpoint_ack) ack_seen_ = true;
        }

        // 到位判定：应答已收到 且 bit10=1
        if (ack_seen_ && cst_.target_reached) {
            if (++settle_ticks_ > cfg_.settle_ms) {
                const double diff = std::fabs(inc_to_mm(in_pos_actual_) - target_mm_);
                if (diff <= cfg_.tol_mm) {
                    result_ = MoveResult::DONE;
                } else {
                    result_   = MoveResult::ERROR;
                    err_text_ = "到位误差超差 (>MV_TOL)";
                }
                settle_ticks_ = 0;
                state_        = State::READY;
            }
        } else {
            settle_ticks_ = 0;
        }
        break;
    }

    case State::JOG: {
        // v0.8.1：60FF 已映射 → 原 PV 速度模式；未映射（SV630 默认档位）→ PP「目标跟随」模拟点动
        // （否则速度写不进驱动器：RxPDO 里没有 60FF，表现为怎么点都不动）。
        if (vel_pdo_mapped_) {
            want_mode_  = 3;   // PV
            mode_valid_ = true;
            out->mode   = want_mode_;

            if (in_mode_disp_ >= 0 && in_mode_disp_ != want_mode_) {
                if (++mode_ticks_ > cfg_.mode_wait_ms) {
                    fail("切换到 PV 模式超时 (6061 != 3)");
                    result_ = MoveResult::ERROR;
                    state_  = State::READY;
                    vel_inc_ = 0;
                }
                break;
            }
            mode_ticks_ = 0;

            if (vel_inc_ == 0) {   // 点动速度归零 => 结束
                state_ = State::READY;
                break;
            }
            out->target_vel = vel_inc_;
        } else {
            want_mode_  = 1;   // PP 跟随
            mode_valid_ = true;
            out->mode   = want_mode_;

            if (in_mode_disp_ >= 0 && in_mode_disp_ != want_mode_) {
                if (++mode_ticks_ > cfg_.mode_wait_ms) {
                    fail("切换到 PP 模式超时 (6061 != 1)");
                    result_ = MoveResult::ERROR;
                    state_  = State::READY;
                    vel_inc_ = 0;
                }
                break;
            }
            mode_ticks_ = 0;

            if (vel_inc_ == 0) {
                state_ = State::READY;
                break;
            }
            // 目标 = 实际位置 + dir*|v|*前视，每拍推进；
            // 前视 = 0.5s 行程 + 减速距离 v²/(2a)（2026-09-27 现场修复：原 100ms 前视（50mm/s 仅 5mm）
            // 小于/接近驱动器减速距离，驱动器每 20ms「先减速去够近目标、再被新目标拉走」→ 一顿一顿；
            // 加大前视后驱动器始终朝远目标加速/巡航，运动平滑）。
            // bit4「新目标」每 20ms 给 2 拍脉冲（**不依赖 bit12 应答**：应答粘滞会让脉冲停发，
            // 台架上表现为点动一下就不动）。
            if (jog_lead_s_ <= 0.0) {
                fail("点动前视未设置：脚本 JOGLEAD(轴, 秒) 后可用（PP 跟随无固件默认值）");
                result_ = MoveResult::ERROR;
                state_  = State::READY;
                vel_inc_ = 0;
                break;
            }
            const double  v_mm   = std::fabs(inc_to_mm(vel_inc_));
            const double  a_mm   = cfg_.def_accel > 0.0 ? cfg_.def_accel : 500.0;
            const double  look_s = jog_lead_s_ + v_mm / (2.0 * a_mm);   // 前视 = 脚本 JOGLEAD 值 + v²/(2a)
            const int32_t lead   = (int32_t)llround(std::fabs(vel_inc_) * look_s);
            target_inc_     = in_pos_actual_ + (vel_inc_ >= 0 ? lead : -lead);
            target_mm_      = inc_to_mm(in_pos_actual_);   // DPOS 对齐实际（点动时屏上「指令位置」不跳变；驱动器目标单独前视）
            out->target_vel = 0;
            // 2026-09-27：必须同时置 **bit5（立即更新）**——否则 SV630 按“单点模式”先把当前段走完
            // 才接受新目标（仅 1 段缓存），表现为“运行一段距离顿一下”。
            if ((++jog_pulse_ % 20) < 2) cw |= CW_NEW_SETPOINT | CW_CHANGE_SET_IMM;
        }
        break;
    }

    case State::STOPPED: {
        // 急停保持：halt 由 c402 给的 cw 携带，速度强制 0
        out->target_vel = 0;
        ack_seen_ = false;
        break;
    }

    default:
        break;
    }

    // ---- 5) 输出控制字 ----
    out->control_word = cw;

    // ---- 6) 状态里的 idle（等价旧 IDLE(0)!=0 的“运动结束”）----
    if (state_ == State::READY || state_ == State::STOPPED)
        st_.idle = cst_.target_reached ? 1 : 0;
    else
        st_.idle = 0;
}

} // namespace kx
