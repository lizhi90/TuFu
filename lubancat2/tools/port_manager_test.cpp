// port_manager_test.cpp —— 脚本端口通道的真实 TCP 自测（不需要 IgH / 硬件）
//
// 用回环 socket 验证 PortManager 与 ZBasic 端口语义一致：
//   * OPEN #n,"TCP_SERVER",0 -> 内核分配监听端口，接受单客户端；
//   * OPEN #n,"TCP_CLIENT",端口,"127.0.0.1" -> 非阻塞连接完成后 PORT_STATUS=1；
//   * 断开后 PORT_STATUS 归 0，且允许新客户端/重连接入（脚本靠此自愈）；
//   * 未打开的通道：send=false、recv=-1（明确报错，不静默）。
#include "script/port_manager.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "script/port_config.h"

using namespace kx;

static int g_fail = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// ---- 回环辅助 ----
static int make_listener(int* port_out) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); return -1; }
    if (::listen(fd, 4) < 0)                      { ::close(fd); return -1; }
    socklen_t l = sizeof(a);
    ::getsockname(fd, (sockaddr*)&a, &l);
    *port_out = ntohs(a.sin_port);
    return fd;
}

static int connect_to(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons((uint16_t)port);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); return -1; }
    return fd;
}

// 从裸 fd 读一包（带超时）；无数据/被关闭返回空串
static std::string read_some(int fd, int timeout_ms) {
    pollfd p{ fd, POLLIN, 0 };
    if (::poll(&p, 1, timeout_ms) <= 0) return {};
    char b[256];
    const ssize_t n = ::recv(fd, b, sizeof(b), 0);
    return n > 0 ? std::string(b, (size_t)n) : std::string();
}

// 轮询 PortManager，直到 status==1
static bool wait_conn(PortManager& pm, int port, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 5) {
        if (pm.status(port)) return true;
        ::usleep(5000);
    }
    return pm.status(port) != 0;
}

// 轮询 PortManager 的 recv，直到收到字节（返回收到的内容）
static std::string wait_recv(PortManager& pm, int port, int timeout_ms) {
    std::string got, err;
    for (int t = 0; t < timeout_ms; t += 5) {
        const int n = pm.recv(port, &got, 64, &err);
        if (n > 0) return got;
        ::usleep(5000);
    }
    return got;
}

