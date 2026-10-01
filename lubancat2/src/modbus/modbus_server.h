// ===========================================================================
// modbus_server.h —— 固件内建 Modbus-TCP 从站（产品化 · planA/20）
//
//   独立服务线程（非 RT）：TCP 502 多客户端 + FC 01/02/03/04/05/06/0F/10；
//   寄存器库存 = 4x 全空间 u16（策略 A：未组态地址=普通寄存器，ZMC 兼容）；
//   组态条目（modbus_config）叠加 类型/权限/持久化/命名 语义：
//     · 主站写只读条目 → 异常 0x02；
//     · persist 条目任何写入（主站或脚本）即落盘 NvramStore，上电恢复；
//     · 脚本按名访问 MB_READ/MB_WRITE（MotionHost 转发到本类）。
//   调试口 D12 `mbreg.get/set` 读写组态文件并热加载（load_config_text）。
// ===========================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modbus/modbus_config.h"

namespace kx {

class NvramStore;

class ModbusServer {
public:
    explicit ModbusServer(NvramStore* nvram = nullptr);
    ~ModbusServer();

    ModbusServer(const ModbusServer&) = delete;
    ModbusServer& operator=(const ModbusServer&) = delete;

    void set_log(std::function<void(const std::string&)> f) { log_ = std::move(f); }

    // ---- 组态（热加载）----
    // 解析 + 校验 + 应用；失败：*err=首错、保留旧表（返回 false）。
    bool load_config_text(const std::string& text, std::string* err);
    int  entry_count() const;
    std::string config_json() const;
    MbConfig    config_copy() const;                  // 交叉校验（主站写点 vs 从站只读）

    // ---- 服务 ----
    void start(int port);                 // 启动线程；绑定失败在内部每 5s 重试（日志说明）
    void stop();
    bool running() const { return running_.load(); }
    int  port() const { return port_; }
    int  client_count() const { return clients_.load(); }

    // ---- 命令面（脚本；线程安全）----
    bool read_name(const std::string& name, double* v, std::string* err) const;
    bool write_name(const std::string& name, double v, std::string* err);
    std::string list_text() const;        // "name,4x300,f32,rw" 每行一条

    // ---- 按地址（兼容/测试）----
    uint16_t read_reg(int a) const;
    void     write_reg(int a, uint16_t v);           // 脚本路径：不校验方向；persist 生效

    // ---- 按区（脚本同步用；打包 u16 大端 = string.unpack(">H") 可解）----
    std::string read_zone(int start, int count) const;
    void        write_zone(int start, const std::string& packed);   // 逐字写；persist 生效

    // ---- D10「通讯状态」：固件 502 客户端快照（面板显示真实连接；planA/13）----
    struct ClientInfo {
        int         fd = -1;
        std::string peer;
        int         peer_port = 0;
    };
    std::vector<ClientInfo> clients_info() const;

private:
    struct Client {
        int fd = -1;
        std::vector<uint8_t> rx;
        std::vector<uint8_t> tx;
    };

    void thread_main();
    void poll_once(std::vector<Client>& cls);
    void accept_new(std::vector<Client>& cls);
    void try_connect();                              // 绑定 502（失败重试）
    void close_all(std::vector<Client>& cls);

    // 处理一个完整请求帧；返回响应字节（含 MBAP）；空 = 无响应（站号不匹配/广播）
    std::vector<uint8_t> handle_frame(const uint8_t* f, size_t n);
    void apply_config_locked(const MbConfig& cfg);
    void persist_words_locked(const MbEntry& e, const uint16_t* w);
    void log(const std::string& s) const { if (log_) log_(s); }

    NvramStore* nvram_ = nullptr;
    std::function<void(const std::string&)> log_;

    mutable std::mutex m_;
    MbConfig cfg_;
    std::vector<uint16_t> regs_{std::vector<uint16_t>(65536, 0)};      // 4x 全空间
    std::vector<uint8_t>  coils_{std::vector<uint8_t>(8192, 0)};       // 0x 位区（65536 位）
    std::vector<uint8_t>  inputs_{std::vector<uint8_t>(8192, 0)};      // 1x 位区（预留）

    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<int>  clients_{0};
    mutable std::mutex       ci_m_;                  // D10 客户端快照
    std::vector<ClientInfo>  cinfo_;
    int listen_fd_ = -1;
    int port_ = 0;
    int64_t retry_at_ms_ = 0;
    int log_div_ = 0;
};

} // namespace kx
