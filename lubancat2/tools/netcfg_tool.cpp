// netcfg_tool.cpp —— 网口配置命令行工具（调试软件「设置 eth1 IP」的后端雏形）
//
// 用法：
//   netcfg_tool show [iface]                  # 不给 iface 时列出所有以太网口
//   netcfg_tool set <iface> <ip>/<prefix> [gw] [--no-persist] [--force]
//   netcfg_tool dhcp <iface> [--no-persist] [--force]
//   netcfg_tool protect [iface ...]           # 打印/覆盖保护名单（仅本进程，用于说明）
//
// 安全：eth0 在默认保护名单里（EtherCAT 专用），改动需显式 --force。
#include "../src/common/netcfg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace kx;

static void usage(const char* a0) {
    std::printf(
        "用法:\n"
        "  %s show [iface]\n"
        "  %s set <iface> <ip>/<prefix> [gateway] [--no-persist] [--force]\n"
        "  %s dhcp <iface> [--no-persist] [--force]\n"
        "\n"
        "说明: eth0 默认受保护（EtherCAT 专用），改动需 --force。\n"
        "      默认持久化：优先写 NetworkManager keyfile，其次 /etc/network/interfaces.d/。\n",
        a0, a0, a0);
}

static void show_one(const std::string& name) {
    IfaceInfo in;
    if (!net_iface_info(name.c_str(), &in)) {
        std::printf("[netcfg] %-6s 不存在\n", name.c_str());
        return;
    }
    std::printf("[netcfg] %-6s %s  mac=%s  %s", in.name.c_str(),
                in.up ? "UP  " : "DOWN", in.mac.c_str(),
                in.protected_iface ? "(受保护) " : "");
    if (in.ip.empty()) std::printf("未配置 IP");
    else               std::printf("ip=%s/%d", in.ip.c_str(), in.prefix);
    if (!in.gateway.empty()) std::printf("  gw=%s", in.gateway.c_str());
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const std::string cmd = argv[1];

    if (cmd == "-h" || cmd == "--help") { usage(argv[0]); return 0; }

    if (cmd == "show") {
        if (argc >= 3) { show_one(argv[2]); return 0; }
        const std::vector<std::string> list = net_list_ethernet();
        if (list.empty()) std::printf("[netcfg] 没有找到以太网口\n");
        for (const std::string& n : list) show_one(n);
        return 0;
    }

    if (cmd == "set") {
        if (argc < 4) { usage(argv[0]); return 2; }
        const char* iface = argv[2];
        const char* cidr  = argv[3];

        bool persist = true, force = false;
        std::string gw;
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--no-persist")) persist = false;
            else if (!std::strcmp(argv[i], "--force")) force = true;
            else if (argv[i][0] != '-') gw = argv[i];
            else { std::fprintf(stderr, "未知参数: %s\n", argv[i]); return 2; }
        }

        std::string ip;
        int prefix = -1;
        if (!net_parse_cidr(cidr, &ip, &prefix)) {
            std::fprintf(stderr, "[netcfg] 非法地址 '%s'（应形如 192.168.1.10/24）\n", cidr);
            return 2;
        }
        if (prefix < 0) prefix = 24;   // 未写掩码时给个常见默认，并提示
        const NetApplyResult r = net_set_static(iface, ip.c_str(), prefix,
                                                gw.empty() ? nullptr : gw.c_str(),
                                                persist, force);
        std::printf("%s", net_result_to_text(r).c_str());
        return r.ok ? 0 : 1;
    }

    if (cmd == "dhcp") {
        if (argc < 3) { usage(argv[0]); return 2; }
        bool persist = true, force = false;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--no-persist")) persist = false;
            else if (!std::strcmp(argv[i], "--force")) force = true;
            else { std::fprintf(stderr, "未知参数: %s\n", argv[i]); return 2; }
        }
        const NetApplyResult r = net_set_dhcp(argv[2], persist, force);
        std::printf("%s", net_result_to_text(r).c_str());
        return r.ok ? 0 : 1;
    }

    if (cmd == "protect") {
        if (argc >= 3) {
            std::vector<std::string> names;
            for (int i = 2; i < argc; ++i) names.push_back(argv[i]);
            net_set_protected(names);
        }
        std::printf("[netcfg] 保护名单（本进程）:\n");
        // 通过 info 间接验证
        for (const char* n : {"eth0", "eth1"}) {
            IfaceInfo in;
            if (net_iface_info(n, &in))
                std::printf("  %s: %s\n", n, in.protected_iface ? "受保护" : "可改");
        }
        return 0;
    }

    usage(argv[0]);
    return 2;
}
