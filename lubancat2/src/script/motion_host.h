// motion_host.h —— 脚本的设备宿主：把 BASIC 里的设备命令接到本控制器
//
// 它只经 kx::Shared 与运动内核交互（业务线程与运动内核的唯一通路），
// 因此**不包含 ecrt、不碰 RT 线程**，可以脱离硬件做逻辑测试（见 tools/motion_host_test.cpp）。
//
// 脚本可用的设备命令（大小写不敏感）：
//   EN / ENABLE              使能（等使能完成）
//   DIS / DISABLE            去使能
//   STOP / RAPIDSTOP         停止当前运动
//   MOVE d[,spd[,acc]]       相对定位 ⚠ZBasic 语义：d 是「距离」（阻塞到到位/出错）
//   MOVR / MOVEREL d[,...]   相对定位（同 MOVE）
//   MOVEABS p[,spd[,acc]]    绝对定位（阻塞到到位/出错）
//   JOG spd                  点动（立即返回，spd 可负）
//   VMOVE dir                连续速度运动（立即返回；dir>0 正转/<0 反转，速度取 SPEED(当前轴)）；
//                            与 JOG 同源（连续速度直到 STOP/CANCEL）
//   CANCEL mode              取消运动（停止当前定位/点动）；各 mode 一律停「当前轴」的运动
//   HOME                     软件回零：绝对定位到约定零点（AxisConfig::home_mm，默认 0），阻塞到到位；
//                            绝对编码器系统下即"回到机械零点"；驱动器内部回零(0x6098)为独立后续项
//   DATUM mode               回零/清错：mode=0 清控制器/驱动器错误（下发内核 FAULT_RESET，
//                            触发 cia402 6040=0x80 复位脉冲）；
//                            mode=3 当前位置置零（内核坐标系偏移 SET_POS，不动电机）；
//                            其余模式未定案，明确报错而非静默（见 docs/planA/04 §1.2 / 08 §8.4）
//   DELAY ms / SLEEP ms      等待毫秒（可被中止打断）
//   WAIT IDLE                等轴到位（引擎在 WAIT/WA 语句里以 WAITIDLE 调用）
//   BASE n / AXIS n          设定当前轴（默认 0；命令后缀 AXIS(n) 亦经此）
//   SPEED/ACCEL/DECEL(n)[=v] 轴参数：读(1参)/写(2参)；MOVE 未显式给速度时取其默认值
//   ATYPE/UNITS/DRIVE_PROFILE/AXIS_ADDRESS(n)[=v]
//                            轴参数读写；本控制器不据其做换算，仅记录
//   DRIVE_CONTROLWORD(n)[=v] CiA402 控制字 6040h：读恒 0；写仅接受标准序列
//                            128(0x80 清错→FAULT_RESET) / 6(0x06 关机→去使能) / 15(0x0F 使能→ENABLE)，
//                            其余值**明确报错**（不直接写控制字，避免与内核使能状态机冲突，见 08 §8.4）
//   AXISSTATUS(n)            轴状态字（ZBasic 手册 6.3 位表）：bit2 通讯错 / bit3 驱动器报错 /
//                            bit8 随动误差超限 / bit22 告警输入；其余位本控制器无数据源，恒 0
//   POS / MPOS(n)            实际位置 mm
//   DPOS(n)                  指令位置 mm
//
//   ⚠本控制器为**多轴系统**（轴数由 AXIS_COUNT 决定，见 common/axis_limits.h）。
//     以上带轴号的命令（BASE/AXIS/轴参数/POS/MPOS/DPOS/IDLE/ISIDLE/AXISSTATUS/
//     DISABLE_GROUP/ENABLE/DISABLE/STOP/BUSY/ENABLED/ALARM/BUS）若轴号 >= 本机轴数，
//     一律**明确报错**，绝不静默回读别的轴数据（否则脚本会读到错误的位置/状态，
//     见 docs/planA/08 §8.4）。不带轴号时一律作用在「当前轴」BASE，
//     且轴选择（BASE/AXIS、命令后缀 AXIS(n)）在本控制器**真正生效**。
//   BUS / BUSOK[(n)]         总线是否就绪（1/0）；n 默认当前轴
//   BUSY[(n)]                是否在运动（1/0）；n 默认当前轴
//   IDLE / ISIDLE[(n)]       运动是否结束 ⚠IDLE 返回 -1(结束)/0(运动中)，与 ZBasic 一致
//   ENABLED[(n)]             是否已使能（1/0）；n 默认当前轴
//   ALARM[(n)]               是否报警（1/0）；n 默认当前轴
//   SLOT_SCAN(0) / SCAN      总线扫描：触发重扫并等 Shared::bus 刷新；返回值 = 扫到的从站数
//                            （0 = 没扫到，RETURN 为假）——供 `IF RETURN THEN` 判成败
//   SLOT_START(0)            总线开启：等总线进入 OP 且已有从站；返回值 = 1 成功 / 0 失败
//   SLOT_STOP(0) / BUSSTOP   总线停止（软停，同 4321 的 BUSSTOP;）；返回值 = 1
//   DISABLE_GROUP n          解除轴分组（无控制器的分组语义：no-op，仍校验轴号）
//
// 命令返回值 -> RETURN 系统变量：脚本里 `SLOT_SCAN(0)` 等命令执行成功后，其返回值会写入
// 引擎的 RETURN 变量，可用 `IF RETURN THEN` 判断（非 0 = 真）。见 script.cpp 的 last_cmd_ret_。
//
// 端口 / 寄存器 / 任务 / 总线（B 层，见 docs/planA/08 §2.1）：
//   OPEN #n, 类型, ...       真实 TCP 通道（TCP_SERVER / TCP_CLIENT），见 port_manager.h
//   PORT_STATUS(n)           通道是否已连接（1/0）
//   PRINT #n, 文本           整包原样发送（不附加换行）
//   PUTCHAR #n, 数组(起点,长度)  按原始字节发送
//   GET #n, 数组[, n]        非阻塞接收，返回本次字节数（宿主回传字节串，引擎写入数组）
//                            注：通道未 OPEN 时报错；已 OPEN 但未连接时 GET 返回 0、PRINT/PUTCHAR
//                            静默丢弃（与 ZBasic 一致，脚本用 PORT_STATUS 守卫并重连）
//   MODBUS_REG(n)            读写 4x 寄存器（Shared::mb_regs 镜像）
//   MODBUS_IEEE(n)           读写 4x 内的 32 位浮点（占 n/n+1）
//   RUNTASK / STOPTASK n, 子程序名   任务登记（本引擎单线程：只记状态，见 §7 限制）
//   PROC_STATUS(n)           任务是否登记为「运行中」（1/0）
//   SCAN_EVENT(in口) / NODE_COUNT / NODE_AXIS_COUNT / NODE_STATUS / NODE_IO /
//   NODE_AIO / NODE_INFO / ETHERCAT / ECUSTOM / ETH_MODE / PORT_MAX
//                            总线设备信息（数据来自 kx::Shared::bus 快照；未扫描则返回 0）
//   SDO_WRITE(...)           **脚本层不可用**：需 EtherCAT 主站通道，脚本宿主不碰 ecrt；调用即明确报错
//                            （遗留脚本里它只在 SCAN_EVENT 恒 0 的调试分支，见 08 §8.4）
//
// 约定（与 ScriptHost::call 一致）：
//   返回 0 = 成功；1 = 不认识（引擎退还为数组访问）；2 = 运行错误；3 = 请求中止。
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/state.h"
#include "script/port_manager.h"
#include "script/script_host.h"   // kx::ScriptHost / kx::Value（语言无关宿主接口）

