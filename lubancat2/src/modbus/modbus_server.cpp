// modbus_server.cpp —— 固件内建 Modbus-TCP 从站实现（planA/20）
#include "modbus/modbus_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "script/nvram_store.h"

namespace kx {

namespace {
constexpr size_t kMaxTxBytes = 256 * 1024;   // 单客户端未发送应答上限（防内存耗尽）
} // namespace

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int bit_get(const std::vector<uint8_t>& a, int i) { return (a[(size_t)i >> 3] >> (i & 7)) & 1; }
void bit_set(std::vector<uint8_t>& a, int i, int v) {
    const uint8_t m = (uint8_t)(1u << (i & 7));
    if (v) a[(size_t)i >> 3] |= m;
    else   a[(size_t)i >> 3] &= (uint8_t)~m;
}

constexpr uint8_t kExcIllegalFn   = 0x01;
constexpr uint8_t kExcIllegalAddr = 0x02;
constexpr uint8_t kExcIllegalData = 0x03;

} // namespace

ModbusServer::ModbusServer(NvramStore* nvram) : nvram_(nvram) {}

ModbusServer::~ModbusServer() { stop(); }

bool ModbusServer::load_config_text(const std::string& text, std::string* err) {
    MbConfig cfg;
    if (!mb_config_parse(text, &cfg, err)) {
        return false;
    }
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        apply_config_locked(cfg);
        cfg_ = std::move(cfg);
        n = (int)cfg_.entries.size();
    }
    log("[mbreg] 组态生效 " + std::to_string(n) + " 条（站号 " + std::to_string(cfg_.station) + "）");
    return true;
}

int ModbusServer::entry_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return (int)cfg_.entries.size();
}

std::string ModbusServer::config_json() const {
    std::lock_guard<std::mutex> lk(m_);
    return mb_config_to_json(cfg_);
}

MbConfig ModbusServer::config_copy() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

void ModbusServer::apply_config_locked(const MbConfig& cfg) {
    for (const auto& e : cfg.entries) {
        const int sp = cfg.span(e);
        uint16_t w[2] = {0, 0};
        bool set = false;
        if (e.persist && nvram_ && nvram_->has(e.addr)) {
            w[0] = nvram_->get(e.addr);
            w[1] = (sp == 2) ? nvram_->get(e.addr + 1) : 0;
            set = true;
        } else if (e.has_default) {
            cfg.encode(e, e.defval, &w[0], &w[1]);
            set = true;
        }
        if (set) {
            regs_[(size_t)e.addr] = w[0];
            if (sp == 2) regs_[(size_t)e.addr + 1] = w[1];
        }
    }
}

void ModbusServer::persist_words_locked(const MbEntry& e, const uint16_t* w) {
    if (!nvram_ || !e.persist) return;
    const int sp = cfg_.span(e);
    std::string err;
    for (int i = 0; i < sp; ++i) {
        if (!nvram_->set(e.addr + i, w[i], &err)) {
            log("[mbreg] 持久化失败（" + std::string("4x") + std::to_string(e.addr + i) + "）：" + err);
            return;
        }
    }
}

bool ModbusServer::read_name(const std::string& name, double* v, std::string* err) const {
    std::lock_guard<std::mutex> lk(m_);
    const MbEntry* e = cfg_.find_name(name);
    if (!e) {
        if (err) *err = "未组态变量：" + name + "（用 MB_LIST() 查看）";
        return false;
    }
    const uint16_t w0 = regs_[(size_t)e->addr];
    const uint16_t w1 = (cfg_.span(*e) == 2) ? regs_[(size_t)e->addr + 1] : 0;
    *v = cfg_.decode(*e, w0, w1);
    return true;
}

bool ModbusServer::write_name(const std::string& name, double v, std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    const MbEntry* e = cfg_.find_name(name);
    if (!e) {
        if (err) *err = "未组态变量：" + name + "（用 MB_LIST() 查看）";
        return false;
    }
    uint16_t w[2] = {0, 0};
    cfg_.encode(*e, v, &w[0], &w[1]);
    regs_[(size_t)e->addr] = w[0];
    if (cfg_.span(*e) == 2) regs_[(size_t)e->addr + 1] = w[1];
    persist_words_locked(*e, w);
    return true;
}

std::string ModbusServer::list_text() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string out;
    for (const auto& e : cfg_.entries) {
        out += e.name + ",4x" + std::to_string(e.addr) + "," + mb_type_name(e.type) + "," +
               mb_access_name(e.access) + (e.persist ? ",persist" : "") + "\n";
    }
    return out;
}

