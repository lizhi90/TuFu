// command_table.h —— 设备命令名与文档清单（两套脚本引擎共享的「单一事实来源」）
//
// 用途：
//   * Lua 引擎据此**批量注册同名全局函数**（见 docs/planA/09 §3.3 / 任务 L-07），
//     使两种语言共享同一份命令名——**同名即同义**；
//   * 插件（VSCodium）的补全 / 签名帮助 / Hover / 语法高亮按钮经
//     `Extension/tools/gen-commands.mjs` 从本文件生成 `data/commands.json`，
//     并同步注入 `syntaxes/*.tmLanguage.json` 的命令名高亮 —— **命令名不允许硬编码在插件里**。
//
// 来源与维护纪律：
//   * 清单对应 `motion_host.cpp` 中 `name == "XXX"` 的分支集合；
//     新增/重命名设备命令时，**必须同时**更新本表与 `MotionHost::call()` 分支，
//     并重跑 `node Extension/tools/gen-commands.mjs`（smoke 会做一致性校验）；
//   * 本表为 header-only（inline），不引入 MotionHost/ecrt 依赖，
//     便于 `lua_engine_test` 用假宿主做纯逻辑测试（见 L-18）；
//   * 分节注释（`// xxx`）会被生成器用作补全分组标签，改动时注意保持一行一个**简短**标签；
//     端口分节的说明：BASIC 侧是 `#`/数组语法糖，Lua 侧改 PORT_* 显式函数（故不注册同名全局）；
//     明确不可用分节：调用即报 NOT_SUPPORTED，绝不静默（同 08 §8.4）。
#pragma once

