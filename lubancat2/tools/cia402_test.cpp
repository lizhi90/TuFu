// cia402_test.cpp —— 增量3 纯逻辑自测（不需要 IgH / 不需要硬件，随处可编译运行）
//
// 覆盖：
//   1) 状态字译码表（8 个标准状态）
//   2) 使能序列必须依次出现 0x06 -> 0x07 -> 0x0F，且每级等状态位确认
//   3) 去使能回 0x06 并回到 IDLE
//   4) 故障时先发 0x80 清故障，再重建使能
//   5) 状态位迟迟不来 -> 超时报错
#include "../src/motion/cia402.h"

#include <cstdio>
#include <cstring>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// 构造状态字：低 7 位状态 + 常用高位
static uint16_t mk(uint16_t low7, uint16_t extra = SW_REMOTE) { return (uint16_t)(low7 | extra); }

static void test_decode() {
    std::printf("== 1) 状态字译码 ==\n");
    check(cia402_decode(0x0000) == Cia402State::NOT_READY_TO_SWITCH_ON, "0x0000 -> NOT_READY");
    check(cia402_decode(0x0040) == Cia402State::SWITCH_ON_DISABLED,     "0x0040 -> SWITCH_ON_DISABLED");
    check(cia402_decode(0x0021) == Cia402State::READY_TO_SWITCH_ON,     "0x0021 -> READY_TO_SWITCH_ON");
    check(cia402_decode(0x0023) == Cia402State::SWITCHED_ON,            "0x0023 -> SWITCHED_ON");
    check(cia402_decode(0x0027) == Cia402State::OPERATION_ENABLED,      "0x0027 -> OPERATION_ENABLED");
    check(cia402_decode(0x0007) == Cia402State::QUICK_STOP_ACTIVE,      "0x0007 -> QUICK_STOP_ACTIVE");
    check(cia402_decode(0x000F) == Cia402State::FAULT_REACTION_ACTIVE,  "0x000F -> FAULT_REACTION");
    check(cia402_decode(0x0008) == Cia402State::FAULT,                  "0x0008 -> FAULT");

    Cia402Status s = cia402_parse(mk(0x0027, SW_REMOTE | SW_TARGET_REACHED | SW_SETPOINT_ACK));
    check(s.op_enabled && s.remote && s.target_reached && s.setpoint_ack, "高位解析：remote/reached/ack");
}

// 模拟一圈 tick，返回控制字
static uint16_t step(Cia402Axis& ax, uint16_t sw, int8_t mode = 1) { return ax.tick(sw, mode); }

static void test_enable_sequence() {
    std::printf("== 2) 使能序列 ==\n");
    Cia402Axis ax;
    Cia402Axis::Config cfg; cfg.stage_timeout_ms = 50; cfg.default_mode = 1;
    ax.init(cfg);

    // 上电：驱动器处于 SWITCH_ON_DISABLED
    uint16_t cw = step(ax, mk(0x0040));
    check(cw == 0x06, "初始输出 0x06(shutdown)");

    ax.request_enable();
    bool saw06 = false, saw07 = false, saw0f = false;

    // 拍1：仍 SHUTDOWN_WAIT，输出 0x06
    cw = step(ax, mk(0x0040));
    if (cw == 0x06) saw06 = true;
    // 拍2：驱动器 ready_to_switch_on -> 下一级 0x07
    cw = step(ax, mk(0x0021));
    check(cw == 0x06, "READY_TO_SWITCH_ON 当拍仍为 0x06");
    // 拍3：SWITCH_ON_WAIT 输出 0x07
    cw = step(ax, mk(0x0021));
    if (cw == 0x07) saw07 = true;
    // 拍4：switched_on -> 进 ENABLE_WAIT
    cw = step(ax, mk(0x0023));
    // 拍5：输出 0x0F
    cw = step(ax, mk(0x0023));
    if (cw == 0x0F) saw0f = true;
    // 拍6：op_enabled -> DONE
    cw = step(ax, mk(0x0027));
    check(cw == 0x0F, "op_enabled 后维持 0x0F");

    check(saw06 && saw07 && saw0f, "序列依次出现 0x06 -> 0x07 -> 0x0F");
    check(ax.enabled(), "axis.enabled() 为真");
    check(!ax.enable_failed(), "无误报");

    // 继续保持
    cw = step(ax, mk(0x0027));
    check(cw == 0x0F && ax.enabled(), "稳态保持使能");
}

