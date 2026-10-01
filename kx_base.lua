-- ============================================================
-- kx_base.lua — 常量 / 运行状态 / 工具函数（Kine-X 控制器脚本模块）
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_base.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== 全局配置（与旧程序同名同值） ========================
local DBG           = 0            -- 总调试开关: 1=打印命令/数值解析等调试信息
local COM_PORT      = 10           -- 4321 服务端端口句柄（编号仅作句柄，无 ZMC 通道规划/抢占）
local SVR_PORT      = 4321         -- TCP 服务端监听端口
local CMDMAX        = 100          -- 命令缓冲上限
local CMD_IDLE_MS   = 200          -- 无结束符空闲超时(ms)
local PULSE_EQUIV   = 14043.41     -- ★脉冲当量(inc/mm)：**由本脚本设置**（控制器不设默认，v0.8.1）
                                   --   不同伺服驱动器/机械机构各不相同，按现场实测修改此值；
                                   --   bus_step 里通过 UNITS(0, PULSE_EQUIV) 写给内核，未设置时运动命令会被拒绝
local MVAXIS        = 0            -- 数值命令运动轴号（单轴）
local MV_SPEED      = 100          -- 数值命令默认速度(mm/s)
local MV_TIMEOUT    = 10000        -- 数值命令到位超时(ms)
local MV_ACCEL      = 500          -- 数值命令默认加速度(mm/s^2)
local MV_SPD_MAX    = 3276.7       -- HMI 速度上限(mm/s)
local RB_PMAIN_FLAG = 201          -- ★4x201：pp_to_main 结果标志（布尔，只写不由屏写）——
                                   --   机器人回 true→1 / false→0；发送 pp_to_main 时先清 0；启动程序（4x152=1）后清 0
local SRAMP_MS      = 0            -- ★S 曲线时间（ms，0~250；0=梯形）——仅 CSP 生效（MOTION_MODE=1）
local FASTDEC_MM    = 0            -- ★急停/停机减速度（mm/s²；0=未设置=旧行为）
local JOG_LEAD_S    = 0.5          -- ★点动 PP 跟随前视（秒）——固件无默认值，须由本脚本 JOGLEAD 设置；
                                   --   实际前视 = 该值 + v²/(2a)；现场可用插件「设备命令」热调：JOGLEAD 0, 0.6
local MV_TOL        = 0.05         -- 到位位置容差(mm)
-- Kine-X 轴状态字只使用 4 个有数据源的位（见 docs/planA/08 §8.2）：
--   bit2=通讯 bit3=驱动器故障 bit8=随动超限 bit22=告警输入
local MV_ERRMASK    = 4 + 8 + 256 + 4194304   -- 4194572
local HB_MS         = 2000         -- 服务端心跳间隔(ms)
local AUTO_REENABLE = 1            -- 总线掉线恢复后是否自动重新使能：0=等操作员按使能(安全) / 1=自动恢复上一次使能（现场选 1）

-- 称重
local SCALE_CH      = 11           -- 称重网关 TCP 客户端端口句柄
local SCALE_IP      = "192.168.1.80"
local SCALE_PORT    = 10123
local SCALE_TXLEN   = 8            -- Modbus RTU 请求帧长
local SCALE_RXMAX   = 64
local SCALE_POLL_MS = 200
local SCALE_TIMEOUT = 1000
local SCALE_RECONN_MS = 5000
local SCALE_DBG     = 0
local SCALE_LOG_MS  = 3000

-- ---- 蠕动泵（保定创锐 M 基本型；RS485 Modbus-RTU 从站，经透传网关，与称重同一套路）----
local PUMP_CH         = 14           -- TCP 客户端端口句柄（10=4321/11=称重/12=机器人/13=502/14=泵）
local PUMP_IP         = "192.168.1.81"
local PUMP_PORT       = 10123
local PUMP_STATION    = 4            -- 从站地址（1~31；出厂 1；★本站现场实测=4，2026-09-27 扫描 0~20 确认）
local PUMP_RECONN_MS  = 5000
local PUMP_POLL_MS    = 500          -- 状态轮询周期（输入寄存器/线圈）
local PUMP_TIMEOUT    = 2000
local PUMP_RXMAX      = 64
local PUMP_LOG_MS     = 3000
local PUMP_DBG        = 0

