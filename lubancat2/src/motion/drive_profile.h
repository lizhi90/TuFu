// drive_profile.h —— 伺服驱动器「档案库」（厂商相关参数的唯一聚集处）
//
// 目的：把原先散落在 config.h / ethercat_master.h / ethercat_master.cpp / axis.h
//       四处的厂商相关常量，收敛为「一个驱动器一份档案」，新增伺服只加一个文件。
//
// 边界（重要）：
//   * 本文件描述【厂商相关】：身份(vid/pid/rev)、PDO 布局、对象存在性、
//     停机语义、上电默认模式、参数对象地址。
//   * 不含【现场相关】：INC_PER_MM（标定出来的）、速度/加速度上限、容差、超时，
//     这些仍然留在 AppConfig::axis，不随伺服型号变化。
//   * 纯 POD、不 include ecrt.h —— 可在任何机器上单测（见 tools/drive_profile_test.cpp）。
//
// 新增一款伺服：
//   1) 在 src/motion/profiles/ 下新建 xxx.cpp，定义一份 const DriveProfile；
//   2) 文件末尾用 KX_REGISTER_DRIVE_PROFILE(xxx) 自注册（无需改本文件/注册表）；
//   3) 在 CMakeLists.txt 的源文件列表里加上该 .cpp。
#pragma once

#include <cstdint>
#include <cstddef>

namespace kx {

// ---------------------------------------------------------------------------
// PDO 布局描述（与 ecrt 的 ec_pdo_entry_info_t 一一对应，但去掉 ecrt 依赖）
// ---------------------------------------------------------------------------
struct PdoEntry {
    uint16_t index;
    uint8_t  subindex;
    uint8_t  bits;
};

// 偏移绑定槽位：顺序必须与 AxisPdoOffset 字段一致（0..7）
enum PdoSlot {
    SLOT_CTRL_WORD   = 0,   // 0x6040
    SLOT_TARGET_POS  = 1,   // 0x607a
    SLOT_MODE        = 2,   // 0x6060
    SLOT_TARGET_VEL  = 3,   // 0x60ff
    SLOT_STATUS_WORD = 4,   // 0x6041
    SLOT_POS_ACTUAL  = 5,   // 0x6064
    SLOT_POS_DEMAND  = 6,   // 0x6062
    SLOT_MODE_DISP   = 7,   // 0x6061
    SLOT_COUNT       = 8
};

// 一条「对象 -> 槽位」的绑定（用于 ecrt_domain_reg_pdo_entry_list）
struct SlotMap {
    uint16_t index;
    uint8_t  subindex;
    uint8_t  slot;      // PdoSlot
};

// 一个 PDO 映射档位：条目按 [Rx..., Tx...] 排列，n_rx 切分
struct PdoProfile {
    const char*     name;
    const PdoEntry* entries;
    unsigned        n_rx;
    unsigned        n_tx;
    const SlotMap*  slots;
    unsigned        n_slots;
};

// ---------------------------------------------------------------------------
// 停机 / 错误语义（SV630 实测定案，见 docs/planA/03 §7.8）
// ---------------------------------------------------------------------------
struct HaltSemantics {
    uint16_t halt_option_index;      // 0x605D：决定 6040.bit8(halt) 的行为
    uint8_t  halt_option_default;    // 实测 1
    uint16_t quick_stop_index;       // 0x605A：决定 6040.bit2=0 的行为
    uint8_t  quick_stop_default;     // 实测 2
    uint16_t shutdown_index;         // 0x605C：决定伺服 OFF 的行为
    uint8_t  shutdown_default;       // 实测 0
    // 唯一错误码对象：SV630 必须用 0x203F，0x603F 会重复（厂商私有）
    uint16_t error_code_index;
    uint8_t  error_code_bits;        // 32 = Uint32
};

// 非 PDO 的轮廓参数对象（本项目走 SDO 下发）
struct ProfileParamObjects {
    uint16_t profile_vel;    // 0x6081
    uint16_t profile_acc;    // 0x6083
    uint16_t profile_dec;    // 0x6084
    bool     via_sdo;        // true = 只走 SDO（不占 PDO）
};

// ---------------------------------------------------------------------------
// 驱动器档案
// ---------------------------------------------------------------------------
struct DriveProfile {
    // ---- 身份 ----
    const char* name;            // 档案标识（配置用 ECAT_PROFILE=<name>）
    const char* vendor_name;     // 人类可读厂商名（日志用）
    uint32_t    vendor_id;       // 0 = 通配（generic 兜底档案）
    uint32_t    product_code;    // 0 = 通配
    uint32_t    revision;        // SII RevisionNo；0 = 不校验
    bool        strict_revision; // true = rev 不符即不算精确匹配（降级为候选）
    // SII 名称候选（IgH 从 SII 读到的 ProductName/OrderName），可为空指针结尾
    const char* sii_names[4];

