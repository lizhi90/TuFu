// ===========================================================================
// modbus_config.h —— Modbus 组态模型（产品化 · planA/20）
//
//   组态条目：变量名 ↔ 4x 地址 ↔ 类型 ↔ 读写方向（对主站） ↔ 掉电保持 ↔ 默认值 ↔ 说明
//   文件：/userdata/kine-x/config/modbus.json（D12 `mbreg.get/set` 读写；热加载）
//
//   方向语义（access，对主站）：r=主站只读（脚本写值）；w=主站只写（脚本读值）；rw=双向。
//   类型：u16/i16（1 字）；u32/f32（2 字，低字在前）；f32hi（2 字，高字在前）。
//         控制器只存 u16 原始字，类型的编解码服务于按名访问与插件展示。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kx {

enum class MbType { U16, I16, U32, F32, F32HI };
enum class MbAccess { R, W, RW };

struct MbEntry {
    std::string name;
    int         addr       = 0;          // 4x 起始地址 0..65535
    MbType      type       = MbType::U16;
    MbAccess    access     = MbAccess::RW;
    bool        persist    = false;
    bool        has_default = false;
    double      defval     = 0.0;
    std::string desc;
};

struct MbConfig {
    int                 station = 1;      // 从站站号 1..247
    std::vector<MbEntry> entries;

    int span(const MbEntry& e) const;                     // 占字数（1/2）
    const MbEntry* find_name(const std::string& n) const;
    const MbEntry* find_addr(int a) const;                // 覆盖该字的条目（含 32 位第二字）

    // 值 ↔ 原始字（u16 为第一字）
    void     encode(const MbEntry& e, double v, uint16_t* w0, uint16_t* w1) const;
    double   decode(const MbEntry& e, uint16_t w0, uint16_t w1) const;
};

// 解析 + 校验（失败：返回 false，*err = 首个错误，人话中文；成功：out 完整可用）
bool mb_config_parse(const std::string& text, MbConfig* out, std::string* err);

// 序列化为 JSON 文本（字段顺序稳定；D12 get / 插件往返用）
std::string mb_config_to_json(const MbConfig& cfg);

// 值 ↔ 原始字（u16 为第一字；主站/从站共用；字序：u32/f32 低字在前，f32hi 高字在前）
void   mb_encode_value(MbType t, double v, uint16_t* w0, uint16_t* w1);
double mb_decode_value(MbType t, uint16_t w0, uint16_t w1);

std::string mb_type_name(MbType t);
bool        mb_type_from_name(const std::string& s, MbType* out);
std::string mb_access_name(MbAccess a);

constexpr int kMbMaxEntries = 1024;

} // namespace kx