-- 机器人
-- 端口上限：引擎命令 PORT_MAX() = 15（0..15，共 16 槽位，见 src/script/port_manager.h）。
-- 注意：不要用 local PORT_MAX 覆盖同名命令，否则后续 PORT_MAX() 调用会报“attempt to call a number”。
-- ★机器人 Modbus 直连（502 多客户端，机器人为主站；**替代原 4320 Socket 链路**，2026-09-28 现场决定）
--   地址与语义见 docs/planA/19（A 区 4x1000~1099 机器人写 / B 区 4x1100~1139 机器人读）
local ROBOT_HB_TIMEOUT_MS = 1500    -- 心跳超时（sta_heartbeat 无更新判离线）
local ROBOT_PULSE_MS      = 100     -- 控制信号脉冲宽度（xCore 手册要求 ≥60ms）
local ROBOT_START_GAP_MS  = 2000    -- 启动时序：电机上电 → ctrl_pptomain → 本间隔 → ctrl_program_start（手册建议 pptomain 留 2s）
local ROBOT_SOFT_ESTOP_MS = 200     -- 软急停脉冲宽度（ctrl_soft_estop=0 触发，之后回 1）
-- ★持久化清单（NVSET/NVGET；16 位单元，浮点占两格）：
--   轴：64/65 定位速度、69/70 点动速度（float）；称重：130 站号、131 轮询、132/133 比例系数；
--   泵：204 转速、211/212 圈数、221/222 ml/圈、225/226 设定容积
local NVRAM_REGS = { 64, 65, 69, 70, 71, 72, 73, 130, 131, 132, 133, 204, 211, 212, 214, 221, 222, 225, 226 }
-- ★4x225（设定容积）输入缩放：现场触摸屏该元件把输入放大 10 倍（8.5→85），控制器侧 ÷10 还原；
--   屏侧元件「小数位数」修好后改为 1 即可（单点常量，2026-09-28 现场决定）
local VOL_IN_SCALE = 0.1
local nvr_last   = {}                            -- 上次已保存值（变化即存）

local ROBOT_IGNORE0_MS   = 1000     -- ★忽略「开」之后该窗口内的 0 写入（汇川屏按钮松开时会补写 0，勿当停止）

-- 4x151~4x155 成对命令（**一个寄存器管一对，布尔状态式**：值变化时下发；1=前一列，0=后一列）
--   实际状态请从读区 4x143~4x150 看（motor_on_state/running/operating_mode…），此区只作命令开关
local REG_CMD_TOGGLE = {
    { reg = 151, on = "xCore::SocketInterface::Enable", off = "xCore::SocketInterface::Disable", noreply = 1 },
    { reg = 152, on = "start",         off = "stop" },               -- 程序启动/停止
    { reg = 153, on = "motor_on",      off = "motor_off" },          -- 电机上电/下电
    { reg = 154, on = "switch_mode:auto", off = "switch_mode:manual" }, -- 1=自动 / 0=手动
    { reg = 155, on = "open_drag",     off = "close_drag" },         -- 拖动 打开/关闭
}

-- 4x156~4x160 点动命令（**一命令一寄存器布尔**：写 1 触发一次，0→1 沿，重复先写 0）
local REG_CMD_PULSE = {
    { reg = 156, cmd = "clear_alarm" },                              -- 清除伺服报警
    { reg = 157, cmd = "pp_to_main" },                               -- 程序指针到main（结果标志 → 4x158）
    { reg = 158, cmd = "list_prog",    text = 1 },                   -- 获取工程列表（文本→4x176/177~200）
    { reg = 159, cmd = "current_prog", text = 1 },                   -- 获取当前工程（同上）
    { reg = 160, cmd = "load_prog",    arg = 1 },                    -- 切换工程（名字取 4x168~4x175）
}

-- Modbus-TCP 从站（触摸屏）
local MB_WIRE       = false        -- ★P3b（2026-10-01）：Lua 502 线上处理退役（固件 ModbusServer 独占 502）；
                                   --   置 true 可回退兼容（与固件 enable 文件互斥，二者只能开一个）
local MB_CH         = 13           -- Modbus 从站监听端口句柄（MB_WIRE=true 时使用）
local MB_PORT       = 502          -- 触摸屏轮询端口（特权端口，需 CAP_NET_BIND_SERVICE）
local MB_STATION    = 1            -- 从站站号（原 ZMC ADDRESS=1）
local MB_REGN       = 256          -- 4x 寄存器数量（触摸屏/旧区 0~255）
local MB_ROBOT_BASE = 1000         -- ★机器人 Modbus 区起始（planA/19；触摸屏+机器人同连 502，2026-09-28）
-- ★P3b（2026-10-01）：用户寄存器区移交固件组态（config/modbus.json，D12/插件编辑）；
--   脚本侧 reg_* 改为与固件库存按区同步（kx_mbsync.lua）；reg_dirty = 本拍被脚本改写的字
local reg_dirty   = {}
local MB_ROBOT_N    = 200          -- 机器人区 1000~1199（4x 保持寄存器；线圈/离散未启用）

