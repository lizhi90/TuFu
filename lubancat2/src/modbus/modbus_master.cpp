// modbus_master.cpp —— 主站引擎实现（planA/21 §3）
#include "modbus/modbus_master.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "modbus/modbus_server.h"

namespace kx {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string w16(uint16_t v) { return std::string{ (char)(v >> 8), (char)(v & 0xFF) }; }

void put_u16(std::string* s, uint16_t v) { *s += (char)(v >> 8); *s += (char)(v & 0xFF); }

uint16_t get_u16(const std::string& s, size_t off) {
    return (uint16_t)(((uint8_t)s[off] << 8) | (uint8_t)s[off + 1]);
}

} // namespace

uint16_t mb_crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

ModbusMaster::ModbusMaster(ModbusServer* store) : store_(store) {}

ModbusMaster::~ModbusMaster() { stop(); }

bool ModbusMaster::load_config_text(const std::string& text, std::string* err) {
    MbMasterConfig cfg;
    if (!mb_master_config_parse(text, &cfg, err)) return false;
    if (store_) {
        const MbConfig slave = store_->config_copy();
        if (!mb_master_cross_check(cfg, slave, err)) return false;
    }
    int nd = 0, np = 0;
    {
        // ★热加载安全：swap 期间持配置交换门闩，等待在飞 IO（do_read/do_write/xfer）结束；
        //   轮询线程的每次设备处理与脚本显式写同样经该门闩，故 clear/resize 时无人持有 cfg_/rt_ 引用。
        std::lock_guard<std::mutex> sw(sw_m_);
        std::lock_guard<std::mutex> lk(m_);
        cfg_ = std::move(cfg);
        rt_.clear();
        rt_.resize(cfg_.devices.size());
        for (size_t i = 0; i < cfg_.devices.size(); ++i) {
            rt_[i].pts.resize(cfg_.devices[i].points.size());
            nd++;
            np += (int)cfg_.devices[i].points.size();
        }
    }
    log("[mbdev] 主站组态生效：" + std::to_string(nd) + " 设备 / " + std::to_string(np) + " 点位");
    return true;
}

int ModbusMaster::device_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return (int)cfg_.devices.size();
}

std::string ModbusMaster::config_json() const {
    std::lock_guard<std::mutex> lk(m_);
    return mb_master_config_to_json(cfg_);
}

bool ModbusMaster::read_point(const std::string& dev, const std::string& pt, double* v,
                              std::string* err) const {
    std::lock_guard<std::mutex> lk(m_);
    int di = 0, pi = 0;
    if (!cfg_.locate(dev, pt, &di, &pi)) {
        if (err) *err = "未组态主站点位：" + dev + "." + pt + "（MBD_LIST() 查看）";
        return false;
    }
    const PtRt& r = rt_[di].pts[pi];
    if (!r.has) {
        if (err) *err = "点位尚无数据（设备离线/首轮未完成）：" + dev + "." + pt;
        return false;
    }
    *v = r.value;
    return true;
}

bool ModbusMaster::write_point(const std::string& dev, const std::string& pt, double v,
                               std::string* err) {
    int di = 0, pi = 0;
    std::lock_guard<std::mutex> sw(sw_m_);      // ★与热加载互斥：持有期间 cfg_/rt_ 不会换表
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!cfg_.locate(dev, pt, &di, &pi)) {
            if (err) *err = "未组态主站点位：" + dev + "." + pt + "（MBD_LIST() 查看）";
            return false;
        }
        if (!cfg_.devices[di].points[pi].write) {
            if (err) *err = "点位为只读（dir=read）：" + dev + "." + pt;
            return false;
        }
    }
    return do_write(di, pi, v);
}

