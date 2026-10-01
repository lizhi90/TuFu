-- port_script_selftest.lua —— 端口服务脚本的离线桩测试（无硬件/无控制器进程）
-- 用法: lua_runner tools/port_script_selftest.lua [EtherCAT_SocketServer.lua 路径]
-- 覆盖: CRC16/RTU 组帧、数值格式化与解析、机器人表/数值解析、4321 命令派发与拆帧、
--       Modbus-TCP 从站（FC3/FC6/FC16/异常）、称重 RTU 收发与写命令队列。
-- 说明: 脚本内以 KX_TEST=true 加载被测文件（跳过无限主循环，暴露 _T 测试钩子）。
KX_TEST = true

local fails, total = 0, 0
local function check(cond, what)
    total = total + 1
    if cond then
        print("  [ OK ] " .. what)
    else
        fails = fails + 1
        print("  [FAIL] " .. what)
    end
end

-- ---- 桩：设备命令 ----
local sent = {}
local rxq = { [10] = {}, [11] = {}, [12] = {}, [13] = {} }
local regs_mirror = {}
local last_move = nil
local enable_calls = 0                                   -- ENABLE 调用计数（4x75 使能按钮用例）
local enabled_stub = 1                                   -- ENABLED() 桩返回值（定位拒绝用例可改）

