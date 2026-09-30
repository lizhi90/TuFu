// port_manager.cpp —— 端口通道的真实 TCP 实现（非阻塞，单客户端）
#include "script/port_manager.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>

namespace kx {

std::atomic<int> PortManager::g_max_ports_{PortManager::kDefaultMaxPorts};
std::atomic<int> PortManager::g_open_[PortManager::kMaxSlots];

std::mutex        PortManager::g_snap_mtx_;
std::vector<PortManager::SnapInfo> PortManager::g_snap_[PortManager::kMaxSlots];
std::string       PortManager::g_tag_[PortManager::kMaxSlots];
std::string       PortManager::g_role_[PortManager::kMaxSlots];

int PortManager::global_max_ports() { return g_max_ports_.load(std::memory_order_relaxed); }

bool PortManager::set_global_max_ports(int n, std::string* err) {
    if (n < 1 || n > kMaxSlots) {
        if (err) *err = "端口数量必须在 1.." + std::to_string(kMaxSlots) + " 之间";
        return false;
    }
    for (int p = n; p < kMaxSlots; ++p) {
        if (g_open_[p].load(std::memory_order_relaxed) > 0) {
            if (err) {
                *err = "端口 " + std::to_string(p) + " 正在使用，不能把上限降到 " +
                       std::to_string(n) + "（请先关闭该端口）";
            }
            return false;
        }
    }
    g_max_ports_.store(n, std::memory_order_relaxed);
    return true;
}

// 刷新某端口的进程级快照（D10；由 open/close/status/drop_client 调用）
// 多客户端（2026-09-28）：TCP_SERVER 每个已连接客户端各一条（同端口多行）；
// 无客户端时保留一条 conn=false 以便面板显示监听中。
void PortManager::publish(int port) {
    if (port < 0 || port >= kMaxSlots) return;
    const Slot& s = slots_[port];
    std::vector<SnapInfo> list;
    if (s.kind == TCP_SERVER) {
        bool any = false;
        for (int i = 0; i < kMaxClients; ++i) {
            if (s.conns[i] < 0) continue;
            any = true;
            SnapInfo o;
            o.port   = port;
            o.kind   = s.kind;
            o.lport  = s.lport;
            o.rport  = s.rports[i];
            o.conn   = true;
            o.target = s.target;
            o.peer   = s.peers[i];
            o.tag    = g_tag_[port];
            o.role   = g_role_[port];
            list.push_back(std::move(o));
        }
        if (!any) {
            SnapInfo o;
            o.port = port; o.kind = s.kind; o.lport = s.lport;
            o.conn = false; o.target = s.target;
            o.tag = g_tag_[port]; o.role = g_role_[port];
            list.push_back(std::move(o));
        }
    } else if (s.kind != NONE) {
        SnapInfo o;
        o.port   = port;
        o.kind   = s.kind;
        o.lport  = s.lport;
        o.rport  = s.rports[0];
        o.conn   = s.conns[0] >= 0;
        o.target = s.target;
        o.peer   = s.peers[0];
        o.tag    = g_tag_[port];
        o.role   = g_role_[port];
        list.push_back(std::move(o));
    }
    std::lock_guard<std::mutex> lk(g_snap_mtx_);
    g_snap_[port] = std::move(list);
}

std::vector<PortManager::SnapInfo> PortManager::snapshot() {
    std::vector<SnapInfo> v;
    std::lock_guard<std::mutex> lk(g_snap_mtx_);
    for (int p = 0; p < kMaxSlots; ++p)
        for (const auto& o : g_snap_[p]) v.push_back(o);
    return v;
}

bool PortManager::set_tag(int port, const std::string& tag, const std::string& role, std::string* err) {
    if (port < 0 || port >= kMaxSlots) {
        if (err) *err = "端口号越界（0.." + std::to_string(kMaxSlots - 1) + "）";
        return false;
    }
    if (tag.size() > 48 || role.size() > 24) {
        if (err) *err = "标签过长（用途 ≤48 字、主从 ≤24 字）";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_snap_mtx_);
    g_tag_[port]  = tag;
    g_role_[port] = role;
    for (auto& o : g_snap_[port]) { o.tag = tag; o.role = role; }
    return true;
}

std::vector<int> PortManager::open_ports() {
    std::vector<int> v;
    for (int p = 0; p < kMaxSlots; ++p)
        if (g_open_[p].load(std::memory_order_relaxed) > 0) v.push_back(p);
    return v;
}

namespace {

bool ieq(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t k = 0; k < n; ++k)
        if (std::toupper((unsigned char)a[k]) != std::toupper((unsigned char)b[k])) return false;
    return true;
}

// v0.8.1：TCP 短保活——对端断电/硬重启不会发 FIN，单客户端（502/4321）会一直抱着
// "半开"连接导致新连接进不来。10s 空闲后每 5s 探测、3 次无响应判死（≈25s）→
// recv/send 报错 → drop_conn → status() 接受新连接。
void tune_tcp_keepalive(int fd) {
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 10, intvl = 5, cnt = 3;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#endif
}

} // namespace

