// netcfg.cpp —— 网口配置实现（只依赖 iproute2 / libc，不依赖 nmcli）
#include "common/netcfg.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ifaddrs.h>

namespace kx {
namespace {

std::vector<std::string> g_protected = {"eth0"};

// 执行一条 shell 命令，返回退出码（-1 = 起不来）
int run_cmd(const std::string& cmd, std::string* output = nullptr) {
    std::string full = cmd;
    if (output) full += " 2>&1";
    FILE* f = ::popen(full.c_str(), "r");
    if (!f) return -1;
    std::string buf;
    if (output) {
        char tmp[256];
        while (std::fgets(tmp, sizeof(tmp), f)) buf += tmp;
        *output = buf;
    }
    const int rc = ::pclose(f);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

bool write_file_mode(const std::string& path, const std::string& content, int mode) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    ::chmod(path.c_str(), mode);
    return true;
}

bool dir_exists(const char* p) {
    struct stat st;
    return ::stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_exists(const char* p) {
    struct stat st;
    return ::stat(p, &st) == 0;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 校验 iface 名：只允许字母数字 . _ - 且长度 < IFNAMSIZ，防止拼串注入
bool valid_ifname(const char* iface) {
    if (!iface || !*iface) return false;
    const size_t n = std::strlen(iface);
    if (n >= IFNAMSIZ) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = iface[i];
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

bool net_valid_ipv4(const char* text) {
    if (!text || !*text) return false;
    struct in_addr a;
    return ::inet_pton(AF_INET, text, &a) == 1;
}

bool net_parse_cidr(const char* text, std::string* ip_out, int* prefix_out) {
    if (!text || !*text) return false;
    std::string s = trim(text);
    std::string ip = s;
    int prefix = -1;
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        ip = s.substr(0, slash);
        const std::string ps = s.substr(slash + 1);
        if (ps.empty()) return false;
        char* end = nullptr;
        const long v = std::strtol(ps.c_str(), &end, 10);
        if (end == ps.c_str() || *end != '\0' || v < 0 || v > 32) return false;
        prefix = (int)v;
    }
    if (!net_valid_ipv4(ip.c_str())) return false;
    if (ip_out) *ip_out = ip;
    if (prefix_out) *prefix_out = prefix;
    return true;
}

bool net_is_protected(const char* iface) {
    if (!iface) return false;
    for (const std::string& s : g_protected)
        if (s == iface) return true;
    return false;
}

void net_set_protected(const std::vector<std::string>& names) {
    g_protected = names;
}

// ---------------------------------------------------------------------------

bool net_iface_info(const char* iface, IfaceInfo* out) {
    if (!iface || !out) return false;
    *out = IfaceInfo{};
    out->name = iface;
    out->protected_iface = net_is_protected(iface);

    const unsigned idx = ::if_nametoindex(iface);
    if (idx == 0) return false;
    out->exists = true;

    // flags
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
        if (::ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
            out->up = (ifr.ifr_flags & IFF_UP) != 0;
        if (::ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
            char mac[32];
            const unsigned char* m = (const unsigned char*)ifr.ifr_hwaddr.sa_data;
            std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
            out->mac = mac;
        }
        ::close(s);
    }

    // 地址（IPv4 + IPv6 跳过）
    struct ifaddrs* addrs = nullptr;
    if (::getifaddrs(&addrs) == 0) {
        for (struct ifaddrs* p = addrs; p; p = p->ifa_next) {
            if (!p->ifa_addr || !p->ifa_name) continue;
            if (std::strcmp(p->ifa_name, iface) != 0) continue;
            if (p->ifa_addr->sa_family != AF_INET) continue;
            char buf[INET_ADDRSTRLEN] = {0};
            const auto* sin = (const struct sockaddr_in*)p->ifa_addr;
            ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            out->ip = buf;
            const auto* mask = (const struct sockaddr_in*)p->ifa_netmask;
            if (mask) {
                uint32_t m = ntohl(mask->sin_addr.s_addr);
                int n = 0;
                while (m & 0x80000000u) { ++n; m <<= 1; }
                out->prefix = n;
            }
            break;
        }
        ::freeifaddrs(addrs);
    }

    // 默认路由（只报与本网口相关的，简单起见读 /proc/net/route）
    std::ifstream rt("/proc/net/route");
    std::string line;
    bool first = true;
    while (std::getline(rt, line)) {
        if (first) { first = false; continue; }
        std::istringstream is(line);
        std::string ifn, dest, gw, flags;
        is >> ifn >> dest >> gw >> flags;
        if (ifn != iface) continue;
        if (dest != "00000000") continue;
        uint32_t g = (uint32_t)std::strtoul(gw.c_str(), nullptr, 16);
        // /proc/net/route 的网关是网络字节序（小端机器上直接按字节排好）
        char gbuf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &g, gbuf, sizeof(gbuf));
        out->gateway = gbuf;
        break;
    }

    out->method = out->ip.empty() ? "unknown" : "manual";
    return true;
}

std::vector<std::string> net_list_ethernet() {
    std::vector<std::string> out;
    struct ifaddrs* addrs = nullptr;
    if (::getifaddrs(&addrs) != 0) return out;
    for (struct ifaddrs* p = addrs; p; p = p->ifa_next) {
        if (!p->ifa_name || std::strcmp(p->ifa_name, "lo") == 0) continue;
        if (!file_exists((std::string("/sys/class/net/") + p->ifa_name).c_str())) continue;
        bool dup = false;
        for (const std::string& s : out) if (s == p->ifa_name) dup = true;
        if (!dup) out.push_back(p->ifa_name);
    }
    ::freeifaddrs(addrs);
    return out;
}

// ---------------------------------------------------------------------------

namespace {

// 写 NetworkManager keyfile（掉电保留路径之一）
bool write_nm_keyfile(const std::string& iface, const std::string& addr_cidr,
                      const std::string& gw, bool dhcp, std::string* note) {
    const char* dir = "/etc/NetworkManager/system-connections";
    if (!dir_exists(dir)) return false;
    const std::string path = std::string(dir) + "/" + iface + ".nmconnection";
    std::ostringstream os;
    os << "[connection]\n"
       << "id=" << iface << "\n"
       << "type=ethernet\n"
       << "interface-name=" << iface << "\n"
       << "autoconnect=true\n\n"
       << "[ipv4]\n";
    if (dhcp) {
        os << "method=auto\n";
    } else {
        os << "method=manual\n"
           << "address1=" << addr_cidr;
        if (!gw.empty()) os << "," << gw;
        os << "\n"
           << "may-fail=false\n";
    }
    os << "\n[ipv6]\nmethod=ignore\n";
    if (!write_file_mode(path, os.str(), 0600)) return false;
    if (note) *note = path;
    return true;
}

// 写 Debian ifupdown 片段（NM 不存在时的兜底）
bool write_ifupdown(const std::string& iface, const std::string& addr_cidr,
                    const std::string& gw, bool dhcp, std::string* note) {
    const char* dir = "/etc/network/interfaces.d";
    if (!dir_exists(dir)) return false;
    const std::string path = std::string(dir) + "/" + iface;
    std::ostringstream os;
    os << "# 由 kine-x netcfg 生成\n"
       << "auto " << iface << "\n";
    if (dhcp) {
        os << "iface " << iface << " inet dhcp\n";
    } else {
        os << "iface " << iface << " inet static\n"
           << "    address " << addr_cidr << "\n";
        if (!gw.empty()) os << "    gateway " << gw << "\n";
    }
    if (!write_file_mode(path, os.str(), 0644)) return false;
    if (note) *note = path;
    return true;
}

// 运行时立即生效（iproute2）
bool apply_runtime(const std::string& iface, const std::string& addr_cidr,
                   const std::string& gw, bool dhcp, bool bring_up,
                   std::vector<std::string>* steps, std::string* err) {
    auto step = [&](const std::string& cmd, bool fatal) {
        std::string out;
        const int rc = run_cmd(cmd, &out);
        if (steps) steps->push_back(cmd + (rc == 0 ? "  [ok]" : "  [rc=" + std::to_string(rc) + "]"));
        if (rc != 0 && fatal) {
            if (err) *err = "命令失败: " + cmd + " -> " + trim(out);
            return false;
        }
        return true;
    };

    if (bring_up && !step("ip link set " + iface + " up", true)) return false;
    if (!step("ip addr flush dev " + iface, true)) return false;

    if (dhcp) {
        // 优先 dhclient（板上可能没装）；失败不算致命，交由上层提示
        step("dhclient -r " + iface, false);
        if (!step("dhclient -1 -nw " + iface, false)) {
            if (err) *err = "dhclient 未成功（可能未安装），请用持久化配置 + 重启，或手工配 DHCP";
        }
        return true;
    }

    if (!step("ip addr add " + addr_cidr + " dev " + iface, true)) return false;
    if (!gw.empty()) {
        if (!step("ip route replace default via " + gw + " dev " + iface, true)) return false;
    }
    return true;
}

NetApplyResult make_result(const std::string& iface, bool persist) {
    NetApplyResult r;
    r.iface = iface;
    (void)persist;
    return r;
}

} // namespace

NetApplyResult net_set_static(const char* iface, const char* ip, int prefix,
                              const char* gateway, bool persistent, bool force) {
    std::string ifn = iface ? iface : "";
    NetApplyResult r = make_result(ifn, persistent);

    if (!valid_ifname(iface)) { r.error = "非法网口名"; return r; }
    if (prefix < 0 || prefix > 32) { r.error = "前缀长度须在 0..32"; return r; }
    if (!net_valid_ipv4(ip)) { r.error = "非法 IPv4 地址"; return r; }
    if (gateway && *gateway && !net_valid_ipv4(gateway)) { r.error = "非法网关地址"; return r; }
    if (!force && net_is_protected(iface)) {
        r.error = std::string("网口 ") + iface + " 在保护名单（EtherCAT 专用），拒绝修改；确需改动请显式 force";
        return r;
    }
    if (::if_nametoindex(iface) == 0) { r.error = std::string("网口不存在: ") + iface; return r; }

    const std::string cidr = std::string(ip) + "/" + std::to_string(prefix);
    const std::string gw = gateway ? gateway : "";
    r.addr = cidr;
    r.gateway = gw;

    if (!apply_runtime(ifn, cidr, gw, /*dhcp=*/false, /*bring_up=*/true, &r.steps, &r.error))
        return r;

    if (persistent) {
        std::string note;
        if (write_nm_keyfile(ifn, cidr, gw, false, &note)) {
            r.persistent = true;
            r.persistence_kind = "NetworkManager";
            r.steps.push_back("写入 " + note);
        } else if (write_ifupdown(ifn, cidr, gw, false, &note)) {
            r.persistent = true;
            r.persistence_kind = "ifupdown";
            r.steps.push_back("写入 " + note);
        } else {
            r.persistence_kind = "none";
            r.steps.push_back("未找到可写持久化目录（NetworkManager / interfaces.d 均不可用），仅运行时生效");
        }
    }
    r.ok = true;
    return r;
}

NetApplyResult net_set_dhcp(const char* iface, bool persistent, bool force) {
    std::string ifn = iface ? iface : "";
    NetApplyResult r = make_result(ifn, persistent);

    if (!valid_ifname(iface)) { r.error = "非法网口名"; return r; }
    if (!force && net_is_protected(iface)) {
        r.error = std::string("网口 ") + iface + " 在保护名单（EtherCAT 专用），拒绝修改";
        return r;
    }
    if (::if_nametoindex(iface) == 0) { r.error = std::string("网口不存在: ") + iface; return r; }

    std::string err;
    apply_runtime(ifn, "", "", /*dhcp=*/true, /*bring_up=*/true, &r.steps, &err);

    if (persistent) {
        std::string note;
        if (write_nm_keyfile(ifn, "", "", true, &note)) {
            r.persistent = true;
            r.persistence_kind = "NetworkManager";
            r.steps.push_back("写入 " + note);
        } else if (write_ifupdown(ifn, "", "", true, &note)) {
            r.persistent = true;
            r.persistence_kind = "ifupdown";
            r.steps.push_back("写入 " + note);
        } else {
            r.persistence_kind = "none";
            r.steps.push_back("无持久化目录，仅运行时生效");
        }
    }
    if (!err.empty()) r.error = err;
    r.ok = err.empty();
    return r;
}

std::string net_result_to_text(const NetApplyResult& r) {
    std::ostringstream os;
    os << (r.ok ? "[netcfg] OK" : "[netcfg] 失败") << " iface=" << r.iface;
    if (!r.addr.empty()) os << " addr=" << r.addr;
    if (!r.gateway.empty()) os << " gw=" << r.gateway;
    if (r.persistent) os << " 持久化=" << r.persistence_kind;
    os << "\n";
    for (const std::string& s : r.steps) os << "  $ " << s << "\n";
    if (!r.error.empty()) os << "  错误: " << r.error << "\n";
    return os.str();
}

} // namespace kx
