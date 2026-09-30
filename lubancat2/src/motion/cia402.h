// cia402.h —— CiA402 状态机 / 使能时序（纯逻辑，不碰 EtherCAT，便于单测）
//
// 职责边界：本类只把"状态字 + 模式回显"翻译成"下一拍要写的控制字"。
// 不读写 domain 数据（那是 ethercat_master 的活），不做 mm/inc 换算（那是 axis 的活）。
//
// 依据 docs/planA/04 第 2 节：使能走 0x80 -> 0x06 -> 0x07 -> 0x0F，
// 每一级都等状态位确认；去使能回 0x06。
#pragma once

#include <cstdint>

namespace kx {

// ---------------------------------------------------------------------------
// 控制字（0x6040）位与命令
// ---------------------------------------------------------------------------
enum : uint16_t {
    CW_SWITCH_ON         = 0x0001,  // bit0
    CW_ENABLE_VOLTAGE    = 0x0002,  // bit1
    CW_QUICK_STOP        = 0x0004,  // bit2（0 = 快速停止）
    CW_ENABLE_OPERATION  = 0x0008,  // bit3
    CW_NEW_SETPOINT      = 0x0010,  // bit4  PP: 新目标点（上升沿）
    CW_CHANGE_SET_IMM    = 0x0020,  // bit5  PP: 立即更新
    CW_CHANGE_SET        = 0x0040,  // bit6  PP: 相对定位
    CW_FAULT_RESET       = 0x0080,  // bit7  上升沿清故障
    CW_HALT              = 0x0100,  // bit8  暂停/停止
};
// 常用控制字组合（CiA402 图 4）
enum : uint16_t {
    CWCMD_DISABLE_VOLTAGE = 0x0000,                                        // 0x00 去电压
    CWCMD_SHUTDOWN        = CW_ENABLE_VOLTAGE | CW_QUICK_STOP,             // 0x06 关机
    CWCMD_SWITCH_ON       = CWCMD_SHUTDOWN | CW_SWITCH_ON,                 // 0x07 开机
    CWCMD_ENABLE_OP       = CWCMD_SWITCH_ON | CW_ENABLE_OPERATION,         // 0x0F 使能运行
    CWCMD_QUICK_STOP      = CW_ENABLE_VOLTAGE,                             // 0x02（bit2=0）
    CWCMD_FAULT_RESET     = CW_FAULT_RESET,                                // 0x80
};

// ---------------------------------------------------------------------------
// 状态字（0x6041）位
// ---------------------------------------------------------------------------
enum : uint16_t {
    SW_READY_TO_SWITCH_ON = 0x0001,  // bit0
    SW_SWITCHED_ON        = 0x0002,  // bit1
    SW_OP_ENABLED         = 0x0004,  // bit2
    SW_FAULT              = 0x0008,  // bit3
    SW_VOLTAGE_ENABLED    = 0x0010,  // bit4
    SW_QUICK_STOP         = 0x0020,  // bit5（0 = 正在快速停止）
    SW_SWITCH_ON_DISABLED = 0x0040,  // bit6
    SW_WARNING            = 0x0080,  // bit7
    SW_REMOTE             = 0x0200,  // bit9  远程控制（必须为 1 才接受主机命令）
    SW_TARGET_REACHED     = 0x0400,  // bit10
    SW_INTERNAL_LIMIT     = 0x0800,  // bit11
    SW_SETPOINT_ACK       = 0x1000,  // bit12
    SW_FOLLOWING_ERROR    = 0x2000,  // bit13
};

// 0x6041 低 7 位定义的 8 个状态（CiA402 图 3）
enum class Cia402State {
    NOT_READY_TO_SWITCH_ON,   // xxxx xxxx x0xx 0000
    SWITCH_ON_DISABLED,       // xxxx xxxx x0xx 0001
    READY_TO_SWITCH_ON,       // xxxx xxxx x0xx 0011
    SWITCHED_ON,              // xxxx xxxx x0xx 0111
    OPERATION_ENABLED,        // xxxx xxxx x0xx 1111
    QUICK_STOP_ACTIVE,        // xxxx xxxx x0xx 0111 + bit5=0
    FAULT_REACTION_ACTIVE,    // xxxx xxxx x0xx 1111 + bit3=1
    FAULT,                    // xxxx xxxx x0xx 1000
    UNKNOWN,
};

const char* cia402_state_name(Cia402State s);

// 解析状态字 -> 状态（位语义按 CiA402 标准）
Cia402State cia402_decode(uint16_t sw);

// 状态字快照（一次解析，多处复用）
struct Cia402Status {
    uint16_t    sw = 0;
    Cia402State state = Cia402State::UNKNOWN;
    bool ready_to_switch_on = false;  // bit0
    bool switched_on        = false;  // bit1
    bool op_enabled         = false;  // bit2
    bool fault              = false;  // bit3
    bool voltage_enabled    = false;  // bit4
    bool quick_stop_ok      = false;  // bit5=1 表示不在快速停止
    bool switch_on_disabled = false;  // bit6
    bool warning            = false;  // bit7
    bool remote             = false;  // bit9
    bool target_reached     = false;  // bit10
    bool internal_limit     = false;  // bit11
    bool setpoint_ack       = false;  // bit12
    bool following_error    = false;  // bit13
};

Cia402Status cia402_parse(uint16_t sw);

// ---------------------------------------------------------------------------
// 使能时序状态机
// ---------------------------------------------------------------------------
class Cia402Axis {
public:
    struct Config {
        uint32_t stage_timeout_ms = 500;  // 每级等待上限（1ms 周期 => 拍数）
        uint32_t reset_pulse_ms   = 20;   // 0x80 保持多久（部分驱动器要求脉冲）
        int8_t   default_mode     = 1;    // 1=PP（上电后切到此模式）
    };

