// ===========================================================================
// modbus_master_config.h —— Modbus 主站组态模型（planA/21，P4）
//
//   设备表：名称 / 链路（tcp=标准 MBAP；rtu-tcp=RTU 帧经 TCP 透传网关）/ 站号 /
//           超时 / 重试 / 轮询周期
//   点位表：点名 / 方向（读/写）/ 功能码 / 地址 / 数量 / 类型 / 映射（4x 地址 或 变量）
//   （方向与功能码匹配；寄存器类 count 必须等于类型的字数；线圈类 v1 仅单点）
// ===========================================================================
#pragma once

#include <string>
#include <vector>

#include "modbus/modbus_config.h"   // 复用 MbType 与编解码

namespace kx {

enum class MbLinkKind { TCP, RTU_TCP };

struct MbPointCfg {
    std::string name;
    bool        write   = false;      // false=读（设备→控制器）/ true=写（控制器→设备）
    int         fc      = 3;
    int         addr    = 0;          // 0..65535
    int         count   = 1;          // 与类型一致（寄存器类 1/2；线圈类 1）
    MbType      type    = MbType::U16;
    bool        map_reg = false;      // true=映射到 4x 地址；false=仅按名（变量）
    int         map_addr = 0;         // map_reg 时的 4x 起始
    bool        on_change = true;     // 写点：4x 变化即下发（false=仅显式写时下发）
};

struct MbDeviceCfg {
    std::string          name;
    MbLinkKind           kind = MbLinkKind::TCP;
    std::string          host;
    int                  port = 502;
    int                  unit = 1;
    int                  timeout_ms = 300;
    int                  retries    = 1;
    int                  poll_ms    = 200;
    std::vector<MbPointCfg> points;
};

struct MbMasterConfig {
    std::vector<MbDeviceCfg> devices;
    const MbDeviceCfg* find_dev(const std::string& n) const;
    const MbPointCfg*  find_point(const std::string& dev, const std::string& pt) const;
    // 供运行时按点位索引定位（dev_idx/pt_idx）
    bool locate(const std::string& dev, const std::string& pt, int* di, int* pi) const;
};

bool        mb_master_config_parse(const std::string& text, MbMasterConfig* out, std::string* err);
std::string mb_master_config_to_json(const MbMasterConfig& cfg);

// 与从站组态交叉校验：①主站写点的 4x 映射与从站只读条目冲突 → 拒绝；
//   ②主站各写点 4x 映射字不得互相重叠
bool mb_master_cross_check(const MbMasterConfig& m, const MbConfig& slave, std::string* err);

std::string mb_link_name(MbLinkKind k);
bool        mb_link_from_name(const std::string& s, MbLinkKind* out);
std::string mb_fc_name(int fc);          // "3" / "16" 等
const char* mb_dir_name(bool write);     // "read"/"write"

constexpr int kMbMaxDevices = 32;
constexpr int kMbMaxPoints  = 256;

} // namespace kx
