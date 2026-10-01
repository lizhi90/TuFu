--[[ ==========================================================================
EtherCAT_SocketServer.lua —— EtherCAT 单轴控制器对外服务（Lua 版）

本文件是根目录 `EtherCAT_SocketServer.bas`（ZMC ZBasic，2020 行）在本控制器
（Kine-X：鲁班猫2 + IgH EtherCAT + Lua 5.4 引擎）上的 **1:1 功能移植**：
IP / 端口 / 报文格式 / 寄存器映射全部不变，上位机与触摸屏无需任何修改。

保留并实现的全部功能：
  1. EtherCAT 总线初始化（扫描/开启/清错/使能，失败 2s 重试；总线重试期间 4321 照常在线）
  2. TCP 服务端 4321：HELLO / EN / DIS / VJOG / STOP / MA / MR / STA / SCAN / BUSSTOP /
     纯数值命令 / W / WSTA / WT / WTC / WZERO / WCLR / WDIV / WSA /
     心跳 beat/n，全部应答以**字面 "/n"** 结尾（R* 机器人命令已随 4320 链路取消，2026-09-28）
  3. Modbus-TCP 从站 502（本脚本自行监听并解析 MBAP；触摸屏 IT7000 站号 1 轮询）：
     寄存器映射与旧程序完全一致（输入/回写/读区/命令区/称重区/机器人区）
  4. 称重采集：TCP 客户端 -> TAS-LAN-869(192.168.1.80:10123 纯透传) -> RS485 称重模块，
     手工组 Modbus-RTU 帧（CRC16 低字节在前）、粘包分包重组、超时计数、写命令队列
  5. 机器人通讯：TCP 客户端 -> 机器人(192.168.1.221:4320)，开机等待 20s、间隔重连、
     12 条监控指令轮询、控制短命令与 RCMD 透传、双格式应答解析、报警汇总
  6. 蠕动泵（框架，2026-09-27）：TCP 客户端 -> 透传网关(192.168.1.81:10123)，与称重同套路自组
     Modbus-RTU 帧（泵为 485 从站，站号 1）；上电/重连先写 reg0=1 使能 485 控制；轮询控制方式/
     方向/转速/流速/运行/错误码(reg50)；HMI 命令位（4x203）启动/停止/写转速/重新使能/全速开关；
     PSTA; 诊断。未实现（TODO）：分配模式（灌装）、校准、回吸、方向设置等

与旧 ZBasic 的差异（均为新控制器能力/限制所致，功能语义不变）：
  * 旧程序用 **4 个并发任务**；新引擎是单线程，本文件改成**一个主循环里的协作步进**
    （socket_step / modbus_step / scale_step / robot_bridge_step / pump_step），仍用 RUNTASK(1..5) 登记任务状态
    （5 = 蠕动泵，2026-09-27 框架新增）。
  * 旧控制器只有 10/11 两个自定义网口（机器人链路原本无通道）；新控制器有 16 个槽位
    （PortManager kMaxPorts=16，编号 0..15 仅是句柄，无 ECUSTOM 概念、无需通道规划/抢占），
    故端口分配：10=4321 服务端、11=称重网关、12=机器人、13=Modbus-TCP 从站(502)、14=蠕动泵（框架）。
    502 是特权端口，服务以 cat 用户运行，需 systemd 授予 CAP_NET_BIND_SERVICE
    （deploy/10-install-service.sh 已配 AmbientCapabilities）。
  * 定位命令：旧程序 MOVEABS 后靠 IDLE 监测异步完成；新 API 的 MOVEABS 默认阻塞，
    本文件用第 4 参 `wait=0` **异步下发**（见 docs/planA/11 §3.1），随后自行监测到位。
  * 计时：Lua 沙箱无 os.time/os.clock，改用设备命令 TICKS()（毫秒倒数，语义同 BASIC TICKS）。
  * 数值格式化 TOSTR / 数值解析 VAL / 字符串函数用 Lua 原生实现（fmt / parse_num / string.*）。
  * CRC16 在 Lua 内实现（标准 Modbus 多项式 0xA001），组帧时低字节在前。
  * 机器人链路（4320，珞石 xCore 手册 10.3，见根《xCore控制系统使用手册V2.2_学习笔记.md》156~158 页）：
    ★机器人链路已改为 **Modbus 直连**（机器人作主站连 502，A/B 区见 docs/planA/19）：
    原 4320 Socket 客户端、R* 命令、报文后缀/间隔/启动延迟等**全部取消**（2026-09-28 现场决定）；
    屏幕命令经桥接整形为 ctrl_* 脉冲（≥60ms），机器人 sta_* 状态镜像回 4x140~150。
  * **启动时序（2026-09-27，仅触摸屏 4x152=1 路径）**：收到启动后**先发 `pp_to_main`**，机器人回 `true`
    才补发 `start`（回 `false`/超时/断链则不发并在日志说明）；`pp_to_main` 结果记录在 4x201。
    4321 的 `RSTART;` 仍为直接下发（未改）。
  * 端口标签（D10 通讯状态，2026-09-26）：启动时 port_tag_declare() 用 PORT_INFO 声明各端口用途/主从
    （10=4321 ASCII 服务端、11=称重网关(Modbus 主站)、12=机器人、13=Modbus-TCP 从站）；老固件无此命令时静默忽略。
  * 输入口调试块（in0~in23 / SCAN_EVENT）删除：本控制器无该 DI 硬件，SCAN_EVENT 恒 0。
  * 旧程序的“任务看门狗自动拉起”由「分步 pcall + 异常限频日志」等效替代（单线程不会整脚本停摆）。

运行方式（方案 A，插件「控制器文件」里把本文件设为主文件即可开机运行）：
  * 下载本文件到控制器 -> 设为主文件 -> 重启控制器（或下次开机自动运行）；
  * 长驻脚本必须把 SCRIPT_MAX_STEPS / DEBUG_MAX_STEPS 设为 0（不限），否则几秒后会被
    步数预算判 BUDGET_EXCEEDED 退出（config/app.conf 出厂已置 0）。
========================================================================== ]]


