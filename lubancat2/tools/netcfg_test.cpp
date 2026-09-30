// netcfg_test.cpp —— 网口配置模块自测（不需要 root，不真正改网口）
//
// 只测「纯判定逻辑」：CIDR/IPv4 校验、保护名单、非法网口名、错误路径的返回。
// 真正的改网口动作需要 root，交给 netcfg_tool 手工验证，不入自动测试。
#include "../src/common/netcfg.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

static void test_ipv4() {
    std::printf("== 1) IPv4 校验 ==\n");
    check(net_valid_ipv4("192.168.1.10"), "合法地址");
    check(net_valid_ipv4("0.0.0.0"), "0.0.0.0 合法");
    check(net_valid_ipv4("255.255.255.255"), "广播地址语法合法");
    check(!net_valid_ipv4("192.168.1.256"), "越界段 -> 非法");
    check(!net_valid_ipv4("192.168.1"), "段数不足 -> 非法");
    check(!net_valid_ipv4("abc"), "非数字 -> 非法");
    check(!net_valid_ipv4(""), "空串 -> 非法");
    check(!net_valid_ipv4(nullptr), "nullptr -> 非法");
}

static void test_cidr() {
    std::printf("== 2) CIDR 解析 ==\n");
    std::string ip;
    int pfx = -1;
    check(net_parse_cidr("10.0.0.5/24", &ip, &pfx) && ip == "10.0.0.5" && pfx == 24, "带掩码");
    check(net_parse_cidr("10.0.0.5", &ip, &pfx) && ip == "10.0.0.5" && pfx == -1, "不带掩码 -> -1");
    check(net_parse_cidr("10.0.0.5/32", &ip, &pfx) && pfx == 32, "/32 合法");
    check(net_parse_cidr("10.0.0.5/0", &ip, &pfx) && pfx == 0, "/0 合法");
    check(!net_parse_cidr("10.0.0.5/33", &ip, &pfx), "/33 非法");
    check(!net_parse_cidr("10.0.0.5/-1", &ip, &pfx), "负数掩码非法");
    check(!net_parse_cidr("10.0.0.5/abc", &ip, &pfx), "非数字掩码非法");
    check(!net_parse_cidr("10.0.0.5/", &ip, &pfx), "空掩码非法");
    check(!net_parse_cidr("", &ip, &pfx), "空串非法");
}

static void test_protection() {
    std::printf("== 3) eth0 保护 ==\n");
    check(net_is_protected("eth0"), "eth0 默认受保护");
    check(!net_is_protected("eth1"), "eth1 默认不受保护");

    // 保护网口必须拒改（即使参数合法）
    NetApplyResult r = net_set_static("eth0", "192.168.1.10", 24, nullptr, false, false);
    check(!r.ok, "改 eth0 被拒");
    check(r.error.find("保护") != std::string::npos, "错误原因提到保护");

    net_set_protected({"eth0", "eth1"});
    check(net_is_protected("eth1"), "保护名单可覆盖");
    net_set_protected({"eth0"});   // 复原
    check(!net_is_protected("eth1"), "保护名单可复原");
}

static void test_reject_bad_args() {
    std::printf("== 4) 参数校验与拒绝 ==\n");
    NetApplyResult r1 = net_set_static("eth0", "bad-ip", 24, nullptr, false, true);
    check(!r1.ok && r1.error.find("非法 IPv4") != std::string::npos, "非法 IP 被拒");

    NetApplyResult r2 = net_set_static("eth0", "192.168.1.10", 99, nullptr, false, true);
    check(!r2.ok && r2.error.find("前缀") != std::string::npos, "非法掩码被拒");

    NetApplyResult r3 = net_set_static("eth0", "192.168.1.10", 24, "bad-gw", false, true);
    check(!r3.ok && r3.error.find("网关") != std::string::npos, "非法网关被拒");

    // 注入尝试：网口名带 shell 元字符必须被拒
    NetApplyResult r4 = net_set_static("eth1; rm -rf /", "192.168.1.10", 24, nullptr, false, true);
    check(!r4.ok && r4.error.find("非法网口名") != std::string::npos, "网口名注入被拒");

    NetApplyResult r5 = net_set_static(nullptr, "192.168.1.10", 24, nullptr, false, true);
    check(!r5.ok, "nullptr 网口名被拒");

    NetApplyResult r6 = net_set_static("eth_no_such_9", "192.168.1.10", 24, nullptr, false, false);
    check(!r6.ok && r6.error.find("不存在") != std::string::npos, "不存在的网口被拒");
}

static void test_enumeration() {
    std::printf("== 5) 枚举（弱断言，不保证环境）==\n");
    const std::vector<std::string> list = net_list_ethernet();
    std::printf("       发现 %zu 个以太网口:", list.size());
    for (const std::string& s : list) std::printf(" %s", s.c_str());
    std::printf("\n");
    check(true, "枚举不崩溃");

    IfaceInfo in;
    const bool has_lo = net_iface_info("lo", &in);
    check(has_lo && in.exists, "lo 可查询");
    if (has_lo) std::printf("       lo: up=%d ip=%s/%d\n", in.up ? 1 : 0, in.ip.c_str(), in.prefix);
}

int main() {
    std::printf("==== netcfg_test ====\n");
    test_ipv4();
    test_cidr();
    test_protection();
    test_reject_bad_args();
    test_enumeration();
    std::printf("=====================\n");
    if (g_fail == 0) std::printf("ALL PASS\n");
    else             std::printf("%d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