int main() {
    std::printf("==== port_manager 端口通道自测 ====\n");
    std::string err;

    // ---- 1) TCP_SERVER ----
    std::printf("== 1) TCP_SERVER：监听 / 接受 / 双向收发 / 断开自愈 ==\n");
    {
        PortManager pm;
        check(pm.open(0, "TCP_SERVER", 0, "", &err), "OPEN TCP_SERVER（内核分配端口）成功");
        const int lp = pm.listen_port(0);
        check(lp > 0, "listen_port() 返回有效端口");
        check(pm.kind(0) == PortManager::TCP_SERVER, "kind()=TCP_SERVER");
        check(pm.status(0) == 0, "尚无客户端时 PORT_STATUS=0");

        const int cli = connect_to(lp);
        check(cli >= 0, "测试客户端连接成功");
        check(wait_conn(pm, 0, 1000), "接受客户端后 PORT_STATUS=1");

        check(::send(cli, "abc", 3, 0) == 3, "客户端发送 abc");
        check(wait_recv(pm, 0, 500) == "abc", "GET 收到 abc");

        check(pm.send(0, "xyz", &err), "服务端发送 xyz");
        check(read_some(cli, 500) == "xyz", "客户端收到 xyz");

        // 含 0 字节的原始字节发送（PUTCHAR 语义）
        const std::string raw("\x01\x00\x02", 3);
        check(pm.send(0, raw, &err), "服务端发送含 0 字节的原始数据");
        check(read_some(cli, 500) == raw, "客户端原样收到 01 00 02");

        // 对端关闭 -> 下一次 recv 探测到断开，PORT_STATUS 归 0
        ::close(cli);
        std::string tmp;
        for (int t = 0; t < 300; ++t) {
            pm.recv(0, &tmp, 64, &err);
            if (pm.status(0) == 0) break;
            ::usleep(5000);
        }
        check(pm.status(0) == 0, "对端断开后 PORT_STATUS=0");

        // 新客户端可再次接入（脚本自愈）
        const int cli2 = connect_to(lp);
        check(cli2 >= 0, "新客户端重新连接");
        check(wait_conn(pm, 0, 1000), "重连后 PORT_STATUS=1");
        ::close(cli2);

        pm.close_port(0);
        check(pm.kind(0) == PortManager::NONE, "close_port 后通道被清空");
    }

    // ---- 1b) TCP_SERVER 多客户端（2026-09-28：触摸屏 + 机器人同连 502）----
    std::printf("== 1b) TCP_SERVER 多客户端：并发接受 / 按 idx 收发 / 独立断开 ==\n");
    {
        PortManager pm;
        check(pm.open(0, "TCP_SERVER", 0, "", &err), "OPEN TCP_SERVER 成功");
        const int lp = pm.listen_port(0);
        const int c1 = connect_to(lp);
        const int c2 = connect_to(lp);
        check(c1 >= 0 && c2 >= 0, "两个客户端先后连接");
        (void)pm.status(0);
        for (int t = 0; t < 300 && pm.clients(0) < 2; ++t) { (void)pm.status(0); ::usleep(5000); }
        check(pm.clients(0) == 2, "clients()=2（并发接受）");
        check(pm.status(0) == 1 && pm.status(0, 0) == 1 && pm.status(0, 1) == 1, "idx 0/1 均在线");

        check(::send(c1, "from-c1", 7, 0) == 7, "c1 发送");
        check(::send(c2, "from-c2", 7, 0) == 7, "c2 发送");
        std::string got0, got1;
        for (int t = 0; t < 300 && (got0.size() < 7 || got1.size() < 7); ++t) {
            std::string tmp;
            if (got0.size() < 7 && pm.recv(0, &tmp, 64, &err, 0) > 0) got0 += tmp;
            if (got1.size() < 7 && pm.recv(0, &tmp, 64, &err, 1) > 0) got1 += tmp;
            ::usleep(3000);
        }
        check(got0 == "from-c1" && got1 == "from-c2", "按 idx 分别收到各自数据（不外串）");

        check(pm.send(0, "to-c1", &err, 0) && pm.send(0, "to-c2", &err, 1), "按 idx 分别发送");
        check(read_some(c1, 500) == "to-c1" && read_some(c2, 500) == "to-c2", "各客户端只收到自己的数据");

        {
            int n = 0;
            for (const auto& s : PortManager::snapshot())
                if (s.port == 0 && s.conn) ++n;
            check(n == 2, "D10 快照：同端口 2 条已连接记录");
        }

        ::close(c1);
        std::string tmp3;
        for (int t = 0; t < 300 && pm.status(0, 0) != 0; ++t) { pm.recv(0, &tmp3, 64, &err, 0); ::usleep(5000); }
        check(pm.status(0, 0) == 0 && pm.status(0, 1) == 1 && pm.clients(0) == 1,
              "c1 断开仅释放其槽位（c2 保持在线）");
        check(::send(c2, "still", 5, 0) == 5, "c2 仍可发送");
        std::string got1b;
        for (int t = 0; t < 300 && got1b.empty(); ++t) {
            std::string tmp;
            if (pm.recv(0, &tmp, 64, &err, 1) > 0) got1b += tmp;
            ::usleep(3000);
        }
        check(got1b == "still", "c1 断开后 c2 仍可收发（idx1）");

        check(!pm.send(0, "x", &err, PortManager::kMaxClients), "越界 idx send 失败并报错");
        check(pm.recv(0, &tmp3, 16, &err, -1) == -1, "非法 idx recv 返回 -1");

        ::close(c2);
        pm.close_port(0);
    }

    // ---- 2) TCP_CLIENT ----
    std::printf("== 2) TCP_CLIENT：非阻塞连接 / 双向收发 ==\n");
    {
        PortManager pm;
        int lp = 0;
        const int lst = make_listener(&lp);
        check(lst >= 0 && lp > 0, "测试监听端已就绪");

        check(pm.open(1, "TCP_CLIENT", lp, "127.0.0.1", &err), "OPEN TCP_CLIENT 成功");
        check(pm.kind(1) == PortManager::TCP_CLIENT, "kind()=TCP_CLIENT");
        check(wait_conn(pm, 1, 1000), "非阻塞连接完成后 PORT_STATUS=1");

        pollfd p{ lst, POLLIN, 0 };
        ::poll(&p, 1, 500);
        const int srv = ::accept(lst, nullptr, nullptr);
        check(srv >= 0, "监听端接受连接");

        check(pm.send(1, "ping", &err), "客户端发送 ping");
        check(read_some(srv, 500) == "ping", "服务端收到 ping");

        check(::send(srv, "pong", 4, 0) == 4, "服务端回复 pong");
        check(wait_recv(pm, 1, 500) == "pong", "客户端收到 pong");

        ::close(srv);
        ::close(lst);
        pm.close_port(1);
    }

    // ---- 3) 错误路径：未打开 / 非法类型 / 越界 ----
    std::printf("== 3) 错误路径：未打开明确报错、非法参数被拒 ==\n");
    {
        PortManager pm;
        std::string tmp;
        check(!pm.send(5, "x", &err) && !err.empty(), "未打开通道 send 失败并给出原因");
        check(pm.recv(5, &tmp, 16, &err) == -1, "未打开通道 recv 返回 -1（错误）");
        check(pm.status(5) == 0, "未打开通道 PORT_STATUS=0");

        check(!pm.open(2, "UDP", 1234, "", &err), "不支持的端口类型被拒绝");
        check(!pm.open(PortManager::kMaxSlots, "TCP_SERVER", 0, "", &err),
              "端口号 ≥ 静态容量（64）被拒");
        check(!pm.open(PortManager::global_max_ports(), "TCP_SERVER", 0, "", &err) &&
                  err.find("越界") != std::string::npos,
              "端口号 ≥ 当前运行期上限被拒（原因含“越界”）");

        check(!pm.open(4, "TCP_SERVER", 22, "", &err), "自定义网口用保留端口 22 被拒（非法）");
        check(!pm.open(4, "TCP_SERVER", 80, "", &err) && err.find("80") != std::string::npos,
              "自定义网口用保留端口 80 被拒且给出原因");
        check(pm.open(4, "TCP_SERVER", 12345, "", &err), "自定义网口用非保留端口 12345 可正常打开");
        check(PortManager::is_reserved_port(22) && PortManager::is_reserved_port(80) &&
                  !PortManager::is_reserved_port(1024) && !PortManager::is_reserved_port(0) &&
                  !PortManager::is_reserved_port(5000) && !PortManager::is_reserved_port(12345),
              "保留端口判定正确（仅 22/80；1024/0/5000 等不受限）");
        pm.close_port(4);

        check(pm.open(3, "TCP_SERVER", 0, "", &err), "再次 OPEN 新通道成功");
        check(pm.kind(3) != PortManager::NONE, "新通道 kind 非空");
        pm.close_all();
        check(pm.kind(3) == PortManager::NONE, "close_all 清空所有通道");
    }

    // ---- 4) 运行期上限（D9）：默认 16、扩容、收缩保护、持久化 .portmax ----
    std::printf("== 4) 运行期端口上限（D9）：默认/扩容/收缩保护/持久化 ==\n");
    {
        PortManager pm;
        std::string err;
        check(PortManager::global_max_ports() == PortManager::kDefaultMaxPorts,
              "默认运行期上限 = kDefaultMaxPorts(16)");
        check(PortManager::set_global_max_ports(24, &err), "上限可扩到 24");
        check(pm.open(20, "TCP_SERVER", 0, "", &err), "扩容后端口 20 可打开");
        check(PortManager::open_ports().size() == 1 && PortManager::open_ports()[0] == 20,
              "open_ports() 回读占用端口 = [20]");
        check(!PortManager::set_global_max_ports(16, &err) && err.find("20") != std::string::npos,
              "端口 20 使用中 → 收缩到 16 被拒且指出占用端口");
        pm.close_port(20);
        check(PortManager::open_ports().empty(), "关闭后 open_ports() 为空");
        check(PortManager::set_global_max_ports(16, &err), "关闭后可收缩回 16");
        check(!PortManager::set_global_max_ports(0, &err) && !PortManager::set_global_max_ports(65, &err),
              "上限 0 与 65 被拒（合法范围 1..64）");
        check(!pm.open(16, "TCP_SERVER", 0, "", &err) && pm.open(15, "TCP_SERVER", 0, "", &err),
              "收缩后 16 越界被拒、15 可打开");
        pm.close_all();

        // 持久化：临时目录 round-trip（缺失/非法 → 回退）
        const std::string dir = "/tmp/kx_portmax_test";
        (void)::system(("rm -rf " + dir).c_str());
        check(read_port_max(dir, PortManager::kDefaultMaxPorts) == PortManager::kDefaultMaxPorts,
              ".portmax 缺失 → 回退默认 16");
        check(write_port_max(dir, 24, &err) && read_port_max(dir, 16) == 24,
              ".portmax 写入/回读 = 24（目录自动创建）");
        {
            std::ofstream f(dir + "/.portmax", std::ios::binary | std::ios::trunc);
            f << "-3\n";
        }
        check(read_port_max(dir, 16) == 16, ".portmax 非法内容（-3）→ 回退默认");
        {
            std::ofstream f(dir + "/.portmax", std::ios::binary | std::ios::trunc);
            f << "128\n";
        }
        check(read_port_max(dir, 16) == 16, ".portmax 超范围（128）→ 回退默认");
        (void)::system(("rm -rf " + dir).c_str());
    }

    // ---- 5) D10「通讯状态」：进程级连接快照 + 对端 IP + PORT_INFO 标签 ----
    std::printf("== 5) 连接快照 / 对端 IP / 标签（D10） ==\n");
    {
        PortManager pm;
        std::string err;
        int lp = 0;
        const int lst = make_listener(&lp);
        check(lst >= 0, "测试监听就绪");

        check(pm.open(2, "TCP_SERVER", 0, "", &err), "TCP_SERVER 打开（内核分配端口）");
        {
            const auto sn = PortManager::snapshot();
            check(sn.size() == 1 && sn[0].port == 2 && sn[0].kind == PortManager::TCP_SERVER &&
                      !sn[0].conn && sn[0].lport > 0,
                  "快照：服务端行（监听端口已填、未连接）");
        }
        const int cli = connect_to(lp);   // 连测试自己的 listener：这里改用 pm 的监听端口
        (void)cli;
        // 换用 pm 的监听端口做真实 accept
        {
            const int c2 = connect_to(pm.listen_port(2));
            check(c2 >= 0, "客户端连上 pm 监听");
            check(wait_conn(pm, 2, 1500), "PORT_STATUS=1（已接受）");
            const auto sn = PortManager::snapshot();
            check(sn.size() == 1 && sn[0].conn && sn[0].peer == "127.0.0.1" && sn[0].rport > 0,
                  "快照：已连接 + 对端 IP/端口 抓取正确");
            check(PortManager::set_tag(2, "4321 ASCII", "—", &err), "PORT_INFO 打标签");
            const auto sn2 = PortManager::snapshot();
            check(sn2.size() == 1 && sn2[0].tag == "4321 ASCII" && sn2[0].role == "—",
                  "快照：标签随行回读");
            check(!PortManager::set_tag(64, "x", "", &err), "PORT_INFO 越界被拒（0..63）");
            check(!PortManager::set_tag(2, std::string(49, 'x'), "", &err), "PORT_INFO 用途超长被拒");
            ::close(c2);
            pm.close_port(2);
            check(PortManager::snapshot().empty(), "关闭后快照清空（标签仍保留在进程级表）");
        }
        // 客户端方向：target/peer/远端端口
        check(pm.open(3, "TCP_CLIENT", lp, "127.0.0.1", &err), "TCP_CLIENT 打开");
        check(wait_conn(pm, 3, 1500), "客户端连接完成");
        {
            const auto sn = PortManager::snapshot();
            check(sn.size() == 1 && sn[0].kind == PortManager::TCP_CLIENT && sn[0].conn &&
                      sn[0].peer == "127.0.0.1" && sn[0].rport == lp,
                  "快照：客户端行（远端 IP/端口、已连接）");
        }
        pm.close_all();
        ::close(lst);
    }

    std::printf("==== %s（失败 %d）====\n", g_fail == 0 ? "ALL PASS" : "HAS FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
