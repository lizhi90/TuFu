// config.h —— 读取 config/app.conf（key = value，# 注释），替代原 ZBasic 顶部 GLOBAL CONST
// 特点：
//   * 键值全部有默认值，缺项/写错不致命，只在日志里告警；
//   * 结构按模块分组，便于各模块只取自己那一份。
//
// 范围：本项目为**控制器开发项目**。与机器人 / 触摸屏 / 称重的具体协议对接
//       改由脚本程序实现（见 docs/planA/12），故不再有 tcp / modbus / scale / robot 配置段。
#pragma once

#include <cstdint>
#include <string>

#include "common/axis_limits.h"   // kAxisMax：多轴系统的编译期上限

namespace kx {

struct EcatCfg {
    unsigned int master_index   = 0;
    // 每轴从站位置：轴 i 默认对应环上第 i 个从站；可用 ECAT_SLAVE_POS_<n> 单独覆盖，
    // 也可用（旧的）ECAT_SLAVE_POS 覆盖轴 0。下标 = 轴号（0..kAxisMax-1）。
    unsigned int slave_position[kAxisMax];
    EcatCfg() { for (int i = 0; i < kAxisMax; ++i) slave_position[i] = (unsigned int)i; }

    uint32_t     cycle_ns       = 1000000;
    uint32_t     dc_assign      = 0x0300;
    uint32_t     dc_shift_ns    = 500000;
    // 以下两项仅在「读不到 SII 身份」时作为兜底使用；正常路径由总线实读身份覆盖。
    // 注意：IgH 的 ec_slave_config_attach() 要求 vendor/product 与 SII 精确相等，
    //       填 0 会导致 attach 失败 -> 从站配置整体失效（SM 长度退回 SII 默认 -> AL 0x001E）。
    uint32_t     vendor_id      = 0x00100000;   // 汇川 SV630
    uint32_t     product_code   = 0x000c0112;
    // 驱动器档案：auto=按总线身份自动识别；也可写档案名（sv630 / generic ...）
    // 用 --list-profiles 可看全部已注册档案。
    std::string  profile_name   = "auto";
    bool         profile_force  = false;  // 1=身份不符也照用指定档案（只记警告）
    bool         auto_identity  = true;   // 用总线实读 vid/pid 覆盖上面两项
    bool         use_explicit_pdo = false;
    int          explicit_profile = 0;    // 0=用档案默认档位；>0 强制该档位
    uint16_t     wd_divider     = 0;
    uint16_t     wd_intervals   = 0;
    bool         param_via_sdo  = true;   // 6081/6083/6084 走 SDO 下发
};

// 单轴的运动/标定参数（每个轴一份，见 AppConfig::axis[]）。
// 现场相关，不随伺服型号变化（对照 drive_profile.h 的边界说明）。
struct AxisCfgFile {
    double   inc_per_mm   = 0.0;      // 脉冲当量（inc/mm）；**0 = 未设置**：由脚本 UNITS(轴, 值) 运行时设置（v0.8.1）
    int      axis_index   = 0;      // 该轴对应的逻辑轴号（默认取数组下标）
    double   def_speed    = 100.0;
    double   def_accel    = 500.0;
    double   spd_max      = 3276.7;
    double   tol_mm       = 0.05;
    uint32_t timeout_ms   = 10000;
};

struct RtCfg {
    int cpu  = 3;      // 绑核 -1 = 不绑
    int prio = 80;     // SCHED_FIFO 优先级
};

// 脚本（见 docs/planA/08/09）：进程启动后按需在独立线程里跑一个脚本。
// 规则：控制器**同一时刻只加载一种脚本语言**（BASIC | Lua 互斥），见 engine_rule.h。
struct ScriptCfg {
    bool         enable     = false;    // SCRIPT_ENABLE：1=启动时自动跑 SCRIPT_FILE
    std::string  file;                  // SCRIPT_FILE：.bas / .lua 路径（空则即使 enable 也跳过）
    std::string  engine     = "basic";  // SCRIPT_ENGINE：脚本引擎，basic(默认) | lua（见 docs/planA/09）
    bool         wait_bus   = true;     // SCRIPT_WAIT_BUS：先等总线就绪再跑
    int          wait_bus_ms= 15000;    // SCRIPT_WAIT_BUS_MS：等总线超时
    bool         trace      = false;    // SCRIPT_TRACE：单步跟踪打印到 stdout
    long long    max_steps  = 5000000;  // SCRIPT_MAX_STEPS：语句级步数预算
};

// 调试通道（DebugServer，见 docs/planA/13/16）：TCP JSON-Lines，PC 上的 VSCodium 插件来连。
//   * 板端是服务端，随 kine-x.service 自启；默认绑业务网（0.0.0.0 / 192.168.1.11），不是 127.0.0.1；
//   * 做 D1~D5（sys.info / var.* / cmd / script.* / 订阅推送 + D4 热更新 + D5 断点单步）；
//     D5 仅 BASIC 引擎实装，Lua 引擎下明确回 NOT_SUPPORTED 且 caps 不报 d5（降级不伪装）；
//   * 与 4321/502 完全无关，也不复用它们的分帧风格。
struct DebugCfg {
    bool        enable    = true;      // DEBUG_ENABLE：默认开机启用
    int         port      = 5000;      // DEBUG_PORT：5000 为本项目自定（非 ZDevelop 的 500）
    std::string bind      = "0.0.0.0"; // DEBUG_BIND：绑业务网卡；局域网 PC 直连
    std::string token;                 // DEBUG_TOKEN：可选加固；空 = 不鉴权
    long long   max_steps = 5000000;   // DEBUG_MAX_STEPS：script.run 的步数预算兜底
    // DEBUG_SCRIPT_DIR：D6 文件管理（file.*）的脚本存取目录。
    // script.compile 带 name 时源码落盘到这里；file.list/get/del 只作用该目录（不递归）。
    std::string script_dir = "/userdata/kine-x/scripts";
    // DEBUG_RESTART_CMD：`sys.restart`（D7 重启控制器）的执行命令，默认经 systemd 重启本服务
    // （服务以 cat 用户运行，板端已配 sudo 免密）。命令失败时回退「优雅退出 + 失败码」，
    // 由 systemd `Restart=on-failure` 兜底。空 = 不提供重启能力（caps 不报 d7）。
    std::string restart_cmd = "sudo -n systemctl restart kine-x.service";
};

struct AppConfig {
    // 运行时实际轴数（AXIS_COUNT，合法范围 1..kAxisMax）；编译期上限见 axis_limits.h。
    int          axis_count = 1;
    // M3：定位规划方式 0=PP（驱动器内部规划，默认）/ 1=CSP（控制器每拍 0x607A 规划，
    //     需 DC 同步（ECAT_DC_ASSIGN）与脚本 UNITS 设置的脉冲当量）。脚本 MOTION_MODE(m) 可运行时切换。
    int          motion_mode_csp = 0;
    // 每轴运动参数：全局键（INC_PER_MM / MV_SPEED ...）写入**全部**轴；
    // 需要单轴覆盖时用 AXIS<n>_* 形式（如 AXIS1_INC_PER_MM）。
    AxisCfgFile  axis[kAxisMax];
    EcatCfg      ecat;
    RtCfg        rt;
    ScriptCfg    script;
    DebugCfg     debug_cfg;

    bool debug   = false;
    int  log_ms  = 3000;

    AppConfig() {
        for (int i = 0; i < kAxisMax; ++i) axis[i].axis_index = i;
    }

    // 解析文件；文件不存在时用默认值并返回 false（不致命）
    bool load(const std::string& path);
    void dump() const;
};

} // namespace kx
