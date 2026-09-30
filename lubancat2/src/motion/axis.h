// axis.h —— 单轴运动内核（mm 语义，纯逻辑，不依赖 ecrt，可单测）
//
// 分层：
//   PdoIn/PdoOut  = 本拍读到的 TxPDO / 本拍要写的 RxPDO（由 ethercat_master 搬运）
//   Cia402Axis    = 状态字 -> 控制字（使能时序）
//   Axis          = 命令(AxisCmd) -> 目标位置/速度/模式 + 到位判定 + 状态(AxisStatus)
//
// 模式：
//   PP(6060=1) 定位：607A 目标、bit6 相对、bit4 新点触发，bit12 应答 + bit10 到位
//   PV(6060=3) 点动：60FF 有符号速度
//   6081/6083/6084（轮廓速度/加/减速度）**不在 PDO 内**，通过 params() 交给上层
//   走 SDO 或扩 PDO 下发（见 docs/planA/04 第 3.1 节，⏳ 待上板定案）。
#pragma once

#include <cstdint>

#include "common/state.h"
#include "motion/cia402.h"
#include "motion/interp.h"

namespace kx {

// ---- 本拍 PDO 输入（TxPDO）----
struct AxisPdoIn {
    uint16_t status_word = 0;
    int32_t  pos_actual  = 0;   // 0x6064 inc
    int32_t  pos_demand  = 0;   // 0x6062 inc
    // v0.8.1：0x6062 是否映射；未映射时 DPOS 取内核目标位置（而非 0）
    bool     pos_demand_valid = false;
    int8_t   mode_disp   = 0;   // 0x6061
    // v0.8.1：0x6061 是否在本档案的 TxPDO 中（SV630 档位1 不含）——
    // 未映射时**不做模式门控**（否则 PP/PV 永远等不到回显，定位静默失败）
    bool     mode_disp_valid = false;
};

// ---- 本拍 PDO 输出（RxPDO）----
struct AxisPdoOut {
    uint16_t control_word = 0;  // 0x6040
    int32_t  target_pos   = 0;  // 0x607A
    int8_t   mode         = 0;  // 0x6060
    int32_t  target_vel   = 0;  // 0x60FF
};

// ---- 需要下发的轮廓参数（非 PDO 对象）----
struct AxisParams {
    int32_t profile_vel = 0;    // 0x6081 inc/s
    int32_t profile_acc = 0;    // 0x6083 inc/s^2
    int32_t profile_dec = 0;    // 0x6084 inc/s^2
    bool    dirty       = false;
};

struct AxisConfig {
    double   inc_per_mm   = 14043.41;
    double   def_speed    = 100.0;    // mm/s
    double   def_accel    = 500.0;    // mm/s^2
    double   def_decel    = 0.0;      // mm/s^2（0 = 与 def_accel 相同；脚本 DECEL/SET_DEFAULTS 运行期可改）
    double   spd_max      = 3276.7;   // mm/s 速度上限
    double   tol_mm       = 0.05;     // 到位容差
    uint32_t timeout_ms   = 10000;    // 定位超时
    uint32_t settle_ms    = 200;      // 到位后稳定确认延时（原 move_done 逻辑）
    uint32_t mode_wait_ms = 500;      // 等 6061 模式回显的超时
    // 软件回零：HOME 以 home_mm 为终点做一次绝对定位（绝对编码器系统下即"回到机械零点"）。
    // 驱动器内部回零（6060=6 + 0x6098 方式）为独立后续项，见 docs/planA/08 §8.4。
    double   home_mm      = 0.0;      // 回零终点（mm，用户坐标）
    double   home_speed   = 0.0;      // 回零速度（mm/s，0=用 def_speed）
};

// 运动结果（供 4321 的 move_done / move_error 消费）
enum class MoveResult { NONE, RUNNING, DONE, ERROR };

class Axis {
public:
    void init(const AxisConfig& cfg, const Cia402Axis::Config& c402cfg);

    // 总线状态（由 ethercat 层喂入；false 时清使能、报通讯故障）
    void set_bus_ok(bool ok);
    bool scale_set() const { return cfg_.inc_per_mm > 0.0; }   // 脉冲当量是否已由脚本设置
    // v0.8.1：0x60FF 是否在 RxPDO 中（SV630 档位1 不含）——未映射时点动改用 PP「目标跟随」
    void set_vel_pdo_mapped(bool m) { vel_pdo_mapped_ = m; }