-- ======================== 模块 include（编译前展开；顺序=依赖顺序，勿调换） ========================
include("kx_base.lua")
include("kx_mbsync.lua")
include("kx_bus.lua")
include("kx_socket.lua")
include("kx_modbus.lua")
include("kx_scale.lua")
include("kx_pump.lua")
include("kx_robot.lua")

-- ======================== 主循环 ========================
-- 触摸屏 HMI 变量（模块级初始化：离线测试 KX_TEST 不走 main 也要可用）
hmi_val  = { -99999, -99999 }        -- 哨兵值：保证首次写 0 也被当成新数据
hmi_flag = { 0, 0 }
hmi_cnt  = { 0, 0 }
mb_cmdold, mb_trigold, mb_jogold = 0, 0, 0
mb_calold = 0
mb_tgt, mb_ax, mb_mode = 0, 0, 0

-- 任务异常隔离：单线程下用 pcall 防止一个异常终止整脚本；
-- 但「中止/预算」标记必须原样上抛，否则控制器关机时脚本无法退出。
local task_fail = {}
local function safe(name, fn)
    local ok, err = pcall(fn)
    if not ok then
        if type(err) == "string" and (string.find(err, "\001zm:abort", 1, true) or
                                      string.find(err, "\001zm:budget", 1, true)) then
            error(err)                                   -- 关机/预算信号：上抛给引擎
        end
        local n = (task_fail[name] or 0) + 1
        task_fail[name] = n
        if n == 1 or elapsed(task_fail[name .. "_t"] or (now() - 4000)) > 3000 then
            task_fail[name .. "_t"] = now()
            print("[" .. name .. "] 异常(累计" .. n .. "次): " .. tostring(err))
        end
    end
end

