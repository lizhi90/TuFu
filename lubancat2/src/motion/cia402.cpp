// cia402.cpp —— CiA402 状态机 / 使能时序实现
#include "cia402.h"

#include <cstdio>

namespace kx {

const char* cia402_state_name(Cia402State s) {
    switch (s) {
        case Cia402State::NOT_READY_TO_SWITCH_ON: return "NOT_READY";
        case Cia402State::SWITCH_ON_DISABLED:     return "SWITCH_ON_DISABLED";
        case Cia402State::READY_TO_SWITCH_ON:     return "READY_TO_SWITCH_ON";
        case Cia402State::SWITCHED_ON:            return "SWITCHED_ON";
        case Cia402State::OPERATION_ENABLED:      return "OPERATION_ENABLED";
        case Cia402State::QUICK_STOP_ACTIVE:      return "QUICK_STOP_ACTIVE";
        case Cia402State::FAULT_REACTION_ACTIVE:  return "FAULT_REACTION_ACTIVE";
        case Cia402State::FAULT:                  return "FAULT";
        default:                                  return "UNKNOWN";
    }
}

Cia402Status cia402_parse(uint16_t sw) {
    Cia402Status s;
    s.sw                  = sw;
    s.ready_to_switch_on  = (sw & SW_READY_TO_SWITCH_ON) != 0;
    s.switched_on         = (sw & SW_SWITCHED_ON) != 0;
    s.op_enabled          = (sw & SW_OP_ENABLED) != 0;
    s.fault               = (sw & SW_FAULT) != 0;
    s.voltage_enabled     = (sw & SW_VOLTAGE_ENABLED) != 0;
    s.quick_stop_ok       = (sw & SW_QUICK_STOP) != 0;
    s.switch_on_disabled  = (sw & SW_SWITCH_ON_DISABLED) != 0;
    s.warning             = (sw & SW_WARNING) != 0;
    s.remote              = (sw & SW_REMOTE) != 0;
    s.target_reached      = (sw & SW_TARGET_REACHED) != 0;
    s.internal_limit      = (sw & SW_INTERNAL_LIMIT) != 0;
    s.setpoint_ack        = (sw & SW_SETPOINT_ACK) != 0;
    s.following_error     = (sw & SW_FOLLOWING_ERROR) != 0;
    s.state               = cia402_decode(sw);
    return s;
}

Cia402State cia402_decode(uint16_t sw) {
    // 掩码取自 CiA402 标准的状态译码表
    if ((sw & 0x4Fu) == 0x00u) return Cia402State::NOT_READY_TO_SWITCH_ON;
    if ((sw & 0x4Fu) == 0x40u) return Cia402State::SWITCH_ON_DISABLED;
    if ((sw & 0x6Fu) == 0x21u) return Cia402State::READY_TO_SWITCH_ON;
    if ((sw & 0x6Fu) == 0x23u) return Cia402State::SWITCHED_ON;
    if ((sw & 0x6Fu) == 0x27u) return Cia402State::OPERATION_ENABLED;
    if ((sw & 0x6Fu) == 0x07u) return Cia402State::QUICK_STOP_ACTIVE;
    if ((sw & 0x4Fu) == 0x0Fu) return Cia402State::FAULT_REACTION_ACTIVE;
    if ((sw & 0x4Fu) == 0x08u) return Cia402State::FAULT;
    return Cia402State::UNKNOWN;
}

// ---------------------------------------------------------------------------

void Cia402Axis::reset() {
    st_              = Cia402Status{};
    stage_           = Stage::IDLE;
    stage_ticks_     = 0;
    reset_left_      = 0;
    req_enable_      = false;
    req_disable_     = false;
    req_quick_stop_  = false;
    req_fault_reset_ = false;
    failed_          = false;
    target_mode_     = cfg_.default_mode;
    mode_disp_       = 0;
    cw_out_          = CWCMD_DISABLE_VOLTAGE;
    last_error_      = "";
    was_enabled_     = false;
}

uint16_t Cia402Axis::tick(uint16_t status_word, int8_t mode_disp) {
    st_        = cia402_parse(status_word);
    mode_disp_ = mode_disp;

    // 暂停位（急停）：叠加到任何控制字上，不破坏使能时序
    const uint16_t halt = req_quick_stop_ ? CW_HALT : 0;

    // 显式清故障请求 -> 进入 RESET 时序
    if (req_fault_reset_ && stage_ != Stage::FAULT_RESET) {
        stage_       = Stage::FAULT_RESET;
        stage_ticks_ = 0;
        reset_left_  = (int)cfg_.reset_pulse_ms;
    }

    // 运行中检测到故障：熔断时序；若仍有使能意图则自动走一次清故障
    if (st_.fault && stage_ != Stage::FAULT_RESET) {
        if (stage_ == Stage::DONE || stage_ == Stage::DISABLING || was_enabled_) {
            failed_     = true;
            last_error_ = "运行中驱动器故障 (6041.bit3=1)";
        }
        was_enabled_ = false;
        if (req_enable_) {
            stage_       = Stage::FAULT_RESET;
            stage_ticks_ = 0;
            reset_left_  = (int)cfg_.reset_pulse_ms;
        } else {
            stage_       = Stage::IDLE;
            stage_ticks_ = 0;
            cw_out_      = (uint16_t)(CWCMD_SHUTDOWN | halt);
            return cw_out_;
        }
    }

    // 去使能请求
    if (req_disable_ && stage_ != Stage::IDLE && stage_ != Stage::DISABLING) {
        stage_       = Stage::DISABLING;
        stage_ticks_ = 0;
    }

    ++stage_ticks_;

    // 超时统一处理：熔断时序，控制字退回 0x06
    auto timeout = [&](const char* what) {
        failed_         = true;
        last_error_     = what;
        req_enable_     = false;
        stage_          = Stage::IDLE;
        stage_ticks_    = 0;
        was_enabled_    = false;
        cw_out_         = (uint16_t)(CWCMD_SHUTDOWN | halt);
    };

    switch (stage_) {
    case Stage::FAULT_RESET: {
        cw_out_ = (uint16_t)(CWCMD_FAULT_RESET | halt);
        if (--reset_left_ <= 0) {
            req_fault_reset_ = false;
            if (st_.fault) {
                // 清故障无效（驱动器仍报错）
                failed_      = true;
                last_error_  = "fault reset 无效 (6041.bit3 仍为 1)";
                req_enable_  = false;
                stage_       = Stage::IDLE;
                stage_ticks_ = 0;
                cw_out_      = (uint16_t)(CWCMD_SHUTDOWN | halt);
            } else {
                failed_      = false;
                last_error_  = "";
                stage_       = req_enable_ ? Stage::SHUTDOWN_WAIT : Stage::IDLE;
                stage_ticks_ = 0;
                cw_out_      = (uint16_t)(CWCMD_SHUTDOWN | halt);
            }
        }
        return cw_out_;
    }

    case Stage::IDLE: {
        cw_out_ = (uint16_t)(CWCMD_SHUTDOWN | halt);
        if (req_enable_) {
            stage_       = Stage::SHUTDOWN_WAIT;
            stage_ticks_ = 0;
        }
        return cw_out_;
    }

    case Stage::SHUTDOWN_WAIT: {
        cw_out_ = (uint16_t)(CWCMD_SHUTDOWN | halt);
        if (st_.ready_to_switch_on) {
            stage_       = Stage::SWITCH_ON_WAIT;
            stage_ticks_ = 0;
        } else if (stage_timed_out()) {
            timeout("关机后未收到 ready_to_switch_on (6041.bit0)");
        }
        return cw_out_;
    }

    case Stage::SWITCH_ON_WAIT: {
        cw_out_ = (uint16_t)(CWCMD_SWITCH_ON | halt);
        if (st_.switched_on) {
            stage_       = Stage::ENABLE_WAIT;
            stage_ticks_ = 0;
        } else if (stage_timed_out()) {
            timeout("开机后未收到 switched_on (6041.bit1)");
        }
        return cw_out_;
    }

    case Stage::ENABLE_WAIT: {
        cw_out_ = (uint16_t)(CWCMD_ENABLE_OP | halt);
        if (st_.op_enabled) {
            stage_       = Stage::DONE;
            stage_ticks_ = 0;
            was_enabled_ = true;
        } else if (stage_timed_out()) {
            timeout("使能后未收到 operation_enabled (6041.bit2)");
        }
        return cw_out_;
    }

    case Stage::DONE: {
        cw_out_ = (uint16_t)(CWCMD_ENABLE_OP | halt);
        if (!st_.op_enabled) {
            // 被驱动器/外部断开使能
            failed_      = true;
            last_error_  = "使能丢失 (6041.bit2=0)";
            stage_       = Stage::IDLE;
            stage_ticks_ = 0;
            was_enabled_ = false;
            cw_out_      = (uint16_t)(CWCMD_SHUTDOWN | halt);
        }
        return cw_out_;
    }

    case Stage::DISABLING: {
        cw_out_ = (uint16_t)(CWCMD_SHUTDOWN | halt);
        req_disable_ = false;
        // 回 shutdown：bit1 清 0 且 bit0 置 1
        if (!st_.switched_on && st_.ready_to_switch_on) {
            stage_       = Stage::IDLE;
            stage_ticks_ = 0;
            was_enabled_ = false;
        } else if (stage_timed_out()) {
            stage_       = Stage::IDLE;
            stage_ticks_ = 0;
            was_enabled_ = false;
        }
        return cw_out_;
    }
    }

    cw_out_ = (uint16_t)(CWCMD_SHUTDOWN | halt);
    return cw_out_;
}

} // namespace kx
