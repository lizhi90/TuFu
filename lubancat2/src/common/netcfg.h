// netcfg.h —— 网口配置（调试软件里「设置 eth1 的 IP」的后端）
//
// 背景（鲁班猫 2 实测）：
//   * eth0 专用于 EtherCAT（IgH generic 驱动独占），**绝不能被本模块改动**；
//   * eth1 用于上位机/调试软件连接，需要能被设为静态 IP 或 DHCP；
//   * 板上没装 nmcli（NetworkManager CLI 缺失），因此：
//       - 立即生效：直接调 `ip addr/route`；
//       - 掉电保留：若存在 /etc/NetworkManager/system-connections，写 NM keyfile；
//         否则回落到 /etc/network/interfaces.d/ 片段（Debian ifupdown）。
//
// 安全设计：
//   * 默认保护名单 {"eth0"}，改保护网口必须显式 force=true；
//   * 所有 IP/掩码/网关先过 inet_pton 校验，杜绝把用户串直接拼进 shell；
//   * 每个动作返回 bool + 可读原因，便于调试软件展示。
#pragma once

#include <string>
#include <vector>

namespace kx {

struct IfaceInfo {
    std::string name;
    bool        exists  = false;
    bool        up      = false;
    std::string mac;
    std::string ip;        // IPv4 地址（可能为空）
    int         prefix  = 0;
    std::string gateway;   // 默认路由网关（may be 空）
    std::string method;    // 猜测：manual / dhcp / unknown
    bool        protected_iface = false;   // 在保护名单里
};

// 读网口信息（不需要 root）
bool net_iface_info(const char* iface, IfaceInfo* out);

// 列出所有以太网口（不含 lo）
std::vector<std::string> net_list_ethernet();

// 是否在保护名单（默认 {"eth0"}）
bool net_is_protected(const char* iface);
// 覆盖/追加保护名单（进程内）
void net_set_protected(const std::vector<std::string>& names);

// ---------------------------------------------------------------------------
// 配置应用
// ---------------------------------------------------------------------------

struct NetApplyResult {
    bool        ok = false;
    std::string iface;
    std::string addr;        // ip/prefix
    std::string gateway;
    bool        persistent = false;   // 是否写了持久化配置
    std::string persistence_kind;     // "NetworkManager" / "ifupdown" / "none"
    std::string error;                // ok=false 时的原因
    std::vector<std::string> steps;   // 逐步执行记录（调试软件日志用）
};

// 设静态 IP。ip 形如 "192.168.1.10"，prefix 0..32；gw 可为空（不设默认路由）。
// force=true 时允许改动保护网口（默认 false）。
// persistent=true 时尽量写盘，重启后仍生效。
NetApplyResult net_set_static(const char* iface, const char* ip, int prefix,
                              const char* gateway, bool persistent, bool force);

// 设 DHCP（运行时用 dhclient 兜底；持久化写 keyfile/interfaces）
NetApplyResult net_set_dhcp(const char* iface, bool persistent, bool force);

// 把结果排成多行文本
std::string net_result_to_text(const NetApplyResult& r);

// 校验工具（调试软件/协议层可先用它拦掉明显错误）
bool net_parse_cidr(const char* text, std::string* ip_out, int* prefix_out);
bool net_valid_ipv4(const char* text);

} // namespace kx
