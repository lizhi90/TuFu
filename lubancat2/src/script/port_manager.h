// port_manager.h —— 脚本端口通道 -> 真实 TCP socket
//
// 把 ZBasic 的「自定义网口」抽象落到本控制器：
//   OPEN #n, "TCP_SERVER", 监听端口          -> 建监听、等待客户端接入
//   OPEN #n, "TCP_CLIENT", 远端端口, "IP"    -> 主动连接远端（非阻塞）
//   PORT_STATUS(n)                           -> 1 = 已连接
//   PRINT #n, 文本 / PUTCHAR #n, 字节        -> 原样发送（不附加换行）
//   GET #n, 数组, n                          -> 非阻塞接收，返回本次收到的字节数
//
// 约定：
//   * 端口号即下标（0..运行期上限-1，默认 16、静态容量 64；上限见 D9 `port.max.*`），
//     与原程序「自定义网口 10/11」的用法一致；编号仅是句柄，无 ZMC 式通道规划；
//   * 全部 fd 非阻塞，port_manager 内部不做线程与 select 循环——谁用谁「推进」
//     （每次 status/send/recv 都会顺带推进 accept / connect 完成）；
//   * 多客户端（2026-09-28）：TCP_SERVER 最多保留 kMaxClients 个并发连接（触摸屏 + 机器人等多主站）；
//     每个客户端独立收发（idx=0..clients-1）；idx=0 为「主客户端」，旧脚本/旧 API 不传 idx 即等价于此；
//     某客户端断开即释放其槽位，允许新客户端接入（便于脚本自愈）；
//   * 自定义网口（TCP_SERVER 监听）不得占用系统保留端口（本工程禁用清单：22(SSH)/80(HTTP)），
//     属非法用法，open() 直接拒绝。
//
// 该类不碰 EtherCAT/RT，可脱机用回环 socket 单测（tools/port_manager_test.cpp）。
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace kx {

class PortManager {
public:
    // 静态容量（槽位数组大小，编译期固定）与出厂默认运行期上限（D9 可改，持久化 .portmax）
    static constexpr int kMaxSlots = 64;
    static constexpr int kDefaultMaxPorts = 16;
    static constexpr int kMaxClients = 4;    // TCP_SERVER 每端口最大并发客户端数（2026-09-28）

    enum Kind { NONE = 0, TCP_SERVER = 1, TCP_CLIENT = 2 };

    // 系统保留端口（本工程禁用清单）：22(SSH) / 80(HTTP)。
    // 约定：自定义网口（TCP_SERVER 监听）不得占用保留端口，属非法，open() 直接拒绝；
    //       TCP_CLIENT 的 portnum 是「远端」端口，不在此限制内；portnum=0 仍表示内核分配。
    static bool is_reserved_port(int portnum) {
        return portnum == 22 || portnum == 80;
    }

    // 运行期上限（进程级，所有实例共享；编号 0..上限-1 有效）
    static int  global_max_ports();
    // 设置运行期上限：合法范围 1..kMaxSlots；任何实例仍占用 >= n 的端口时拒绝（err 说明占用端口）
    static bool set_global_max_ports(int n, std::string* err);
    // 当前被占用的端口号（升序；open 到 close 之间，用于诊断/回读）
    static std::vector<int> open_ports();

    // ---- 进程级连接快照（D10「通讯状态」；任何实例的端口都可被调试口读取）----
    struct SnapInfo {
        int         port   = -1;     // 槽位号
        int         kind   = NONE;   // NONE / TCP_SERVER / TCP_CLIENT
        int         lport  = 0;      // server：实际监听端口；client：0
        int         rport  = 0;      // server：客户端对端端口；client：远端端口
        bool        conn   = false;  // 已连接（server：已接受客户端；client：连接完成）
        std::string target;          // server："listen:PORT"；client："ip:port"（远端）
        std::string peer;            // 对端 IP（server：accept 抓取；client：远端 IP）
        std::string tag;             // PORT_INFO：用途（脚本声明）
        std::string role;            // PORT_INFO：主/从（"主站"/"从站"/""）
    };
    // 读取当前所有打开端口的快照（升序；只含 kind != NONE）
    static std::vector<SnapInfo> snapshot();
    // 打标签（PORT_INFO 命令；进程级，保持到显式修改/重启）：port 0..kMaxSlots-1，
    //   用途 ≤48 字、主从 ≤24 字（超长/越界返回 false 并填 err）
    static bool set_tag(int port, const std::string& tag, const std::string& role, std::string* err);

    PortManager() = default;
    ~PortManager();
    PortManager(const PortManager&) = delete;
    PortManager& operator=(const PortManager&) = delete;

    // 打开通道。type 大小写不敏感："TCP_SERVER" / "TCP_CLIENT"。
    //   TCP_SERVER：portnum = 监听端口（0 = 内核分配，用 listen_port() 查询）
    //   TCP_CLIENT：portnum = 远端端口，ip = 远端地址
    // 重复 OPEN 同一通道 = 先关闭再重开（脚本按此做重连）。
    bool open(int port, const std::string& type, double portnum,
              const std::string& ip, std::string* err);

    void close_port(int port);
    void close_all();

    int  kind(int port) const;                  // NONE / TCP_SERVER / TCP_CLIENT
    // 1 = 已连接（会顺带推进 accept/connect）。idx < 0：任一客户端（旧语义）；
    // idx >= 0：指定客户端（server；0 = 主客户端）。
    int  status(int port, int idx = -1);
    int  clients(int port);                     // 当前已连接的客户端数（server；client 端口 0/1）
    int  listen_port(int port) const;           // server：实际监听端口（0 = 未监听）
    std::string target(int port) const;         // "ip:port"（诊断/日志用）

    // 发送原始字节（含 0 字节）给指定客户端；未连接或出错时填 err 返回 false
    bool send(int port, const std::string& bytes, std::string* err, int idx = 0);

    // 非阻塞接收最多 max 字节（指定客户端，默认主客户端）：
    //   >=0 = 本次收到的字节数（0 = 暂无数据/未连接）；-1 = 错误（通道未打开/越界，填 err）；out 收原始字节
    int  recv(int port, std::string* out, size_t max, std::string* err, int idx = 0);

private:
    struct Slot {
        int         kind  = NONE;
        int         fd    = -1;     // server: 监听 fd；client: socket fd
        int         conns[kMaxClients] = {-1, -1, -1, -1};   // server: 各客户端 fd；client: 仅 [0]
        int         lport = 0;      // server: 实际监听端口
        int         rports[kMaxClients] = {0, 0, 0, 0};      // server: 各客户端对端端口；client: [0]=远端端口
        std::string target;         // "ip:port"
        std::string peers[kMaxClients];                      // 各客户端 IP（D10 快照用）
        std::string txs[kMaxClients];                        // 各客户端未发完的待发字节
    };

    bool valid(int port) const { return port >= 0 && port < global_max_ports(); }
    bool flush(Slot& s, int idx, std::string* err);      // 尽力把指定客户端 tx 发完
    int  conn_fd(Slot& s, int idx) const;                // 指定客户端可收发的 fd（-1 = 未连接）
    void drop_client(Slot& s, int idx);                  // 断开指定客户端（server）/ 连接（client）
    void publish(int port);                              // 刷新进程级连接快照（D10）

    Slot slots_[kMaxSlots];

    // 进程级共享状态：运行期上限 + 各槽占用标记（D9；跨实例校验收缩安全性）
    static std::atomic<int> g_max_ports_;
    static std::atomic<int> g_open_[kMaxSlots];

    // 进程级连接快照 + PORT_INFO 标签（D10）
    static std::mutex g_snap_mtx_;
    static std::vector<SnapInfo> g_snap_[kMaxSlots];   // 多客户端：每端口多条（2026-09-28）
    static std::string g_tag_[kMaxSlots];
    static std::string g_role_[kMaxSlots];
};

} // namespace kx
