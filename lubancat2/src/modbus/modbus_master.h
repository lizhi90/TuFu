// ===========================================================================
// modbus_master.h —— Modbus 主站引擎（planA/21，P4）
//
//   独立服务线程：按设备轮询读点（→ 4x 库存 / 变量表）、监听 4x 写点变化下发；
//   支持 tcp（MBAP）与 rtu-tcp（RTU+CRC 经 TCP 透传网关）两类链路；
//   超时重试、断线退避重连、同设备串行；点名访问供脚本（MB_READ/MB_WRITE）与
//   状态查询（MBD_STATUS/MBD_LIST、D13 mbdev.status）。
// ===========================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modbus/modbus_master_config.h"

namespace kx {

class ModbusServer;

uint16_t mb_crc16(const uint8_t* d, size_t n);

class ModbusMaster {
public:
    explicit ModbusMaster(ModbusServer* store);
    ~ModbusMaster();

    ModbusMaster(const ModbusMaster&) = delete;
    ModbusMaster& operator=(const ModbusMaster&) = delete;

    void set_log(std::function<void(const std::string&)> f) { log_ = std::move(f); }

    // 解析 + 交叉校验（对从站只读冲突）+ 应用（失败保留旧表）
    bool load_config_text(const std::string& text, std::string* err);
    int  device_count() const;
    std::string config_json() const;

    // 点名访问（"设备.点名"；MotionHost 转发）
    bool read_point(const std::string& dev, const std::string& pt, double* v,
                    std::string* err = nullptr) const;
    bool write_point(const std::string& dev, const std::string& pt, double v, std::string* err);

    // 状态（脚本/插件）
    std::string status_text(const std::string& dev) const;     // "" = 全部设备
    std::string list_text() const;                             // 设备与点位清单
    std::string status_json() const;                           // D13 mbdev.status

    void start();
    void stop();

private:
    struct PtRt {
        double  value     = 0.0;    // 最近一次成功读/写值（变量映射与状态用）
        bool    has       = false;
        bool    sent      = false;  // 写点：是否已建立下发基线
        double  last_sent = 0.0;
        int64_t next_poll = 0;
    };
    struct DevRt {
        bool        online   = false;
        int         err      = 0;      // 0=正常；>0=最近异常（Modbus 异常码/内部码 0x40+）
        int         timeouts = 0;
        int         ok_count = 0;
        int64_t     last_ok  = 0;
        int64_t     retry_at = 0;
        int64_t     backoff  = 1000;
        int         fd       = -1;
        std::string last_err;
        std::vector<PtRt> pts;
        std::mutex  io_mtx;            // 同设备 IO 串行（轮询线程 vs 脚本显式写）
    };

    bool do_read(int di, int pi);                       // 读一个点并落库
    bool do_write(int di, int pi, double v);            // 写一个点（立即）
    bool xfer(int di, const std::string& req, std::string* resp);   // 发送+接收（含重试）
    bool dev_connect(int di);
    void dev_close(DevRt& rt);
    void thread_main();
    void log(const std::string& s) const { if (log_) log_(s); }

    ModbusServer* store_ = nullptr;
    std::function<void(const std::string&)> log_;

    mutable std::mutex      m_;
    mutable std::mutex      sw_m_;      // ★配置交换门闩（重载 vs 轮询/脚本 IO）：获取顺序恒为 sw_m_ → m_
    MbMasterConfig          cfg_;
    std::deque<DevRt>       rt_;
    std::thread             th_;
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       running_{false};
    std::atomic<uint16_t>   tid_{1};
};

} // namespace kx
