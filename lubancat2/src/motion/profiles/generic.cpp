// profiles/generic.cpp —— 通用 CiA402 伺服兜底档案
//
// 用途：总线读到的身份在档案库里找不到时，提供一套「只含标准 CiA402 最小对象」的
//       安全映射，让设备至少能进 OP、能被观察；**不可静默当成本机 SV630 使用**。
//
// 约束：
//   * vid/pid 全 0 = 通配，只会被 profile_match() 以 GENERIC 级别返回；
//   * 只含 6040/607a/6060/6041/6064 —— 这 5 个对象是所有 CiA402 位置模式驱动器
//     都必备的，不含任何厂商私有对象（60ff/6061/6062/203F 等一概不假设）；
//   * 停机语义按 CiA402 标准取值（605D=1 halt、605A=2 quick-stop、605C=0 shutdown），
//     错误码用标准 0x603F(Uint16)；
//   * allow_sii_default_pdo=true：通用路径无法预知厂商 PDO，优先信任从站的 SII 默认，
//     但 IgH 未加载 SII 默认 PDO 时会失败 —— 此时应手工指定档案，不要瞎猜。
#include "motion/drive_profile.h"

namespace kx {
namespace {

const PdoEntry kStdEntries[] = {
    {0x6040, 0x00, 16},   // controlword
    {0x607a, 0x00, 32},   // target position
    {0x6060, 0x00,  8},   // modes of operation
    {0x6041, 0x00, 16},   // statusword
    {0x6064, 0x00, 32},   // position actual value
};
const SlotMap kStdSlots[] = {
    {0x6040, 0, SLOT_CTRL_WORD},
    {0x607a, 0, SLOT_TARGET_POS},
    {0x6060, 0, SLOT_MODE},
    {0x6041, 0, SLOT_STATUS_WORD},
    {0x6064, 0, SLOT_POS_ACTUAL},
};

const PdoProfile kGenericPdOs[] = {
    {"CiA402最小(3Rx+2Tx)", kStdEntries, 3, 2, kStdSlots, 5},
};

} // namespace

const DriveProfile kDriveGeneric = {
    /*name             */ "generic",
    /*vendor_name      */ "通用 CiA402",
    /*vendor_id        */ 0,          // 通配
    /*product_code     */ 0,          // 通配
    /*revision         */ 0,          // 不校验
    /*strict_revision  */ false,
    /*sii_names        */ {nullptr, nullptr, nullptr, nullptr},

    /*default_gear     */ 1,
    /*pdos             */ kGenericPdOs,
    /*n_pdos           */ sizeof(kGenericPdOs) / sizeof(kGenericPdOs[0]),
    /*allow_sii_default_pdo*/ true,   // 通用路径优先信任从站 SII 默认映射

    /*param_objs       */ {0x6081, 0x6083, 0x6084, true},
    /*halt             */ {0x605D, 1, 0x605A, 2, 0x605C, 0, 0x603F, 16},
    /*default_mode     */ 1,
    /*supports_pp      */ true,
    /*supports_pv      */ true,
    /*supports_hm      */ false,      // 回零方法厂商差异大，通用档案不假设
    /*notes            */ "兜底档案：仅标准 CiA402 最小映射，务必人工确认后改用专用档案",
};

KX_REGISTER_DRIVE_PROFILE(kDriveGeneric)

} // namespace kx