std::string ModbusServer::read_zone(int start, int count) const {
    if (count <= 0 || start < 0 || start + count > 65536) return std::string();
    std::lock_guard<std::mutex> lk(m_);
    std::string out((size_t)count * 2, '\0');
    for (int i = 0; i < count; ++i) {
        const uint16_t v = regs_[(size_t)(start + i)];
        out[(size_t)i * 2]     = (char)((v >> 8) & 0xFF);
        out[(size_t)i * 2 + 1] = (char)(v & 0xFF);
    }
    return out;
}

void ModbusServer::write_zone(int start, const std::string& packed) {
    const int count = (int)(packed.size() / 2);
    if (count <= 0 || start < 0 || start + count > 65536) return;
    std::lock_guard<std::mutex> lk(m_);
    for (int i = 0; i < count; ++i) {
        const uint16_t v = (uint16_t)(((uint8_t)packed[(size_t)i * 2] << 8) |
                                      (uint8_t)packed[(size_t)i * 2 + 1]);
        regs_[(size_t)(start + i)] = v;
        const MbEntry* e = cfg_.find_addr(start + i);
        if (e && e->persist && nvram_) {
            std::string err;
            nvram_->set(start + i, v, &err);
        }
    }
}

uint16_t ModbusServer::read_reg(int a) const {
    std::lock_guard<std::mutex> lk(m_);
    if (a < 0 || a > 65535) return 0;
    return regs_[(size_t)a];
}

void ModbusServer::write_reg(int a, uint16_t v) {
    if (a < 0 || a > 65535) return;
    std::lock_guard<std::mutex> lk(m_);
    regs_[(size_t)a] = v;
    const MbEntry* e = cfg_.find_addr(a);
    if (e && e->persist && nvram_) {
        std::string err;
        nvram_->set(a, v, &err);
    }
}

// ---------------------------------------------------------------------------
// 服务线程
// ---------------------------------------------------------------------------
void ModbusServer::start(int port) {
    if (th_.joinable()) return;
    port_ = port;
    stop_ = false;
    retry_at_ms_ = 0;
    th_ = std::thread([this] { thread_main(); });
}

void ModbusServer::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void ModbusServer::try_connect() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        retry_at_ms_ = now_ms() + 5000;
        return;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port_);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        retry_at_ms_ = now_ms() + 5000;
        log("[mbreg] 端口 " + std::to_string(port_) +
            " 绑定失败（被占用？——若旧脚本 502 仍在运行，属预期），5s 后重试");
        return;
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in got{};
    socklen_t gl = sizeof(got);
    if (::getsockname(fd, (sockaddr*)&got, &gl) == 0) port_ = ntohs(got.sin_port);
    listen_fd_ = fd;
    running_ = true;
    log("[mbreg] Modbus-TCP 从站已监听 " + std::to_string(port_) + "（固件内建，站号 " +
        std::to_string(cfg_.station) + "）");
}

void ModbusServer::close_all(std::vector<Client>& cls) {
    for (auto& c : cls) {
        if (c.fd >= 0) ::close(c.fd);
    }
    cls.clear();
    clients_ = 0;
    std::lock_guard<std::mutex> lk(ci_m_);
    cinfo_.clear();
}

std::vector<ModbusServer::ClientInfo> ModbusServer::clients_info() const {
    std::lock_guard<std::mutex> lk(ci_m_);
    return cinfo_;
}