namespace kx {

class NvramStore;   // 持久化存储（NVSET/NVGET；见 nvram_store.h）

class MotionHost : public ScriptHost {
public:
    struct Config {
        int  poll_ms          = 2;       // 等待运动/使能时的轮询间隔
        int  move_timeout_ms  = 12000;   // 定位/点动等待超时（应 > axis.timeout）
        int  enable_timeout_ms= 5000;    // 等使能超时
        int  scan_timeout_ms  = 2000;    // SLOT_SCAN 等总线扫描结果的超时（至少覆盖 ~100ms 发布周期）
        int  start_timeout_ms = 2000;    // SLOT_START 等总线进入 OP 的超时
        bool echo             = false;   // 是否把命令回显到输出（调试用）
    };

    explicit MotionHost(Shared& sh) : sh_(&sh) {}
    MotionHost(Shared& sh, const Config& cfg) : sh_(&sh), cfg_(cfg) {}

    void set_config(const Config& cfg) { cfg_ = cfg; }

    // 输出落点：默认写到 stdout（供 CLI 观察）
    void set_output(std::function<void(const std::string&)> cb) { out_ = std::move(cb); }

    // 外部中止标志（例如进程收到 SIGINT）；脚本长循环会被及时叫停
    void set_abort_flag(const std::atomic<bool>* f) { abort_ = f; }