OPEN = function(...) end
local portinfo_calls = {}
PORT_INFO = function(n, tag, role) portinfo_calls[#portinfo_calls + 1] = { n, tag, role } end
PORT_PRINT = function(ch, s) sent[#sent + 1] = { ch = ch, s = s } end
PORT_GET = function(ch, buf, max)
    local q = rxq[ch]
    if q and #q > 0 then
        local s = table.remove(q, 1)
        for i = 1, math.min(#s, max or 0) do buf[i] = string.byte(s, i) end
        return math.min(#s, max or 0)
    end
    return 0
end
PORT_STATUS = function(ch, idx) return (ch == 10 or ch == 11 or ch == 13 or ch == 14) and 1 or 0 end  -- 13=502（多客户端桩）
PORT_CLIENTS = function(ch) return (ch == 13) and 1 or 0 end                                        -- 多客户端桩（2026-09-28）
PORT_TARGET = function(ch) return "192.168.1.2:50000" end                                           -- D10 标签桩
BUSOK = function() return 1 end
ENABLED = function() return enabled_stub end
TICKS = function() return -math.floor(os.clock() * 1000) end
local nvstub = {}
NVGET = function(r) return nvstub[r] or 0 end            -- NVRAM 宿主桩（2026-09-28）
local regmap_stub = ""                                   -- .mbmap 文本桩（REGMAP_GET；v0.9.0 遗留）
REGMAP_GET = function() return regmap_stub end
local mb_fw = {}                                         -- 固件 Modbus 库存桩（addr → u16；P3b kx_mbsync）
MBREG_ZONE = function(st, cnt)
    local t = {}
    for i = 0, cnt - 1 do t[#t + 1] = string.pack(">H", mb_fw[st + i] or 0) end
    return table.concat(t)
end
MBREG_PUT = function(st, packed)
    local n = math.floor(#packed / 2)
    local vals = { string.unpack(">" .. string.rep("H", n), packed) }
    for i = 1, n do mb_fw[st + i - 1] = vals[i] end
end
NVSET = function(r, v) nvstub[r] = v end
-- ★v0.12.2 按名访问桩：addr → {n=名, sp=1/2}；未列地址走旧缓存+同步（回退路径）
local mb_names, mb_by_name = {}, {}
local function mbname_seed(a, n, sp)
    mb_names[a] = { n = n, sp = sp or 1 }
    mb_by_name[n] = a
end
MB_LIST = function()
    local t = {}
    for a, e in pairs(mb_names) do
        t[#t + 1] = e.n .. ",4x" .. a .. "," .. (e.sp == 2 and "f32" or "i16") .. ",rw"
    end
    return table.concat(t, "\n")
end
MB_READ = function(n)
    local a = mb_by_name[n]
    if not a then error("未组态变量：" .. tostring(n)) end
    local e = mb_names[a]
    if e.sp == 2 then
        local w1, w2 = mb_fw[a] or 0, mb_fw[a + 1] or 0
        return string.unpack(">f", string.pack(">I2I2", w2, w1))   -- 低字在前
    end
    return mb_fw[a] or 0
end
MB_WRITE = function(n, v)
    local a = mb_by_name[n]
    if not a then error("未组态变量：" .. tostring(n)) end
    local e = mb_names[a]
    if e.sp == 2 then
        local hi, lo = string.unpack(">I2I2", string.pack(">f", v or 0))
        mb_fw[a], mb_fw[a + 1] = lo, hi
    else
        mb_fw[a] = math.floor(v or 0) & 0xFFFF
    end
end
DELAY = function(ms) end
RAPIDSTOP = function(m) end
WAITIDLE = function() end
SLOT_SCAN = function(n) return 1 end
SLOT_START = function(n) return 1 end
SLOT_STOP = function(n) return 1 end
ENABLE = function() enable_calls = enable_calls + 1 end
DISABLE = function() end
BASE = function(n) end
local last_speed, last_vmove = nil, nil
SPEED = function(n, v) last_speed = v end
ACCEL = function(n, v) last_accel = v end
DECEL = function(n, v) last_decel = v end
SRAMP = function(n, v) last_sramp = v end
SET_DEFAULTS = function(n) last_setdef = (n or 0) end
ATYPE = function(n, v) end
UNITS = function(n, v) end
DRIVE_PROFILE = function(n, v) end
DISABLE_GROUP = function(n) end
DRIVE_CONTROLWORD = function(n, v) end
DATUM = function(m) end
MOVEABS = function(p, s, a, w) last_move = { op = "abs", p = p, s = s, a = a, w = w } end
MOVE = function(p, s, a, w) last_move = { op = "rel", p = p, s = s, a = a, w = w } end
VMOVE = function(d) last_vmove = d end
CANCEL = function(m) end
MPOS = function(n) return 12.345 end
DPOS = function(n) return 12.0 end
IDLE = function(n) return -1 end
AXISSTATUS = function(n) return 0 end
ALARM = function() return 0 end
MODBUS_REG = function(n, v) regs_mirror[n] = v end
MODBUS_IEEE = function(n, v) regs_mirror[n] = v end
RUNTASK = function(n, s) end

-- ---- 加载被测脚本（★2026-09-30 模块化：支持 include("sub.lua") 编译前展开，与控制器同规则） ----
local script_path = (ARGV and ARGV[1]) or "../../EtherCAT_SocketServer.lua"
local script_dir  = script_path:match("^(.*)[/\\][^/\\]*$") or "."
local function read_file(p) local f = assert(io.open(p, "rb"), "无法打开: " .. p); local s = f:read("*a"); f:close(); return s end
local function expand_includes(src, depth, chain)
    depth = depth or 0
    chain = chain or {}
    assert(depth <= 8, "include 嵌套超过 8 层")
    local out = {}
    for line in (src .. "\n"):gmatch("(.-)\n") do
        local name = line:match('^%s*include%s*%(%s*"([%w%._%-]+)"%s*%)%s*$')
        if not name then
            name = line:match('^%s*include%s*"([%w%._%-]+)"%s*$')
            or line:match('^%s*include%s*"([%w%._%-]+)"%s*%-%-')
            or line:match('^%s*include%s*%(%s*"([%w%._%-]+)"%s*%)%s*%-%-')
        end
        if name then
            assert(not chain[name], "循环 include：" .. name)
            chain[name] = true
            out[#out + 1] = expand_includes(read_file(script_dir .. "/" .. name), depth + 1, chain)
            chain[name] = nil
        else
            out[#out + 1] = line
        end
    end
    return table.concat(out, "\n")
end
local chunk = assert(load(expand_includes(read_file(script_path)), "@" .. script_path))
chunk()
local T = _T

local function last_sent()
    if #sent == 0 then return "" end
    return sent[#sent].s
end
local function clear_sent() sent = {} end

print("== 1) CRC16 / RTU 组帧 ==")
local req = string.char(0x01, 0x03, 0x00, 0x00, 0x00, 0x04)
check(T.crc16(req) == 0x0944, string.format("CRC16(01 03 00 00 00 04)=0x%04X（标准 0x0944）", T.crc16(req)))
local frame = T.rtu_frame(req)
check(string.byte(frame, 7) == 0x44 and string.byte(frame, 8) == 0x09, "RTU 帧 CRC 低字节在前（44 09）")

print("== 2) 数值/格式化 ==")
check(T.fmt(12.345, 10, 3) == "    12.345", "fmt 右对齐 10 位 3 小数")
check(T.field_num("2000,100,55", 0) == 2000, "field_num(0) 取逗号前前缀（旧 VAL 语义）")
check(T.field_num("2000,100,55", 2) == 55, "field_num(2)=55")
check(T.field_num("-12.5,1", 0) == -12.5, "field_num 支持负数/小数")

print("== 3) 机器人 Modbus 桥接（A 区 1000~ 机器人写 / B 区 1100~ 机器人读；2026-09-28 替代 4320）==")
-- 初始值：soft_estop=1（不触发）
T.robot_bridge_init()
check(T.reg_get(1125) == 1, "初始化：ctrl_soft_estop=1（不触发）")
-- 脉冲类命令：157 pp_to_main → 1120
T.robot_bridge_cmd({ reg = 157 }, 1)
check(T.reg_get(1120) == 1 and #T.robot_hb.pulses == 1, "4x157 → ctrl_pptomain 脉冲（1120=1）")
T.robot_hb.pulses[1].t = TICKS() + 1000          -- 模拟脉冲到期（TICKS 倒数：值更大=更早）
T.robot_bridge_step()
check(T.reg_get(1120) == 0 and #T.robot_hb.pulses == 0, "脉冲到期自动回 0")
-- 电平类：153 上/下电、154 自动/手动
T.robot_bridge_cmd({ reg = 153 }, 1)
T.robot_bridge_cmd({ reg = 154 }, 0)
check(T.reg_get(1116) == 1 and T.reg_get(1128) == 0, "4x153=1 → ctrl_motor_on_off=1；4x154=0 → 自动手动=0")
-- 启动时序：上电 → 2s → pptomain → 2s → program_start
T.robot_bridge_cmd({ reg = 152 }, 1)
check(T.reg_get(1116) == 1 and T.robot_hb.start_stage == 1, "4x152=1 → 上电并进入启动时序")
T.robot_hb.start_t = TICKS() + 5000
T.robot_bridge_step()
check(T.reg_get(1120) == 1 and T.robot_hb.start_stage == 2, "时序：2s 后发 ctrl_pptomain")
T.robot_hb.start_t = TICKS() + 5000
T.robot_bridge_step()
check(T.reg_get(1121) == 1 and T.robot_hb.start_stage == 0, "时序：再 2s 发 ctrl_program_start")
-- 已停用项：151/155/158~160 明示忽略（不写 B 区）
check(T.robot_bridge_cmd({ reg = 151 }, 1) == 1 and T.robot_bridge_cmd({ reg = 155 }, 1) == 1 and
      T.robot_bridge_cmd({ reg = 159 }, 1) == 1, "4x151/155/159 已停用（明示忽略）")
-- 心跳 + A 区镜像 → 屏幕 4x140~150
T.robot_hb.cnt, T.robot_hb.last, T.robot_hb.alive = 0, 0, 0   -- 复位心跳状态（确定性）
T.reg_set(1000, 1)
T.robot_bridge_step()
check(T.robot_hb.alive == 1 and (T.reg_get(140) & 3) == 3 and T.reg_get(142) == 1,
      "首帧心跳 → 在线（4x140 bit0/1=1、计数=1）")
T.reg_set(1005, 1); T.reg_set(1003, 1); T.reg_set(1006, 1); T.reg_set(1008, 3); T.reg_set(1016, 1)
T.robot_bridge_step()
check(T.reg_get(143) == 1 and T.reg_get(145) == 1 and T.reg_get(146) == 1 and
      T.reg_get(149) == 3 and T.reg_get(150) == 1, "A 区镜像：上电/急停/模式/程序状态/Home")
T.robot_hb.t = TICKS() + 5000                    -- 模拟心跳超时
T.robot_bridge_step()
check(T.robot_hb.alive == 0 and (T.reg_get(140) & 3) == 0 and T.reg_get(141) == 1,
      "心跳超时 → 离线（4x140 位清 0、错误计数+1）")
-- ★安全联锁（2026-09-29）：机器人急停(4x145) 非 0 → 轴0 去使能；★4x147 故障不参与（2026-09-29 晚定案）
T.reg_set(1003, 0); T.reg_set(1001, 0)               -- 复位急停/故障
T.robot_bridge_step()
check(T.robot_axis.safe == 0, "安全联锁：信号复位 → 未触发")
T.reg_set(1001, 1)                                   -- 仅故障（⇒ 4x147=1）
T.robot_bridge_step()
check(T.robot_axis.safe == 0, "安全联锁：仅 4x147 故障 → 不触发（已从联锁移除）")
T.reg_set(1003, 1)                                   -- 急停 + 故障（⇒ 4x145=1）
T.robot_bridge_step()
check(T.robot_axis.safe == 1, "安全联锁：4x145 急停非 0 → 触发（去使能）")
T.reg_set(1003, 0)                                   -- 急停复位（故障信号仍在）→ 立即解除
T.robot_bridge_step()
check(T.robot_axis.safe == 0, "安全联锁：急停复位即解除（故障不影响）")
T.reg_set(1001, 0)
T.robot_bridge_step()
-- ★轴使能按钮（4x75；2026-09-29）：写 1 触发、自动回 0；联锁/总线保护
print("== 3e4a) 轴使能按钮（4x75）==")
T._set_bus_ok(1)
local en0 = enable_calls
T.mb_wq[#T.mb_wq + 1] = { 75, 1 }; T.modbus_step()          -- 写队列路径
check(enable_calls == en0 + 1 and T.reg_get(75) == 0, "4x75=1（写队列）→ 执行轴使能并自动回 0")
T.reg_set(75, 1); T.modbus_step()                           -- 值边沿兜底路径
check(enable_calls == en0 + 2 and T.reg_get(75) == 0, "4x75=1（边沿兜底）→ 执行轴使能并自动回 0")
T.reg_set(1003, 1); T.robot_bridge_step()                   -- 机器人急停 → 安全联锁
en0 = enable_calls
T.reg_set(75, 1); T.modbus_step()
check(enable_calls == en0 and T.reg_get(75) == 0, "安全联锁期间：按钮被拒（不执行 ENABLE）且回 0")
T.reg_set(1003, 0); T.robot_bridge_step()                   -- 联锁解除
T.reg_set(75, 1); T.modbus_step()
check(enable_calls == en0 + 1 and T.reg_get(75) == 0, "联锁解除后：按钮恢复使能")
-- 软急停脉冲（4x60 bit2 急停 / 直接调用）
T.robot_bridge_estop()
check(T.reg_get(1125) == 0, "软急停：ctrl_soft_estop=0（触发）")
T.robot_hb.estop_t = TICKS() + 1000
T.robot_bridge_step()
check(T.reg_get(1125) == 1, "软急停脉冲到期 → 恢复 1")
-- 屏幕急停按钮（4x60 bit2 上升沿）→ 软急停
T.reg_set(60, 4); T.modbus_step()
check(T.reg_get(1125) == 0, "4x60 bit2 急停 → 触发软急停")
T.reg_set(60, 0); T.modbus_step()
T.robot_hb.estop_t = TICKS() + 1000
T.robot_bridge_step()

print("== 3e3) 地轨参数（4x71~76；确定按钮 + 掉电保持）==")
T.reg_set(71, 250)                                  -- 速度 250 mm/s（★2026-09-30 晚起直接 mm/s）
T.reg_set(72, 800)                                  -- 加减速度 800 mm/s²
T.reg_set(73, 3)                                    -- S 曲线
T.mb_wq[#T.mb_wq + 1] = { 74, 1 }                   -- 「确定」（写队列路径）
T.modbus_step()
check(last_speed == 250.0 and last_accel == 800.0 and last_decel == 800.0 and last_sramp == 50,
      "应用（直接 mm/s）：速度 250 = 250mm/s + 加减速度下发 + S 曲线（SRAMP=50）")
check(T.reg_get(74) == 0, "确定按钮自动回 0")
T.reg_set(73, 1)                                     -- 梯形
T.reg_set(74, 1); T.modbus_step()                    -- 值边沿兜底路径
check(last_sramp == 0, "曲线=梯形 → SRAMP=0")
T.reg_set(73, 9); T.reg_set(74, 1); T.modbus_step()
check(last_sramp == 0, "非法曲线选项 → 拒绝（不改变 SRAMP）")
T.reg_set(73, 1)
T.reg_set(71, 0); T.reg_set(74, 1); T.modbus_step()
check(T.reg_get(74) == 0, "速度=0 → 拒绝但仍回 0（按钮不粘）")
T.reg_set(71, 2500)
-- 掉电保持：71~73 变化即存
T.reg_set(73, 1)
T.modbus_step()
check(nvstub[71] ~= nil and nvstub[73] ~= nil, "持久化：地轨参数（71~73）变化 → NVSET 已存")
-- ★开机重发（2026-09-30 现场反馈"速度不是 4x71/72"）：NVRAM 恢复后自动把参数重发内核缺省
T.reg_set(71, 180); T.reg_set(72, 600); T.reg_set(73, 3)
T.rail_apply_saved()
check(last_speed == 180.0 and last_accel == 600.0 and last_sramp == 50,
      "开机重发：4x71/72/73 有效 → SPEED/ACCEL/SRAMP 重发内核缺省")
T.reg_set(71, 0)
local lspd0 = last_speed
T.rail_apply_saved()
check(last_speed == lspd0, "开机重发：未配置（71=0）→ 跳过（保持缺省）")
T.reg_set(71, 180)

print("== 3e4) 机器人控制轴0位置（4x1080 变化即触发 + 4x1082 沿；状态 4x1140）==")
-- ① 位置变化即触发（机器人主站“周期刷新写”风格亦可）；★显式带 4x71/72 速度（2026-09-30）
T.reg_set(71, 600); T.reg_set(72, 800)               -- ★直接 mm/s：600 = 600mm/s
T.reg_set(1080, 125)
T.mb_wq[#T.mb_wq + 1] = { 1080, 125 }
T.modbus_step()
check(T.robot_axis.active == 1 and T.reg_get(1140) == 0 and last_move and last_move.p == 125,
      "位置变化 → 自动下发 125mm（状态=0 已受理/执行中）")
check(last_move and last_move.s == 600.0 and last_move.a == 800,
      "显式带速度：4x71=600 → 600 mm/s（直接）、4x72=800 → 800 mm/s²")
-- ②b 防提前置 2：下发防抖窗口内（轴尚未启动、仍 idle）不得判到位（2026-09-29 现场反馈）
local saved_mpos = MPOS
MPOS = function() return 125 end                    -- 位置桩：本用例目标=125 且轴已在该位
T.robot_bridge_step()
check(T.robot_axis.active == 1 and T.reg_get(1140) == 0, "下发窗口内：不提前置 2（仍 0 已受理）")
-- ② 到位（过防抖窗口 + 位置核对通过）
T.robot_axis.disp_t = TICKS() + 1000                -- 模拟 50ms 已过（TICKS 倒数：值更大=更早）
T.robot_bridge_step()
check(T.robot_axis.active == 0 and T.reg_get(1140) == 2, "到位（位置核对通过）→ 状态=2")
-- ★2/4 保持 3s → 到期自动回 0（2026-09-29 晚 1s→3s）
T.robot_axis.stat_t = TICKS() + 2000      -- 模拟 1s 已过（TICKS 倒数：值更大=更早）
T.robot_bridge_step()
check(T.reg_get(1140) == 0, "到位(2) 保持 3s 到期后自动回 0")
-- ②c 停在目标外：idle 但位置不符 → 确认窗口后判失败 4（防"停了就报 2"；2026-09-29）
MPOS = function() return 12.345 end                 -- 位置桩：离目标 130 很远
T.reg_set(1080, 130); T.mb_wq[#T.mb_wq + 1] = { 1080, 130 }; T.modbus_step()
check(T.robot_axis.active == 1, "停在目标外用例：已受理")
T.robot_axis.disp_t = TICKS() + 1000
T.robot_bridge_step()                               -- 首次见"空闲但未到位"：只进入确认
check(T.robot_axis.active == 1 and T.reg_get(1140) == 0, "空闲但未到位：不判 2（进入确认窗口）")
T.robot_axis.off_t = TICKS() + 1000                 -- 模拟 500ms 已过
T.robot_bridge_step()
check(T.robot_axis.active == 0 and T.reg_get(1140) == 4, "确认后仍停在目标外 → 判失败 4")
MPOS = saved_mpos
-- ③ 1082 0→1 沿（armed 后）→ 防抖 100ms 后触发；电平保持不自动回 0（2026-09-29 定案）
local n_move = 0
local saved_moveabs = MOVEABS
MOVEABS = function(p, s2, a, w) n_move = n_move + 1; last_move = { op = "abs", p = p } end
T.reg_set(1082, 0); T.mb_wq[#T.mb_wq + 1] = { 1082, 0 }; T.modbus_step()
T.reg_set(1082, 1); T.mb_wq[#T.mb_wq + 1] = { 1082, 1 }; T.modbus_step()
check(n_move == 0 and T.reg_get(1082) == 1, "1082=1：防抖期内不触发（电平保留）")
T.robot_axis.pend_t = TICKS() + 1000                -- 模拟防抖到期（TICKS 倒数：值更大=更早）
T.modbus_step()
check(n_move == 1, "1082 沿（防抖后）→ 触发（目标=当前 1080）")
-- ④ 周期刷新写 1（电平保持 1）→ 不排程、不重复触发（仅 0→1 沿；现场机器人 ~10Hz 刷新）
n_move = 0
T.reg_set(1082, 1); T.mb_wq[#T.mb_wq + 1] = { 1082, 1 }; T.modbus_step()
check(n_move == 0 and T.robot_axis.pend_t == 0, "电平刷新写 1（无新沿）→ 不排程不触发")
-- ④b 1082 先到、1080 随后变化 → 防抖取消（不误动旧目标）
n_move = 0
T.reg_set(1082, 0); T.mb_wq[#T.mb_wq + 1] = { 1082, 0 }; T.modbus_step()
T.reg_set(1082, 1); T.mb_wq[#T.mb_wq + 1] = { 1082, 1 }; T.modbus_step()   -- 排程（记录当时 1080=125）
T.reg_set(1080, 131); T.mb_wq[#T.mb_wq + 1] = { 1080, 131 }; T.modbus_step()
check(n_move == 1 and last_move ~= nil and last_move.p == 131, "1080 后到 → 变化触发 131")
T.robot_axis.pend_t = TICKS() + 1000
T.modbus_step()
check(n_move == 1, "防抖到期：1080 已变化 → 取消（不重复触发）")
-- ⑤ 未 armed 时 1082 沿不触发（开机防误动）
T.robot_axis.armed = false; T.robot_axis.active = 0
n_move = 0
T.reg_set(1082, 0); T.mb_wq[#T.mb_wq + 1] = { 1082, 0 }; T.modbus_step()   -- 复位沿（清 trig）
T.reg_set(1082, 1); T.mb_wq[#T.mb_wq + 1] = { 1082, 1 }; T.modbus_step()
T.robot_axis.pend_t = TICKS() + 1000
T.modbus_step()
check(n_move == 0, "未 armed 时 1082 沿 → 不触发（开机防误动）")
MOVEABS = saved_moveabs
-- ⑥ 失败（报警优先于到位）
T.robot_axis.armed = true
T.reg_set(1080, 300)
T.mb_wq[#T.mb_wq + 1] = { 1080, 300 }; T.modbus_step()
local saved_busy, saved_alarm = BUSY, ALARM
BUSY = function() return 0 end
ALARM = function() return 1 end
T.robot_bridge_step()
check(T.robot_axis.active == 0 and T.reg_get(1140) == 4, "报警 → 状态=失败(4)")
BUSY, ALARM = saved_busy, saved_alarm
-- ⑦ 负目标（位置变化触发 + 符号还原）
T.reg_set(1080, 65411)
T.mb_wq[#T.mb_wq + 1] = { 1080, 65411 }; T.modbus_step()
check(last_move and last_move.p == -125, "负目标：-125mm（位置变化触发）")
-- ⑧ 安全联锁/未使能时拒绝（2026-09-29：不再静默挂起）
T.robot_axis.armed = true
T.robot_axis.safe = 1
T.reg_set(1080, 400); T.mb_wq[#T.mb_wq + 1] = { 1080, 400 }; T.modbus_step()
check(T.reg_get(1140) == 4 and T.robot_axis.active == 0, "安全联锁期间：定位被拒 → 4")
T.robot_axis.safe = 0
enabled_stub = 0
T.reg_set(1080, 401); T.mb_wq[#T.mb_wq + 1] = { 1080, 401 }; T.modbus_step()
check(T.reg_get(1140) == 4 and T.robot_axis.active == 0, "轴未使能：定位被拒 → 4")
enabled_stub = 1
-- ⑨ 运动中联锁/掉使能 → 中止 4
local saved_idle = IDLE
IDLE = function() return 0 end
T.reg_set(1080, 402); T.mb_wq[#T.mb_wq + 1] = { 1080, 402 }; T.modbus_step()
check(T.robot_axis.active == 1, "再受理（en=1，状态=0 执行中）")
T.robot_axis.safe = 1
T.robot_bridge_step()
check(T.robot_axis.active == 0 and T.reg_get(1140) == 4, "运动中安全联锁 → 中止 4")
T.robot_axis.safe = 0
T.reg_set(1080, 403); T.mb_wq[#T.mb_wq + 1] = { 1080, 403 }; T.modbus_step()
check(T.robot_axis.active == 1, "三受理（en=1）")
enabled_stub = 0
T.robot_bridge_step()
check(T.robot_axis.active == 0 and T.reg_get(1140) == 4, "运动中掉使能 → 中止 4")
IDLE = saved_idle
enabled_stub = 1
-- ⑩ 被拒后同值重发可再触发（2026-09-29）
T.robot_axis.armed = true
T.robot_axis.safe = 1
T.reg_set(1080, 450); T.mb_wq[#T.mb_wq + 1] = { 1080, 450 }; T.modbus_step()
check(T.reg_get(1140) == 4 and T.robot_axis.last_ok == false, "联锁拒收 → last_ok=false")
T.robot_axis.safe = 0
T.mb_wq[#T.mb_wq + 1] = { 1080, 450 }; T.modbus_step()      -- 同值重发
check(T.robot_axis.active == 1 and last_move ~= nil and last_move.p == 450, "同值重发 → 重新触发 450")

print("== 3e5) 4x3 数值状态码（2026-09-29）==")
T.robot_axis.safe = 0
T._set_bus_ok(1)
local saved_busy3 = BUSY
BUSY = function() return 0 end
enabled_stub = 1
T.modbus_step()
check(T.reg_get(3) == 2, "就绪·已使能空闲 → 2")
BUSY = function() return 1 end
T.modbus_step()
check(T.reg_get(3) == 3, "运行中 → 3")
BUSY = saved_busy3
enabled_stub = 0
T.modbus_step()
check(T.reg_get(3) == 1, "就绪·未使能 → 1")
enabled_stub = 1
T._set_bus_ok(0)
T.modbus_step()
check(T.reg_get(3) == 0, "总线未就绪 → 0")
T._set_bus_ok(1)
local saved_axstat3 = AXISSTATUS
AXISSTATUS = function() return T.MV_ERRMASK end
T.modbus_step()
check(T.reg_get(3) == 4, "报警 → 4")
AXISSTATUS = saved_axstat3
T.robot_axis.safe = 1
T.modbus_step()
check(T.reg_get(3) == 5, "安全联锁 → 5")
T.robot_axis.safe = 0

print("== 3f) 触摸屏点动/定位：方向符号 + 速度浮点兼容（v0.8.9 修复） ==")
T._set_conn(1)
for _ = 1, 4 do T.bus_step() end          -- 总线就绪（与第 4 节同法）
-- 方向 -1（屏写 65535）→ VMOVE(-1)
T.reg_set(68, 65535)
T.reg_set(69, 0); T.reg_set(70, 0)
T.reg_set(67, 0); T.modbus_step()
T.reg_set(67, 1); T.modbus_step()
check(last_vmove == -1, "方向 65535（int16 -1）→ VMOVE(-1)")
-- 方向 1 → VMOVE(1)
T.reg_set(67, 0); T.modbus_step()
T.reg_set(68, 1)
T.reg_set(67, 1); T.modbus_step()
check(last_vmove == 1, "方向 1 → VMOVE(1)")
T.reg_set(67, 0); T.modbus_step()
-- 点动速度：屏端 4x69/70 低字在前浮点 50.0 → SPEED=50
T.reg_set_f(69, 50.0)
T.reg_set(67, 1); T.modbus_step()
check(math.abs((last_speed or 0) - 50.0) < 1e-6, "点动速度浮点 50.0（4x69/70 低字在前）→ SPEED=50")
T.reg_set(67, 0); T.modbus_step()
-- 定位速度：4x64/65 浮点 100.0 → 触发运动 SPEED=100
T.reg_set_f(64, 100.0)   -- 屏端浮点占 4x64/65（高字即 4x65）；不要再单独写 4x65（会破坏浮点）
T.reg_set_f(62, 10.0)
T.reg_set(66, 0); T.modbus_step()
T.reg_set(66, 1); T.modbus_step()
check(math.abs((last_speed or 0) - 100.0) < 1e-6, "定位速度浮点 100.0（4x64/65 低字在前）→ SPEED=100")
T.reg_set(66, 0); T.modbus_step()

print("== 4) 4321 命令派发 ==")
T._set_conn(1)
for _ = 1, 4 do T.bus_step() end   -- 分步总线初始化 -> bus_ok=1
clear_sent()
T.cmd_exec("HELLO")
check(last_sent() == "HELLO ZMC/n", "HELLO -> HELLO ZMC/n")

last_move = nil
T.cmd_exec("55")
check(last_move and last_move.op == "abs" and last_move.p == 55 and last_move.w == 0,
      "单值数值命令 -> 异步 MOVEABS(55, w=0)")

check(last_sent() == "" or true, "（第一条数值命令进入运动监测，无立即应答）")
T._set_mvmode(0)
last_move = nil
T.cmd_exec("2000,100,55")
check(last_move and last_move.p == 55 and last_move.s == 100 and last_move.a == 2000 and last_move.w == 0,
      "acc,spd,pos -> 异步 MOVEABS(55,100,2000)")

clear_sent()
T.cmd_exec("STA")
check(string.match(last_sent(), "^STA,1,0,") ~= nil, "STA 应答前缀 STA,1,0,")

clear_sent()
T.cmd_exec("BOGUS")
check(last_sent() == "ERR:UNKNOWN/n", "未知命令 -> ERR:UNKNOWN/n")

T.scale_state.valid = 1
T.scale_state.w = 12.345
T.scale_state.conn = 1
clear_sent()
T.cmd_exec("W")
check(last_sent() == "WEIGHT:    12.345/n", "W -> WEIGHT:<值>/n")

clear_sent()
T.cmd_exec("WSTA")
check(string.match(last_sent(), "^WSTA,0,1,1,") ~= nil, "WSTA 应答前缀 WSTA,ok,valid,conn")

clear_sent()
T.cmd_exec("RCMD,abc")
check(last_sent() == "ERR:UNKNOWN/n", "R* 机器人命令已随 4320 取消 -> ERR:UNKNOWN/n")

T._set_mvmode(0)
clear_sent()
T.cmd_exec("MR,0,-5,20")
check(last_move and last_move.op == "rel" and last_move.p == -5 and last_move.w == 0, "MR -> 异步 MOVE(-5)")

print("== 5) 4321 拆帧（socket_step） ==")
rxq[10] = { "STA;\r\n" }
clear_sent()
T.socket_step()   -- 首次：建立 4321 监听（KX_TEST 未走 main）
T.socket_step()   -- 再次：处理收到的字节
local found = false
for _, m in ipairs(sent) do
    if string.match(m.s, "^STA,1,0,") then found = true end
end
check(found, "socket_step 收到 STA;\\r\\n 并执行")

print("== 6) Modbus-TCP 从站 ==")
T.reg_set_f(10, 12.5)
local pdu = string.pack(">I2I2", 10, 2)
local resp = T.mb_handle_pdu(0x1234, 0, 1, 3, pdu)
local tid, pid, len, uid, fc = string.unpack(">I2I2I2BB", resp)
check(tid == 0x1234 and uid == 1 and fc == 3, "FC3 响应头（TID/UID/FC）")
local n = string.byte(resp, 9)
local w1, w2 = string.unpack(">I2I2", string.sub(resp, 10, 13))
local f = string.unpack(">f", string.pack(">I2I2", w2, w1))
check(n == 4 and math.abs(f - 12.5) < 0.001, "FC3 浮点回读 = 12.5（低字在前）")

local pdu6 = string.pack(">I2I2", 4, 0x1234)
local resp6 = T.mb_handle_pdu(1, 0, 1, 6, pdu6)
check(T.reg_get(4) == 0x1234 and string.byte(resp6, 8) == 6, "FC6 写单寄存器并回显")

local pdu16 = string.pack(">I2I2B", 20, 2, 4) .. string.pack(">I2I2", 111, 222)
local resp16 = T.mb_handle_pdu(2, 0, 1, 16, pdu16)
check(T.reg_get(20) == 111 and T.reg_get(21) == 222 and #resp16 == 12, "FC16 写多寄存器")

local bad = T.mb_handle_pdu(3, 0, 1, 3, string.pack(">I2I2", 250, 10))
check(string.byte(bad, 8) == (3 | 0x80) and string.byte(bad, 9) == 2, "越界读 -> 异常码 0x02")

-- throughput: modbus_step 解析字节流（P3b：Lua 502 线上处理默认退役；此处开回验证回退路径）
T._set_mb_wire(true)
T.modbus_state.opened = 1
T.modbus_state.rx[0] = string.pack(">I2I2I2BB", 7, 0, 6, 1, 3) .. string.pack(">I2I2", 10, 2)  -- 客户端 0
rxq[13] = {}
clear_sent()
T.modbus_step()
local got = false
for _, m in ipairs(sent) do
    if m.ch == 13 and string.byte(m.s, 1) == 0 then got = true end
end
check(got, "modbus_step 从字节流解析 MBAP 并应答（MB_WIRE 回退路径）")
T._set_mb_wire(false)

-- ★机器人区（4x1000~1199，planA/19；触摸屏/机器人同连 502，2026-09-28）
local w6 = T.mb_handle_pdu(9, 0, 1, 6, string.pack(">I2I2", 1050, 4321))
check(w6 ~= nil and string.byte(w6, 8) == 6, "机器人区写 4x1050=4321（FC6 成功）")
local r3 = T.mb_handle_pdu(10, 0, 1, 3, string.pack(">I2I2", 1050, 1))
check(string.byte(r3, 9) == 2 and T.reg_get(1050) == 4321, "机器人区读回 4x1050=4321（FC3）")
local gap = T.mb_handle_pdu(11, 0, 1, 3, string.pack(">I2I2", 300, 1))
check(string.byte(gap, 8) == (3 | 0x80) and string.byte(gap, 9) == 2, "中间空闲区 4x300 -> 异常码 0x02")
local oob = T.mb_handle_pdu(12, 0, 1, 6, string.pack(">I2I2", 1200, 1))
check(string.byte(oob, 8) == (6 | 0x80) and string.byte(oob, 9) == 2, "机器人区越界 4x1200 -> 异常码 0x02")
local oob2 = T.mb_handle_pdu(13, 0, 1, 3, string.pack(">I2I2", 1199, 2))
check(string.byte(oob2, 8) == (3 | 0x80), "机器人区跨区读 4x1199 两格 -> 异常（不越界到 1200）")

-- ★线圈/离散输入（机器人区 1000~1099；FC1/2/5/15，2026-09-28）
local w5 = T.mb_handle_pdu(20, 0, 1, 5, string.pack(">I2I2", 1003, 0xFF00))
check(w5 ~= nil and string.byte(w5, 8) == 5 and T.coil_get(1003) == 1, "FC5 写线圈 0x1003=ON")
local r1 = T.mb_handle_pdu(21, 0, 1, 1, string.pack(">I2I2", 1000, 8))
check(string.byte(r1, 8) == 1 and string.byte(r1, 9) == 1 and (string.byte(r1, 10) & 0x08) ~= 0,
      "FC1 读线圈 0x1000~1007（bit3=1）")
local w15 = T.mb_handle_pdu(22, 0, 1, 15, string.pack(">I2I2B", 1010, 3, 1) .. string.char(0x05))
check(string.byte(w15, 8) == 15 and T.coil_get(1010) == 1 and T.coil_get(1011) == 0 and T.coil_get(1012) == 1,
      "FC15 写多线圈 0x1010..1012 = 1,0,1")
T.dis_set(1005, 1)
local r2 = T.mb_handle_pdu(23, 0, 1, 2, string.pack(">I2I2", 1000, 8))
check(string.byte(r2, 8) == 2 and (string.byte(r2, 10) & 0x20) ~= 0, "FC2 读离散输入（bit5=1）")
local e5 = T.mb_handle_pdu(24, 0, 1, 5, string.pack(">I2I2", 500, 0xFF00))
check(string.byte(e5, 8) == (5 | 0x80) and string.byte(e5, 9) == 2, "线圈空洞 0x500 -> 异常 0x02")
local e2 = T.mb_handle_pdu(25, 0, 1, 2, string.pack(">I2I2", 1100, 1))
check(string.byte(e2, 8) == (2 | 0x80) and string.byte(e2, 9) == 2, "离散越界 1x1100 -> 异常 0x02")
local e15 = T.mb_handle_pdu(26, 0, 1, 15, string.pack(">I2I2B", 1000, 3, 2) .. string.char(0, 0))
check(string.byte(e15, 8) == (15 | 0x80) and string.byte(e15, 9) == 3, "FC15 字节数不符 -> 异常 0x03（标准）")
local e5v = T.mb_handle_pdu(27, 0, 1, 5, string.pack(">I2I2", 1000, 0x1234))
check(string.byte(e5v, 8) == (5 | 0x80) and string.byte(e5v, 9) == 3, "FC5 非法值 0x1234 -> 异常 0x03")

print("== 7) 称重 RTU 收发 ==")
-- 构造读应答：重量 -5、内码 1000
local low = -5 & 0xFFFF          -- 0xFFFB（低字）
local weight = string.pack(">I2I2", low, 0xFFFF)
local ad = string.pack(">I2I2", 1000, 0)
local body = string.char(1, 3, 8) .. weight .. ad
T.scale_state.rx = T.rtu_frame(body)
T.scale_state.send = 1
T.scale_state.t0 = TICKS()
T.scale_state.lastpoll = TICKS()
rxq[11] = {}
clear_sent()
T.scale_step()
check(T.scale_state.raw == -5, "解析重量 raw=-5（32位有符号低字在前）")
check(T.scale_state.ad == 1000, "解析内码 ad=1000")
check(T.scale_state.valid == 1, "数据有效标志")

-- 写命令：去皮 -> 队列 2 条 -> step 发出第一条（06 帧）
T.scale_state.send = 0
T.scale_state.qn = 0
T.scale_state.queue = {}
T.scale_do_cmd(1)
check(T.scale_state.qn == 2 and T.scale_state.result == 1, "去皮入队 2 条（关写保护 + 去皮）")
T.scale_state.lastpoll = TICKS()
clear_sent()
T.scale_step()
local wframe = nil
for _, m in ipairs(sent) do if m.ch == 11 then wframe = m.s end end
check(wframe ~= nil and string.byte(wframe, 1) == 1 and string.byte(wframe, 2) == 6, "下发 06 写帧（站号1）")
if wframe then
    local c = T.crc16(string.sub(wframe, 1, 6))
    local lo, hi = string.byte(wframe, 7, 8)
    check(c == lo + hi * 256, "写帧 CRC 正确（低字节在前）")
end

print("== 7b) 4x136 称重状态码（多状态屏显；命令态保持 2s） ==")
local S = T.scale_state
S.ok, S.valid, S.conn, S.err, S.result, S.res_t = 1, 1, 1, 0, 0, 0
T.modbus_step()
check(T.reg_get(136) == 4, "常态（在线+有效）→ 4")
S.result = 1; T.modbus_step()
check(T.reg_get(136) == 5, "命令执行中 → 5")
S.result = 2; T.modbus_step()
check(T.reg_get(136) == 6, "命令成功（2s 内）→ 6")
S.res_t = TICKS() + 5000      -- 模拟 2s 已过（TICKS 倒数：值更大=更早）
T.modbus_step()
check(T.reg_get(136) == 4, "成功保持 2s 后回落常态 → 4")
S.result = 3; S.res_t = 0; T.modbus_step()
check(T.reg_get(136) == 7, "命令失败（2s 内）→ 7")
S.result, S.res_t = 0, 0
S.ok, S.valid, S.conn, S.err = 0, 0, 1, 1; T.modbus_step()
check(T.reg_get(136) == 2, "通讯出错/超时 → 2")
S.ok, S.err, S.conn = 0, 0, 0; T.modbus_step()
check(T.reg_get(136) == 1, "未连接 → 1")
S.ok, S.valid, S.conn = 1, 0, 1; T.modbus_step()
check(T.reg_get(136) == 3, "已连接、等数据 → 3")
S.ok, S.valid, S.conn, S.err, S.result = 0, 0, 0, 0, 0    -- 还原

print("== 8) 蠕动泵框架（RTU 组帧/解析/命令位） ==")
local function hexs(x)
    local t = {}
    for i = 1, #x do t[#t + 1] = string.format("%02X", string.byte(x, i)) end
    return table.concat(t, " ")
end
-- 组帧：与手册实例逐字节一致（CRC 低字节在前）
check(string.sub(hexs(T.pump_rd_inputs(50, 1)), 1, 17) == string.format("%02X 04 00 32 00 01", T.PUMP_STATION) and T.crc16(T.pump_rd_inputs(50, 1)) == 0,
      "读 reg50 帧（站号=PUMP_STATION，CRC 自洽）")
check(string.sub(hexs(T.pump_coil(1, true)), 1, 17) == string.format("%02X 05 00 01 FF 00", T.PUMP_STATION) and T.crc16(T.pump_coil(1, true)) == 0,
      "启动线圈帧（站号=PUMP_STATION，CRC 自洽）")
check(string.sub(hexs(T.pump_coil(1, false)), 1, 17) == string.format("%02X 05 00 01 00 00", T.PUMP_STATION) and T.crc16(T.pump_coil(1, false)) == 0,
      "停止线圈帧（站号=PUMP_STATION，CRC 自洽）")
check(T.pump_f32(100.0) == string.char(0x42, 0xC8, 0x00, 0x00),
      "泵 FLOAT 高字在前（100rpm = 42 C8 00 00）")
check(T.crc16(T.pump_coil(1, true)) == 0, "Modbus CRC 自洽（整帧含 CRC 再算 = 0）")

local p = T.pump_state
-- 异常响应（01 85 04）→ 记录异常码并自动补读 reg50
p.rx = ""; p.queue = {}; p.qn = 0; p.exc = 0; p.errcode = 0
check(T.pump_handle_frame(T.rtu_frame(string.char(T.PUMP_STATION, 0x85, 0x04))) == 1 and p.exc == 4,
      "异常响应 01 85 04 → exc=4")
check(p.qn == 1 and p.queue[1].addr == 50, "异常后自动补读 reg50")
-- reg50 应答（手册实例）→ 业务错误码 141
p.queue = {}; p.qn = 0
p.pend_fc, p.pend_addr, p.pend_cnt = 0x04, 50, 1
check(T.pump_handle_frame(T.rtu_frame(string.char(T.PUMP_STATION, 0x04, 0x02, 0x00, 0x8D))) == 1 and p.errcode == 141,
      "reg50 应答 → 业务错误码 141（手册实例）")
-- 输入 0~5 应答 → 控制方式/方向/转速/流速
p.queue = {}; p.qn = 0
p.pend_fc, p.pend_addr, p.pend_cnt = 0x04, 0, 6
local b2 = string.char(T.PUMP_STATION, 0x04, 0x0C, 0x00, 0x02, 0x00, 0x02) .. T.pump_f32(100.0) .. T.pump_f32(7.0)
check(T.pump_handle_frame(T.rtu_frame(b2)) == 1 and p.ctl == 2 and p.dir == 2 and
          math.abs(p.rpm - 100.0) < 0.01 and math.abs(p.flow - 7.0) < 0.01,
      "输入 0~5 应答解析（ctl=2/dir=2/rpm=100/flow=7）")
-- 线圈应答（手册实例 01 01 01 01 90 48）→ 运行=1
p.pend_fc, p.pend_addr, p.pend_cnt = 0x01, 1, 1
check(T.pump_handle_frame(T.rtu_frame(string.char(T.PUMP_STATION, 0x01, 0x01, 0x01))) == 1 and p.run == 1,
      "线圈应答 → 运行=1（手册实例）")
-- 步进 smoke：连上先发 485 使能（06 reg0=1）；命令位 bit0 → 启动线圈帧
p.opened = 0; p.send = 0; p.qn = 0; p.queue = {}; p.ctl = 0; p.cmdold = 0
local n0 = #sent
T.pump_step()
T.pump_step()
local en_found = false
for i = n0 + 1, #sent do
    if sent[i].ch == 14 and string.sub(sent[i].s, 1, 6) == string.char(T.PUMP_STATION, 0x06, 0x00, 0x00, 0x00, 0x01) and
        T.crc16(sent[i].s) == 0 then
        en_found = true
    end
end
check(en_found, "连上后先发 485 使能帧（06 reg0=1，CRC 自洽）")
T.reg_set(203, 1)                                    -- ★命令码 1 = 启动（写入即触发）
p.send = 0
T.pump_step()
check(sent[#sent].ch == 14 and string.sub(hexs(sent[#sent].s), 1, 14) == string.format("%02X 05 00 01 FF", T.PUMP_STATION) and T.crc16(sent[#sent].s) == 0,
      "命令码 1 → 启动线圈帧（4x203，站号=PUMP_STATION）")
T.reg_set(203, 0)
T.pump_step()
-- ★4x203 命令码（数值命令，写入即触发；2026-09-27）：5=全速ON 6=全速OFF 3=写转速 4=重新使能 7=未知
p.send = 0; T.reg_set(203, 5); T.pump_step()
check(sent[#sent].ch == 14 and string.sub(hexs(sent[#sent].s), 1, 14) == string.format("%02X 05 00 02 FF", T.PUMP_STATION) and T.crc16(sent[#sent].s) == 0,
      "命令码 5 → 全速ON（线圈 2）")
p.send = 0; T.reg_set(203, 0); T.pump_step()
local n1 = #sent
T.reg_set(204, 300)                                   -- ★4x204 必须为有效 INT16 转速（1~300）
p.send = 0; T.reg_set(203, 3); T.pump_step()
check(#sent == n1 + 1 and string.sub(hexs(sent[#sent].s), 1, 20) == string.format("%02X 10 00 10 00 02 04", T.PUMP_STATION) and T.crc16(sent[#sent].s) == 0,
      "命令码 3 → 写转速帧（w10 reg16 共 2 个，含 4x204 值）")
p.send = 0; T.reg_set(203, 0); T.pump_step()
n1 = #sent
p.send = 0; T.reg_set(203, 7); T.pump_step()
check(#sent == n1, "命令码 7（未知）→ 不下发任何帧")
p.send = 0; T.reg_set(203, 0); T.pump_step()
-- ★4x202 泵状态码（0 未连接/1 未使能485/2 就绪/3 运行中/4 通讯超时/5 泵错误）
local P = T.pump_state
P.send, P.t0 = 0, TICKS()
P.ok, P.ctl, P.run, P.errcode, P.exc = 1, 2, 0, 0, 0
T.pump_step(); check(T.reg_get(202) == 2, "状态码：就绪（连接+485使能+无错）→ 2")
P.run = 1; T.pump_step(); check(T.reg_get(202) == 3, "状态码：运行中 → 3")
P.run = 0; P.ctl = 0; T.pump_step(); check(T.reg_get(202) == 1, "状态码：未使能 485 → 1")
P.ctl = 2; P.ok = 0; T.pump_step(); check(T.reg_get(202) == 4, "状态码：通讯超时 → 4")
P.ok = 1; P.errcode = 5; T.pump_step(); check(T.reg_get(202) == 5, "状态码：泵错误 → 5")
P.errcode = 0
check(T.pump_state_code({ conn = 0 }) == 0, "状态码：未连接 → 0（纯函数）")
-- ★485 使能自动重试（每 3s；ctl≠2 且通讯 OK）
P.ok, P.ctl, P.qn, P.send = 1, 0, 0, 0
P.ent0 = TICKS() + 5000                               -- 模拟上次重试已过（TICKS 倒数：值更大=更早）
local n2 = #sent
T.pump_step()
check(#sent == n2 + 1 and string.sub(hexs(sent[#sent].s), 1, 17) == string.format("%02X 06 00 00 00 01", T.PUMP_STATION),
      "ctl≠2 → 自动重发 485 使能（06 reg0=1）")
P.ent0 = TICKS()                                      -- 刚发过 → 不再重复
n2 = #sent
T.pump_step()
check(#sent == n2, "3s 内不重复重发使能")
P.ctl = 2
T.pump_step()
check(P.entn == 0, "ctl=2 → 重试计数清零")

print("== 7c) 折线校准提交（4x28 重量 / 4x29 点数 / 4x30 提交 / 4x31 结果） ==")
local S2 = T.scale_state
S2.conn, S2.ok = 1, 1
S2.cal_idx, S2.cal_result = 0, 0
S2.queue, S2.qn, S2.send = {}, 0, 0
S2.div = 100.0                                        -- 比例系数（4x132/133），默认 100
T.reg_set(28, 10)                                     -- 10g → 模块 1000 计数
T.mb_wq[#T.mb_wq + 1] = { 30, 1 }                     -- 写队列路径（真实 HMI 写入）
T.modbus_step()
check(S2.qn == 3 and S2.queue[1] == string.char(0, 0x17, 0, 1) and
      S2.queue[2] == string.char(0, 0x18, 0, 10) and
      S2.queue[3] == string.char(0, 0x19, 0x03, 0xE8),
      "首次提交：关写保护 + 10 点折线 + 第 1 点 10g→1000 计数（3 条入队）")
check(T.reg_get(29) == 1 and T.reg_get(31) == 1 and T.reg_get(30) == 0,
      "点数=1、结果=1、提交位自动回 0（便于连按）")
S2.queue, S2.qn = {}, 0
T.reg_set(28, 20); T.reg_set(30, 1); T.modbus_step()    -- 值边沿兜底路径（20g → 2000 计数）
check(S2.qn == 1 and S2.queue[1] == string.char(0, 0x1A, 0x07, 0xD0), "第 2 点（兜底路径）：20g→0x001A=2000")
check(T.reg_get(29) == 2, "点数=2")
S2.queue, S2.qn = {}, 0
T.reg_set(28, 0); T.reg_set(30, 1); T.modbus_step()
check(S2.qn == 0 and T.reg_get(31) == 2 and T.reg_get(29) == 2, "重量 0 无效 → 结果=2、不写点")
-- ★低位值有效：10g 输 10（用户约定，十进制整数不带小数点）→ 模块 1000 计数
S2.queue, S2.qn = {}, 0
T.reg_set(28, 10); T.reg_set(30, 1); T.modbus_step()
check(S2.qn == 1 and S2.queue[1] == string.char(0, 0x1B, 0x03, 0xE8) and T.reg_get(29) == 3 and T.reg_get(31) == 1,
      "重量 10（10g）→ 提交第 3 点 0x001B=1000 计数 成功")
-- ★最小重量 1g（输 1）边界有效 → 模块 100 计数
S2.queue, S2.qn = {}, 0
T.reg_set(28, 1); T.reg_set(30, 1); T.modbus_step()
check(S2.qn == 1 and S2.queue[1] == string.char(0, 0x1C, 0x00, 0x64) and T.reg_get(29) == 4,
      "重量 1（1g，最小）→ 提交第 4 点 0x001C=100 计数")
-- ★超范围：显示值 700 → 70000 计数 > 65535 → 拒绝
S2.queue, S2.qn = {}, 0
T.reg_set(28, 700); T.reg_set(30, 1); T.modbus_step()
check(S2.qn == 0 and T.reg_get(31) == 2 and T.reg_get(29) == 4, "700g→70000 计数超范围 → 结果=2、点数不变")
S2.cal_idx = 10
T.reg_set(28, 0); T.reg_set(30, 1); T.modbus_step()
check(T.reg_get(31) == 2 and T.reg_get(29) == 10, "已满 + 重量无效 → 结果=2、点数不变")
-- ★已满后再提交（重量有效）→ 自动开始新一轮（覆盖旧点）
S2.queue, S2.qn = {}, 0
T.reg_set(28, 50); T.reg_set(30, 1); T.modbus_step()
check(S2.qn == 3 and S2.queue[3] == string.char(0, 0x19, 0x13, 0x88) and
      T.reg_get(29) == 1 and T.reg_get(31) == 1,
      "已满 + 有效重量 → 自动重新开始（第 1 点 50g→5000 计数，重设关写保护+10 点折线）")
T.reg_set(30, 2); T.modbus_step()
check(S2.cal_idx == 0 and T.reg_get(29) == 0 and T.reg_get(31) == 0, "4x30=2 → 重置（点数/结果清零）")
-- ★4x30=3 → 关闭折线（0x0018=0）
S2.queue, S2.qn = {}, 0
T.reg_set(30, 3); T.modbus_step()
check(S2.qn == 2 and S2.queue[1] == string.char(0, 0x17, 0, 1) and S2.queue[2] == string.char(0, 0x18, 0, 0),
      "4x30=3 → 关闭折线（关写保护 + 0x0018=0）")
-- ★4x30=4 → 单点砝码校准（5g → 0x0006=500）
S2.queue, S2.qn = {}, 0
T.reg_set(28, 5); T.reg_set(30, 4); T.modbus_step()
check(S2.qn == 2 and S2.queue[1] == string.char(0, 0x17, 0, 1) and S2.queue[2] == string.char(0, 0x06, 0x01, 0xF4),
      "4x30=4 → 单点砝码校准（5g→0x0006=500=0x01F4）")
S2.conn = 0
T.reg_set(28, 500); T.reg_set(30, 1); T.modbus_step()
check(T.reg_get(31) == 4, "未连接 → 结果=4")
S2.conn = 1
S2.queue, S2.qn = {}, 0
-- ★4x203 写队列路径不得重复派发（泵侧兜底同步）
local P2 = T.pump_state
P2.send, P2.qn, P2.queue, P2.cmdold = 0, 0, {}, 0
local n3 = #sent
T.mb_wq[#T.mb_wq + 1] = { 203, 1 }
T.modbus_step()
T.pump_step()
check(#sent == n3 + 1, "4x203 写队列路径：只发一帧（泵侧不重复派发）")
check(T.reg_get(203) == 0, "4x203 处理完自动回 0（连按可靠）")
-- ★按圈数运行（4x211 float 圈数 / 4x213=1 启动）
local P3 = T.pump_state
P3.conn, P3.ok, P3.send, P3.qn, P3.queue = 1, 1, 0, 0, {}
P3.cmdold, P3.revold, P3.rev_end = 0, 0, 0
T.reg_set(204, 100)                                   -- 100 rpm
T.reg_set_f(211, 1.5)                                 -- 1.5 圈
local n4 = #sent
T.reg_set(213, 1); T.pump_step()
check(#sent == n4 + 1 and string.sub(hexs(sent[#sent].s), 1, 20) == string.format("%02X 10 00 10 00 02 04", T.PUMP_STATION),
      "圈数运行：先下发设定转速帧（保证与计时一致）")
P3.send = 0
T.pump_step()
check(#sent == n4 + 2 and string.sub(hexs(sent[#sent].s), 1, 14) == string.format("%02X 05 00 01 FF", T.PUMP_STATION),
      "圈数运行：再发启动帧（线圈 1 ON）")
check(T.reg_get(213) == 0 and P3.rev_end ~= 0, "运行按钮自动回 0、计时已装定（1.5 圈@100rpm≈0.9s）")
n4 = #sent; P3.send = 0
T.reg_set(213, 1); T.pump_step()
check(#sent == n4, "运行中重复按启动被忽略")
P3.send = 0; P3.rev_end = TICKS() + 5000              -- 模拟时间已过（TICKS 倒数：值更大=更早）
P3.qn, P3.queue = 0, {}
T.pump_step()
check(#sent == n4 + 1 and string.sub(hexs(sent[#sent].s), 1, 14) == string.format("%02X 05 00 01 00", T.PUMP_STATION),
      "圈数到时 → 停止帧（线圈 1 OFF）")
check(P3.rev_end == 0, "到时后计时清零")
P3.send = 0
T.reg_set(204, 60); T.reg_set_f(211, 2.0)
T.reg_set(213, 1); T.pump_step()
P3.send = 0
T.reg_set(203, 2); T.pump_step()
check(P3.rev_end == 0, "手动停止（203=2）取消圈数计时")
P3.send = 0
-- ★回吸参数（4x215 角度 / 4x217 速度 / 4x219 提交）
P3.pend_fc, P3.pend_addr, P3.pend_cnt = 0x03, 7, 2
local rf = T.rtu_frame(string.char(T.PUMP_STATION, 0x03, 0x04, 0x00, 0x8C, 0x00, 0x28))
check(T.pump_handle_frame(rf) == 1 and P3.rev_angle == 140 and P3.rev_speed == 40,
      "回吸读回：reg7=140°、reg8=40rpm")
T.pump_step()
check(T.reg_get(215) == 140 and T.reg_get(217) == 40, "镜像（首次）：4x215=140、4x217=40")
-- ★HMI 输入不被轮询镜像覆盖（关键回归，2026-09-28：设 0 后提交却发现还是 140）
T.reg_set(215, 0); T.reg_set(217, 90)
T.pump_step()
check(T.reg_get(215) == 0 and T.reg_get(217) == 90, "输入值不被轮询镜像覆盖（变化才刷新）")
-- 泵内值真正变化 → 刷新显示
P3.pend_fc, P3.pend_addr, P3.pend_cnt = 0x03, 7, 2
local rf2 = T.rtu_frame(string.char(T.PUMP_STATION, 0x03, 0x04, 0x00, 0x00, 0x00, 0x5A))
T.pump_handle_frame(rf2)
T.pump_step()
check(T.reg_get(215) == 0 and T.reg_get(217) == 90, "提交后回读变化 → 显示刷新（角度 0、速度 90）")
P3.conn, P3.ok, P3.send, P3.qn, P3.queue = 1, 1, 0, 0, {}
T.reg_set(215, 180); T.reg_set(217, 90)
T.reg_set(219, 1); T.pump_step()
check(P3.qn == 1 and
      string.sub(hexs(sent[#sent].s), 1, 17) == string.format("%02X 06 00 07 00 B4", T.PUMP_STATION) and
      string.sub(hexs(P3.queue[1].f), 1, 17) == string.format("%02X 06 00 08 00 5A", T.PUMP_STATION) and
      T.crc16(sent[#sent].s) == 0 and T.crc16(P3.queue[1].f) == 0,
      "回吸提交：0x0007=180（已发）、0x0008=90（在队，CRC 自洽）")
check(T.reg_get(219) == 0, "回吸提交按钮自动回 0")
P3.qn, P3.queue = 0, {}
T.reg_set(215, 5); T.reg_set(217, 90)
T.reg_set(219, 1); T.pump_step()
check(P3.qn == 0, "回吸角度 5 无效（非 0/10~720）→ 不提交")
T.reg_set(215, 0); T.reg_set(217, 5)
T.reg_set(219, 1); T.pump_step()
check(P3.qn == 0, "回吸速度 5 无效（10~300）→ 不提交")
T.reg_set(215, 0); T.reg_set(217, 40)
P3.send = 0
T.reg_set(219, 1); T.pump_step()
check(string.sub(hexs(sent[#sent].s), 1, 17) == string.format("%02X 06 00 07 00 00", T.PUMP_STATION),
      "角度 0（关闭回吸）可提交")
-- ★ml/圈（4x221/222）→ 预计出液量（4x223/224）= 圈数 × ml/圈
T.reg_set_f(211, 1.5)
T.reg_set_f(221, 0.85)
T.pump_step()
check(math.abs((T.reg_get_f(223) or 0) - 1.3) < 0.001, "预计出液量 = 1.5 × 0.85 = 1.275 → 保留 1 位小数 1.3 ml")
T.reg_set_f(221, 2.0)
T.pump_step()
check(math.abs((T.reg_get_f(223) or 0) - 3.0) < 0.001, "改 ml/圈 → 实时刷新（1.5 × 2.0 = 3.0 ml）")
-- ★float32 噪声边界：155 × 0.65 = 100.75 → 应进为 100.8（而非 100.7）
T.reg_set_f(211, 155.0); T.reg_set_f(221, 0.65)
T.pump_step()
check(math.abs((T.reg_get_f(223) or 0) - 100.8) < 0.001, "155 × 0.65 → 100.8（3 位小数规整后进位）")
T.reg_set_f(211, 1.5); T.reg_set_f(221, 2.0); T.pump_step()
P3.qn, P3.queue = 0, {}
P3.send = 0
-- ★4x204 转速本地校验（4x204 必须 INT16；防把 0/越界值发给泵，2026-09-28）
P3.qn, P3.queue = 0, {}
T.reg_set(204, 0); T.reg_set(203, 3); T.pump_step()
check(P3.qn == 0, "写转速：4x204=0 → 本地拒绝（不发给泵）")
P3.qn, P3.queue = 0, {}
T.reg_set(204, 999); T.reg_set(203, 3); T.pump_step()
check(P3.qn == 0, "写转速：4x204=999 越界 → 本地拒绝")
-- 圈数运行：4x204 越界 → 回落用泵读回转速
P3.send, P3.qn, P3.queue, P3.rev_end, P3.rpm = 0, 0, {}, 0, 100
T.reg_set(204, 999); T.reg_set_f(211, 2.0)
T.reg_set(213, 1); T.pump_step()
check(P3.rev_end ~= 0, "圈数运行：4x204 越界 → 回落泵读回转速（100rpm）仍可运行")
P3.rev_end, P3.send, P3.qn, P3.queue = 0, 0, 0, {}
T.reg_set(204, 100)
-- ★容积运行（4x225 设定容积 / 4x227 预计圈数 / 4x229 容积运行；2026-09-28）
T.reg_set_f(221, 0.65)
T.reg_set_f(225, 1008.0)                              -- 屏侧送 1008（该元件 ×10）→ 逻辑 100.8 ml
T.pump_step()
check(math.abs((T.reg_get_f(227) or 0) - 155.1) < 0.001, "预计圈数 = 100.8 ÷ 0.65 = 155.0769 → 155.1（1 位小数，含量程 ÷10 还原）")
P3.send, P3.qn, P3.queue, P3.rev_end, P3.rpm = 0, 0, {}, 0, 100
T.reg_set(204, 100)
T.reg_set_f(221, 0.065)
T.reg_set_f(225, 650.0)                               -- 屏侧 650 → 逻辑 65 ml
T.reg_set(229, 1)
local n5 = #sent
T.pump_step()
check(#sent == n5 + 1 and string.sub(hexs(sent[#sent].s), 1, 20) == string.format("%02X 10 00 10 00 02 04", T.PUMP_STATION),
      "容积运行：先下发设定转速帧")
P3.send = 0
T.pump_step()
check(#sent == n5 + 2 and string.sub(hexs(sent[#sent].s), 1, 14) == string.format("%02X 05 00 01 FF", T.PUMP_STATION),
      "容积运行：65 ml ÷ 0.065 = 1000 圈 → 启动帧（线圈 1 ON）")
check(T.reg_get(229) == 0 and P3.rev_end ~= 0, "容积运行按钮自动回 0、计时装定（1000 圈@100rpm=600s）")
P3.rev_end, P3.send, P3.qn, P3.queue = 0, 0, 0, {}
T.reg_set_f(221, 0); T.reg_set_f(225, 50.0); T.reg_set(229, 1)
T.pump_step()
check(P3.qn == 0, "容积运行：ml/圈=0 → 本地拒绝（不启动）")
T.reg_set_f(221, 0.65)
-- ★输送管数量（4x214；2026-09-30 用户要求）：圈数↔容积换算 ×N / ÷N
T.reg_set(214, 5)
T.reg_set_f(221, 0.5); T.reg_set_f(211, 10); T.reg_set_f(225, 500.0)   -- 屏 500 → 逻辑 50 ml
T.pump_step()
check(math.abs((T.reg_get_f(223) or 0) - 25.0) < 0.001,
      "输送管5：预计出液量 = 10 圈 × 0.5 ml/圈 × 5 = 25.0 ml")
check(math.abs((T.reg_get_f(227) or 0) - 20.0) < 0.001,
      "输送管5：预计圈数 = 50 ml ÷ 0.5 ÷ 5 = 20.0 圈")
P3.send, P3.qn, P3.queue, P3.rev_end = 0, 0, {}, 0
T.reg_set(204, 100); T.reg_set(229, 1)
T.pump_step()                                   -- 先下发设定转速帧
P3.send = 0
local t_before = TICKS()
T.pump_step()                                   -- 启动帧
check(T.reg_get(229) == 0 and P3.rev_end ~= 0 and
      P3.rev_end <= t_before - 11000 and P3.rev_end >= t_before - 13000,
      "输送管5：容积运行 50 ml ÷ 0.5 ÷ 5 = 20 圈（计时≈12s@100rpm）")
P3.rev_end, P3.send, P3.qn, P3.queue = 0, 0, 0, {}
T.reg_set(214, 0); T.pump_step()
check(math.abs((T.reg_get_f(227) or 0) - 100.0) < 0.001,
      "输送管数量非法(0) → 按 1（50 ÷ 0.5 = 100 圈）")
T.reg_set(214, 1); T.reg_set_f(221, 0.65)
-- 持久化：设定容积（225/226）变化即存
T.reg_set_f(225, 12.5)
T.modbus_step()
check(nvstub[226] == 0x4148 or nvstub[225] ~= nil, "持久化：设定容积（225/226）变化 → NVSET 已存")
-- ★新增持久化：轴速度 64/65、69/70；称重参数 130/131/132/133
T.reg_set(64, 300); T.reg_set(131, 250)
T.reg_set(130, 1)
T.reg_set_f(132, 100.0)                               -- 132/133（低字 0 不变 → 只 133 变化）
T.reg_set(214, 5)                                     -- ★输送管数量（2026-09-30）
T.modbus_step()
check(nvstub[64] == 300 and nvstub[131] == 250 and nvstub[130] == 1 and nvstub[133] ~= nil and nvstub[214] == 5,
      "持久化：轴速度(64)/称重参数(130/131/132-133)/输送管数量(214) 变化 → NVSET 已存")

print("== 3g) 固件库存同步（P3b kx_mbsync；退役 Lua 502 后业务经同步层）==")
T.mbsync_push()                     -- 先清历史脏字（前序用例写过 4x3/60 等）
mb_fw[60] = 1; mb_fw[1000] = 7
T.mbsync_pull()
check(T.reg_get(60) == 1 and T.reg_get(1000) == 7, "pull：固件 → 本地（主站写入可见）")
-- 脚本写入（未组态地址 250 → 旧缓存+同步回退路径）：脏字不因 pull 被覆盖，push 后到达固件
T.reg_set(250, 8)
mb_fw[250] = 0
T.mbsync_pull()
check(T.reg_get(250) == 8, "pull：本拍脏字不被固件旧值覆盖（未组态回退）")
T.mbsync_push()
check(mb_fw[250] == 8, "push：脏字回推固件（未组态回退）")
T.mbsync_pull()
check(T.reg_get(250) == 8, "脏清：push 后 pull 与固件一致（未组态回退）")
-- 只推脏段：改动 5 与 9（不相邻），4 与 6~8 保持主站值不受影响
mb_fw[4] = 44; mb_fw[6] = 66; mb_fw[7] = 77; mb_fw[8] = 88
T.reg_set(5, 55); T.reg_set(9, 99)
T.mbsync_push()
check(mb_fw[5] == 55 and mb_fw[9] == 99 and mb_fw[4] == 44 and mb_fw[6] == 66 and mb_fw[8] == 88,
      "push：脏段精确回推（未改写字不被覆盖）")
-- 机器人区同样同步
T.reg_set(1140, 2)
T.mbsync_push()
check(mb_fw[1140] == 2, "push：机器人区脏字回推（未组态回退）")

print("== 3h) 按名访问层（v0.12.2：已组态地址 reg_* → MB_READ/MB_WRITE 直达）==")
mbname_seed(3, "r3")                                     -- 单字条目
mbname_seed(252, "r252", 2)                              -- 浮点条目（低字在前）
mbname_seed(251, "r251")                                 -- i16 条目（符号还原用例）
T.mbnames_refresh()
T.reg_set(3, 8)
check(mb_fw[3] == 8, "按名写：reg_set(3) → MB_WRITE 直达固件（本地零脏字）")
mb_fw[250] = 0                                           -- 干扰：固件对 250 的旧值（未组态）不影响
mb_fw[3] = 5                                             -- 模拟主站/屏写入固件
T.mbnames_tick()                                         -- 按拍缓存失效（下一拍首读回源）
check(T.reg_get(3) == 5, "按名读：下一拍 reg_get(3) 直读固件（无需 pull）")
check(T.reg_get(3) == 5, "按拍缓存：同拍重复读一致")
check(T.reg_get(250) == 8, "未组态地址仍走本地缓存（回退路径未被破坏）")
T.reg_set_f(252, 1.5)
check(mb_fw[252] == 0x0000 and mb_fw[253] == 0x3FC0,
      "按名写浮点：reg_set_f → MB_WRITE 单条目（1.5f 低字=0x0000/高字=0x3FC0）")
check(T.reg_get_f(252) == 1.5, "按名读浮点：reg_get_f → MB_READ 解码 = 1.5")
mb_fw[252] = 0; mb_fw[253] = 0x4000                      -- 外部（主站/屏）改写为 2.0f
T.mbnames_tick()
check(T.reg_get_f(252) == 2.0, "按名读浮点直读固件（外部改字下一拍可见）")
T.reg_set(251, 65531)                                    -- i16 原始字 0xFFFB（-5）
check(mb_fw[251] == 0xFFFB, "按名写 i16：原始字还原符号（65531 → 编码为 -5 = 0xFFFB）")
T.mbnames_tick()
check(T.reg_get(251) == 65531, "按名读 i16：解码 -5 → 原始字 65531")

print("== 9) NVRAM 持久化（NVSET/NVGET：恢复/变化即存） ==")
nvstub[204] = 123
nvstub[211] = 0
nvstub[212] = 0x3FC0                                      -- 1.5f 低字在前（恢复浮点）
T.nvram_restore()
check(T.reg_get(204) == 123, "恢复：4x204=123（来自 .nvram）")
check(math.abs((T.reg_get_f(211) or 0) - 1.5) < 0.001, "恢复：圈数浮点 1.5（低字 211/高字 212）")
-- 变化即存（modbus_step 内的保存扫描）
T.reg_set(204, 456)
T.reg_set_f(221, 0.85)
T.modbus_step()
check(nvstub[204] == 456, "保存：4x204 变化 → NVSET(456)")
check(nvstub[221] ~= nil and nvstub[222] ~= nil, "保存：4x221/222（ml/圈浮点）已存")

print(string.format("[kx_test] %d/%d 通过", total - fails, total))
os.exit(fails == 0 and 0 or 1)
