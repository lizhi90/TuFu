// esi_parser.h —— EtherCAT 从站描述文件（ESI / EtherCATInfo XML）解析
//
// 用途（对应需求「支持导入 xml/esi 和解析，以后开发本设备的调试软件」）：
//   1) 设备选型阶段：调试软件直接读厂商 ESI，显示型号/PDO 布局，不必先接硬件；
//   2) 档案校验：把 ESI 里的 PDO 布局与本机 DriveProfile 对比，提前发现不一致；
//   3) 档案生成：从 ESI 抽出一份候选档案参数（供人工确认后落成 profiles/*.cpp）。
//
// 边界：
//   * 只解析与运动相关的子集（Vendor/Device/Type/Sm/RxPdo/TxPdo/Entry），
//     忽略 DC、Mailbox、Coe 细节；不追求做通用 XML 库。
//   * 不依赖第三方库（无 libxml2 / tinyxml），可在鲁班猫 2 上直编；
//   * 纯逻辑，可单测（tools/esi_parser_test.cpp），不需要 IgH 与硬件。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "motion/drive_profile.h"

namespace kx {

// ESI 里的一个 PDO 条目
struct EsiEntry {
    uint16_t    index    = 0;
    uint8_t     subindex = 0;
    uint8_t     bits     = 0;
    std::string name;
};

// ESI 里的一个 PDO（RxPdo = 主站->从站；TxPdo = 从站->主站）
struct EsiPdo {
    uint16_t             index = 0;      // 0x1600 / 0x1a00 ...
    std::string          name;
    bool                 is_rx = false;
    std::vector<EsiEntry> entries;
};

// ESI 里的一个同步管理器（只取索引与默认 PDO 分配）
struct EsiSm {
    uint8_t              index = 0;
    bool                 is_input = false;
    std::vector<uint16_t> pdo_indexes;   // 默认分配的 PDO 索引
};

struct EsiDevice {
    bool        valid = false;
    std::string error;                  // valid=false 时的原因

    // ---- 身份 ----
    uint32_t    vendor_id    = 0;
    uint32_t    product_code = 0;
    uint32_t    revision     = 0;
    std::string vendor_name;
    std::string device_name;            // <Device><Name>
    std::string device_type;            // <Type> 文本

    // ---- 结构与 PDO ----
    std::vector<EsiSm>  sms;
    std::vector<EsiPdo> pdos;

    // 便捷：按索引找 PDO
    const EsiPdo* find_pdo(uint16_t index) const;
    // 便捷：按对象索引找条目（返回第一个匹配）
    const EsiEntry* find_entry(uint16_t index, uint8_t subindex) const;
    // 统计
    unsigned count_rx_entries() const;
    unsigned count_tx_entries() const;
};

// 从文件 / 从内存字符串解析。成功时 out->valid=true。
bool esi_parse_file(const char* path, EsiDevice* out);
bool esi_parse_string(const char* xml, size_t len, EsiDevice* out);
inline bool esi_parse_string(const std::string& xml, EsiDevice* out) {
    return esi_parse_string(xml.data(), xml.size(), out);
}

// 解析器支持 "#x000c0112" / "0x..." / 十进制；失败返回 false
bool esi_parse_u32(const std::string& token, uint32_t* out);

// ---------------------------------------------------------------------------
// 与驱动器档案对比（提前发现「ESI 说的」与「代码里写的」不一致）
// ---------------------------------------------------------------------------
struct EsiCheckReport {
    bool vid_ok = false;
    bool pid_ok = false;
    bool rev_ok = false;
    // 档案档位与 ESI 的 PDO 条目集合是否一致（按对象 index:sub 比较，不计顺序）
    bool pdo_matches = false;
    unsigned gear_checked = 0;      // 实际比对的档位号（1-based）
    unsigned esi_rx = 0, profile_rx = 0;
    unsigned esi_tx = 0, profile_tx = 0;
    std::vector<std::string> findings;  // 人类可读的逐条结论
    bool all_ok() const { return vid_ok && pid_ok && rev_ok && pdo_matches; }
};

// gear_1based = 0 表示用档案默认档位
EsiCheckReport esi_check_profile(const EsiDevice& dev, const DriveProfile& prof,
                                 int gear_1based = 0);

// 把对比结论排成多行文本（调试软件日志用）
std::string esi_check_to_text(const EsiCheckReport& r);

// ---------------------------------------------------------------------------
// 从 ESI 生成一份「候选档案」的代码片段（给人看，不自动写盘）
//   —— 落成 profiles/<name>.cpp 前必须人工确认 vid/pid 与档位是否真的适配本机。
// ---------------------------------------------------------------------------
std::string esi_to_profile_snippet(const EsiDevice& dev, const char* profile_name);

} // namespace kx
