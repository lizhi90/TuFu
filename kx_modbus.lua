-- ============================================================
-- kx_modbus.lua — Modbus-TCP 从站（502）+ 触摸屏 HMI 轴命令区
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_modbus.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== Modbus-TCP 从站（502） ========================
modbus_state = { rx = {}, opened = 0, logt = 1e18, frames = 0, logged = 0,
                 nclold = 0, lastfc = 0, bad_uid = 0,
                 dbgseen = {}, dbgkeys = {} }   -- v0.8.2 诊断：触摸屏读写窗口（首次各记一条，上限 40）

-- ★当前正在处理的 502 客户端下标（响应写回用；单线程协作无重入，2026-09-28）
local mb_cur = 0

local function mb_exception(tid, pid, uid, fc, code)
    local pdu = string.char(fc | 0x80, code)
    return string.pack(">I2I2I2", tid, pid, #pdu + 1) .. string.char(uid) .. pdu
end

-- v0.8.2 诊断：首次出现的读/写窗口各记一条日志（对照触摸屏画面用；上限 40 条防刷屏）
local function mb_dbg_first(key)
    if modbus_state.dbgseen[key] or #modbus_state.dbgkeys >= 40 then return false end
    modbus_state.dbgseen[key] = true
    table.insert(modbus_state.dbgkeys, key)
    return true
end

-- ★地轨参数应用（屏幕 4x71~74：「确定」写 1 触发；2026-09-29 改 INT16）
--   4x71 速度(INT16，显示值×10 → ÷10 得 mm/s) / 4x72 加减速度(mm/s²，共用)
--   4x73 曲线 1 梯形 2 三角形 3 S 曲线 / 4x74 确定（写 1）
rail_apply = function()
    local spd  = reg_get(71)                           -- ★直接 mm/s（2026-09-30 晚 用户定案；原 ×10 约定作废）
    local accl = reg_get(72)
    local opt  = reg_get(73)
    if spd <= 0 or spd > MV_SPD_MAX then
        print(string.format("[rail] 速度无效（%.3f，需 1~%.1f mm/s 整数）", spd, MV_SPD_MAX))
        return 1
    end
    if accl <= 0 or accl > 1e7 then
        print(string.format("[rail] 加减速度无效（%.1f）", accl))
        return 1
    end
    if opt < 1 or opt > 3 then
        print("[rail] 曲线选项无效（1 梯形 / 2 三角形 / 3 S 曲线）")
        return 1
    end
    BASE(0)
    pcall(SPEED, 0, spd)
    pcall(ACCEL, 0, accl)
    pcall(DECEL, 0, accl)
    if SET_DEFAULTS then pcall(SET_DEFAULTS, 0) end      -- 同步内核缺省（后续移动沿用）
    if opt == 3 then
        pcall(SRAMP, 0, 50)                              -- S 曲线（50ms）
    else
        pcall(SRAMP, 0, 0)                               -- 梯形（短程自动成三角形）
    end
    local curve = (opt == 1) and "梯形" or (opt == 2) and "三角形(短程自动)" or "S 曲线(50ms)"
    print(string.format("[rail] 地轨参数应用：速度 %.3f mm/s、加减速度 %.1f mm/s²、曲线 %s", spd, accl, curve))
    return 1
end

-- ★开机重发地轨参数（2026-09-30 现场反馈"机器人走位不是 4x71/72 的速度"）：
--   NVRAM 恢复只还原寄存器；不重发的话内核缺省仍是 app.conf（100/500），
--   机器人/触摸屏移动（MOVEABS 速度=0）会按旧缺省走。仅当 71/72/73 有效时重发。
rail_apply_saved = function()
    if reg_get(71) > 0 and reg_get(72) > 0 and reg_get(73) >= 1 and reg_get(73) <= 3 then
        if rail_apply then rail_apply() end
    else
        print("[rail] 未配置（4x71/72/73），内核缺省保持 app.conf")
    end
end

local function mb_handle_pdu(tid, pid, uid, fc, pdu)
    if fc == 1 or fc == 2 then                               -- 读线圈 / 读离散输入（机器人区 1000~1099）
        if #pdu < 4 then return nil end
        local start, count = string.unpack(">I2I2", pdu, 1)
        if count < 1 or count > 2000 or not mb_bit_ok(start, count) then
            return mb_exception(tid, pid, uid, fc, 2)
        end
        if mb_dbg_first("rc" .. start .. ":" .. count) then
            print(string.format("[mbdbg] 主站#%d 读%s 0x%04X..0x%04X 共%d位",
                                mb_cur, (fc == 1) and "线圈" or "离散输入", start, start + count - 1, count))
        end
        local src = (fc == 1) and coils or dis
        local nbytes = math.floor((count + 7) / 8)
        local packed_bits = {}
        for i = 0, nbytes - 1 do packed_bits[i] = 0 end
        for i = 0, count - 1 do
            if (src[start + i] or 0) ~= 0 then
                local bi = math.floor(i / 8)
                packed_bits[bi] = packed_bits[bi] | (1 << (i % 8))
            end
        end
        local data = string.char(nbytes)
        for i = 0, nbytes - 1 do data = data .. string.char(packed_bits[i]) end
        local out = string.char(fc) .. data
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    elseif fc == 5 then                                      -- 写单个线圈（机器人区）
        if #pdu < 4 then return nil end
        local addr, val = string.unpack(">I2I2", pdu, 1)
        if not mb_bit_ok(addr, 1) then return mb_exception(tid, pid, uid, fc, 2) end
        if val ~= 0xFF00 and val ~= 0x0000 then return mb_exception(tid, pid, uid, fc, 3) end
        coil_set(addr, val == 0xFF00 and 1 or 0)
        if mb_dbg_first("wc" .. addr) then
            print(string.format("[mb] 主站#%d 写线圈 0x%04X = %s", mb_cur, addr, val == 0xFF00 and "ON" or "OFF"))
        end
        local out = string.char(fc) .. string.pack(">I2I2", addr, val)
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    elseif fc == 15 then                                     -- 写多个线圈（机器人区）
        if #pdu < 5 then return nil end
        local addr, count, bc = string.unpack(">I2I2B", pdu, 1)
        if count < 1 or count > 1968 or not mb_bit_ok(addr, count) then
            return mb_exception(tid, pid, uid, fc, 2)
        end
        if bc ~= math.floor((count + 7) / 8) or #pdu < 5 + bc then
            return mb_exception(tid, pid, uid, fc, 3)   -- 字节数/数据长度不符：非法数据值
        end
        for i = 0, count - 1 do
            local b = string.byte(pdu, 6 + math.floor(i / 8)) or 0
            coil_set(addr + i, (b >> (i % 8)) & 1)
        end
        if mb_dbg_first("wmc" .. addr .. ":" .. count) then
            print(string.format("[mb] 主站#%d 写线圈 0x%04X..0x%04X 共%d位",
                                mb_cur, addr, addr + count - 1, count))
        end
        local out = string.char(fc) .. string.pack(">I2I2", addr, count)
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    elseif fc == 3 then                                      -- 读保持寄存器
        if #pdu < 4 then return nil end
        local start, count = string.unpack(">I2I2", pdu, 1)
        if count < 1 or count > 125 or not mb_addr_ok(start, count) then
            return mb_exception(tid, pid, uid, fc, 2)
        end
        local dbg_show = mb_dbg_first("r" .. start .. ":" .. count)
        if dbg_show then
            local note = ""
            if start <= 10 and start + count >= 12 then
                note = string.format("；4x10 MPOS=%.3f，4x12 DPOS=%.3f", reg_get_f(10), reg_get_f(12))
            end
            print(string.format("[mbdbg] 主站#%d 读 4x%d..%d 共%d个%s", mb_cur, start, start + count - 1, count, note))
        end
        local bytes = {}
        for i = 0, count - 1 do
            local v = reg_get(start + i)
            bytes[#bytes + 1] = string.char((v >> 8) & 0xFF, v & 0xFF)
        end
        local data = string.char(count * 2) .. table.concat(bytes)
        if dbg_show then
            local hexs = {}
            for i = 1, math.min(#data, 20) do
                hexs[#hexs + 1] = string.format("%02X", string.byte(data, i))
            end
            print(string.format("[mbdbg]   → 发回 4x%d..%d 原始字节: %s",
                                start, start + count - 1, table.concat(hexs, " ")))
        end
        local out  = string.char(fc) .. data
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    elseif fc == 6 then                                      -- 写单个寄存器
        if #pdu < 4 then return nil end
        local addr, val = string.unpack(">I2I2", pdu, 1)
        if not mb_addr_ok(addr, 1) then return mb_exception(tid, pid, uid, fc, 2) end
        if mb_dbg_first("w6:" .. addr .. ":" .. val) then
            print(string.format("[mbdbg] 主站#%d 写 4x%d = %d（0x%04X）", mb_cur, addr, val, val))
        end
        if (addr >= 151 and addr <= 160) or (addr >= 201 and addr <= 203) or (addr >= 28 and addr <= 30) or addr == 213 or addr == 219 or addr == 229 or addr == 74 or addr == 75 or addr == 1080 or addr == 1082 or (addr >= 60 and addr <= 70) then
            if #mb_wq < 64 then mb_wq[#mb_wq + 1] = { addr, val } end
            print(string.format("[mb] HMI写 4x%d = %d（写入触发命令）", addr, val))
        end
        reg_set(addr, val)
        local out = string.char(fc) .. string.pack(">I2I2", addr, val)
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    elseif fc == 16 then                                     -- 写多个寄存器
        if #pdu < 5 then return nil end
        local addr, count, bc = string.unpack(">I2I2B", pdu, 1)
        if count < 1 or bc ~= count * 2 or #pdu < 5 + bc or not mb_addr_ok(addr, count) then
            return mb_exception(tid, pid, uid, fc, 2)
        end
        if mb_dbg_first("w16:" .. addr .. ":" .. count) then
            local vals = {}
            for i = 0, math.min(count, 6) - 1 do
                vals[#vals + 1] = tostring(string.unpack(">I2", pdu, 6 + i * 2))
            end
            print(string.format("[mbdbg] 主站#%d 写 4x%d..%d（%d个）: %s",
                                mb_cur, addr, addr + count - 1, count, table.concat(vals, ",")))
        end
        -- ★命令类寄存器逐次日志（2026-09-27）：便于现场分辨“按键没反应”是「无 0→1 边沿」还是「未收到写」
        if addr + count > 151 and addr <= 160 or addr + count > 201 and addr <= 203
           or addr + count > 28 and addr <= 30 or addr == 213 or addr == 219 or addr == 229
           or addr == 74 or addr == 75 or (addr <= 1082 and addr + count > 1080) or (addr + count > 60 and addr <= 70) then
            local vals = {}
            for i = 0, math.min(count, 8) - 1 do
                vals[#vals + 1] = tostring(string.unpack(">I2", pdu, 6 + i * 2))
            end
            for i = 0, count - 1 do
                local a = addr + i
                if (a >= 151 and a <= 160) or (a >= 201 and a <= 203) or (a >= 28 and a <= 30) or a == 213 or a == 219 or a == 229 or a == 74 or a == 75 or (a >= 1080 and a <= 1082) or (a >= 60 and a <= 70) then
                    if #mb_wq < 64 then
                        mb_wq[#mb_wq + 1] = { a, string.unpack(">I2", pdu, 6 + i * 2) }
                    end
                end
            end
            print(string.format("[mb] HMI写 4x%d..%d = %s（写入触发命令）",
                                addr, addr + count - 1, table.concat(vals, ",")))
        end
        for i = 0, count - 1 do
            reg_set(addr + i, string.unpack(">I2", pdu, 6 + i * 2))
        end
        local out = string.char(fc) .. string.pack(">I2I2", addr, count)
        return string.pack(">I2I2I2", tid, pid, #out + 1) .. string.char(uid) .. out
    end
    return mb_exception(tid, pid, uid, fc, 1)                -- 不支持的功能码
end

-- v0.8.6：把脚本用到的寄存器（输出区 + HMI 输入区）全量镜像到控制器侧（MODBUS_REG/MODBUS_IEEE），
-- 供插件 Modbus 面板「已用寄存器」监视。每 100ms 节流（TICKS 倒数计时），值取自 502 服务端 regs 真值。
local MIRROR_INT = {
    0, 3, 4, 9, 60, 61, 64, 65, 66, 67, 68, 69,
    120, 121, 130, 131, 134, 135,
    140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150,
}
for a = 151, 210 do MIRROR_INT[#MIRROR_INT + 1] = a end   -- 机器人命令/状态/标志/工程名/文本应答区 + 蠕动泵 4x202~210
local MIRROR_FLT = { 1, 5, 10, 12, 62, 122, 124, 126, 128, 132 }   -- 起始地址（各占 n/n+1）
local mirror_t0 = nil
local function mirror_used()
    local t = now()
    if mirror_t0 and (mirror_t0 - t) < 100 then return end
    mirror_t0 = t
    for i = 1, #MIRROR_INT do
        local a = MIRROR_INT[i]
        MODBUS_REG(a, reg_get(a))
    end
    for i = 1, #MIRROR_FLT do
        local a = MIRROR_FLT[i]
        MODBUS_IEEE(a, reg_get_f(a))
    end
end

local function modbus_step()
    -- 打开/重试监听 502（无 CAP_NET_BIND_SERVICE 时权限不足，明确报错不伪装；5s 重试）
    if modbus_state.opened == 0 then
        if elapsed(modbus_state.logt) <= 5000 then return end
        modbus_state.logt = now()
        local ok, err = pcall(OPEN, MB_CH, "TCP_SERVER", MB_PORT)
        if ok then
            modbus_state.opened = 1
            print("[mb] Modbus-TCP 从站已监听 502（站号 " .. MB_STATION .. "）")
        else
            print("[mb] 502 监听失败：" .. tostring(err) ..
                  "（5s 后重试；需 systemd AmbientCapabilities=CAP_NET_BIND_SERVICE）")
            return
        end
    end

    -- 连接状态变化（多客户端：触摸屏/机器人等可同时连 502，各自独立会话）
    local ncl = PORT_CLIENTS(MB_CH)                         -- 顺带推进 accept
    if ncl ~= (modbus_state.nclold or 0) then
        modbus_state.nclold = ncl
        if ncl > 0 then
            print("[mb] 主站已连接（" .. tostring(PORT_TARGET(MB_CH)) .. "，客户端 " ..
                  math.floor(ncl) .. " 个）")
        else
            print("[mb] 主站断开，等待重连")
            for ci = 0, 3 do modbus_state.rx[ci] = "" end
        end
    end

    local rxbytes = 0
    for ci = 0, ncl - 1 do
        if PORT_STATUS(MB_CH, ci) == 1 then
            local chunk = recv_str(MB_CH, 256, ci)
            if #chunk > 0 then
                modbus_state.rx[ci] = (modbus_state.rx[ci] or "") .. chunk
                -- 防异常客户端把缓冲撑爆
                if #modbus_state.rx[ci] > 4096 then modbus_state.rx[ci] = "" end
            end
        elseif (modbus_state.rx[ci] or "") ~= "" then
            modbus_state.rx[ci] = ""                         -- 客户端断开：清其缓冲
        end
        rxbytes = rxbytes + #(modbus_state.rx[ci] or "")
    end

    for ci = 0, ncl - 1 do
        mb_cur = ci                                          -- 本客户端（响应写回用）
        local buf = modbus_state.rx[ci] or ""
        local pos = 1
        while #buf - pos + 1 >= 8 do
            local tid, pid, len, uid, fc = string.unpack(">I2I2I2BB", buf, pos)
            local total = 6 + len
            if len < 2 or len > 260 then
                buf = ""                                        -- 非法长度，整包丢弃
                break
            end
            if #buf - pos + 1 < total then break end            -- 半包，等下一批
            local pdu = string.sub(buf, pos + 8, pos + total - 1)   -- 含 fc
            pos = pos + total

            if uid ~= MB_STATION then
                -- 非本站号：忽略（不破坏流边界）
                modbus_state.bad_uid = modbus_state.bad_uid + 1
            else
                local resp = mb_handle_pdu(tid, pid, uid, fc, pdu)
                if resp then
                    PORT_PRINT(MB_CH, resp, ci)
                    modbus_state.frames = modbus_state.frames + 1
                    modbus_state.lastfc = fc
                end
            end
        end
        modbus_state.rx[ci] = string.sub(buf, pos)
    end
    mb_cur = 0

    -- 每 200 帧一条汇总日志（≈40s@5Hz）：现场确认触摸屏轮询被正常应答
    if modbus_state.frames - modbus_state.logged >= 200 then
        modbus_state.logged = modbus_state.frames
        print(string.format("[mb] 已应答 %d 帧（最近 FC%d，非本站号 %d 帧，客户端 %d，缓冲 %d 字节）",
                            modbus_state.frames, modbus_state.lastfc, modbus_state.bad_uid,
                            ncl, rxbytes))
    end

    -- ---- HMI 业务逻辑（等价旧 modbus_task）----
    -- 输入区：4x0 int16 / 4x1 float32 -> hmi 值 + 新数据标志 + 计数
    local rd = reg_get(0)
    if rd ~= hmi_val[1] then
        hmi_val[1] = rd
        hmi_flag[1] = 1
        hmi_cnt[1] = hmi_cnt[1] + 1
    end
    local rf = reg_get_f(1)
    if rf ~= hmi_val[2] then
        hmi_val[2] = rf
        hmi_flag[2] = 1
        hmi_cnt[2] = hmi_cnt[2] + 1
    end
    -- 回写区：4x4 = 4x0 原值；4x5/4x6 = 4x1 原值
    reg_set(4, hmi_val[1])
    reg_set_f(5, hmi_val[2])

    -- 读区：状态字/轴数/位置
    -- 使能位与内核真实状态对齐（掉线/报警时内核会自动去使能，HMI 不能显示"已使能"残留）
    local en_now = (pcall(ENABLED) and ENABLED() == 1) and 1 or 0
    if en_now ~= wdog then wdog = en_now end
    local moving = (pcall(BUSY) and BUSY() == 1) and 1 or 0
    local alarm  = ((pcall(AXISSTATUS, 0) and AXISSTATUS(0) or 0) & MV_ERRMASK) ~= 0 and 1 or 0
    -- ★4x3 数值状态码（2026-09-29 现场定义，取代旧位域；屏端「多状态指示」用）：
    --   0 总线未就绪 / 1 就绪·未使能 / 2 就绪·已使能(空闲) / 3 运行中 / 4 报警 / 5 安全联锁(机器人急停/故障)
    local mb_state
    if robot_axis and robot_axis.safe == 1 then
        mb_state = 5
    elseif bus_ok ~= 1 then
        mb_state = 0
    elseif alarm == 1 then
        mb_state = 4
    elseif moving == 1 then
        mb_state = 3
    elseif wdog == 1 then
        mb_state = 2
    else
        mb_state = 1
    end
    reg_set(3, mb_state)
    reg_set(9, 1)                                               -- 单轴
    if MB_TEST_POS then
        reg_set_f(10, MB_TEST_POS)                              -- 调试：固定测试值（确认屏端解码后改回 nil）
    elseif pcall(MPOS, 0) then
        reg_set_f(10, MPOS(0))
    end
    if pcall(DPOS, 0) then reg_set_f(12, DPOS(0)) end

    -- 读区：称重
    local s = scale_state or {}
    local scst = 0
    if s.ok == 1 then scst = scst + 1 end
    if s.valid == 1 then scst = scst + 2 end
    if s.ok ~= 1 and (s.err or 0) > 0 then scst = scst + 4 end
    if s.conn == 1 then scst = scst + 8 end
    if s.result == 1 then scst = scst + 16 end
    if s.result == 2 then scst = scst + 32 end
    if s.result == 3 then scst = scst + 64 end
    reg_set(120, scst)
    reg_set(121, s.err or 0)
    reg_set_f(122, s.w or 0)
    reg_set_f(124, s.raw or 0)
    reg_set_f(126, s.ad or 0)
    reg_set_f(128, s.zero or 0)
    reg_set(135, s.result or 0)
    -- ★4x136 称重状态码（多状态屏显用；0~7，2026-09-27）：优先“出错/未连接”，命令态保持 2s 后回落常态
    local scode = 0
    local res_show = 0
    if s.result == 1 then
        res_show = 5                                  -- 命令执行中
    elseif s.result == 2 or s.result == 3 then
        if (s.res_t or 0) == 0 then s.res_t = now() end
        if elapsed(s.res_t) <= 2000 then
            res_show = (s.result == 2) and 6 or 7     -- 命令成功 / 命令失败（保持 2s）
        end
    end
    if (s.result or 0) ~= 2 and (s.result or 0) ~= 3 then s.res_t = 0 end
    if s.ok ~= 1 and (s.err or 0) > 0 then scode = 2            -- 通讯出错/超时
    elseif (s.conn or 0) ~= 1 then scode = 1                    -- 未连接
    elseif res_show ~= 0 then scode = res_show
    elseif s.ok == 1 and s.valid == 1 then scode = 4            -- 数据有效（常态）
    elseif s.ok == 1 then scode = 3                             -- 已连接，等数据
    else scode = 0 end
    reg_set(136, scode)
    reg_set(29, s.cal_idx or 0)                       -- ★折线校准：已提交点数
    reg_set(31, s.cal_result or 0)                    -- ★折线校准：结果码

    -- 读区：机器人（4x140~150 由 robot_bridge_step 维护；2026-09-28 改 Modbus 直连）

    -- 写区：称重参数与命令
    local scpar = reg_get(130)
    if scpar >= 1 and scpar <= 247 and scpar ~= s.stasave then
        s.station = scpar
        s.stasave = scpar
        print("[mb] HMI称重:模块站号改为 " .. tostring(scpar))
    end
    local poll = reg_get(131)
    if poll < 20 or poll > 5000 then poll = SCALE_POLL_MS end
    s.poll = poll
    local div = reg_get_f(132)
    if div > 0.000001 and div ~= s.divsave then
        s.div = div
        s.divsave = div
        print("[mb] HMI称重:比例系数改为 " .. tostring(div))
    end
    local sccmd = reg_get(134)
    if sccmd > 0 and s.cmdold == 0 then
        if scale_do_cmd then scale_do_cmd(sccmd) end
    end
    s.cmdold = sccmd

    -- 写区：触摸屏命令字（上升沿）
    local cmdw = reg_get(60)
    if (cmdw & 1) ~= 0 and (mb_cmdold & 1) == 0 then
        request_axis_enable("4x60 命令字")
    end
    if (cmdw & 2) ~= 0 and (mb_cmdold & 2) == 0 then
        wdog = 0
        BASE(0)
        pcall(DISABLE)
        print("[mb] HMI命令:轴0去使能")
    end
    if (cmdw & 4) ~= 0 and (mb_cmdold & 4) == 0 then
        RAPIDSTOP(0)   -- Kine-X: 参数是轴号（0=本机唯一轴）；ZBasic 的 mode 2=全停
        if robot_bridge_estop then robot_bridge_estop() end   -- ★机器人软急停（ctrl_soft_estop=0 脉冲）
        print("[mb] HMI命令:急停")
    end
    if (cmdw & 8) ~= 0 and (mb_cmdold & 8) == 0 then
        rescan_flag = 1
        print("[mb] HMI命令:重扫总线")
    end
    mb_cmdold = cmdw

    -- 运动触发（上升沿）
    local trig = reg_get(66)
    if trig ~= 0 and mb_trigold == 0 then
        if bus_ok == 1 then
            local ax = reg_get(61)
            local spd = reg_get(64) / 10                  -- 旧约定：int16=显示值×10
            if spd > MV_SPD_MAX or spd < 0 then spd = MV_SPD_MAX end
            if spd < 1 then                               -- 兼容屏端 32 位浮点（低字在前）写 4x64/65
                local f = reg_get_f(64)
                if f > 0 and f <= MV_SPD_MAX then spd = f end
            end
            local mode = reg_get(65)
            local tgt = reg_get_f(62)
            if ax == 0 then
                if spd >= 1 then SPEED(ax, spd) end
                if mode == 1 then
                    pcall(MOVE, tgt, 0, 0, 0)
                    print(string.format("[mb] HMI命令:相对运动 位置%.3f 速度%.3f", tgt, spd))
                else
                    pcall(MOVEABS, tgt, 0, 0, 0)
                    print(string.format("[mb] HMI命令:绝对运动 位置%.3f 速度%.3f", tgt, spd))
                end
            else
                print("[mb] HMI命令:轴号非0(单轴),忽略运动")
            end
        else
            print("[mb] HMI命令:总线未就绪,忽略运动")
        end
    end
    mb_trigold = trig

    -- 点动（上升沿启动/下降沿停止）
    local jog = reg_get(67)
    if jog == 1 and mb_jogold == 0 then
        local ax = reg_get(61)
        local dirw = reg_get(68)                      -- 屏端 int16：1=正 / -1=负（-1 → 65535）
        local dir = (dirw > 32767) and (dirw - 65536) or dirw   -- ★v0.8.9 修复：符号还原（此前恒正转）
        local spd = reg_get(69) / 10                  -- 旧约定：int16=显示值×10
        if spd > MV_SPD_MAX or spd < 0 then spd = MV_SPD_MAX end
        if spd < 1 then                               -- 兼容屏端 32 位浮点（低字在前）写 4x69/70
            local f = reg_get_f(69)
            if f > 0 and f <= MV_SPD_MAX then spd = f end
        end
        if bus_ok == 1 and ax == 0 then
            if spd < 1 then spd = MV_SPEED end
            pcall(JOGLEAD, ax, JOG_LEAD_S)            -- ★确保点动前视已设置（PP 跟随无固件默认值）
            SPEED(ax, spd)
            pcall(VMOVE, dir < 0 and -1 or 1)
            print(string.format("[mb] HMI命令:点动 方向%d 速度%.3f", dir, spd))
        end
    elseif jog == 0 and mb_jogold == 1 then
        local ax = reg_get(61)
        if ax == 0 then
            BASE(ax)
            pcall(CANCEL, 2)
            print("[mb] HMI命令:停止点动")
        end
    end
    mb_jogold = jog

    -- 写区：4x151~4x155 成对命令（值变化触发：0→1 下发 on、1→0 下发 off）
    -- ★① 写队列（按写入顺序派发；同循环内 1→0 成对各自按原值处理，2026-09-27）
    if #mb_wq > 0 then
        for qi = 1, #mb_wq do
            local it = mb_wq[qi]
            local m  = REG_CMD_BYREG[it[1]]
            if m then
                cmd_apply_write(m.e, it[2], m.toggle)
            elseif it[1] == 203 and it[2] ~= 0 and pump_cmd_apply then
                pump_cmd_apply(it[2])                       -- ★4x203 泵命令码（写入即触发，2026-09-27）
                reg_set(203, 0)                             -- ★处理完自动回 0（屏「写常数」按钮连按可靠，2026-09-28）
                if pump_state then pump_state.cmdold = 0 end
            elseif it[1] == 30 and it[2] ~= 0 and scale_calib_submit then
                scale_calib_submit(it[2])                   -- ★4x30 折线校准提交（写入即触发，2026-09-28）
            elseif it[1] == 213 and it[2] ~= 0 and pump_rev_run then
                pump_rev_run()                              -- ★4x213 按圈数运行（写入即触发，2026-09-28）
                reg_set(213, 0)                             -- 处理完自动回 0
                if pump_state then pump_state.revold = 0 end
            elseif it[1] == 219 and it[2] ~= 0 and pump_rev_cfg_apply then
                pump_rev_cfg_apply()                        -- ★4x219 回吸参数提交（写入即触发，2026-09-28）
                reg_set(219, 0)
                if pump_state then pump_state.revcfg_old = 0 end
            elseif it[1] == 229 and it[2] ~= 0 and pump_run_by_volume then
                pump_run_by_volume()                        -- ★4x229 容积运行（写入即触发，2026-09-28）
                reg_set(229, 0)
                if pump_state then pump_state.volold = 0 end
            elseif it[1] == 1080 and robot_axis_apply then
                -- ★4x1080 目标位置变化即触发（适应机器人主站“周期刷新写”风格）
                --   ★2026-09-29：上次命令被拒/中止时（last_ok=false），同值重发也触发（可原地重试）
                robot_axis.armed = true
                if it[2] ~= (mb_tgtold or 0) or robot_axis.last_ok ~= true then
                    robot_axis_apply()
                    mb_tgtold = it[2]
                end
            elseif it[1] == 1082 then
                -- ★4x1082 触发（2026-09-29）：0→1 沿 → 排程（防抖 100ms）；
                --   现场实测机器人以 ~10Hz 周期刷新 1082=1 → 电平刷新不重复触发（仅 0→1 沿）
                if it[2] == 1 and (robot_axis.trig or 0) ~= 1 then
                    robot_axis_sched()
                end
                robot_axis.trig = it[2]
            elseif it[1] == 74 and it[2] ~= 0 and rail_apply then
                rail_apply()                                -- ★4x74 地轨参数「确定」（写入即触发）
                reg_set(74, 0)
            elseif it[1] == 75 and it[2] ~= 0 then
                request_axis_enable("4x75 按钮")            -- ★4x75 轴使能按钮（写入即触发）
                reg_set(75, 0)                              -- 处理完自动回 0
            end
        end
        for i = #mb_wq, 1, -1 do mb_wq[i] = nil end                 -- 原地清空（保持表引用，测试钩子/句柄共用）
    end
    -- ★② 值变化兜底（自测/内部直接 reg_set 的路径；队列已处理过的值不会重复触发）
    for i = 1, #REG_CMD_TOGGLE do
        local e = REG_CMD_TOGGLE[i]
        local v = reg_get(e.reg) & 1
        if v ~= (mb_rboldcmd[e.reg] or 0) then cmd_apply_write(e, v, true) end
    end
    for i = 1, #REG_CMD_PULSE do
        local e = REG_CMD_PULSE[i]
        local v = reg_get(e.reg)
        if v ~= 0 and (mb_rboldcmd[e.reg] or 0) == 0 then cmd_apply_write(e, v, false) end
        mb_rboldcmd[e.reg] = v
    end
    -- ★持久化保存（变化即存；NVSET 立即原子落盘）：屏输入类寄存器（2026-09-28）
    if NVSET then
        for i = 1, #NVRAM_REGS do
            local r = NVRAM_REGS[i]
            local v = reg_get(r) & 0xFFFF
            if v ~= nvr_last[r] then
                nvr_last[r] = v
                NVSET(r, v)
            end
        end
    end
    -- ★折线校准提交（4x30）值边沿兜底：仅在写队列之后（队列已处理的写入不会重复触发）
    local c30 = reg_get(30)
    if c30 ~= 0 and (mb_calold or 0) == 0 and scale_calib_submit then
        scale_calib_submit(c30)
    end
    mb_calold = reg_get(30)
    -- ★4x1080 目标位置变化即触发（值边沿兜底；正常写入走写队列）
    local c_tgt = reg_get(1080)
    if c_tgt ~= (mb_tgtold or 0) then
        mb_tgtold = c_tgt
        robot_axis.armed = true
        if robot_axis_apply then robot_axis_apply() end
    end
    -- ★4x1082 机器人轴0定位触发（值边沿兜底，仅 0→1 沿；正常写入走写队列）
    --   注意：仅在寄存器值**变化**时判定，避免覆盖写队列路径刚置的 trig（否则会重复触发）
    local c_ax = reg_get(1082)
    if c_ax ~= (mb_axold or 0) then
        if c_ax == 1 and (robot_axis.trig or 0) ~= 1 and robot_axis_sched then
            robot_axis_sched()
        end
        robot_axis.trig = c_ax
        mb_axold = c_ax
    end
    -- ★4x1082 防抖执行：到期且 1080 未变化 → 以当前目标触发（1082 先到、1080 后变时自动取消）
    if (robot_axis.pend_t or 0) ~= 0 and elapsed(robot_axis.pend_t) >= 0 then
        robot_axis.pend_t = 0
        if robot_axis.armed and reg_get(1080) == (robot_axis.pend_tgt or -1) then
            robot_axis_apply()
        end
    end
    -- ★4x74 地轨参数「确定」（值边沿兜底；正常写入走写队列）
    local c74 = reg_get(74)
    if c74 ~= 0 and (mb_railold or 0) == 0 and rail_apply then
        rail_apply()
        reg_set(74, 0)
        c74 = 0
    end
    mb_railold = c74
    -- ★4x75 轴使能按钮（值边沿兜底；正常写入走写队列）
    local c75 = reg_get(75)
    if c75 ~= 0 and (mb_enold or 0) == 0 then
        request_axis_enable("4x75 按钮")
        reg_set(75, 0)
        c75 = 0
    end
    mb_enold = c75
    if #mb_rbpend > 0 then
        local it = table.remove(mb_rbpend, 1)             -- 发送槽空闲才取下一个（逐拍下发）
        local e, v = it[1], it[2]
        local ret = 1
        if robot_bridge_cmd then ret = robot_bridge_cmd(e, v) end
        print("[mb] 机器人命令 4x" .. e.reg .. " 值=" .. v .. " 结果码=" .. ret)
    end

    -- 兼容调试面板：关键量每循环镜像 + 全量「已用寄存器」镜像（100ms 节流，v0.8.6）
    MODBUS_REG(3, mb_state)
    MODBUS_REG(120, scst)
    MODBUS_REG(136, scode)
    if pcall(MPOS, 0) then MODBUS_IEEE(10, MPOS(0)) end
    if pcall(DPOS, 0) then MODBUS_IEEE(12, DPOS(0)) end
    MODBUS_IEEE(122, s.w or 0)
    mirror_used()
end

