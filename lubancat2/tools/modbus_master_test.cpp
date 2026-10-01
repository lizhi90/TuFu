// modbus_master_test.cpp —— 主站引擎单测（planA/21 M-P1；无硬件）
//
// 以现有 ModbusServer 当"假从站"做环回：
//   读点位（fc3→4x 映射 / fc3→变量映射）· 写点位（fc16，4x 变化即下发）· 线圈（fc5/fc1）
//   · RTU-over-TCP 链路（测试内起极简 RTU 从站）· 超时/离线状态 · 交叉校验拒绝
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "modbus/modbus_master.h"
#include "modbus/modbus_server.h"
#include "script/nvram_store.h"

using namespace kx;

static int g_total = 0;
static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_total;                                                        \
        if (!(cond)) {                                                    \
            ++g_fail;                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 等条件成立（超时返回 false）
template <typename F>
static bool wait_until(F f, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; ++i) {
        if (f()) return true;
        wait_ms(20);
    }
    return f();
}

// ---- 极简 Modbus-TCP 客户端（FC1/FC3）----
struct TcpCli {
    int fd = -1;
    bool open(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        return ::connect(fd, (sockaddr*)&a, sizeof(a)) == 0;
    }
    std::vector<uint8_t> ask(const std::vector<uint8_t>& b, size_t want) {
        ::send(fd, b.data(), b.size(), 0);
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::vector<uint8_t> out;
        while (out.size() < want) {
            uint8_t t[256];
            const ssize_t n = ::recv(fd, t, sizeof(t), 0);
            if (n <= 0) break;
            out.insert(out.end(), t, t + n);
        }
        return out;
    }
    ~TcpCli() { if (fd >= 0) ::close(fd); }
};