    // 业务命令（RT 线程从 Shared::take() 取到后调用；一次性）
    void apply(const AxisCmd& c);

    // RT 每周期：喂入本拍输入，取回本拍输出
    void tick(const AxisPdoIn& in, AxisPdoOut* out);

    // ---- 只读 ----
    const AxisStatus&   status() const { return st_; }
    const AxisParams&   params() const { return pa_; }
    void                params_flushed() { pa_.dirty = false; }
    const Cia402Axis&   cia402() const { return c402_; }
    MoveResult          move_result() const { return result_; }
    void                clear_result() { result_ = MoveResult::NONE; }
    bool                moving() const { return state_ == State::MOVING || state_ == State::JOG; }
    bool                enabled() const { return st_.enabled != 0; }
    double              target_mm() const { return target_mm_; }
    const char*         state_name() const;
    const char*         last_error() const { return err_text_; }

    // ---- 单位换算（对外暴露，服务层也要用）----
    // 换算考虑坐标系偏移（pos_offset_inc_）：毫米 = 用户坐标，inc = 机器坐标。
    int32_t mm_to_inc(double mm) const;
    double  inc_to_mm(int32_t inc) const;

    // 坐标系置零/置位（DATUM(3)）：令「当前实际位置」在当前用户坐标系下等于 mm（不动电机）
    void    set_position(double mm);

    // ---- M3：CSP（循环同步位置）----
    // 模式切换（0=PP 驱动器规划 / 1=CSP 控制器每拍规划）；运动中拒绝。config 的 MOTION_MODE 初始化。
    void    set_csp_mode(bool on) { csp_mode_ = on; }
    bool    csp_mode() const { return csp_mode_; }
    // 当前实际位置（机器 inc，不含坐标系偏移）——LIN 组起点用
    int32_t mpos_inc() const { return in_pos_actual_; }
    // 进入 LIN 子模式（RT 线程在受理插补会话时调用；要求已使能）
    void    lin_begin();
    // LIN 会话每拍目标（机器 inc）；仅 sub_==LIN 时被 tick 输出
    void    csp_feed(int32_t inc) { lin_target_inc_ = inc; }

    // 点动 PP 跟随前视（秒；实际前视 = 该值 + v²/(2a)）。**固件不设默认值**（0 = 未设置）：
    // 须由脚本 JOGLEAD(轴, 秒) 运行时设置，未设置时 PP 点动明确拒绝（与 UNITS 同一纪律）。
    void    set_jog_lead(double s) {
        if (s < 0.05) s = 0.05;
        if (s > 5.0)  s = 5.0;
        jog_lead_s_ = s;
    }
    double  jog_lead() const { return jog_lead_s_; }

    // S 曲线时间（秒，0~0.25；0=纯梯形）。ZBasic `SRAMP=0~250ms` 对应（脚本运行时设置）。
    void    set_sramp(double s) { sramp_s_ = (s < 0.0) ? 0.0 : (s > 0.25 ? 0.25 : s); }
    double  sramp() const { return sramp_s_; }
    // 急停/停机减速度（mm/s²；0=未设置=旧行为：halt/规划器瞬时对齐）。ZBasic `FASTDEC`。
    void    set_fastdec(double v) { fastdec_mm_ = (v > 0.0) ? v : 0.0; }
    double  fastdec() const { return fastdec_mm_; }
    // 缺省速度/加减速（脚本 SPEED/ACCEL/DECEL 下发；MOVE/JOG 未显式给参时使用）
    void    set_defaults(double spd, double acc, double dec) {
        if (spd > 0.0) cfg_.def_speed = spd;
        if (acc > 0.0) cfg_.def_accel = acc;
        cfg_.def_decel = (dec > 0.0) ? dec : 0.0;
    }
    // LIN 会话结束：ok=true 置 DONE；否则置 ERROR（中止）
    void    lin_end(bool ok);
    // 到位容差（mm）——LIN 组到点确认用
    double  tol_mm() const { return cfg_.tol_mm; }

private:
    enum class State { BOOT, DISABLED, ENABLING, READY, MOVING, JOG, STOPPED, FAULT };
    enum class Sub   { NONE, SELF, LIN };   // MOVING 的子模式：NONE/PP|PV 旧路径，SELF=CSP 单轴，LIN=直线插补参与