PortManager::~PortManager() { close_all(); }

int PortManager::kind(int port) const { return valid(port) ? slots_[port].kind : NONE; }

int PortManager::listen_port(int port) const {
    return (valid(port) && slots_[port].kind == TCP_SERVER) ? slots_[port].lport : 0;
}

std::string PortManager::target(int port) const {
    return valid(port) ? slots_[port].target : std::string();
}

int PortManager::conn_fd(Slot& s, int idx) const {
    if (idx < 0 || idx >= kMaxClients) return -1;
    if (s.kind == TCP_SERVER || s.kind == TCP_CLIENT) return s.conns[idx];
    return -1;
}

void PortManager::drop_client(Slot& s, int idx) {
    if (idx < 0 || idx >= kMaxClients) return;
    if (s.kind == TCP_CLIENT) {
        if (s.fd >= 0) { ::close(s.fd); s.fd = -1; }
    } else if (s.conns[idx] >= 0) {
        ::close(s.conns[idx]);
    }
    s.conns[idx] = -1;
    s.rports[idx] = 0;
    s.peers[idx].clear();
    s.txs[idx].clear();
}

void PortManager::close_port(int port) {
    if (port < 0 || port >= kMaxSlots) return;            // 清理用：不比运行期上限更严
    Slot& s = slots_[port];
    for (int i = 0; i < kMaxClients; ++i) {
        if (s.conns[i] >= 0 && s.conns[i] != s.fd) ::close(s.conns[i]);
    }
    if (s.fd >= 0) ::close(s.fd);
    s = Slot{};
    g_open_[port].store(0, std::memory_order_relaxed);
    publish(port);
}

void PortManager::close_all() {
    for (int p = 0; p < kMaxSlots; ++p) close_port(p);
}

bool PortManager::open(int port, const std::string& type, double portnum,
                       const std::string& ip, std::string* err) {
    if (!valid(port)) {
        if (err) *err = "端口号越界（0.." + std::to_string(global_max_ports() - 1) + "，当前上限 " +
                        std::to_string(global_max_ports()) + "）";
        return false;
    }

    const bool server = ieq(type, "TCP_SERVER");
    const bool client = ieq(type, "TCP_CLIENT") || ieq(type, "TCP");
    if (!server && !client) {
        if (err) *err = "不支持的端口类型: " + type + "（仅 TCP_SERVER / TCP_CLIENT）";
        return false;
    }

    // 自定义网口（监听）不得占用系统保留端口（22/80）——非法，直接拒绝；不打断已有通道。
    if (server && is_reserved_port((int)portnum)) {
        if (err) {
            *err = "端口 " + std::to_string((int)portnum) +
                   " 为系统保留端口（22=SSH / 80=HTTP），不能用于自定义网口";
        }
        return false;
    }

    close_port(port);                                     // 重复 OPEN = 先关后开（重连）

    Slot& s = slots_[port];
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { if (err) *err = std::string("socket() 失败: ") + std::strerror(errno); return false; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    tune_tcp_keepalive(fd);                               // 客户端同样受益（网关/机器人掉电）

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)(int)portnum);

    if (server) {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) {
            if (err) *err = "bind 端口 " + std::to_string((int)portnum) + " 失败: " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (::listen(fd, 1) < 0) {
            if (err) *err = std::string("listen() 失败: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        sockaddr_in got{};
        socklen_t gl = sizeof(got);
        s.lport = (::getsockname(fd, (sockaddr*)&got, &gl) == 0) ? ntohs(got.sin_port) : (int)portnum;
        s.kind  = TCP_SERVER;
        s.fd    = fd;
        s.target = "listen:" + std::to_string(s.lport);
        g_open_[port].store(1, std::memory_order_relaxed);
        publish(port);
        return true;
    }

    if (::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        if (err) *err = "远端 IP 非法: " + ip;
        ::close(fd);
        return false;
    }
    const int rc = ::connect(fd, (sockaddr*)&a, sizeof(a));
    if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        if (err) *err = "connect " + ip + ":" + std::to_string((int)portnum) + " 失败: " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    s.kind   = TCP_CLIENT;
    s.fd     = fd;
    s.target = ip + ":" + std::to_string((int)portnum);
    s.peers[0]  = ip;                                     // TCP_CLIENT：单连接用客户端 0
    s.rports[0] = (int)portnum;
    g_open_[port].store(1, std::memory_order_relaxed);
    publish(port);
    return true;                                          // 连接仍在进行，改由 status() 推进
}

int PortManager::status(int port, int idx) {
    if (!valid(port)) return 0;
    Slot& s = slots_[port];
    if (s.kind == NONE) return 0;

    if (s.kind == TCP_SERVER) {                           // 推进 accept（多客户端，最多 kMaxClients）
        while (s.fd >= 0) {
            int slot = -1;
            for (int i = 0; i < kMaxClients; ++i) if (s.conns[i] < 0) { slot = i; break; }
            if (slot < 0) break;                          // 位满：新连接排队等待
            sockaddr_in pa{};
            socklen_t   pl = sizeof(pa);
            const int c = ::accept4(s.fd, (sockaddr*)&pa, &pl, SOCK_NONBLOCK);
            if (c < 0) break;                             // EAGAIN 等：暂无新连接
            int one = 1;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            tune_tcp_keepalive(c);                        // 断电/重启后 ~25s 内释放旧连接
            s.conns[slot] = c;
            if (pa.sin_family == AF_INET) {               // D10：记录客户端 IP/端口
                char ip[INET_ADDRSTRLEN] = {0};
                if (::inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof(ip))) s.peers[slot] = ip;
                s.rports[slot] = (int)ntohs(pa.sin_port);
            }
            publish(port);
        }
        if (idx < 0) {                                    // 任一客户端（旧语义）
            for (int i = 0; i < kMaxClients; ++i) if (s.conns[i] >= 0) return 1;
            return 0;
        }
        if (idx >= kMaxClients) return 0;
        return s.conns[idx] >= 0 ? 1 : 0;
    }

    // TCP_CLIENT：检查非阻塞连接是否完成（单连接，conns[0]）
    if (s.conns[0] < 0) {
        if (s.fd < 0) return 0;
        pollfd p{ s.fd, POLLOUT, 0 };
        const int r = ::poll(&p, 1, 0);
        if (r > 0 && (p.revents & (POLLOUT | POLLERR | POLLHUP))) {
            int soerr = 0;
            socklen_t l = sizeof(soerr);
            if (::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &soerr, &l) == 0 && soerr == 0) {
                s.conns[0] = s.fd;
                publish(port);                            // D10：连接完成 → 快照置“已连接”
            } else {
                drop_client(s, 0);                        // 连接失败：清掉 fd，等脚本重新 OPEN
                return 0;
            }
        }
    }
    return s.conns[0] >= 0 ? 1 : 0;
}