std::string ModbusMaster::status_text(const std::string& dev) const {
    std::lock_guard<std::mutex> lk(m_);
    std::string out;
    for (size_t i = 0; i < cfg_.devices.size(); ++i) {
        const auto& d = cfg_.devices[i];
        if (!dev.empty() && d.name != dev) continue;
        const auto& r = rt_[i];
        out += d.name + ": " + (r.online ? "online" : "offline") + ", err=" + std::to_string(r.err) +
               ", timeouts=" + std::to_string(r.timeouts) + ", ok=" + std::to_string(r.ok_count);
        if (!r.last_err.empty()) out += ", last=" + r.last_err;
        out += "\n";
    }
    if (out.empty()) out = "未组态主站设备\n";
    return out;
}

std::string ModbusMaster::list_text() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string out;
    for (const auto& d : cfg_.devices) {
        for (const auto& p : d.points) {
            out += d.name + "." + p.name + "," + mb_dir_name(p.write) + ",fc" + std::to_string(p.fc) +
                   "," + mb_type_name(p.type) +
                   (p.map_reg ? (",4x" + std::to_string(p.map_addr)) : ",var") + "\n";
        }
    }
    return out;
}

std::string ModbusMaster::status_json() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string out = "{\"devices\":[";
    for (size_t i = 0; i < cfg_.devices.size(); ++i) {
        const auto& d = cfg_.devices[i];
        const auto& r = rt_[i];
        if (i) out += ",";
        out += "{\"name\":\"" + d.name + "\",\"online\":" + (r.online ? "true" : "false") +
               ",\"err\":" + std::to_string(r.err) + ",\"timeouts\":" + std::to_string(r.timeouts) +
               ",\"ok\":" + std::to_string(r.ok_count) +
               ",\"last_ok\":" + std::to_string(r.last_ok) +
               ",\"last_err\":\"" + r.last_err + "\",\"points\":" +
               std::to_string((int)d.points.size()) + "}";
    }
    out += "]}";
    return out;
}

// ---------------------------------------------------------------------------
// 链路：连接/收发
// ---------------------------------------------------------------------------
void ModbusMaster::dev_close(DevRt& rt) {
    if (rt.fd >= 0) ::close(rt.fd);
    rt.fd = -1;
    rt.online = false;
}

bool ModbusMaster::dev_connect(int di) {
    DevRt& rt = rt_[di];
    const MbDeviceCfg& d = cfg_.devices[di];
    dev_close(rt);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)d.port);
    if (::inet_pton(AF_INET, d.host.c_str(), &a.sin_addr) != 1) {
        ::close(fd);
        rt.last_err = "host 非法";
        return false;
    }
    const int rc = ::connect(fd, (sockaddr*)&a, sizeof(a));
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        rt.last_err = "connect 失败";
        return false;
    }
    pollfd pf{fd, POLLOUT, 0};
    if (::poll(&pf, 1, d.timeout_ms) <= 0 || (pf.revents & (POLLERR | POLLHUP))) {
        ::close(fd);
        rt.last_err = "connect 超时";
        return false;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err != 0) {
        ::close(fd);
        rt.last_err = "connect SO_ERROR=" + std::to_string(err);
        return false;
    }
    rt.fd = fd;
    rt.online = true;
    rt.err = 0;
    log("[mbdev] " + d.name + " 已连接 " + d.host + ":" + std::to_string(d.port) +
        (d.kind == MbLinkKind::RTU_TCP ? "（rtu-tcp）" : ""));
    return true;
}

namespace {
// 响应期望长度（PDU 部分，不含 MBAP/CRC）：读 1/2=1+1+ceil(bits/8)；3/4=1+1+2n；写 5/6/15/16=5；异常=2
size_t resp_pdu_len(int fc, int count) {
    if (fc == 1 || fc == 2) return 2 + (size_t)((count + 7) / 8);
    if (fc == 3 || fc == 4) return 2 + (size_t)count * 2;
    return 5;
}
} // namespace