    // ---- PDO ----
    int                   default_gear;   // 默认档位（1-based，索引 n_pdos）
    const PdoProfile*     pdos;
    unsigned              n_pdos;
    bool                  allow_sii_default_pdo;  // 是否允许走从站 SII 默认映射
                                                  // （IgH 下一般 false，见 07 §4）

    // ---- 语义 ----
    ProfileParamObjects param_objs;
    HaltSemantics       halt;
    int8_t              default_mode;   // 上电默认 6060（1=PP）
    bool                supports_pp;    // 607a 周期定位
    bool                supports_pv;    // 60ff 速度
    bool                supports_hm;    // 6060=6 回零
    const char*         notes;          // 坑位备忘（日志/文档用）
};

// ---------------------------------------------------------------------------
// 匹配结果
// ---------------------------------------------------------------------------
enum class ProfileMatch {
    EXACT,        // vid/pid(/rev) 全中，且 SII 名称也对得上
    ID_ONLY,      // vid/pid 命中，但 rev 或 SII 名称不符（疑似克隆/新固件）
    GENERIC,      // 只有兜底档案可用
    NONE,         // 无任何可用档案（应报错，绝不静默猜）
};

struct MatchResult {
    ProfileMatch        kind = ProfileMatch::NONE;
    const DriveProfile* profile = nullptr;   // 命中的档案（NONE 时为 nullptr）
    bool                rev_mismatch = false;
    bool                name_mismatch = false;
    char                detail[192] = {0};   // 人类可读的判决理由
};

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------
class ProfileRegistry {
public:
    static ProfileRegistry& instance();

    void add(const DriveProfile* p);
    size_t size() const;
    const DriveProfile* at(size_t i) const;

    // 按档案名精确查（大小写不敏感）；找不到返回 nullptr
    const DriveProfile* find_by_name(const char* name) const;

    // 兜底档案：name == "generic" 的那份（vid/pid 全 0）
    const DriveProfile* fallback() const;
};

// 精确按身份匹配（不做兜底）。sii_name 可为 nullptr。
MatchResult profile_match(uint32_t vendor_id, uint32_t product_code, uint32_t revision,
                          const char* sii_name);

// 名字解析：支持 "auto" / "generic" / 具体档案名。
// 返回 nullptr 表示用户给的名字不认识（调用方须报错，不要静默继续）。
// is_auto 输出是否要求自动识别。
const DriveProfile* profile_resolve_name(const char* name, bool* is_auto, bool* unknown);

// 打印全部已注册档案（调试软件 / CLI --list-profiles 用）
void profile_dump_all(void (*sink)(const char* line, void* user), void* user);

// 单行描述规范（"-" 表示无），返回写入的字符数
int profile_describe(const DriveProfile& p, char* buf, size_t n);

} // namespace kx

// 自注册宏：放在 profiles/xxx.cpp 文件末尾。
// 用函数内静态（Meyers singleton）规避跨 TU 静态初始化顺序问题。
#define KX_REGISTER_DRIVE_PROFILE(var)                                        \
    namespace {                                                               \
    struct KxProfileReg_##var {                                               \
        KxProfileReg_##var() { ::kx::ProfileRegistry::instance().add(&var); }  \
    } g_kx_profile_reg_##var;                                                 \
    }
