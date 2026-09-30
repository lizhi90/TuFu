// profiles/sv630.cpp —— 汇川 SV630 单轴伺服档案（实测定案，见 docs/planA/03 §7）
//
// 实测依据（一块 SV630N，SII 名称 SV630_1Axis_03716 / InoSV630N）：
//   * Vendor 0x00100000 / Product 0x000c0112 / Rev 0x00010000；Enable SDO Info = no。
//   * 真实默认 PDO：SM2=0x1600{6040,607a,6060}(7B) / SM3=0x1a00{6041,6064}(6B)。
//   * 档位3（Rx 增 60ff、Tx 增 6062/6061）会被从站拒绝（AL 0x001E Invalid input config）
//     -> 60FF（速度）只能走 SDO，故 supports_pv=true 但默认档位不含 60ff。
//   * 停机语义：halt(6040.bit8) 由 0x605D 决定（实读 1）；6040.bit2=0 由 0x605A 决定（实读 2）；
//     伺服 OFF 由 0x605C 决定（实读 0）。
//   * 唯一错误码须用 0x203F(Uint32)；0x603F(Uint16) 会重复计数。
#include "motion/drive_profile.h"

namespace kx {
namespace {

// ---- 档位1：实测默认（Rx 3 条 / Tx 2 条）—— 最稳，首选 ----
const PdoEntry kMinEntries[] = {
    // RxPDO
    {0x6040, 0x00, 16},   // controlword
    {0x607a, 0x00, 32},   // target position
    {0x6060, 0x00,  8},   // modes of operation
    // TxPDO
    {0x6041, 0x00, 16},   // statusword
    {0x6064, 0x00, 32},   // position actual value
};
const SlotMap kMinSlots[] = {
    {0x6040, 0, SLOT_CTRL_WORD},
    {0x607a, 0, SLOT_TARGET_POS},
    {0x6060, 0, SLOT_MODE},
    {0x6041, 0, SLOT_STATUS_WORD},
    {0x6064, 0, SLOT_POS_ACTUAL},
};

// ---- 档位2：档位1 + Tx 0x6061（模式回显，供使能/模式确认）----
const PdoEntry kModeDispEntries[] = {
    {0x6040, 0x00, 16},
    {0x607a, 0x00, 32},
    {0x6060, 0x00,  8},
    {0x6041, 0x00, 16},
    {0x6064, 0x00, 32},
    {0x6061, 0x00,  8},   // modes of operation display
};
const SlotMap kModeDispSlots[] = {
    {0x6040, 0, SLOT_CTRL_WORD},
    {0x607a, 0, SLOT_TARGET_POS},
    {0x6060, 0, SLOT_MODE},
    {0x6041, 0, SLOT_STATUS_WORD},
    {0x6064, 0, SLOT_POS_ACTUAL},
    {0x6061, 0, SLOT_MODE_DISP},
};

// ---- 档位3：扩展（Rx 增 0x60ff；Tx 增 0x6062/0x6061）----
// 注意：本机 SV630 拒绝该配置（AL 0x001E），保留仅供换固件/换型号时试用。
const PdoEntry kExtEntries[] = {
    {0x6040, 0x00, 16},
    {0x607a, 0x00, 32},
    {0x6060, 0x00,  8},
    {0x60ff, 0x00, 32},   // target velocity
    {0x6041, 0x00, 16},
    {0x6064, 0x00, 32},
    {0x6062, 0x00, 32},   // position demand value
    {0x6061, 0x00,  8},
};
const SlotMap kExtSlots[] = {
    {0x6040, 0, SLOT_CTRL_WORD},
    {0x607a, 0, SLOT_TARGET_POS},
    {0x6060, 0, SLOT_MODE},
    {0x60ff, 0, SLOT_TARGET_VEL},
    {0x6041, 0, SLOT_STATUS_WORD},
    {0x6064, 0, SLOT_POS_ACTUAL},
    {0x6062, 0, SLOT_POS_DEMAND},
    {0x6061, 0, SLOT_MODE_DISP},
};

const PdoProfile kSv630PdOs[] = {
    {"SV630默认(3Rx+2Tx)",   kMinEntries,      3, 2, kMinSlots,       5},
    {"SV630默认+模式回显",   kModeDispEntries, 3, 3, kModeDispSlots,  6},
    {"扩展(60ff/6062/6061)", kExtEntries,      4, 4, kExtSlots,       8},
};

} // namespace

const DriveProfile kDriveSv630 = {
    /*name            */ "sv630",
    /*vendor_name     */ "汇川 Inovance",
    /*vendor_id       */ 0x00100000,
    /*product_code    */ 0x000c0112,
    /*revision        */ 0x00010000,
    /*strict_revision */ false,   // 同型号不同批次 rev 可能不同：降级为提示，不拒用
    /*sii_names       */ {"SV630_1Axis_03716", "InoSV630N", nullptr, nullptr},

    /*default_gear    */ 1,
    /*pdos            */ kSv630PdOs,
    /*n_pdos          */ sizeof(kSv630PdOs) / sizeof(kSv630PdOs[0]),
    /*allow_sii_default_pdo*/ false,   // IgH 未加载 SII 默认 PDO，必须显式映射

    /*param_objs      */ {0x6081, 0x6083, 0x6084, true},
    /*halt            */ {0x605D, 1, 0x605A, 2, 0x605C, 0, 0x203F, 32},
    /*default_mode    */ 1,       // 上电 PP
    /*supports_pp     */ true,
    /*supports_pv     */ true,    // 60FF 经 SDO（档位3 被从站拒绝）
    /*supports_hm     */ true,
    /*notes           */ "60ff 只能走 SDO；错误码用 203F 勿用 603F；档位3 在 REV 0x00010000 被拒",
};

KX_REGISTER_DRIVE_PROFILE(kDriveSv630)

} // namespace kx