local function main()
    print("==== Kine-X EtherCAT_SocketServer.lua 启动 ====")
    print("[init] IP/端口: 控制器 192.168.1.11; 4321 服务端 / 502 Modbus从站 / " ..
          SCALE_IP .. ":" .. SCALE_PORT .. " 称重 / 机器人 = Modbus 直连 502（A/B 区见 planA/19）")

    -- 1) 4321 服务端在总线初始化之前开启（总线重试期间端口永远在线）
    local ok, err = pcall(OPEN, COM_PORT, "TCP_SERVER", SVR_PORT)
    if not ok then
        print("[init] 4321 监听失败：" .. tostring(err))
    else
        svr_open = 1
        print("[init] Socket 服务端已开启，端口 " .. SVR_PORT)
    end
    -- 1b) Modbus 502 也先开（对齐 ZMC：Modbus 服务先于总线初始化就绪，触摸屏无需等总线）
    --     ★P3b/v0.12.2：MB_WIRE=false 时 502 由固件内建从站接管，本脚本不得再尝试 bind（否则每次启动误报占用）
    if not MB_WIRE then
        print("[mb] 502 由固件内站接管（MB_WIRE=false；Lua 502 线上处理已退役）")
    else
    do
        local okmb, ermb = pcall(OPEN, MB_CH, "TCP_SERVER", MB_PORT)
        if okmb then
            modbus_state.opened = 1
            print("[mb] Modbus-TCP 从站已监听 502（站号 " .. MB_STATION .. "）")
        else
            print("[mb] 502 监听失败：" .. tostring(ermb) .. "（5s 后重试；需 CAP_NET_BIND_SERVICE）")
        end
    end
    end

    beat_last = now()
    last_rx = now()

    -- 2) 任务登记（单线程只登记状态，PROC_STATUS 可查；与原 4 任务编号一致）
    pcall(RUNTASK, 1, "socket_task")
    pcall(RUNTASK, 2, "modbus_task")
    pcall(RUNTASK, 3, "scale_task")
    pcall(RUNTASK, 4, "robot_task")
    pcall(RUNTASK, 5, "pump_task")          -- 蠕动泵（框架，2026-09-27）

    -- 2b) D10：端口标签（用途/主从）；点动前视（脚本可调，内核默认 0.5s）
    port_tag_declare()
    pcall(SRAMP, 0, SRAMP_MS)          -- S 曲线（仅 CSP；PP 下不生效并有日志提示）
    pcall(FASTDEC, 0, FASTDEC_MM)      -- 急停/停机减速度（0=未设置）
    pcall(JOGLEAD, 0, JOG_LEAD_S)      -- ★放最后：命令槽为覆盖式，避免被紧随的 SRAMP/FASTDEC 顶掉
    local jl_check_t = 0               -- ★JOGLEAD 自愈计时（见主循环）
    local mbn_check_t = 0              -- ★v0.12.2：按名访问名字表刷新计时（组态可变，5s 一次）
    if mbnames_refresh then mbnames_refresh() end    -- ★v0.12.2：先装载「地址→变量名」映射（按名访问层）
    if nvram_restore then nvram_restore() end        -- ★掉电保存的屏输入参数恢复（.nvram；已组态地址按名直达固件）
    if mbsync_pull then mbsync_pull() end            -- ★P3b：从固件库存拉取一次（业务寄存器初值）
    -- ★地轨参数重发已移至 bus_step「进入运行」时刻（2026-09-30）：init 阶段命令槽可能未被 RT 取走

    -- 3) 称重/机器人首次 OPEN 由各自 step 负责；总线由 bus_step 分步初始化
    if robot_bridge_init then robot_bridge_init() end        -- ★机器人桥接初始值（soft_estop=1）

    while true do
        if rescan_flag == 1 then
            rescan_flag = 0
            mvmode = 0
            bus_reset()
            print("[bus] 收到重扫请求，重新初始化总线")
        end

        -- 网络服务优先：bus_step 里的总线扫描/使能会阻塞本线程，
        -- 先把 Modbus(502) 与上位机(4321) 的应答跑完，触摸屏不被总线动作拖超时。
        if mbnames_tick then mbnames_tick() end      -- ★v0.12.2：按拍读缓存失效（本拍首读回源固件）
        safe("mbsync_pull", mbsync_pull)             -- ★P3b：先取固件库存（主站写入/组态值）再跑业务
        safe("modbus", modbus_step)
        safe("socket", socket_step)
        safe("bus", bus_step)
        safe("scale", scale_step)
        safe("robot", robot_bridge_step)

        -- ★JOGLEAD 自愈（2026-09-29）：内核无默认值，命令槽覆盖式；若开机未生效则点动被静默拒绝
        if elapsed(jl_check_t) > 5000 then
            jl_check_t = now()
            local ok, v = pcall(JOGLEAD, 0)
            if ok and (type(v) ~= "number" or v <= 0) then
                pcall(JOGLEAD, 0, JOG_LEAD_S)
                print("[init] JOGLEAD 自愈重发（上次读回=" .. tostring(v) .. "，目标 " .. JOG_LEAD_S .. "s）")
            end
        end
        safe("pump", pump_step)
        safe("mbsync_push", mbsync_push)             -- ★P3b：本拍脏字按段回推固件（不触碰主站未改写字）
        if elapsed(mbn_check_t) > 5000 then          -- ★v0.12.2：组态热更新后名称映射自动跟随
            mbn_check_t = now()
            safe("mbnames", mbnames_refresh)
        end

        DELAY(2)
    end