bool ModbusMaster::xfer(int di, const std::string& req, std::string* resp) {
    DevRt& rt = rt_[di];
    const MbDeviceCfg& d = cfg_.devices[di];
    const bool rtu = (d.kind == MbLinkKind::RTU_TCP);
    for (int attempt = 0; attempt <= d.retries; ++attempt) {
        if (rt.fd < 0 && !dev_connect(di)) {
            rt.err = 0x41;
            rt.timeouts++;
            continue;
        }
        // 发送
        size_t off = 0;
        while (off < req.size()) {
            const ssize_t n = ::send(rt.fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pf{rt.fd, POLLOUT, 0};
                if (::poll(&pf, 1, d.timeout_ms) <= 0) break;
                continue;
            }
            dev_close(rt);
            break;
        }
        if (rt.fd < 0) {
            rt.err = 0x42;
            rt.timeouts++;
            rt.last_err = "发送失败/断开";
            continue;
        }
        int fc = (uint8_t)req[rtu ? 1 : 7];
        int count = (fc == 1 || fc == 2 || fc == 3 || fc == 4) ? (int)get_u16(req, rtu ? 4 : 10) : 1;
        const size_t need_pdu = resp_pdu_len(fc, count);
        const size_t need_total = rtu ? (1 + need_pdu + 2) : (6 + 1 + need_pdu);
        // 接收（按需长度收满；异常帧更短 → 收到 2 字节就提前判）
        std::string buf;
        const int64_t deadline = now_ms() + d.timeout_ms;
        bool ok = false;
        while (now_ms() < deadline) {
            char tmp[256];
            const ssize_t n = ::recv(rt.fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                buf.append(tmp, (size_t)n);
                if (buf.size() >= (rtu ? 2u : 9u)) {
                    const size_t pdu_off = rtu ? 1 : 7;
                    const uint8_t rfc = (uint8_t)buf[pdu_off];
                    if (rfc & 0x80) {
                        if (buf.size() >= pdu_off + 2) { ok = true; break; }
                    } else if (buf.size() >= need_total) {
                        ok = true;
                        break;
                    }
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pf{rt.fd, POLLIN, 0};
                if (::poll(&pf, 1, d.timeout_ms) <= 0) break;
                continue;
            }
            dev_close(rt);
            break;
        }
        if (rt.fd < 0) {
            rt.err = 0x42;
            rt.timeouts++;
            rt.last_err = "接收中断线";
            continue;
        }
        if (!ok) {
            rt.err = 0x43;
            rt.timeouts++;
            rt.last_err = "响应超时";
            continue;
        }
        // 校验
        const size_t pdu_off = rtu ? 1 : 7;
        if (rtu) {
            const size_t body = (uint8_t)buf[pdu_off] & 0x80 ? 2 : need_pdu;
            if (buf.size() < 1 + body + 2) { rt.err = 0x44; rt.last_err = "RTU 帧过短"; continue; }
            const uint16_t crc = mb_crc16((const uint8_t*)buf.data(), buf.size() - 2);
            const uint16_t got = (uint16_t)((uint8_t)buf[buf.size() - 2] | ((uint8_t)buf[buf.size() - 1] << 8));
            if (crc != got) { rt.err = 0x45; rt.last_err = "RTU CRC 错"; continue; }
            if ((uint8_t)buf[0] != d.unit) { rt.err = 0x46; rt.last_err = "站号不符"; continue; }
        } else {
            if (get_u16(buf, 0) != get_u16(req, 0)) { rt.err = 0x47; rt.last_err = "tid 不符"; continue; }
            if ((uint8_t)buf[6] != d.unit) { rt.err = 0x46; rt.last_err = "站号不符"; continue; }
        }
        const uint8_t rfc = (uint8_t)buf[pdu_off];
        if (rfc & 0x80) {
            rt.err = (int)buf[pdu_off + 1];
            rt.last_err = "Modbus 异常码 " + std::to_string(rt.err);
            return false;
        }
        *resp = buf;
        rt.online = true;
        rt.err = 0;
        rt.ok_count++;
        rt.last_ok = now_ms();
        rt.backoff = 1000;
        return true;
    }
    rt.online = false;
    rt.retry_at = now_ms() + rt.backoff;
    rt.backoff = rt.backoff < 10000 ? rt.backoff * 2 : 10000;
    return false;
}

// ---------------------------------------------------------------------------
// 读/写点位
// ---------------------------------------------------------------------------
bool ModbusMaster::do_read(int di, int pi) {
    std::string req, resp;
    int fc = 0, addr = 0, count = 1;
    bool rtu = false, map_reg = false;
    int map_addr = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        const MbDeviceCfg& d = cfg_.devices[di];
        const MbPointCfg& p = d.points[pi];
        fc = p.fc;
        addr = p.addr;
        count = p.count;
        map_reg = p.map_reg;
        map_addr = p.map_addr;
        rtu = (d.kind == MbLinkKind::RTU_TCP);
        std::string pdu;
        pdu += (char)fc;
        pdu += w16((uint16_t)addr);
        pdu += w16((uint16_t)count);
        if (rtu) {
            std::string f = std::string{ (char)d.unit } + pdu;
            const uint16_t crc = mb_crc16((const uint8_t*)f.data(), f.size());
            f += (char)(crc & 0xFF);
            f += (char)(crc >> 8);
            req = f;
        } else {
            const uint16_t tid = tid_.fetch_add(1);
            req = w16(tid) + w16(0) + w16((uint16_t)(pdu.size() + 1)) + std::string{ (char)d.unit } + pdu;
        }
    }
    std::lock_guard<std::mutex> iolk(rt_[di].io_mtx);
    if (!xfer(di, req, &resp)) return false;
    const size_t off = rtu ? 1 : 7;
    if ((uint8_t)resp[off] != fc) return false;

    const MbPointCfg& p = cfg_.devices[di].points[pi];
    double v = 0;
    if (fc == 1 || fc == 2) {
        v = (double)((uint8_t)resp[off + 2] & 0x01);
        if (map_reg && store_) store_->write_reg(map_addr, (uint16_t)v);   // ★线圈读点同样落映射 4x
    } else {
        const size_t d0 = off + 2;
        const uint16_t w0 = get_u16(resp, d0);
        const uint16_t w1 = (count == 2) ? get_u16(resp, d0 + 2) : 0;
        v = mb_decode_value(p.type, w0, w1);
        if (map_reg && store_) {
            store_->write_reg(map_addr, w0);
            if (count == 2) store_->write_reg(map_addr + 1, w1);
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    rt_[di].pts[pi].value = v;
    rt_[di].pts[pi].has = true;
    return true;
}

bool ModbusMaster::do_write(int di, int pi, double v) {
    std::string req;
    int fc = 0;
    bool rtu = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const MbDeviceCfg& d = cfg_.devices[di];
        const MbPointCfg& p = d.points[pi];
        fc = p.fc;
        rtu = (d.kind == MbLinkKind::RTU_TCP);
        std::string pdu;
        pdu += (char)fc;
        if (fc == 5) {
            pdu += w16((uint16_t)p.addr);
            pdu += (v != 0.0) ? w16(0xFF00) : w16(0x0000);
        } else if (fc == 6) {
            uint16_t w0 = 0, w1 = 0;
            mb_encode_value(p.type, v, &w0, &w1);
            pdu += w16((uint16_t)p.addr);
            pdu += w16(w0);
        } else if (fc == 15) {
            pdu += w16((uint16_t)p.addr);
            pdu += w16(1);
            pdu += (char)1;
            pdu += (char)(v != 0.0 ? 0x01 : 0x00);
        } else {   // fc16
            uint16_t w0 = 0, w1 = 0;
            mb_encode_value(p.type, v, &w0, &w1);
            pdu += w16((uint16_t)p.addr);
            pdu += w16((uint16_t)p.count);
            pdu += (char)(p.count * 2);
            pdu += w16(w0);
            if (p.count == 2) pdu += w16(w1);
        }
        if (rtu) {
            std::string f = std::string{ (char)d.unit } + pdu;
            const uint16_t crc = mb_crc16((const uint8_t*)f.data(), f.size());
            f += (char)(crc & 0xFF);
            f += (char)(crc >> 8);
            req = f;
        } else {
            const uint16_t tid = tid_.fetch_add(1);
            req = w16(tid) + w16(0) + w16((uint16_t)(pdu.size() + 1)) + std::string{ (char)d.unit } + pdu;
        }
    }
    std::lock_guard<std::mutex> iolk(rt_[di].io_mtx);
    std::string resp;
    if (!xfer(di, req, &resp)) return false;
    std::lock_guard<std::mutex> lk(m_);
    rt_[di].pts[pi].sent = true;
    rt_[di].pts[pi].last_sent = v;
    rt_[di].pts[pi].value = v;
    rt_[di].pts[pi].has = true;
    return true;
}

// ---------------------------------------------------------------------------
// 服务线程
// ---------------------------------------------------------------------------
void ModbusMaster::start() {
    if (th_.joinable()) return;
    stop_ = false;
    th_ = std::thread([this] { thread_main(); });
}

void ModbusMaster::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(m_);
    for (auto& r : rt_) dev_close(r);
}