// ---- 极简 RTU-over-TCP 从站（fc3/fc6；用于 rtu-tcp 链路用例）----
struct RtuFake {
    int lfd = -1;
    std::atomic<bool> stop{false};
    std::thread th;
    std::string reg3;                      // fc3 起始 0 起的 8 个字（大端）
    uint16_t port = 0;
    bool start() {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(lfd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(lfd, 4) != 0) return false;
        socklen_t l = sizeof(a);
        ::getsockname(lfd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        timeval tv{0, 100000};
        ::setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (!stop.load()) {
            const int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            std::string buf;
            while (!stop.load()) {
                uint8_t t[256];
                const ssize_t n = ::recv(c, t, sizeof(t), 0);
                if (n <= 0) break;
                buf.append((char*)t, (size_t)n);
                // 一条 RTU 帧：unit fc ...（本用例固定 fc3=8B / fc6=8B）
                for (;;) {
                    if (buf.size() < 8) break;
                    const uint8_t fc = (uint8_t)buf[1];
                    if (fc == 3) {
                        const uint16_t addr = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
                        const uint16_t cnt = ((uint8_t)buf[4] << 8) | (uint8_t)buf[5];
                        std::string p = std::string{ buf[0], (char)3, (char)(cnt * 2) };
                        for (int i = 0; i < cnt; ++i) {
                            const size_t off = (size_t)(addr + i) * 2;
                            p += (off + 1 < reg3.size()) ? reg3.substr(off, 2) : std::string("\0\0", 2);
                        }
                        const uint16_t crc = mb_crc16((const uint8_t*)p.data(), p.size());
                        p += (char)(crc & 0xFF);
                        p += (char)(crc >> 8);
                        ::send(c, p.data(), p.size(), 0);
                    } else if (fc == 6) {
                        std::string p = buf.substr(0, 6);   // 回显 unit fc addr val
                        const uint16_t crc = mb_crc16((const uint8_t*)p.data(), p.size());
                        p += (char)(crc & 0xFF);
                        p += (char)(crc >> 8);
                        ::send(c, p.data(), p.size(), 0);
                    }
                    buf.clear();
                }
            }
            ::close(c);
        }
    }
    void done() {
        stop = true;
        if (th.joinable()) th.join();
        if (lfd >= 0) ::close(lfd);
    }
};

int main() {
    // ---- 假从站（ModbusServer）：4x10/11 = f32 12.5（低字先）；4x20 = 4321 ----
    NvramStore nv("/tmp/kx_mb_master_test.nvram");
    ::remove("/tmp/kx_mb_master_test.nvram");
    nv.load(nullptr);
    ModbusServer slave(&nv);
    slave.set_log([](const std::string& s) { std::printf("   [slave] %s\n", s.c_str()); });
    std::string err;
    CHECK(slave.load_config_text(
        R"({"version":1,"station":1,"registers":[{"name":"写只读","addr":"4x700","type":"u16","access":"r"}]})",
        &err));
    {
        uint16_t w0 = 0, w1 = 0;
        mb_encode_value(MbType::F32, 12.5, &w0, &w1);
        slave.write_reg(10, w0);
        slave.write_reg(11, w1);
        slave.write_reg(20, 4321);
    }
    slave.start(0);
    CHECK(wait_until([&] { return slave.running() && slave.port() > 0; }, 3000));
    const uint16_t sport = (uint16_t)slave.port();

    // ---- 主站组态：读→4x600（f32）/ 读→变量 / 写（fc16, 4x610 变化即发）----
    const std::string cfg =
        std::string("{\"version\":1,\"devices\":[{") +
        "\"name\":\"假从站\",\"link\":{\"kind\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":" +
        std::to_string(sport) + "},\"unit\":1,\"timeout_ms\":300,\"retries\":1,\"poll_ms\":50," +
        "\"points\":[" +
        "{\"name\":\"频率\",\"dir\":\"read\",\"fc\":3,\"addr\":10,\"count\":2,\"type\":\"f32\",\"map\":{\"kind\":\"reg\",\"addr\":\"4x600\"}}," +
        "{\"name\":\"状态\",\"dir\":\"read\",\"fc\":3,\"addr\":20,\"count\":1,\"type\":\"u16\",\"map\":{\"kind\":\"var\"}}," +
        "{\"name\":\"设定\",\"dir\":\"write\",\"fc\":16,\"addr\":100,\"count\":1,\"type\":\"u16\",\"map\":{\"kind\":\"reg\",\"addr\":\"4x610\"}}" +
        "]}]}";

    ModbusMaster m(&slave);
    m.set_log([](const std::string& s) { std::printf("   [master] %s\n", s.c_str()); });
    CHECK(m.load_config_text(cfg, &err));
    CHECK(m.device_count() == 1);
    m.start();

    std::printf("== 1) 读点位（fc3 → 4x 映射 / 变量映射）· 写点位（fc16 变化即发）==\n");
    {
        // 4x600/601 由主站轮询写入（f32 12.5 低字先）
        CHECK(wait_until([&] { return slave.read_reg(601) == 0x4148; }, 3000));   // 12.5f=0x41480000
        CHECK(slave.read_reg(600) == 0x0000);
        double v = 0;
        CHECK(m.read_point("假从站", "频率", &v) && v == 12.5);
        CHECK(m.read_point("假从站", "状态", &v) && v == 4321.0);
        // 写：基线后改 4x610 → 下发到从站 0x64（100）
        slave.write_reg(610, 0);
        wait_ms(200);
        slave.write_reg(610, 7);
        CHECK(wait_until([&] { return slave.read_reg(100) == 7; }, 3000));
        // 显式写（脚本路径）
        CHECK(m.write_point("假从站", "设定", 9, &err));
        CHECK(slave.read_reg(100) == 9);
        // 读只读点位写 → 拒绝
        double v2 = 0;
        CHECK(!m.write_point("假从站", "频率", 1.0, &err) && !err.empty());
        CHECK(!m.read_point("假从站", "不存在", &v2, &err) && !err.empty());
        const std::string st = m.status_text("假从站");
        CHECK(st.find("online") != std::string::npos);
        CHECK(m.list_text().find("假从站.频率") != std::string::npos);
        CHECK(m.status_json().find("\"online\":true") != std::string::npos);
    }

    std::printf("== 2) RTU-over-TCP 链路（极简 RTU 从站；fc3 读 → 变量）==\n");
    {
        RtuFake rf;
        rf.reg3 = std::string("\x12\x34", 2) + std::string("\x00\x05", 2);
        CHECK(rf.start());
        const std::string rtu_cfg =
            std::string("{\"version\":1,\"devices\":[{") +
            "\"name\":\"RTU从站\",\"link\":{\"kind\":\"rtu-tcp\",\"host\":\"127.0.0.1\",\"port\":" +
            std::to_string(rf.port) + "},\"unit\":2,\"timeout_ms\":300,\"retries\":1,\"poll_ms\":50," +
            "\"points\":[{\"name\":\"值\",\"dir\":\"read\",\"fc\":3,\"addr\":0,\"count\":1,\"type\":\"u16\",\"map\":{\"kind\":\"var\"}}]}]}";
        ModbusMaster m2(&slave);
        CHECK(m2.load_config_text(rtu_cfg, &err));
        m2.start();
        double v = 0;
        CHECK(wait_until([&] { return m2.read_point("RTU从站", "值", &v, nullptr) && v == 0x1234; }, 3000));
        CHECK(m2.write_point("RTU从站", "值", 1, &err) == false);   // 读点位写 → 拒绝
        rf.done();
        m2.stop();
    }

    std::printf("== 3) 离线/超时状态 ==\n");
    {
        const std::string dead =
            std::string("{\"version\":1,\"devices\":[{") +
            "\"name\":\"不通\",\"link\":{\"kind\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":1}," +
            "\"unit\":1,\"timeout_ms\":100,\"retries\":0,\"poll_ms\":50," +
            "\"points\":[{\"name\":\"x\",\"dir\":\"read\",\"fc\":3,\"addr\":0,\"count\":1,\"type\":\"u16\",\"map\":{\"kind\":\"var\"}}]}]}";
        ModbusMaster m3(&slave);
        CHECK(m3.load_config_text(dead, &err));
        m3.start();
        CHECK(wait_until([&] { return m3.status_text("不通").find("offline") != std::string::npos; }, 3000));
        CHECK(m3.status_text("不通").find("timeouts=") != std::string::npos);
        m3.stop();
    }