static void test_disable() {
    std::printf("== 3) 去使能 ==\n");
    Cia402Axis ax;
    Cia402Axis::Config cfg; cfg.stage_timeout_ms = 50;
    ax.init(cfg);
    ax.request_enable();
    for (int i = 0; i < 20; ++i) {
        uint16_t sw = mk(0x0027);
        ax.tick(sw, 1);
        if (ax.enabled()) break;
    }
    check(ax.enabled(), "先使能成功");

    ax.request_disable();
    uint16_t cw = ax.tick(mk(0x0027), 1);   // 进 DISABLING，输出 0x06
    check(cw == 0x06, "去使能输出 0x06");
    cw = ax.tick(mk(0x0021), 1);            // 回 ready -> IDLE
    check(cw == 0x06 && !ax.enabled(), "回到 IDLE 且未使能");
}

static void test_fault_reset() {
    std::printf("== 4) 故障清错 ==\n");
    Cia402Axis ax;
    Cia402Axis::Config cfg; cfg.stage_timeout_ms = 50; cfg.reset_pulse_ms = 3;
    ax.init(cfg);

    // 处于故障态，此时请求使能 -> 应先发 0x80
    ax.request_enable();
    uint16_t cw = ax.tick(mk(0x0008), 1);
    check(cw == 0x80, "故障下请求使能 -> 先发 0x80");
    cw = ax.tick(mk(0x0008), 1);
    check(cw == 0x80, "0x80 保持脉冲宽度");
    // 清故障成功（状态字回 SWITCH_ON_DISABLED）
    cw = ax.tick(mk(0x0040), 1);
    check(cw == 0x06, "清故障后回到 0x06 继续使能时序");
    check(!ax.enable_failed(), "清故障成功不报错");

    // 清故障无效场景
    Cia402Axis ax2;
    ax2.init(cfg);
    ax2.request_enable();
    for (int i = 0; i < 6; ++i) ax2.tick(mk(0x0008), 1);
    check(ax2.enable_failed(), "清故障无效 -> enable_failed()=true");
    check(std::strlen(ax2.last_error()) > 0, "有可读错误信息");
}

static void test_timeout() {
    std::printf("== 5) 时序超时 ==\n");
    Cia402Axis ax;
    Cia402Axis::Config cfg; cfg.stage_timeout_ms = 5;
    ax.init(cfg);
    ax.request_enable();
    uint16_t cw = 0;
    for (int i = 0; i < 20; ++i) cw = ax.tick(mk(0x0040), 1);  // 永远不 ready
    check(ax.enable_failed(), "等待 bit0 超时 -> 报错");
    check(cw == 0x06, "超时后控制字回到 0x06");
    check(!ax.enable_busy(), "超时后不再占用时序");
}

static void test_quick_stop_bit() {
    std::printf("== 6) 急停位叠加 ==\n");
    Cia402Axis ax;
    Cia402Axis::Config cfg; cfg.stage_timeout_ms = 50;
    ax.init(cfg);
    ax.request_enable();
    for (int i = 0; i < 20 && !ax.enabled(); ++i) ax.tick(mk(0x0027), 1);
    ax.request_quick_stop();
    uint16_t cw = ax.tick(mk(0x0027), 1);
    check((cw & CW_HALT) != 0, "急停时控制字 bit8=1");
    check((cw & CW_ENABLE_OPERATION) != 0, "急停不丢失使能位");
    ax.clear_quick_stop();
    cw = ax.tick(mk(0x0027), 1);
    check((cw & CW_HALT) == 0, "解除急停后 bit8=0");
}

int main() {
    std::printf("---- cia402 逻辑自测 ----\n");
    test_decode();
    test_enable_sequence();
    test_disable();
    test_fault_reset();
    test_timeout();
    test_quick_stop_bit();
    std::printf("---- %s (失败项 %d) ----\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