void ModbusMaster::thread_main() {
    running_ = true;
    while (!stop_.load()) {
        size_t ndev = 0;
        {
            std::lock_guard<std::mutex> sw(sw_m_);
            std::lock_guard<std::mutex> lk(m_);
            ndev = cfg_.devices.size();
        }
        for (size_t di = 0; di < ndev && !stop_.load(); ++di) {
            // ★本设备本轮全程持交换门闩：内部对 cfg_/rt_ 的无锁引用（xfer/dev_connect/do_*）均受保护；
            //   热加载只需等待至多一次设备处理，不会与 IO 竞争换表。
            std::lock_guard<std::mutex> sw(sw_m_);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (di >= cfg_.devices.size()) break;          // 重载缩小设备数：本轮到此为止
                if (rt_[di].fd < 0 && now_ms() < rt_[di].retry_at) continue;
            }
            size_t npt = 0;
            int poll_ms = 200;
            {
                std::lock_guard<std::mutex> lk(m_);
                npt = cfg_.devices[di].points.size();
                poll_ms = cfg_.devices[di].poll_ms;
            }
            for (size_t pi = 0; pi < npt && !stop_.load(); ++pi) {
                bool is_write = false, map_reg = false, on_change = true;
                int map_addr = 0, count = 1;
                MbType type = MbType::U16;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    const MbPointCfg& p = cfg_.devices[di].points[pi];
                    is_write = p.write;
                    map_reg = p.map_reg;
                    on_change = p.on_change;
                    map_addr = p.map_addr;
                    count = p.count;
                    type = p.type;
                }
                if (!is_write) {
                    bool due = false;
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        due = now_ms() >= rt_[di].pts[pi].next_poll;
                        if (due) rt_[di].pts[pi].next_poll = now_ms() + poll_ms;
                    }
                    if (due) do_read((int)di, (int)pi);
                } else if (map_reg && on_change && store_) {
                    const uint16_t w0 = store_->read_reg(map_addr);
                    const uint16_t w1 = (count == 2) ? store_->read_reg(map_addr + 1) : 0;
                    const double v = mb_decode_value(type, w0, w1);
                    double last = 0;
                    bool sent = false;
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        sent = rt_[di].pts[pi].sent;
                        last = rt_[di].pts[pi].last_sent;
                    }
                    if (!sent) {
                        std::lock_guard<std::mutex> lk(m_);
                        rt_[di].pts[pi].sent = true;
                        rt_[di].pts[pi].last_sent = v;      // 建立基线，不主动下发
                    } else if (v != last) {
                        do_write((int)di, (int)pi, v);
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    running_ = false;
}

} // namespace kx