    // ---- ScriptHost ----
    int  call(const std::string& name, const std::vector<Value>& args,
              Value* ret, std::string* err) override;
    void print(const std::string& line) override;
    bool aborted() override;

    // 计数（供测试/诊断）
    int  cmds()   const { return cmds_; }
    int  errors() const { return errors_; }

    // 持久化存储（NVSET/NVGET）：由 main.cpp 注入（脚本目录 .nvram）；未注入时命令明确报不支持
    void set_nvram(NvramStore* nv) { nv_ = nv; }

    // 端口通道（B 层）：供外部注入/观察（默认内建一个）
    PortManager&       ports()       { return ports_; }
    const PortManager& ports() const { return ports_; }

private:
    void post_cmd(const AxisCmd& c);                  // ★等槽位投递（不丢命令；见 state.h post_wait）
    // 等待指定轴的 move_result 到达终态；返回 true=到位
    bool wait_move(int axis, int timeout_ms, std::string* err);
    bool wait_enabled(int axis, bool want, int timeout_ms, std::string* err);
    bool sleep_ms(int ms);
    // M3：等待直线插补会话终态（state 3=done 4=error）；返回 true=done
    bool wait_lin(int timeout_ms, std::string* err);

    // 轴号是否合法（在 [0, axis_count) 内）
    bool axis_valid(int a) const { return sh_ && sh_->axis_ok(a); }

    // B 层子分发（返回 ScriptHost::call 约定值）
    int call_port(const std::string& name, const std::vector<Value>& args,
                  Value* ret, std::string* err);
    int call_modbus(const std::string& name, const std::vector<Value>& args,
                    Value* ret, std::string* err);
    int call_nvram(const std::string& name, const std::vector<Value>& args,
                   Value* ret, std::string* err);
    int call_task(const std::string& name, const std::vector<Value>& args,
                  Value* ret, std::string* err);
    int call_bus(const std::string& name, const std::vector<Value>& args,
                 Value* ret, std::string* err);

    Shared*     sh_ = nullptr;
    NvramStore* nv_ = nullptr;                       // 持久化存储（NVSET/NVGET；可空=未启用）
    Config      cfg_{};
    std::function<void(const std::string&)> out_;
    const std::atomic<bool>* abort_ = nullptr;
    int         cmds_   = 0;
    int         errors_ = 0;

    PortManager ports_;
    int         base_ = 0;                                   // 当前轴（BASE/AXIS；多轴时=base_list_[0]）
    int         base_list_[8] = {0};                         // M3：BASE 多轴轴表（直线插补参与轴）
    int         base_n_ = 1;                                 // 轴表长度（1=单轴，原语义）
    int         motion_mode_ = 0;                            // M3：0=PP 1=CSP（MOTION_MODE 命令/config 初始化）
    std::unordered_map<std::string, std::map<int, double>> params_;   // 轴参数：名 -> 轴 -> 值

    double axis_param(const std::string& name, int axis) const {
        auto it = params_.find(name);
        if (it == params_.end()) return 0.0;
        auto jt = it->second.find(axis);
        return jt == it->second.end() ? 0.0 : jt->second;
    }
};

} // namespace kx