-- ======================== 运行状态 ========================
-- 总线
local bus_ok        = 0
local rescan_flag   = 0
-- 4321
local svr_open      = 0
local svr_logt      = 1e18    -- 首次立即尝试 OPEN（TICKS 为倒数，elapsed=1e18-now>5000）
local conn_old      = 0
local cmd_buf       = ""
local last_rx       = 0
local beat_last     = 0
-- 使能总开关（HMI bit2 用；与旧程序 wdog 同义）
local wdog          = 0

-- ★统一「请求轴0使能」入口（4x60 bit0 命令字 / 4x75 使能按钮共用；2026-09-29）
--   保护：安全联锁（机器人急停/故障未复位）或总线未就绪时明确拒绝并日志，不执行 ENABLE
function request_axis_enable(src)
    if robot_axis and robot_axis.safe == 1 then
        print("[mb] HMI命令:轴使能被拒（安全联锁：机器人急停/故障未复位）")
        return
    end
    if bus_ok ~= 1 then
        print("[mb] HMI命令:轴使能被拒（总线未就绪）")
        return
    end
    BASE(0)
    pcall(ENABLE)
    wdog = 1
    print("[mb] HMI命令:轴0使能（" .. (src or "HMI") .. "）")
end
-- 写区：机器人命令（4x151~4x165，一命令一寄存器）边沿记忆 + 待发队列（逐拍下发，避免发送槽覆盖）
local mb_rboldcmd   = {}
local mb_rbpend     = {}
-- ★命令写队列（2026-09-27）：按 Modbus 写入**顺序**保存每个命令寄存器的写入值并逐个派发——
-- 同一循环里收到成对写入（如屏瞬时按钮 1→0）时，中间值不再被最终值覆盖（“1 丢失”修复）
local mb_wq         = {}
local mb_laston     = {}      -- 各命令寄存器最近一次「开」派发时间（回弹 0 去抖用）

-- 命令寄存器 → { e=表项, toggle=是否成对命令 }（写队列派发用）
local REG_CMD_BYREG = {}
for _, e in ipairs(REG_CMD_TOGGLE) do REG_CMD_BYREG[e.reg] = { e = e, toggle = true  } end
for _, e in ipairs(REG_CMD_PULSE)  do REG_CMD_BYREG[e.reg] = { e = e, toggle = false } end
-- 数值命令运动监测
local mvmode        = 0
local mvt0          = 0
local mvpos         = 0
local mvwait        = 0

-- ======================== 工具函数 ========================
-- TICKS(): 控制器毫秒倒数计时（两次差值=经过毫秒；与 BASIC TICKS 同义）
local function now() return TICKS() end
local function elapsed(t0) return t0 - now() end

