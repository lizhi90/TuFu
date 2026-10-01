// modbus_server_test.cpp —— 固件 ModbusServer 单测（planA/20 P1；无硬件/无 ecrt）
//
// 覆盖：组态解析/校验（重名/重叠/越界/默认值）· 类型编解码（u16/i16/u32/f32/f32hi）
//     · FC 01/03/05/06/10 帧级往返 · 主站写只读 → 异常 0x02 · 命名访问 MB_* 对应 API
//     · persist（写即存 + 重建恢复）· 热加载（坏表保留旧表）
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "modbus/modbus_server.h"
#include "script/nvram_store.h"

using namespace kx;

static int g_total = 0;
static int g_fail = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_total;                                                               \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

// ---- 极简 TCP 客户端 ----
struct Client {
    int fd = -1;
    bool open(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        for (int i = 0; i < 50; ++i) {
            if (::connect(fd, (sockaddr*)&a, sizeof(a)) == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    bool send_all(const std::vector<uint8_t>& b) {
        size_t off = 0;
        while (off < b.size()) {
            const ssize_t n = ::send(fd, b.data() + off, b.size() - off, 0);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }
    // 读满 n 字节（超时返回已读）
    std::vector<uint8_t> recv_n(size_t n) {
        std::vector<uint8_t> out;
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (out.size() < n) {
            uint8_t buf[512];
            const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) break;
            out.insert(out.end(), buf, buf + r);
        }
        return out;
    }
    ~Client() { if (fd >= 0) ::close(fd); }
};

static std::vector<uint8_t> req6(uint8_t fc, uint16_t a, uint16_t v) {
    std::vector<uint8_t> b = {0, 1, 0, 0, 0, 6, 1, fc,
                              (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(v >> 8), (uint8_t)v};
    return b;
}
static std::vector<uint8_t> req3(uint8_t fc, uint16_t a, uint16_t c) {
    std::vector<uint8_t> b = {0, 2, 0, 0, 0, 6, 1, fc,
                              (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(c >> 8), (uint8_t)c};
    return b;
}

static const char* kCfg = R"({
  "version": 1, "station": 1,
  "registers": [
    { "name": "温度设定", "addr": "4x300", "type": "f32", "access": "rw", "persist": true, "default": 25.0, "desc": "t" },
    { "name": "高字先",   "addr": "4x302", "type": "f32hi", "access": "rw" },
    { "name": "状态",     "addr": "4x310", "type": "u16", "access": "r" },
    { "name": "启动命令", "addr": "4x311", "type": "u16", "access": "w" },
    { "name": "计米",     "addr": "4x312", "type": "i16", "access": "rw", "default": -5 }
  ]
})";

int main() {
    ::signal(SIGPIPE, SIG_IGN);      // 断开类用例中向已关闭的对端 send 时返回 EPIPE 而非杀进程
    std::printf("== 1) 组态解析与校验 ==\n");
    {
        MbConfig cfg;
        std::string err;
        CHECK(mb_config_parse(kCfg, &cfg, &err));
        CHECK(cfg.entries.size() == 5 && cfg.station == 1);
        CHECK(cfg.span(cfg.entries[0]) == 2);
        // 编解码：f32 低字在前
        uint16_t w0 = 0, w1 = 0;
        cfg.encode(cfg.entries[0], 25.0, &w0, &w1);
        CHECK(w0 == 0x0000 && w1 == 0x41C8);                 // 25.0f = 0x41C80000
        CHECK(cfg.decode(cfg.entries[0], w0, w1) == 25.0);
        // f32hi 高字在前
        cfg.encode(cfg.entries[1], 30.5, &w0, &w1);
        CHECK(w0 == 0x41F4 && w1 == 0x0000);                 // 30.5f = 0x41F40000
        CHECK(cfg.decode(cfg.entries[1], w0, w1) == 30.5);
        // i16 负值
        cfg.encode(cfg.entries[4], -5, &w0, &w1);
        CHECK(w0 == 0xFFFB && cfg.decode(cfg.entries[4], w0, 0) == -5.0);

        // 反例
        CHECK(!mb_config_parse(R"({"registers":[{"name":"a","addr":"4x300"},{"name":"a","addr":"4x301"}]})", &cfg, &err));
        CHECK(!mb_config_parse(R"({"registers":[{"name":"a","addr":"4x300","type":"f32"},{"name":"b","addr":"4x301"}]})", &cfg, &err));
        CHECK(!mb_config_parse(R"({"registers":[{"name":"a","addr":"4x300","access":"x"}]})", &cfg, &err));
        CHECK(!mb_config_parse(R"({"registers":[{"name":"a","addr":"4x2100","persist":true}]})", &cfg, &err));  // 超出 NVRAM 1024
        CHECK(!mb_config_parse(R"({"registers":[{"name":"a","addr":"4x300","type":"u16","default":70000}]})", &cfg, &err));
    }

    std::printf("== 2) 服务帧级往返（FC 01/03/05/06/10；只读异常）==\n");
    const std::string nvfile = "/tmp/kx_mb_test.nvram";
    ::remove(nvfile.c_str());
    NvramStore nv(nvfile);
    nv.load(nullptr);
    ModbusServer srv(&nv);
    srv.set_log([](const std::string& s) { std::printf("   [srv] %s\n", s.c_str()); });
    std::string err;
    CHECK(srv.load_config_text(kCfg, &err));
    CHECK(srv.entry_count() == 5);
    srv.start(0);
    for (int i = 0; i < 40 && !srv.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(srv.running() && srv.port() > 0);

    Client c;
    CHECK(c.open((uint16_t)srv.port()));
    {
        // FC3 读 4x300..301（f32 默认 25.0 → 低字先：0000 41C8）
        CHECK(c.send_all(req3(3, 300, 2)));
        auto r = c.recv_n(9 + 4);
        CHECK(r.size() == 13 && r[7] == 3 && r[8] == 4 && r[9] == 0x00 && r[10] == 0x00 &&
              r[11] == 0x41 && r[12] == 0xC8);
        // FC6 写只读 4x310 → 异常 0x02
        CHECK(c.send_all(req6(6, 310, 9)));
        r = c.recv_n(9);
        CHECK(r.size() == 9 && r[7] == 0x86 && r[8] == 0x02);
        // FC6 写命令 4x311 = 7（w 条目）
        CHECK(c.send_all(req6(6, 311, 7)));
        r = c.recv_n(12);
        CHECK(r.size() == 12 && r[7] == 6);
        double v = 0;
        CHECK(srv.read_name("启动命令", &v, &err) && v == 7.0);
        // FC16 写 4x300..301 = 30.5f（低字先 0000 41F4）
        std::vector<uint8_t> w16 = {0, 3, 0, 0, 0, 11, 1, 16, 1, 44, 0, 2, 4, 0x00, 0x00, 0x41, 0xF4};
        CHECK(c.send_all(w16));
        r = c.recv_n(12);
        CHECK(r.size() == 12 && r[7] == 16);
        CHECK(srv.read_name("温度设定", &v, &err) && v == 30.5);
        // FC5/FC1 线圈
        std::vector<uint8_t> w5 = {0, 4, 0, 0, 0, 6, 1, 5, 0x04, 0x00, 0xFF, 0x00};   // 写 1024 位 = 1
        CHECK(c.send_all(w5));
        r = c.recv_n(12);
        CHECK(r.size() == 12 && r[7] == 5);
        CHECK(c.send_all(req3(1, 1024, 1)));
        r = c.recv_n(9 + 1);
        CHECK(r.size() == 10 && r[7] == 1 && r[8] == 1 && r[9] == 0x01);
    }
    c = Client{};

    std::printf("== 3) 命名访问 + persist ==\n");
    {
        double v = 0;
        CHECK(srv.write_name("高字先", 30.5, &err) && srv.read_name("高字先", &v, &err) && v == 30.5);
        CHECK(srv.read_reg(302) == 0x41F4 && srv.read_reg(303) == 0x0000);   // 高字在前
        CHECK(srv.write_name("计米", -5, &err) && srv.read_name("计米", &v, &err) && v == -5.0);
        CHECK(!srv.read_name("不存在", &v, &err) && !err.empty());
        const std::string list = srv.list_text();
        CHECK(list.find("温度设定,4x300,f32,rw,persist") != std::string::npos);
        // persist：温度设定 已写 30.5 → nv 有 300/301 字
        CHECK(nv.has(300) && nv.get(300) == 0x0000 && nv.get(301) == 0x41F4);
    }

    std::printf("== 4) 重建恢复（persist 上电）==\n");
    {
        srv.stop();
        ModbusServer srv2(&nv);
        CHECK(srv2.load_config_text(kCfg, &err));
        double v = 0;
        CHECK(srv2.read_name("温度设定", &v, &err) && v == 30.5);            // 从 nvram 恢复（覆盖 default）
        CHECK(srv2.read_name("计米", &v, &err) && v == -5.0);                // 未 persist → default
        std::printf("== 5) 热加载（坏表保留 / 好表替换）==\n");
        CHECK(!srv2.load_config_text(R"({"registers":[{"name":"x","addr":"4x300","type":"f32"},{"name":"y","addr":"4x301"}]})", &err));
        CHECK(srv2.read_name("温度设定", &v, &err));                          // 旧表仍在
        CHECK(srv2.load_config_text(R"({"registers":[{"name":"新变量","addr":"4x400","type":"u32"}]})", &err));
        CHECK(srv2.entry_count() == 1 && !srv2.read_name("温度设定", &v, &err));
        CHECK(srv2.write_name("新变量", 70000, &err) && srv2.read_name("新变量", &v, &err) && v == 70000);
        CHECK(srv2.read_reg(400) == (70000 & 0xFFFF) && srv2.read_reg(401) == (70000 >> 16));  // u32 低字先
        srv2.stop();
    }

    std::printf("== 5b) 多字写地址越界 → 异常 0x02（不得越界写；回归 §1）==\n");
    {
        ModbusServer srv5b(&nv);
        CHECK(srv5b.load_config_text(kCfg, &err));
        srv5b.start(0);
        for (int i = 0; i < 40 && !srv5b.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(srv5b.running() && srv5b.port() > 0);
        Client c;
        CHECK(c.open((uint16_t)srv5b.port()));
        // FC0F：addr=0xFFFF, cnt=2（bc=1, data=0x01）
        std::vector<uint8_t> f15 = {0, 9, 0, 0, 0, 8, 1, 0x0F, 0xFF, 0xFF, 0x00, 0x02, 0x01, 0x01};
        CHECK(c.send_all(f15));
        auto r15 = c.recv_n(9);
        CHECK(r15.size() == 9 && r15[7] == (0x0F | 0x80) && r15[8] == 0x02);
        // FC10：addr=0xFFFF, cnt=2（bc=4）
        std::vector<uint8_t> f16 = {0, 10, 0, 0, 0, 11, 1, 0x10, 0xFF, 0xFF, 0x00, 0x02, 0x04,
                                    0x11, 0x11, 0x22, 0x22};
        CHECK(c.send_all(f16));
        auto r16 = c.recv_n(9);
        CHECK(r16.size() == 9 && r16[7] == (0x10 | 0x80) && r16[8] == 0x02);
        // 边界内仍可用：addr=65534(cnt=2) 正常写
        std::vector<uint8_t> ok16 = {0, 11, 0, 0, 0, 11, 1, 0x10, 0xFF, 0xFE, 0x00, 0x02, 0x04,
                                     0xAB, 0xCD, 0xEF, 0x01};
        CHECK(c.send_all(ok16));
        auto rok = c.recv_n(12);
        CHECK(rok.size() == 12 && rok[7] == 0x10);
        CHECK(srv5b.read_reg(65534) == 0xABCD && srv5b.read_reg(65535) == 0xEF01);
        srv5b.stop();
    }

    std::printf("== 6) 断开回收（对端 close → 槽位必须回收；防 CLOSE-WAIT 占死 8 槽）==\n");
    {
        ModbusServer srv3(&nv);
        CHECK(srv3.load_config_text(kCfg, &err));
        srv3.start(0);
        for (int i = 0; i < 40 && !srv3.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(srv3.running() && srv3.port() > 0);
        const uint16_t port = srv3.port();
        std::vector<Client> burst(8);                       // 连满上限 8（每连确认可收发）
        int opened = 0;
        for (auto& c : burst) {
            if (c.open(port) && c.send_all(req3(3, 400, 1))) {
                auto r0 = c.recv_n(9 + 2);
                if (r0.size() == 11 && r0[7] == 3) ++opened;
            }
        }
        CHECK(opened == 8);
        CHECK(srv3.clients_info().size() == 8);             // D10 快照：8 条客户端可见
        burst.clear();                                      // 全部关闭（对端只发 FIN，半开）
        std::this_thread::sleep_for(std::chrono::milliseconds(500));   // 等 poll 循环看到 EOF 并回收
        Client c9;                                          // 修复前：8 槽被 CLOSE-WAIT 占死 → 新连被拒/即断
        CHECK(c9.open(port));
        CHECK(c9.send_all(req3(3, 400, 1)));
        auto r9 = c9.recv_n(9 + 2);
        CHECK(r9.size() == 11 && r9[7] == 3);
        srv3.stop();
    }

    std::printf("== 7) 断开隔离（A 硬复位 RST 不得误断 B；回归 pf/cls 错位）==\n");
    {
        ModbusServer srv7(&nv);
        CHECK(srv7.load_config_text(kCfg, &err));
        srv7.start(0);
        for (int i = 0; i < 40 && !srv7.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(srv7.running() && srv7.port() > 0);
        const uint16_t port7 = srv7.port();
        Client a, b;
        CHECK(a.open(port7) && b.open(port7));
        CHECK(a.send_all(req3(3, 400, 1)));
        CHECK(a.recv_n(9 + 2).size() == 11);
        CHECK(b.send_all(req3(3, 400, 1)));
        CHECK(b.recv_n(9 + 2).size() == 11);
        linger lg{1, 0};                                   // A 硬复位（RST），非正常 FIN
        ::setsockopt(a.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(a.fd);
        a.fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(b.send_all(req3(3, 400, 1)));                // 修复前：B 会被 A 的陈旧 revents 误断
        auto rb = b.recv_n(9 + 2);
        CHECK(rb.size() == 11 && rb[7] == 3);
        srv7.stop();
    }

    std::printf("[modbus_server_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