    std::printf("== 4) 交叉校验（主站写点 vs 从站只读）==\n");
    {
        const std::string bad =
            std::string("{\"version\":1,\"devices\":[{") +
            "\"name\":\"冲突\",\"link\":{\"kind\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":" +
            std::to_string(sport) + "},\"unit\":1,\"points\":[{\"name\":\"w\",\"dir\":\"write\",\"fc\":16,\"addr\":1,\"count\":1,\"type\":\"u16\",\"map\":{\"kind\":\"reg\",\"addr\":\"4x700\"}}]}]}";
        ModbusMaster m4(&slave);
        CHECK(!m4.load_config_text(bad, &err) && err.find("只读") != std::string::npos);
    }

    std::printf("== 5) 线圈读点映射 4x + fc6 2 字类型拒绝 ==\n");
    {
        // 置从站线圈 5 = ON（FC05 → 0xFF00）
        TcpCli cli;
        CHECK(cli.open(sport));
        std::vector<uint8_t> f5 = {0, 21, 0, 0, 0, 6, 1, 0x05, 0x00, 0x05, 0xFF, 0x00};
        auto r5 = cli.ask(f5, 12);
        CHECK(r5.size() == 12 && r5[7] == 0x05);
        const std::string cfg5 =
            std::string("{\"version\":1,\"devices\":[{") +
            "\"name\":\"线圈从站\",\"link\":{\"kind\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":" +
            std::to_string(sport) +
            "},\"unit\":1,\"points\":[{\"name\":\"线圈\",\"dir\":\"read\",\"fc\":1,\"addr\":5,\"count\":1,"
            "\"type\":\"u16\",\"map\":{\"kind\":\"reg\",\"addr\":\"4x620\"}}]}]}";
        ModbusMaster m5(&slave);
        CHECK(m5.load_config_text(cfg5, &err));
        m5.start();
        double v = 0;
        CHECK(wait_until([&] { return m5.read_point("线圈从站", "线圈", &v) && v == 1.0; }, 3000));
        CHECK(wait_until([&] { return slave.read_reg(620) == 1; }, 3000));   // ★修复前恒 0
        m5.stop();

        // fc6 + f32（2 字类型）必须在解析期拒绝
        const std::string bad6 =
            std::string("{\"version\":1,\"devices\":[{") +
            "\"name\":\"坏点\",\"link\":{\"kind\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":" +
            std::to_string(sport) +
            "},\"unit\":1,\"points\":[{\"name\":\"f\",\"dir\":\"write\",\"fc\":6,\"addr\":30,\"count\":2,"
            "\"type\":\"f32\",\"map\":{\"kind\":\"reg\",\"addr\":\"4x630\"}}]}]}";
        ModbusMaster m6(&slave);
        CHECK(!m6.load_config_text(bad6, &err) && err.find("fc6") != std::string::npos);
    }

    m.stop();
    slave.stop();
    std::printf("[modbus_master_test] %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