    // 未使能时下发的运动命令先挂起，使能后自动执行
    struct Pend {
        bool   active   = false;
        bool   jog      = false;
        bool   relative = false;
        double pos      = 0.0;
        double speed    = 0.0;
        double accel    = 0.0;
    };

    void start_move(bool relative, double pos_mm, double speed, double accel);
    void start_jog(double speed_mm);
    void stop_motion();
    void run_pending();
    void fail(const char* text);
    int32_t clamp_speed_inc(double mm_per_s) const;
    int32_t dist_to_inc(double mm) const;   // 相对定位「距离」-> inc（不含坐标系偏移）
    void convert_input(const AxisPdoIn& in);

    Pend         pend_{};


    AxisConfig   cfg_{};
    Cia402Axis   c402_;
    Cia402Status cst_{};
    AxisStatus   st_{};
    AxisParams   pa_{};
    State        state_ = State::BOOT;

    // ---- M3：CSP ----
    Sub       sub_ = Sub::NONE;       // MOVING 子模式
    bool      csp_mode_ = false;      // 运行模式：false=PP（默认，驱动器规划）true=CSP（控制器规划）
    Trapezoid traj_;                  // CSP 单轴规划器（sub_==SELF）
    int32_t   lin_target_inc_ = 0;    // LIN 会话每拍目标（sub_==LIN）

    // 输入快照
    int32_t in_pos_actual_ = 0;
    int32_t in_pos_demand_ = 0;
    bool    in_pos_demand_known_ = false;   // v0.8.1：0x6062 是否已上报（未映射时 DPOS 用目标位置）
    int8_t  in_mode_disp_  = 0;
    int32_t pos_offset_inc_ = 0;     // 坐标系偏移（机器 inc）：用户 mm = (inc - 偏移)/inc_per_mm

    // 运动上下文
    bool     relative_   = false;
    bool     ack_seen_   = false;   // 已见 6041.bit12（新点已被驱动器接受）
    double   target_mm_  = 0.0;
    int32_t  target_inc_ = 0;
    int32_t  vel_inc_    = 0;       // PV 速度（有符号）
    uint32_t move_ticks_ = 0;       // 定位已运行拍数
    uint32_t settle_ticks_ = 0;     // 到位后稳定拍数
    uint32_t mode_ticks_ = 0;       // 等模式回显拍数
    int8_t   want_mode_  = 0;       // 期望模式（0=不改，回显现模式）
    bool     mode_valid_ = false;   // 是否已经指定过模式
    bool     stop_req_   = false;
    MoveResult result_   = MoveResult::NONE;
    const char* err_text_ = "";
    bool     bus_ok_     = false;
    bool     vel_pdo_mapped_ = true;   // 0x60FF 映射开关（main 每拍按 PDO 偏移设置）
    int      jog_pulse_      = 0;      // PP 跟随点动的 bit4 脉冲计数
    double   jog_lead_s_     = 0.0;    // 点动前视（秒；0=未设置，须脚本 JOGLEAD；固件无默认）
    double   sramp_s_        = 0.0;    // S 曲线时间（秒；0=梯形；脚本 SRAMP 设置）
    double   fastdec_mm_     = 0.0;    // 急停减速度（mm/s²；0=未设置）
    bool     stopping_       = false;  // CSP 减速停机中（完成转 READY，不报 move_done）
    double   stop_t_         = 0.0;    // 停机斜坡已用时（秒）
    double   stop_v0_        = 0.0;    // 停机起始速度（inc/s）
    double   stop_a_         = 0.0;    // 停机减速度（inc/s²）
    int32_t  stop_p0_        = 0;      // 停机起始实际位置（inc）
    int      stop_dir_       = 1;      // 停机方向（+1/-1）
    double   vel_est_mm_     = 0.0;    // 速度估计（mm/s；VP_SPEED）
    int32_t  last_pos_inc_   = 0;      // 上一拍实际位置（速度估计用）
};

} // namespace kx
