// state.h —— 跨线程共享状态（替代原 ZBasic 的 GLOBAL 变量）
// 约定：
//   * motion 线程(RT) 只极短时间持锁；业务线程读状态、写命令。
//   * 所有对外语义（mm、状态字位）与原程序保持一致，见 docs/planA/04、05。
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "common/axis_limits.h"   // kAxisMax：编译期轴数上限（运行时轴数见 Shared::axis_count）

namespace kx {

// ---- B 层：Modbus 4x 寄存器镜像 ----
// 供脚本宿主（MODBUS_REG / MODBUS_IEEE）使用的 4x 寄存器 scratch 区。
// 说明：与触摸屏的**具体协议对接**改由脚本程序实现（见 docs/planA/12），
//       控制器本体不再内置 502 从站；此镜像仅作为脚本层的通用原语保留。
// 用专用递归锁保护，脚本宿主读写在 mb_mtx 下进行。
constexpr int kModbusRegCount = 256;

// ---- B 层：脚本任务表（RUNTASK/STOPTASK/PROC_STATUS）----
// 注：本引擎为单线程解释器，RUNTASK 只做「登记 + 状态」，不会并发执行任务体
//     （真正的并发由脚本层「端口通道」承担，见 docs/planA/08 §7）。
constexpr int kScriptTaskMax = 8;

struct TaskSlot {
    int         running = 0;     // PROC_STATUS 返回 1=在跑
    std::string name;            // 子程序名（登记用，便于诊断）
};

// ---- B 层：总线设备信息快照（SCAN_EVENT/NODE_*）----
// 由运动/总线层扫描后填入；未扫描（node_count==0）时 NODE_* 返回 0，即「无设备」。
struct BusNodeInfo {
    int axis_count = 0;      // NODE_AXIS_COUNT
    int status     = 0;      // NODE_STATUS（= AL 状态码）
    int io_base    = 0;      // NODE_IO
    int in_count   = 0;      // NODE_INFO(...,10)
    int out_count  = 0;      // NODE_INFO(...,11)
    int aio_base   = 0;      // NODE_AIO
    int ad_count   = 0;      // NODE_INFO(10,...,12)
    int da_count   = 0;      // NODE_INFO(10,...,13)
    // ---- D10「通讯状态」从站明细（由 EthercatMaster::scan_bus 填，100ms 刷新）----
    int       axis      = -1;      // 该从站映射的轴号（-1 = 非轴从站）
    int       online    = 0;       // 1 = 在线（轴从站取 ecrt_slave_config_state.online）
    int       al_state  = 0;       // AL 状态码（SII/主站视角）
    uint32_t  vendor_id    = 0;    // SII Vendor ID
    uint32_t  product_code = 0;    // SII Product Code
    uint32_t  revision     = 0;    // SII Revision
    char      name[64]     = {0};  // SII OrderName/ProductName
};
constexpr int kBusNodeMax = 32;

struct BusInfo {
    int         node_count = 0;
    BusNodeInfo node[kBusNodeMax];
    // ---- D10「通讯状态」主站健康（由 EthercatMaster::read_state 填）----
    int link_up           = 0;   // 链路
    int slaves_responding  = 0;  // 响应从站数
    int master_al         = 0;   // 主站视角 AL 状态
    int slave_al          = 0;   // 聚合 AL（最差轴）
    int slave_online      = 0;   // 全部轴在线
    int slave_op          = 0;   // 全部轴 OP(0x08)
};

// AXISSTATUS 位（ZBasic 手册 6.3「轴状态」；仅列本控制器有数据源可映射的位）
// 脚本用 `AXISSTATUS(n) and MV_ERRMASK` 判报警，位定义须与手册一致。
enum AxisStatusBit : unsigned int {
    kAxisStFollowWarn = 0x000002,   // bit1  随动误差超限告警
    kAxisStComm       = 0x000004,   // bit2  与远程轴通讯出错（总线断开/从站不响应）
    kAxisStDriveFault = 0x000008,   // bit3  远程驱动器报错（CiA402 6041.bit3 fault）
    kAxisStFollowErr  = 0x000100,   // bit8  随动误差超限出错（CiA402 6041.bit13）
    kAxisStAlarmIn    = 0x400000,   // bit22 告警信号输入（CiA402 6041.bit7 warning）
};

// 单轴状态：由 motion 线程刷新
struct AxisStatus {
    int    bus_ok   = 0;      // 总线正常(原 bus_ok)
    int    enabled  = 0;      // 已使能(原 wdog)
    int    idle     = 0;      // 运动结束(原 IDLE(0)!=0)
    int    alarm    = 0;      // 有报警(原 AXISSTATUS & MV_ERRMASK)
    unsigned int axis_status = 0;   // 轴状态字(手册 6.3 位表，见 AxisStatusBit；供脚本 AXISSTATUS(n))
    double mpos     = 0.0;    // 实际位置 mm(原 MPOS(0))
    double dpos     = 0.0;    // 指令位置 mm(原 DPOS(0))
    int    err_code = 0;      // 报警码(0x603F)
    // 以下三项供脚本层判定 move_done / move_error / busy（由 RT 从 Axis 节流发布）
    int    move_result = 0;   // 0=NONE 1=RUNNING 2=DONE 3=ERROR
    double inc_per_mm  = 0.0; // 当前生效的脉冲当量（0=未设置；脚本 UNITS 运行时设置）
    int    moving      = 0;   // 状态机在 MOVING/JOG
    double target_mm   = 0.0; // 当前/最近一次定位目标 mm
    double jog_lead_s  = 0.0; // 点动 PP 跟随前视（秒；0=未设置，脚本 JOGLEAD 设置）
    double vel_mm      = 0.0; // 当前运动速度估计 mm/s（实际位置差分+一阶平滑；VP_SPEED 读）
};

// 运动命令：由业务线程下发，motion 线程按轴消费一次
struct AxisCmd {
    enum Op { NONE = 0, ENABLE, DISABLE, MOVE_ABS, MOVE_REL, JOG, STOP, SET_POS, HOME,
              FAULT_RESET, SET_SCALE, SET_MOTION_MODE, SET_JOG_LEAD,
              SET_SRAMP, SET_FASTDEC, SET_DEFAULTS } op = NONE;
    int    axis  = 0;         // 目标轴号（0..kAxisMax-1）；post(const AxisCmd&) 据此投递
    double pos   = 0.0;       // MOVE_*：目标 mm；SET_POS：置零后该点对应的用户坐标 mm（通常 0）；
                              // SET_JOG_LEAD：点动前视（秒，附加在 v²/(2a) 之上）；JOG：忽略；
                              // SET_MOTION_MODE：0=PP（驱动器规划）1=CSP（控制器每拍规划）
    double speed = 0.0;       // mm/s（JOG 可负；HOME 未给则用内核默认）
    double accel = 0.0;       // mm/s^2（0=用默认）
};

// ---- M3：多轴直线插补会话（宿主写请求，RT 线程消费执行）----
// 语义：各轴同时启动、共享归一化时间轴（进度 s: 0→1），位置_i = start_i + dist_i*s(t)
//       → 严格直线（CSP 下每拍 607A 对齐）；主导轴（|dist| 最大）承受 speed/accel。
// 状态机：0=idle，1=请求（宿主已写好参数），2=运行（RT 受理），3=done，4=error。
struct LinReq {
    std::mutex   mtx;
    unsigned long seq = 0;                            // 宿主每次请求 +1（RT 检测变化）
    int          state = 0;
    char         err[96] = {0};                       // state=4 时的原因（宿主读取报错）
    int          n = 0;                               // 参与轴数（1..kAxisMax）
    int          axis[kAxisMax] = {0};
    double       target_mm[kAxisMax] = {0};           // 各轴绝对目标（用户坐标 mm）
    double       speed = 0;                           // 主导轴速度 mm/s（0=用各轴默认？取轴0 SPEED）
    double       accel = 0;                           // 主导轴加速度 mm/s^2（0=默认）
};

// 全局共享容器
struct Shared {
    std::mutex  mtx;
    int         axis_count = 1;              // 运行时实际轴数（由 main 从 AXIS_COUNT 写入）
    AxisStatus  axis[kAxisMax];              // 每轴状态（RT 刷新；下标=轴号）
    AxisCmd     axis_cmd[kAxisMax];          // 每轴待执行命令（op != NONE 表示有）
    int         rescan_flag  = 0;   // 对应原 rescan_flag（SCAN;）
    int         busstop_flag = 0;   // 对应原 BUSSTOP;