    void init(const Config& cfg) { cfg_ = cfg; reset(); }
    void reset();   // 清状态与请求，不动 cfg_

    // ---- 业务线程下发请求（RT 线程消费；均为“意图”，非电平）----
    void request_enable()      { req_enable_ = true;  req_disable_ = false; }
    void request_disable()     { req_disable_ = true; req_enable_ = false; }
    void request_quick_stop()  { req_quick_stop_ = true; }
    void clear_quick_stop()    { req_quick_stop_ = false; }
    void pulse_fault_reset()   { reset_left_ = (int)cfg_.reset_pulse_ms; req_fault_reset_ = true; }
    void set_target_mode(int8_t m) { target_mode_ = m; }
    int8_t target_mode() const { return target_mode_; }

    // ---- RT 周期调用：喂入本拍读到的状态字/模式回显，取回本拍要写的控制字 ----
    uint16_t tick(uint16_t status_word, int8_t mode_disp);

    // ---- 只读观察 ----
    const Cia402Status& status() const { return st_; }
    bool  enabled()       const { return stage_ == Stage::DONE && st_.op_enabled; }
    bool  enable_busy()   const { return stage_ != Stage::IDLE && stage_ != Stage::DONE; }
    bool  enable_failed() const { return failed_; }
    bool  fault()         const { return st_.fault; }
    bool  mode_active()   const { return mode_disp_ == target_mode_; }
    int8_t mode_display() const { return mode_disp_; }
    // 最近一次时序失败原因（人可读，用于 4321 的报错返回）
    const char* last_error() const { return last_error_; }
    // 本拍写出的控制字（调试/观测）
    uint16_t controlword() const { return cw_out_; }

private:
    enum class Stage {
        IDLE,          // 未使能、无请求
        FAULT_RESET,   // 正在发 0x80 清故障
        SHUTDOWN_WAIT, // 0x06 等 bit0
        SWITCH_ON_WAIT,// 0x07 等 bit1
        ENABLE_WAIT,   // 0x0F 等 bit2
        DONE,          // 已使能
        DISABLING,     // 去使能 -> 回 IDLE
    };

    bool stage_timed_out() const { return stage_ticks_ > (long)cfg_.stage_timeout_ms; }

    Config       cfg_{};
    Cia402Status st_{};
    Stage        stage_        = Stage::IDLE;
    long         stage_ticks_  = 0;
    int          reset_left_   = 0;     // 0x80 剩余保持拍数
    bool         req_enable_   = false;
    bool         req_disable_  = false;
    bool         req_quick_stop_ = false;
    bool         req_fault_reset_ = false;
    bool         failed_       = false;
    int8_t       target_mode_  = 1;
    int8_t       mode_disp_    = 0;
    uint16_t     cw_out_       = 0;
    const char*  last_error_   = "";
    bool         was_enabled_  = false; // 去使能后是否曾使能（用于复位）
};

} // namespace kx