void ModbusServer::accept_new(std::vector<Client>& cls) {
    for (;;) {
        sockaddr_in peer{};
        socklen_t pl = sizeof(peer);
        const int fd = ::accept(listen_fd_, (sockaddr*)&peer, &pl);
        if (fd < 0) return;
        if (cls.size() >= 8) {                        // 客户端上限（超出直接拒绝）
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        int idle = 10, intvl = 5, cnt = 3;            // 半开连接 ~25s 判死（与脚本端口一致）
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        const int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        char ip[32] = "?";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        log(std::string("[mbreg] 主站已连接 ") + ip + ":" + std::to_string(ntohs(peer.sin_port)));
        cls.push_back(Client{fd, {}, {}});
        clients_ = (int)cls.size();
        {
            std::lock_guard<std::mutex> lk(ci_m_);           // D10：快照
            cinfo_.push_back(ClientInfo{fd, ip, (int)ntohs(peer.sin_port)});
        }
    }
}

void ModbusServer::poll_once(std::vector<Client>& cls) {
    accept_new(cls);
    std::vector<pollfd> pf;
    pf.push_back(pollfd{listen_fd_, POLLIN, 0});
    for (auto& c : cls) {
        pf.push_back(pollfd{c.fd, (short)(POLLIN | (c.tx.empty() ? 0 : POLLOUT)), 0});
    }
    const int rc = ::poll(pf.data(), pf.size(), 200);
    if (rc <= 0) return;

    // 1) 收与处理
    for (size_t i = 0; i < cls.size();) {
        Client& c = cls[i];
        bool dead = false;
        if (pf[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
            uint8_t buf[512];
            for (;;) {
                const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    c.rx.insert(c.rx.end(), buf, buf + n);
                    if (c.rx.size() > 4096) c.rx.clear();   // 异常流量保护
                } else if (n == 0) {
                    dead = true;                            // ★对端关闭（EOF）：必须回收，否则 CLOSE-WAIT 占死客户端槽（8 槽满后拒新连）
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    dead = true;                            // ★硬错误（ECONNRESET 等）同样回收
                    break;
                } else {
                    break;                                  // EAGAIN：本轮已读完
                }
            }
        }
        // 拆帧：MBAP 6 字节头（tid/pid/len）+ uid + PDU(len-1)
        for (;;) {
            if (c.rx.size() < 6) break;
            const size_t len = ((size_t)c.rx[4] << 8) | c.rx[5];
            if (len < 2 || len > 260) { c.rx.clear(); break; }
            const size_t total = 6 + len;
            if (c.rx.size() < total) break;
            std::vector<uint8_t> resp = handle_frame(c.rx.data(), total);
            if (!resp.empty()) c.tx.insert(c.tx.end(), resp.begin(), resp.end());
            c.rx.erase(c.rx.begin(), c.rx.begin() + total);
            if (c.tx.size() > kMaxTxBytes) {           // ★应答积压上限：客户端不读 → 断开回收
                dead = true;
                break;
            }
        }
        // 发（已判死则跳过发送，交由下方统一回收）
        while (!dead && !c.tx.empty()) {
            const ssize_t n = ::send(c.fd, c.tx.data(), c.tx.size(), MSG_NOSIGNAL);
            if (n > 0) {
                c.tx.erase(c.tx.begin(), c.tx.begin() + n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                dead = true;
                break;
            }
        }
        if (dead || (pf[i + 1].revents & (POLLHUP | POLLERR))) {
            log("[mbreg] 主站断开");
            const int dead_fd = c.fd;
            ::close(c.fd);
            {
                std::lock_guard<std::mutex> lk(ci_m_);       // D10：快照回收
                for (size_t k = 0; k < cinfo_.size(); ++k) {
                    if (cinfo_[k].fd == dead_fd) { cinfo_.erase(cinfo_.begin() + k); break; }
                }
            }
            cls.erase(cls.begin() + i);
            pf.erase(pf.begin() + i + 1);              // ★与 cls 同步移位：否则下一客户端继承本项的 revents 被误断
            clients_ = (int)cls.size();
            continue;
        }
        ++i;
    }
}

std::vector<uint8_t> ModbusServer::handle_frame(const uint8_t* f, size_t n) {
    if (n < 8) return {};
    const uint8_t tid0 = f[0], tid1 = f[1];
    const uint16_t len = (uint16_t)(((size_t)f[4] << 8) | f[5]);
    const uint8_t uid = f[6];
    const uint8_t fc  = f[7];
    const uint8_t* pdu = f + 7;
    const size_t plen = (size_t)len - 1;

    auto resp = [&](const uint8_t* body, size_t blen) {
        std::vector<uint8_t> r;
        r.reserve(9 + blen);
        r.push_back(tid0);
        r.push_back(tid1);
        r.push_back(0);
        r.push_back(0);
        const uint16_t rl = (uint16_t)(blen + 1);
        r.push_back((uint8_t)(rl >> 8));
        r.push_back((uint8_t)(rl & 0xFF));
        r.push_back(uid);
        r.insert(r.end(), body, body + blen);
        return r;
    };
    auto except = [&](uint8_t code) {
        const uint8_t b[2] = {(uint8_t)(fc | 0x80), code};
        return resp(b, 2);
    };
    auto u16at = [&](size_t off) -> uint16_t {
        return (uint16_t)(((uint16_t)pdu[off] << 8) | pdu[off + 1]);
    };

    std::lock_guard<std::mutex> lk(m_);
    if (uid != cfg_.station) return {};               // 站号不匹配：不响应

    // ---- FC 0x01/0x02：读线圈/离散输入 ----
    if (fc == 0x01 || fc == 0x02) {
        if (plen < 5) return except(kExcIllegalData);
        const int addr = u16at(1), cnt = u16at(3);
        if (cnt < 1 || cnt > 2000 || addr + cnt > 65536) return except(kExcIllegalAddr);
        const auto& src = (fc == 0x01) ? coils_ : inputs_;
        const int bc = (cnt + 7) / 8;
        std::vector<uint8_t> body(2 + (size_t)bc);
        body[0] = fc;
        body[1] = (uint8_t)bc;
        for (int i = 0; i < cnt; ++i) {
            if (bit_get(src, addr + i)) body[2 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
        }
        return resp(body.data(), body.size());
    }
    // ---- FC 0x03/0x04：读保持/输入寄存器 ----
    if (fc == 0x03 || fc == 0x04) {
        if (plen < 5) return except(kExcIllegalData);
        const int addr = u16at(1), cnt = u16at(3);
        if (cnt < 1 || cnt > 125 || addr + cnt > 65536) return except(kExcIllegalAddr);
        std::vector<uint8_t> body(2 + (size_t)cnt * 2);
        body[0] = fc;
        body[1] = (uint8_t)(cnt * 2);
        for (int i = 0; i < cnt; ++i) {
            // 策略 A：4x 全空间普通寄存器；FC04 读预留输入区（恒 0，待扩展）
            const uint16_t v = (fc == 0x03) ? regs_[(size_t)(addr + i)] : 0;
            body[2 + i * 2]     = (uint8_t)(v >> 8);
            body[2 + i * 2 + 1] = (uint8_t)(v & 0xFF);
        }
        return resp(body.data(), body.size());
    }
    // ---- FC 0x05：写单线圈 ----
    if (fc == 0x05) {
        if (plen < 5) return except(kExcIllegalData);
        const int addr = u16at(1);
        const uint16_t v = u16at(3);
        if (v != 0xFF00 && v != 0x0000) return except(kExcIllegalData);
        bit_set(coils_, addr, v == 0xFF00 ? 1 : 0);
        return resp(pdu, 5);
    }
    // ---- FC 0x06：写单寄存器 ----
    if (fc == 0x06) {
        if (plen < 5) return except(kExcIllegalData);
        const int addr = u16at(1);
        const uint16_t v = u16at(3);
        const MbEntry* e = cfg_.find_addr(addr);
        if (e && e->access == MbAccess::R) return except(kExcIllegalAddr);  // 主站写只读
        regs_[(size_t)addr] = v;
        if (e && e->persist && nvram_) {
            std::string err;
            nvram_->set(addr, v, &err);
        }
        return resp(pdu, 5);
    }
    // ---- FC 0x0F：写多线圈 ----
    if (fc == 0x0F) {
        if (plen < 6) return except(kExcIllegalData);
        const int addr = u16at(1), cnt = u16at(3);
        const int bc = pdu[5];
        if (cnt < 1 || cnt > 1968 || addr + cnt > 65536 || bc != (cnt + 7) / 8 ||
            plen < 6 + (size_t)bc)
            return except(kExcIllegalAddr);
        for (int i = 0; i < cnt; ++i) {
            bit_set(coils_, addr + i, (pdu[6 + (i >> 3)] >> (i & 7)) & 1);
        }
        return resp(pdu, 5);
    }
    // ---- FC 0x10：写多寄存器 ----
    if (fc == 0x10) {
        if (plen < 6) return except(kExcIllegalData);
        const int addr = u16at(1), cnt = u16at(3);
        const int bc = pdu[5];
        if (cnt < 1 || cnt > 123 || addr + cnt > 65536 || bc != cnt * 2 ||
            plen < 6 + (size_t)bc)
            return except(kExcIllegalAddr);
        for (int i = 0; i < cnt; ++i) {                 // 覆盖到只读条目 → 整段拒绝
            const MbEntry* e = cfg_.find_addr(addr + i);
            if (e && e->access == MbAccess::R) return except(kExcIllegalAddr);
        }
        for (int i = 0; i < cnt; ++i) {
            const uint16_t v = (uint16_t)(((uint16_t)pdu[6 + i * 2] << 8) | pdu[7 + i * 2]);
            regs_[(size_t)(addr + i)] = v;
            const MbEntry* e = cfg_.find_addr(addr + i);
            if (e && e->persist && nvram_) {
                std::string err;
                nvram_->set(addr + i, v, &err);
            }
        }
        const uint8_t body[5] = {fc, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
                                 (uint8_t)(cnt >> 8), (uint8_t)(cnt & 0xFF)};
        return resp(body, 5);
    }
    return except(kExcIllegalFn);
}

void ModbusServer::thread_main() {
    std::vector<Client> cls;
    while (!stop_.load()) {
        if (listen_fd_ < 0) {
            if (now_ms() >= retry_at_ms_) {
                try_connect();
                if (listen_fd_ < 0) retry_at_ms_ = now_ms() + 5000;
            }
            if (listen_fd_ < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
        }
        poll_once(cls);
    }
    close_all(cls);
    running_ = false;
}

} // namespace kx