    // ---- B 层共享状态（见本文件上方注释）----
    std::recursive_mutex mb_mtx;                      // 保护 mb_regs（脚本宿主读写）
    uint16_t    mb_regs[kModbusRegCount] = {0};       // Modbus 4x 寄存器镜像
    // v0.8.6：脚本访问过（读或写）的 4x 寄存器位图（256 位 = 4×uint64）；插件「已用寄存器」监视用
    std::atomic<uint64_t> mb_used[4] = {};
    void mb_mark_used(int idx) {
        if (idx < 0 || idx >= kModbusRegCount) return;
        mb_used[idx >> 6].fetch_or(1ull << (idx & 63), std::memory_order_relaxed);
    }
    TaskSlot    task[kScriptTaskMax];                 // 脚本任务表（mtx 保护）
    LinReq      lin;                                  // M3 多轴直线插补会话（自有 mtx）
    BusInfo     bus;                                  // 总线设备快照（mtx 保护）
    unsigned long bus_seq = 0;                        // 快照发布序号：RT 每发布一次 +1；
                                                      // SLOT_SCAN 等待其变化，确保读到「本次扫描后」的新快照

    // 命令按轴投递：目标轴取 c.axis；越界一律丢弃（绝不静默落到别的轴）。
    void post(const AxisCmd& c) {
        if (c.axis < 0 || c.axis >= kAxisMax) return;
        std::lock_guard<std::mutex> lk(mtx);
        axis_cmd[c.axis] = c;      // 命令覆盖式：运动类命令允许覆盖
    }
    // ★等待槽位后可投递（2026-09-29）：命令槽为单槽覆盖式，快速连发（如 JOGLEAD+SPEED+JOG）
    //   会把未取走的命令顶掉；本接口等 RT 线程取走（≤1ms 周期）后再投，最多 timeout_ms。
    //   返回 false = 超时未投递（调用方须显式报错，不静默）。
    bool post_wait(const AxisCmd& c, int timeout_ms = 8) {
        if (c.axis < 0 || c.axis >= kAxisMax) return false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (axis_cmd[c.axis].op == AxisCmd::NONE) {
                    axis_cmd[c.axis] = c;
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
    }
    // 取轴 a 的待执行命令（取出后清空）；越界返回 NONE。
    AxisCmd take(int a) {
        AxisCmd c;
        if (a < 0 || a >= kAxisMax) return c;
        std::lock_guard<std::mutex> lk(mtx);
        c = axis_cmd[a];
        axis_cmd[a].op = AxisCmd::NONE;
        return c;
    }
    // 轴号是否在本机实际轴数范围内（axis_count 启动后只读，故此处不加锁）。
    bool axis_ok(int a) const { return a >= 0 && a < axis_count && a < kAxisMax; }

    // 读取指定轴快照；a 越界返回默认值（安全，不崩）。
    AxisStatus snapshot(int a = 0) {
        if (a < 0 || a >= kAxisMax) return AxisStatus{};
        std::lock_guard<std::mutex> lk(mtx);
        return axis[a];
    }
    AxisCmd snapshot_cmd(int a = 0) {
        if (a < 0 || a >= kAxisMax) return AxisCmd{};
        std::lock_guard<std::mutex> lk(mtx);
        return axis_cmd[a];
    }
};

} // namespace kx