int PortManager::clients(int port) {
    if (!valid(port)) return 0;
    Slot& s = slots_[port];
    if (s.kind == NONE) return 0;
    (void)status(port);                                   // 顺带推进 accept/connect
    if (s.kind == TCP_CLIENT) return s.conns[0] >= 0 ? 1 : 0;
    int n = 0;
    for (int i = 0; i < kMaxClients; ++i) if (s.conns[i] >= 0) ++n;
    return n;
}

bool PortManager::flush(Slot& s, int idx, std::string* err) {
    const int fd = conn_fd(s, idx);
    if (fd < 0) { if (err) *err = "端口未连接"; return false; }
    while (!s.txs[idx].empty()) {
        const ssize_t n = ::send(fd, s.txs[idx].data(), s.txs[idx].size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;  // 余量留待下次
            const std::string why = std::strerror(errno);
            drop_client(s, idx);
            if (err) *err = "发送失败: " + why;
            return false;
        }
        s.txs[idx].erase(0, (size_t)n);
    }
    return true;
}

bool PortManager::send(int port, const std::string& bytes, std::string* err, int idx) {
    if (!valid(port)) { if (err) *err = "通道号越界"; return false; }
    Slot& s = slots_[port];
    if (s.kind == NONE) { if (err) *err = "通道未打开（先用 OPEN）"; return false; }
    if (idx < 0 || idx >= kMaxClients) { if (err) *err = "客户端下标越界（0..3）"; return false; }
    (void)status(port);                                   // 顺带推进 accept/connect
    if (s.txs[idx].empty() && conn_fd(s, idx) < 0) { if (err) *err = "端口未连接"; return false; }
    s.txs[idx] += bytes;
    return flush(s, idx, err);
}

int PortManager::recv(int port, std::string* out, size_t max, std::string* err, int idx) {
    if (!valid(port)) { if (err) *err = "通道号越界"; return -1; }
    Slot& s = slots_[port];
    if (s.kind == NONE) { if (err) *err = "通道未打开（先用 OPEN）"; return -1; }
    if (idx < 0 || idx >= kMaxClients) { if (err) *err = "客户端下标越界（0..3）"; return -1; }
    (void)status(port);
    const int fd = conn_fd(s, idx);
    if (fd < 0) return 0;                                 // 未连接：无数据（非错误）

    char buf[1024];
    size_t want = max;
    if (want == 0 || want > sizeof(buf)) want = sizeof(buf);
    const ssize_t n = ::recv(fd, buf, want, 0);
    if (n == 0) { drop_client(s, idx); return 0; }        // 对端关闭
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        drop_client(s, idx);
        return 0;
    }
    out->assign(buf, (size_t)n);
    return (int)n;
}

} // namespace kx