-- 统一派发：成对命令（151~155）与点动命令（156~160）共用；由写队列与值变化兜底两条路径调用
local function cmd_apply_write(e, v, is_toggle)
    if is_toggle then
        if v == 1 then
            -- ★启动时序在途时忽略重复启动（“按了没反应再按一下”防双序列，2026-09-27）
            if e.reg == 152 and robot_hb ~= nil and (robot_hb.start_stage or 0) ~= 0 then
                mb_rboldcmd[e.reg] = v
                print("[mb] 4x152 启动时序在途，忽略重复启动（防双序列）")
                return
            end
            mb_laston[e.reg] = now()
            mb_rbpend[#mb_rbpend + 1] = { e, 1 }
        elseif elapsed(mb_laston[e.reg] or 0) >= ROBOT_IGNORE0_MS then
            mb_rbpend[#mb_rbpend + 1] = { e, 0 }
        else
            print("[mb] 忽略 4x" .. e.reg .. " 的回弹 0（距上次开 <" .. ROBOT_IGNORE0_MS .. "ms，疑似按钮松开）")
        end
    else
        if v ~= 0 then mb_rbpend[#mb_rbpend + 1] = { e, 1 } end
    end
    mb_rboldcmd[e.reg] = v
end


-- TOSTR(x,width,dec) 等价：右对齐空格补位
local function fmt(v, w, d)
    local s = string.format("%." .. tostring(d) .. "f", v or 0)
    if w and #s < w then s = string.rep(" ", w - #s) .. s end
    return s
end

-- 十六进制（两位大写，调试用）
local function hex(v) return string.format("%02X", (v or 0) & 0xFF) end

-- 取逗号分隔字段（idx=0 表示整串；与旧 get_num 的字段语义一致）
local function field_str(cmd, idx)
    if idx == 0 then return cmd end
    local pos = 1
    for _ = 1, idx do
        local c = string.find(cmd, ",", pos, true)
        if not c then return "" end
        pos = c + 1
    end
    local c2 = string.find(cmd, ",", pos, true)
    if c2 then return string.sub(cmd, pos, c2 - 1) end
    return string.sub(cmd, pos)
end

-- 字段转数值（非法=0，与旧 VAL 的宽容行为一致）
-- idx=0 时取**整串前缀到第一个逗号**（旧 get_num(0) 用 VAL 解析，遇逗号即停）
local function field_num(cmd, idx)
    local s
    if idx == 0 then
        s = string.match(cmd, "^[^,]*") or ""
    else
        s = field_str(cmd, idx)
    end
    local v = tonumber(s)
    if v == nil then return 0 end
    return v
end

-- 取端口收到的原始字节 -> Lua 字符串
local function recv_str(ch, max, idx)
    local buf = {}
    local n
    if idx ~= nil then
        n = PORT_GET(ch, buf, max, idx)
    else
        n = PORT_GET(ch, buf, max)          -- 单客户端旧路径：不传 idx（避免 nil 进 C++ 参数检查）
    end
    if n <= 0 then return "" end
    local t = {}
    for i = 1, n do t[i] = string.char(buf[i] & 0xFF) end
    return table.concat(t)
end

-- Modbus CRC16（标准：初值 0xFFFF / 多项式 0xA001）
local function crc16(s)
    local crc = 0xFFFF
    for i = 1, #s do
        crc = crc ~ string.byte(s, i)
        for _ = 1, 8 do
            if (crc & 1) ~= 0 then
                crc = (crc >> 1) ~ 0xA001
            else
                crc = crc >> 1
            end
        end
    end
    return crc & 0xFFFF
end

-- RTU 组帧：data + CRC(低字节在前)
local function rtu_frame(body)
    local c = crc16(body)
    return body .. string.char(c & 0xFF, (c >> 8) & 0xFF)
end

-- ======================== 4x 寄存器（Modbus 从站存储） ========================
local regs = {}
for i = 0, MB_REGN - 1 do regs[i] = 0 end

-- ★机器人区线圈/离散输入（与机器人 4x 区同号段 1000~1099，各 100 位；planA/19，2026-09-28）
local MB_BIT_N = 100
local coils, dis = {}, {}
for i = MB_ROBOT_BASE, MB_ROBOT_BASE + MB_BIT_N - 1 do coils[i] = 0; dis[i] = 0 end

local function coil_get(a) return coils[a] or 0 end
local function coil_set(a, v)
    if a >= MB_ROBOT_BASE and a < MB_ROBOT_BASE + MB_BIT_N then coils[a] = v & 1 end
end
local function dis_get(a) return dis[a] or 0 end
local function dis_set(a, v)
    if a >= MB_ROBOT_BASE and a < MB_ROBOT_BASE + MB_BIT_N then dis[a] = v & 1 end
end
local function mb_bit_ok(a, count)
    return a >= MB_ROBOT_BASE and a + count <= MB_ROBOT_BASE + MB_BIT_N
end

-- 地址是否在对外可读写的 4x 范围内（触摸屏区 0~255 或机器人区 1000~1199）
local function mb_addr_ok(a, count)
    if a >= 0 and a + count <= MB_REGN then return true end
    if a >= MB_ROBOT_BASE and a + count <= MB_ROBOT_BASE + MB_ROBOT_N then return true end
    return false
end

-- ★v0.12.2（按名访问层，planA/20 §4/§5）：业务寄存器对**已组态地址**经 MB_READ/MB_WRITE 按名访问
--   （名字由固件组态经 MB_LIST() 自动装载）；未组态地址回退旧缓存 + kx_mbsync 同步（安全网）。
local mb_name      = {}        -- addr → name（仅条目起始地址）
local mb_name_span = {}        -- addr → 1/2（类型字数）
local mb_name_i16  = {}        -- addr → true（i16 条目：原始字回写需还原符号）
local mb_read_cache_w = {}     -- ★按拍读缓存（字）：每拍由 mbnames_tick 清空，降低按名调用开销
local mb_read_cache_f = {}     -- ★按拍读缓存（浮点 2 字条目）
local function mbnames_tick()
    if next(mb_read_cache_w) ~= nil then mb_read_cache_w = {} end
    if next(mb_read_cache_f) ~= nil then mb_read_cache_f = {} end
end
local function mbnames_refresh()
    if not MB_LIST then return end
    local ok, txt = pcall(MB_LIST)
    if not ok or type(txt) ~= "string" then return end
    local n_name, n_span, n_i16 = {}, {}, {}
    for line in txt:gmatch("[^\n]+") do
        local name, addr, typ = line:match("^([^,]+),(4x%d+),([%w]+)")
        if name and addr then
            local a = tonumber(addr:sub(3))
            if a then
                n_name[a] = name
                n_span[a] = (typ == "f32" or typ == "f32hi" or typ == "u32") and 2 or 1
                if typ == "i16" then n_i16[a] = true end
            end
        end
    end
    mb_name, mb_name_span, mb_name_i16 = n_name, n_span, n_i16
    mbnames_tick()                                          -- 名字表变更 → 读缓存失效
end

local function reg_get(a)
    local n = mb_name[a]
    if n and (mb_name_span[a] or 1) == 1 and MB_READ then
        local cv = mb_read_cache_w[a]                    -- 按拍缓存：本拍已读过则不再跨语言调用
        if cv ~= nil then return cv end
        local ok, v = pcall(MB_READ, n)
        if ok and type(v) == "number" then
            cv = math.floor(v) & 0xFFFF
            mb_read_cache_w[a] = cv
            return cv
        end
    end
    return regs[a] or 0
end
local function reg_set(a, v)
    v = math.floor(v or 0) & 0xFFFF
    local n = mb_name[a]
    if n and (mb_name_span[a] or 1) == 1 and MB_WRITE then
        local w = v
        if mb_name_i16[a] and v >= 0x8000 then w = v - 0x10000 end   -- ★i16：原始字还原符号（MB_WRITE 按类型编码）
        if pcall(MB_WRITE, n, w) then
            mb_read_cache_w[a] = v                      -- 写后同拍读一致（缓存原始字）
            return
        end
    end
    if (a >= 0 and a < MB_REGN) or (a >= MB_ROBOT_BASE and a < MB_ROBOT_BASE + MB_ROBOT_N) then
        if regs[a] ~= v then reg_dirty[a] = true end    -- ★P3b：脏字由 kx_mbsync 回推固件（未组态回退）
        regs[a] = v
    end
end
-- 32 位浮点字序（触摸屏对象的字序设置必须与此一致，否则显示 #### 或极大值）：
--   0 = 高字在前（"1234"）  1 = 低字在前（"4321"）
-- 实测定案（2026-09-26，汇川 IT7000）：屏端为低字在前 → 默认 1（与旧 ZMC MODBUS_IEEE 一致）
local MB_FLOAT_ORDER = 1
-- 调试用：非 nil 时 4x10/11 固定显示该值（确认屏端解码方式用，用完必须改回 nil）
local MB_TEST_POS = nil
local function reg_set_f(a, v)
    local n = mb_name[a]
    if n and (mb_name_span[a] or 1) == 2 and MB_WRITE then
        if pcall(MB_WRITE, n, v) then
            mb_read_cache_f[a] = v or 0
            return
        end
    end
    local hi, lo = string.unpack(">I2I2", string.pack(">f", v or 0))
    if MB_FLOAT_ORDER == 0 then
        reg_set(a, hi)
        reg_set(a + 1, lo)
    else
        reg_set(a, lo)
        reg_set(a + 1, hi)
    end
end
local function reg_get_f(a)
    local n = mb_name[a]
    if n and (mb_name_span[a] or 1) == 2 and MB_READ then
        local cv = mb_read_cache_f[a]
        if cv ~= nil then return cv end
        local ok, v = pcall(MB_READ, n)
        if ok and type(v) == "number" then
            mb_read_cache_f[a] = v
            return v
        end
    end
    local w1 = reg_get(a)
    local w2 = reg_get(a + 1)
    if MB_FLOAT_ORDER == 0 then
        return string.unpack(">f", string.pack(">I2I2", w1, w2))
    end
    return string.unpack(">f", string.pack(">I2I2", w2, w1))
end