end

-- 离线自测钩子：测试脚本设置 KX_TEST=true 后 dofile 本文件，
-- 可直接调用命令解析/Modbus 处理/CRC 等函数（不进入无限主循环）。
if KX_TEST then
    _T = {
        cmd_exec = cmd_exec,
        _set_conn = function(v) conn_old = v end,
        _set_mvmode = function(v) mvmode = v end,
        _set_bus_ok = function(v) bus_ok = v end,
        socket_step = socket_step,
        modbus_step = modbus_step,
        scale_step = scale_step,
        bus_step = bus_step,
        modbus_state = modbus_state,
        crc16 = crc16,
        rtu_frame = rtu_frame,
        mb_handle_pdu = mb_handle_pdu,
        regs = regs,
        reg_get = reg_get,
        reg_set = reg_set,
        reg_set_f = reg_set_f,
        reg_get_f = reg_get_f,
        fmt = fmt,
        field_num = field_num,
        robot_getkey = robot_getkey,
        robot_getctl = robot_getctl,
        robot_rsock = robot_rsock,
        robot_start_flush = robot_start_flush,
        mb_wq = mb_wq,
        mb_laston = mb_laston,
        pump_state = pump_state,
        pump_cmd_apply = pump_cmd_apply,
        pump_rev_run = pump_rev_run,
        pump_rev_cfg_apply = pump_rev_cfg_apply,
        pump_rd_hold = pump_rd_hold,
        nvram_restore = nvram_restore,
        coil_get = coil_get, coil_set = coil_set,
        dis_get = dis_get, dis_set = dis_set,
        pump_run_by_volume = pump_run_by_volume,
        NVRAM_REGS = NVRAM_REGS,
        PUMP_STATION = PUMP_STATION,
        pump_state_code = pump_state_code,
        pump_tubes = pump_tubes,
        pump_w6 = pump_w6,
        pump_w10 = pump_w10,
        pump_coil = pump_coil,
        pump_rd_inputs = pump_rd_inputs,
        pump_rd_coils = pump_rd_coils,
        pump_f32 = pump_f32,
        pump_handle_frame = pump_handle_frame,
        pump_step = pump_step,
        robot_reg_cmd = robot_reg_cmd,
        robot_progname_read = robot_progname_read,
        port_tag_declare = port_tag_declare,
        robot_hb = robot_hb,
        robot_bridge_step = robot_bridge_step,
        robot_bridge_cmd = robot_bridge_cmd,
        robot_bridge_init = robot_bridge_init,
        rail_apply = rail_apply,
        mbsync_pull = mbsync_pull,
        mbsync_push = mbsync_push,
        mbnames_refresh = mbnames_refresh,
        mbnames_tick = mbnames_tick,
        _set_mb_wire = function(v) MB_WIRE = v end,
        rail_apply_saved = rail_apply_saved,
        robot_axis_apply = robot_axis_apply,
        robot_axis = robot_axis,
        robot_bridge_pulse = robot_bridge_pulse,
        robot_bridge_estop = robot_bridge_estop,
        RB = RB,
        scale_state = scale_state,
        scale_calib_submit = scale_calib_submit,
        scale_do_cmd = scale_do_cmd,
        scale_par32 = scale_par32,
        MV_ERRMASK = MV_ERRMASK,
    }
    return
end

main()