namespace kx {

// ---- 命令名（顺序无关，仅需「存在且唯一」；nullptr 结尾）----
inline const char* const* motion_command_names() {
    static const char* const kNames[] = {
        // 运动 / 使能 / 回零
        "EN", "ENABLE", "DIS", "DISABLE", "STOP", "RAPIDSTOP",
        "MOVE", "MOVR", "MOVEREL", "MOVEABS", "MOVE_ABS",
        "JOG", "VJOG", "VMOVE", "CANCEL", "HOME", "DATUM",
        "MOTION_MODE",
        "DELAY", "SLEEP", "WAIT", "WAITIDLE", "TICKS",
        // 轴 / 参数
        "BASE", "AXIS", "SPEED", "ACCEL", "DECEL",
        "ATYPE", "UNITS", "DRIVE_PROFILE", "AXIS_ADDRESS", "DRIVE_CONTROLWORD",
        // 状态查询
        "POS", "MPOS", "DPOS", "AXISSTATUS", "IDLE", "ISIDLE", "BUSY",
        "BUS", "BUSOK", "ENABLED", "ALARM", "DISABLE_GROUP",
        // 总线
        "SLOT_SCAN", "SCAN", "SLOT_START", "SLOT_STOP", "BUSSTOP",
        "SCAN_EVENT", "NODE_COUNT", "NODE_AXIS_COUNT", "NODE_STATUS", "NODE_IO",
        "NODE_AIO", "NODE_INFO", "ETHERCAT", "ECUSTOM", "ETH_MODE",
        // 端口
        "OPEN", "CLOSE", "PRINT", "PUTCHAR", "GET", "PORT_STATUS", "PORT_CLIENTS", "PORT_TARGET", "PORT_MAX",
        "PORT_INFO", "JOGLEAD", "SRAMP", "FASTDEC", "VP_SPEED",
        // Modbus / 持久化 / 任务
        "MODBUS_REG", "MODBUS_IEEE", "NVSET", "NVGET", "REGMAP_GET",
        "MB_READ", "MB_WRITE", "MB_LIST", "MBREG_ZONE", "MBREG_PUT", "MBD_STATUS", "MBD_LIST",
        "RUNTASK", "STOPTASK", "PROC_STATUS",
        // 明确不可用
        "SDO_WRITE",
        nullptr
    };
    return kNames;
}

// ---- 命令文档（签名 + 一句话说明；插件补全/Hover/签名帮助的数据源）----
// 签名用「调用形态」书写：[] = 可选参数；BASIC 的赋值式写法在说明里指出。
// 说明中**不要出现双引号**（生成器按行解析；用「」代替）。
struct MotionCommandDoc {
    const char* name;
    const char* sig;
    const char* brief;
};

inline const MotionCommandDoc* motion_command_docs() {
    static const MotionCommandDoc kDocs[] = {
        // 运动 / 使能 / 回零
        {"EN", "EN()", "使能当前轴（BASE 选择），阻塞到使能成功或超时"},
        {"ENABLE", "ENABLE([axis])", "使能指定轴（缺省当前轴），阻塞到生效"},
        {"DIS", "DIS()", "去使能当前轴（阻塞到生效）"},
        {"DISABLE", "DISABLE([axis])", "去使能指定轴"},
        {"STOP", "STOP([axis])", "按参数减速停止（halt），立即返回"},
        {"RAPIDSTOP", "RAPIDSTOP([axis])", "立即停止；参数是**轴号**（非 ZBasic 的 mode）"},
        {"MOVE", "MOVE(d[, spd[, acc[, wait]]])", "相对定位（距离 mm）；wait=0 异步下发立即返回；BASE 多轴时 d_i 为各轴距离（直线插补）"},
        {"MOVR", "MOVR(d[, spd[, acc[, wait]]])", "同 MOVE（相对定位）"},
        {"MOVEREL", "MOVEREL(d[, spd[, acc[, wait]]])", "同 MOVE（相对定位）"},
        {"MOVEABS", "MOVEABS(p[, spd[, acc]])", "绝对定位（目标 mm）；BASE 多轴时 p_i 为各轴目标（M3 直线插补，阻塞，spd/acc 为主导轴参数）"},
        {"MOVE_ABS", "MOVE_ABS(p[, spd[, acc[, wait]]])", "同 MOVEABS（绝对定位）"},
        {"JOG", "JOG(v)", "点动（速度 mm/s），立即返回；用 CANCEL/STOP 停止"},
        {"VJOG", "VJOG(dir)", "方向点动（沿用当前 SPEED）；dir=1/-1"},
        {"VMOVE", "VMOVE(dir)", "速度模式持续运动（dir=1/-1）；立即返回"},
        {"CANCEL", "CANCEL([mode])", "停止当前轴运动并清命令队列"},
        {"HOME", "HOME([axis])", "回零，阻塞到完成/出错"},
        {"DATUM", "DATUM([axis])", "故障复位（6040h 0x80 脉冲）"},
        {"MOTION_MODE", "MOTION_MODE([m])", "M3 定位规划方式：0=PP（驱动器规划，默认）/ 1=CSP（控制器每拍规划，需 DC+UNITS）；缺省读当前值"},
        {"DELAY", "DELAY(ms)", "等待毫秒（可被中止打断）"},
        {"SLEEP", "SLEEP(ms)", "同 DELAY"},
        {"WAIT", "WAIT(ms)", "同 DELAY"},
        {"WAITIDLE", "WAITIDLE()", "等待到运动停止（超时报错）"},
        {"TICKS", "TICKS()", "毫秒倒数计时：两次差值=经过毫秒（Lua 侧唯一时间源）"},
        // 轴 / 参数
        {"BASE", "BASE([n1[, n2...]])", "设置/读取当前轴号；M3：多参数=轴表（配合 MOVEABS/MOVE 多目标做直线插补）"},
        {"AXIS", "AXIS([n])", "同 BASE（轴选择，单轴）"},
        {"SPEED", "SPEED(axis[, v])", "读写定位速度（mm/s）；BASIC 亦可写 SPEED(axis)=v"},
        {"ACCEL", "ACCEL(axis[, v])", "读写加速度（mm/s^2）"},
        {"DECEL", "DECEL(axis[, v])", "读写减速度"},
        {"ATYPE", "ATYPE(axis[, v])", "轴类型（本控制器仅记录，不参与换算）"},
        {"UNITS", "UNITS(axis[, v])", "脉冲当量（仅记录；内部单位恒为 mm）"},
        {"DRIVE_PROFILE", "DRIVE_PROFILE(axis[, v])", "驱动器档案选择（仅记录）"},
        {"AXIS_ADDRESS", "AXIS_ADDRESS(axis[, v])", "从站地址（仅记录）"},
        {"DRIVE_CONTROLWORD", "DRIVE_CONTROLWORD(axis, v)", "直写控制字；仅允许 128(清错)/6(关机)/15(使能)"},
        // 状态查询
        {"POS", "POS([axis])", "实际位置（mm，=MPOS）"},
        {"MPOS", "MPOS([axis])", "测量/反馈位置（mm）"},
        {"DPOS", "DPOS([axis])", "指令位置（mm）"},
        {"AXISSTATUS", "AXISSTATUS([axis])", "轴状态字（位表见 docs/planA/08 §8.2）"},
        {"IDLE", "IDLE([axis])", "-1=到位 / 0=运动中"},
        {"ISIDLE", "ISIDLE([axis])", "同 IDLE"},
        {"BUSY", "BUSY()", "运动标志：1=运动中"},
        {"BUS", "BUS()", "总线快照（node_count / nodes 等）"},
        {"BUSOK", "BUSOK()", "总线是否就绪：1=就绪"},
        {"ENABLED", "ENABLED([axis])", "是否已使能：1=是"},
        {"ALARM", "ALARM()", "是否报警：1=有报警"},
        {"DISABLE_GROUP", "DISABLE_GROUP(n)", "分组去使能（单轴机型为空操作）"},
        // 总线
        {"SLOT_SCAN", "SLOT_SCAN([slot])", "扫描总线，返回从站数（0=未扫到）"},
        {"SCAN", "SCAN()", "请求重扫总线（软触发）"},
        {"SLOT_START", "SLOT_START([slot])", "启动总线：1=成功"},
        {"SLOT_STOP", "SLOT_STOP([slot])", "停止总线"},
        {"BUSSTOP", "BUSSTOP()", "总线软停"},
        {"SCAN_EVENT", "SCAN_EVENT(...)", "输入事件查询（本控制器无本地 IO，恒 0）"},
        {"NODE_COUNT", "NODE_COUNT()", "从站数量"},
        {"NODE_AXIS_COUNT", "NODE_AXIS_COUNT(i)", "第 i 个从站的轴数"},
        {"NODE_STATUS", "NODE_STATUS(i)", "第 i 个从站 AL 状态（1/2/3/4/8）"},
        {"NODE_IO", "NODE_IO(i)", "第 i 个从站数字量 IO"},
        {"NODE_AIO", "NODE_AIO(i)", "第 i 个从站模拟量"},
        {"NODE_INFO", "NODE_INFO(i)", "第 i 个从站信息"},
        {"ETHERCAT", "ETHERCAT()", "EtherCAT 状态查询"},
        {"ECUSTOM", "ECUSTOM(...)", "自定义总线设置（预留）"},
        {"ETH_MODE", "ETH_MODE(...)", "EtherCAT 模式（预留）"},
        // 端口
        {"OPEN", "OPEN(id, type, ...)", "打开端口：TCP_SERVER(端口) / TCP_CLIENT(端口, IP)"},
        {"CLOSE", "CLOSE(id)", "关闭端口"},
        {"PRINT", "PRINT(id, s[, idx])", "发送字节（不追加换行；协议自负）；idx=客户端下标（多客户端端口，省略=0）"},
        {"PUTCHAR", "PUTCHAR(id, v)", "发送单字节"},
        {"GET", "GET(id[, n[, ...]])", "非阻塞读取（返回收到的字节/数据）；第 5 参 idx=客户端下标（多客户端端口）"},
        {"PORT_STATUS", "PORT_STATUS(id[, idx])", "连接状态：1=已连接（先判断再收发）；idx=客户端下标（省略=任一客户端）"},
        {"PORT_CLIENTS", "PORT_CLIENTS(id)", "端口当前已连接客户端数（多客户端 502：触摸屏 + 机器人）"},
        {"PORT_TARGET", "PORT_TARGET(id)", "对端地址（字符串）"},
        {"PORT_MAX", "PORT_MAX()", "通道上限（16 通道：0..15）"},
        {"PORT_INFO", "PORT_INFO(port, 用途[, 主从])", "给端口打标签（D10 通讯状态面板；用途≤48字、主从≤24字）"},
        {"JOGLEAD", "JOGLEAD(axis[, s])", "点动 PP 跟随前视（秒；实际前视 = s + v²/(2a)，默认 0.5）"},
        {"SRAMP", "SRAMP(axis[, ms])", "S 曲线时间（0~250ms，0=梯形；CSP 生效，PP 模式下不生效并提示）"},
        {"FASTDEC", "FASTDEC(axis[, v])", "急停/停机减速度（mm/s²；0=未设置；CSP 规划减速、PP 写 6084）"},
        {"VP_SPEED", "VP_SPEED(axis)", "当前运动速度 mm/s（只读；内核实际位置差分估计）"},
        // Modbus / 任务
        {"NVSET", "NVSET(reg, value)", "把 4x 寄存器值持久化到脚本目录 .nvram（立即落盘；掉电/重启后 NVGET 恢复）"},
        {"REGMAP_GET", "REGMAP_GET()", "读 Modbus 寄存器表文本（脚本目录 .mbmap；用户寄存器配置载体，D11 mbmap.set 写入）"},
        {"MB_READ", "MB_READ(变量名)", "按名读 Modbus 组态变量（planA/20；插件「Modbus 配置」组态，固件热加载）"},
        {"MB_WRITE", "MB_WRITE(变量名, 值)", "按名写 Modbus 组态变量（掉电保持条目写即落盘；主站侧权限另按组态）"},
        {"MB_LIST", "MB_LIST()", "列出全部组态变量：每行 name,4x地址,类型,读写[,persist]"},
        {"MBREG_ZONE", "MBREG_ZONE(start, count)", "按区读 4x 寄存器（打包 u16 大端字符串；可 string.unpack 解码）"},
        {"MBREG_PUT", "MBREG_PUT(start, packed)", "按区写 4x 寄存器（packed 为 u16 大端序列；persist 条目生效）"},
        {"MBD_STATUS", "MBD_STATUS(设备名)", "Modbus 主站设备状态（在线/错误/超时/ok 计数；planA/21）"},
        {"MBD_LIST", "MBD_LIST()", "列出主站设备点位：每行 设备.点位,read|write,fcN,类型,4x地址|var"},
        {"NVGET", "NVGET(reg)", "读回 .nvram 保存的寄存器值（无记录返回 0）；恢复由脚本 init 阶段完成"},
        {"MODBUS_REG", "MODBUS_REG(i[, v])", "读写 4x 寄存器镜像"},
        {"MODBUS_IEEE", "MODBUS_IEEE(i[, v])", "读写 32 位浮点镜像（低字在前）"},
        {"RUNTASK", "RUNTASK(n, fn)", "任务登记（单线程引擎：只登记状态，不并发执行）"},
        {"STOPTASK", "STOPTASK(n)", "停止任务（登记状态）"},
        {"PROC_STATUS", "PROC_STATUS(n)", "任务状态"},
        // 明确不可用（调用即明确报错，绝不静默）
        {"SDO_WRITE", "SDO_WRITE(...)", "明确不可用：调用即报 NOT_SUPPORTED（见 docs/planA/08 §8.4）"},
        {nullptr, nullptr, nullptr}
    };
    return kDocs;
}

} // namespace kx
