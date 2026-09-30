-- ============================================================
-- kx_pump.lua — 蠕动泵（保定创锐 M 基本型）
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_pump.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== 蠕动泵（保定创锐 M 基本型；RS485 Modbus-RTU 从站） ========================
-- 链路与称重一致：TCP 客户端 → 透传网关（192.168.1.81:10123），线上就是 485 总线原始字节
-- （站号 + 功能码 + 数据 + CRC16 低字节在前）——见根《保定创锐蠕动泵通讯规约_学习笔记.md》。
-- 泵侧要点：8N1/9600、地址 1~31（出厂 1）；**每次上电必须先写保持寄存器 0=1 使能 485 控制**
-- （掉电不存储；未使能时写操作回异常 04 且 reg50=3）；FLOAT **高字在前**（与触摸屏 502 低字在前相反，
-- 本框架在收发两侧转换）；业务错误码统一读**输入寄存器 50**。
-- 已实现（框架）：连接/重连、485 使能、状态轮询、命令位（启动/停止/写转速/重新使能/全速开/全速关）、
--   PSTA 诊断、寄存器镜像（4x202~210）。
-- TODO（待现场核对后填）：分配模式（灌装量/时间/次数）、校准、回吸、方向设置（手册疑点②）、
--   reg6~11 语义（疑点①）、泵头/泵管型号（reg2/3）。

local function pump_f32(v)                        -- 泵 FLOAT：高字在前（big-endian）
    return string.pack(">f", v or 0)
end
local function pump_f32_at(rx, i)                 -- 解析泵 FLOAT（i 为 1 基偏移）
    local s4 = string.sub(rx, i, i + 3)
    if #s4 < 4 then return 0.0 end
    return (string.unpack(">f", s4))
end

-- 帧构建（站号 + 功能码 + 数据 + CRC 低字节在前；crc16/rtu_frame 与称重共用）
local function pump_w6(addr, val)                 -- 0x06 写单个保持寄存器
    return rtu_frame(string.char(PUMP_STATION, 0x06, addr >> 8, addr & 0xFF, val >> 8, val & 0xFF))
end
local function pump_w10(addr, cnt, data)          -- 0x10 写多个保持寄存器（data 已按泵字序拼好）
    return rtu_frame(string.char(PUMP_STATION, 0x10, addr >> 8, addr & 0xFF,
                                 cnt >> 8, cnt & 0xFF, #data) .. data)
end
local function pump_coil(addr, on)                -- 0x05 写单个线圈（FF00=ON / 0000=OFF）
    local v = on and 0xFF00 or 0x0000
    return rtu_frame(string.char(PUMP_STATION, 0x05, addr >> 8, addr & 0xFF, v >> 8, v & 0xFF))
end
local function pump_rd_inputs(addr, cnt)          -- 0x04 读输入寄存器（状态 / 错误码）
    return rtu_frame(string.char(PUMP_STATION, 0x04, addr >> 8, addr & 0xFF, cnt >> 8, cnt & 0xFF))
end
local function pump_rd_coils(addr, cnt)           -- 0x01 读线圈（启停/全速/校准）
    return rtu_frame(string.char(PUMP_STATION, 0x01, addr >> 8, addr & 0xFF, cnt >> 8, cnt & 0xFF))
end

local function pump_rd_hold(addr, cnt)            -- 0x03 读保持寄存器（回吸参数等）
    return rtu_frame(string.char(PUMP_STATION, 0x03, addr >> 8, addr & 0xFF, cnt >> 8, cnt & 0xFF))
end

pump_state = {
    opened = 0, conn = 0, connold = 0, ok = 0, err = 0, reconn = 0, try = 0,
    send = 0, t0 = 0, lastpoll = 0, logt = 0, tocnt = 0, badcnt = 0, ent0 = 0, entn = 0,
    rev_end = 0,                                   -- ★圈数运行截止时刻（TICKS 倒数：now()-ms）
    rx = "", queue = {}, qn = 0,
    pend_fc = 0, pend_addr = 0, pend_cnt = 0,      -- 在途请求（应答归属）
    -- 读回（泵侧状态）
    ctl = 0, dir = 0, rpm = 0.0, flow = 0.0, run = 0, errcode = 0, exc = 0,
    rev_angle = 0, rev_speed = 0,                  -- ★回吸参数读回（reg7/reg8，2026-09-28）
    rev_angle_disp = nil, rev_speed_disp = nil,    -- 上次镜像到 4x215/217 的值（变化才刷新，避免覆盖 HMI 输入）
    -- 写区
    cmdold = 0, revcfg_old = 0, volold = 0,
}

-- 入队（item = {f=帧, fc=功能码, addr=, cnt=}；帧归属随条目走；上限 16 防堆积）
local function pump_push(frame, fc, addr, cnt)
    local p = pump_state
    if p.qn >= 16 then
        print("[pump] 命令队列满，丢弃本帧")
        return
    end
    p.qn = p.qn + 1
    p.queue[p.qn] = { f = frame, fc = fc or 0, addr = addr or 0, cnt = cnt or 0 }
end

-- 发送队首（调用方保证链路已连、发送槽空闲）
local function pump_flush()
    local p = pump_state
    if p.qn <= 0 or p.send == 1 then return end
    local it = table.remove(p.queue, 1)
    p.qn = p.qn - 1
    p.pend_fc   = it.fc
    p.pend_addr = it.addr
    p.pend_cnt  = it.cnt
    PORT_PRINT(PUMP_CH, it.f)
    p.send = 1
    p.t0 = now()
    if PUMP_DBG == 1 then
        local hx = {}
        for i = 1, #it.f do hx[#hx + 1] = hex(string.byte(it.f, i)) end
        print("[pump] TX " .. table.concat(hx, " "))
    end
end

-- 解析一帧（已含 CRC；f 为帧字符串）。返回 1=已处理 / 0=丢弃（调用方负责按长度切帧）
pump_handle_frame = function(f)
    local p = pump_state
    local b1, b2 = string.byte(f, 1, 2)
    if b1 ~= PUMP_STATION then return 0 end
    if b2 >= 0x80 then                              -- 异常响应：业务错误码再读输入寄存器 50
        p.exc = string.byte(f, 3) or 0
        p.errcode = -1                              -- 待 reg50 刷新
        if p.exc ~= 0 then
            print("[pump] 异常响应：功能码" .. (b2 - 0x80) .. " 异常码" .. p.exc .. "（读 reg50 查业务错误码）")
        end
        pump_push(pump_rd_inputs(50, 1), 0x04, 50, 1)
        return 1
    end
    if b2 == 0x04 then                              -- 读输入寄存器应答
        local n = string.byte(f, 3) or 0
        if p.pend_fc == 0x04 and p.pend_addr == 0 and p.pend_cnt >= 6 then
            -- ★按寄存器对齐解析（每寄存器 2 字节；2026-09-27 修复：原先按紧凑字节取，方向/转速/流速错位）
            p.ctl  = ((string.byte(f, 4) or 0) * 256) + (string.byte(f, 5) or 0)   -- reg0 控制方式（2=485 通讯控）
            p.dir  = ((string.byte(f, 6) or 0) * 256) + (string.byte(f, 7) or 0)   -- reg1 方向（1=左/2=右）
            p.rpm  = pump_f32_at(f, 8)              -- reg2~3 转速 rpm（泵高字在前=大端）
            p.flow = pump_f32_at(f, 12)             -- reg4~5 流速 ml/min
        elseif p.pend_fc == 0x04 and p.pend_addr == 50 then
            p.errcode = (string.byte(f, 4) or 0) * 256 + (string.byte(f, 5) or 0)   -- reg50 业务错误码
        end
        p.ok = 1
        p.exc = 0
        return 1
    end
    if b2 == 0x01 then                              -- 读线圈应答
        if p.pend_fc == 0x01 and p.pend_addr == 1 then
            p.run = (string.byte(f, 4) or 0) & 0x01   -- 线圈1 = 启停
        end
        p.ok = 1
        return 1
    end
    if b2 == 0x03 then                              -- 读保持寄存器应答（回吸参数）
        if p.pend_fc == 0x03 and p.pend_addr == 7 and p.pend_cnt >= 2 then
            p.rev_angle = ((string.byte(f, 4) or 0) * 256) + (string.byte(f, 5) or 0)   -- reg7 回吸角度
            p.rev_speed = ((string.byte(f, 6) or 0) * 256) + (string.byte(f, 7) or 0)   -- reg8 回吸速度
        end
        p.ok = 1
        return 1
    end
    if b2 == 0x05 or b2 == 0x06 or b2 == 0x10 then  -- 写应答（原样回显）
        p.ok = 1
        return 1
    end
    return 0
end

-- ★4x203 泵命令码（数值命令，写入即触发；2026-09-27）：
--   1 启动 / 2 停止 / 3 写转速（取 4x204）/ 4 重新使能 485 / 5 全速 ON / 6 全速 OFF
pump_cmd_apply = function(v)
    if v == 1 then
        pump_push(pump_coil(1, true), 0x05, 1, 1)
    elseif v == 2 then
        if pump_state then pump_state.rev_end = 0 end      -- 手动停止 → 取消圈数计时
        pump_push(pump_coil(1, false), 0x05, 1, 1)
    elseif v == 3 then
        local rpm = reg_get(204)
        if rpm < 1 or rpm > 300 then
            print("[pump] 写转速：4x204=" .. tostring(rpm) .. " 无效（需 1~300，且 4x204 必须配 INT16）")
            return -1
        end
        pump_push(pump_w10(16, 2, pump_f32(rpm)), 0x10, 16, 2)
    elseif v == 4 then
        pump_push(pump_w6(0, 1), 0x06, 0, 1)
    elseif v == 5 then
        pump_push(pump_coil(2, true), 0x05, 2, 1)
    elseif v == 6 then
        pump_push(pump_coil(2, false), 0x05, 2, 1)
    else
        print("[pump] 未知命令码 " .. tostring(v) .. "（有效 1~6），忽略")
        return -1
    end
    print("[pump] HMI命令码 " .. tostring(v))
    return 1
end

-- ★持久化恢复（NVSET/NVGET，2026-09-28）：把 .nvram 里保存的屏输入寄存器写回（导出供自测）
nvram_restore = function()
    if not NVGET then return end
    local nz = 0
    for i = 1, #NVRAM_REGS do
        local r = NVRAM_REGS[i]
        local v = (NVGET(r) or 0) & 0xFFFF
        reg_set(r, v)
        nvr_last[r] = v
        if v ~= 0 then nz = nz + 1 end
    end
    print("[init] NVRAM 恢复 " .. #NVRAM_REGS .. " 个寄存器（非零 " .. nz .. " 项）")
end

-- ★回吸参数提交（HMI：4x215 角度、4x217 速度、4x219 写 1 提交；2026-09-28）：
--   下发泵 reg7（0=无回吸；10~720°）与 reg8（10~300rpm），存 4x215/4x217 读回显示
pump_rev_cfg_apply = function()
    local p = pump_state
    if (p.conn or 0) ~= 1 or p.ok ~= 1 then
        print("[pump] 回吸参数：泵未连接/无通讯，忽略")
        return -1
    end
    local ang = reg_get(215)
    local spd = reg_get(217)
    if not (ang == 0 or (ang >= 10 and ang <= 720)) then
        print("[pump] 回吸角度无效（" .. tostring(ang) .. "，0=关闭 或 10~720）")
        return -1
    end
    if spd < 10 or spd > 300 then
        print("[pump] 回吸速度无效（" .. tostring(spd) .. "，10~300）")
        return -1
    end
    pump_push(pump_w6(7, ang), 0x06, 7, 1)
    pump_push(pump_w6(8, spd), 0x06, 8, 1)
    print("[pump] 回吸参数提交：角度 " .. tostring(ang) .. "°、速度 " .. tostring(spd) .. "rpm")
    return 1
end

-- ★按圈数运行的公共起步：定时 + 启动线圈（4x213 圈数运行 / 4x229 容积运行共用；2026-09-28）
local function pump_start_revs(revs, tag)
    local p = pump_state
    local rpm = reg_get(204)
    local write_spd = false
    if rpm >= 1 and rpm <= 300 then
        write_spd = true                                -- ★启动前统一下发设定转速，保证与计时一致（2026-09-28）
    else
        rpm = math.floor((p.rpm or 0) + 0.5)
    end
    if rpm < 1 or rpm > 300 then
        print("[pump] " .. tag .. "：转速无效（4x204=" .. tostring(reg_get(204)) ..
              "，泵读回=" .. tostring(p.rpm) .. "），忽略")
        return nil
    end
    local ms = math.floor(revs * 60000.0 / rpm + 0.5)
    if ms > 3600000 then ms = 3600000 end
    if write_spd then
        pump_push(pump_w10(16, 2, pump_f32(rpm)), 0x10, 16, 2)   -- 先写 485 转速（reg16~17）
    end
    p.rev_end = now() - ms                              -- TICKS 倒数：截止时刻 = now()-ms
    pump_push(pump_coil(1, true), 0x05, 1, 1)
    return rpm, ms
end

-- ★输送管数量（4x214；2026-09-30 用户要求）：
--   并联 N 管时总流量 = 单管 ml/圈 × N；用于圈数↔容积换算（4x223 ×N / 4x227 ÷N / 4x229 容积运行 ÷N）
--   取值 1~99，非法按 1（不阻塞运行）；掉电保持（NVRAM_REGS）
pump_tubes = function()
    local n = reg_get(214)
    if n < 1 or n > 99 then n = 1 end
    return n
end

-- ★按容积运行（HMI：4x225 设定容积 ml、4x221 ml/圈、4x229 写 1 启动；2026-09-28）：
--   圈数 = 容积 ÷ ml/圈（3 位小数规整）÷ 输送管数量(4x214)，运行时间 = 圈数 ÷ 转速 × 60s；到时自动停
pump_run_by_volume = function()
    local p = pump_state
    if (p.rev_end or 0) ~= 0 then
        print("[pump] 运行进行中，忽略容积运行")
        return -1
    end
    if (p.conn or 0) ~= 1 or p.ok ~= 1 then
        print("[pump] 容积运行：泵未连接/无通讯，忽略")
        return -1
    end
    local vol = (reg_get_f(225) or 0) * VOL_IN_SCALE    -- ★屏元件 ×10 → ÷10 还原（见 VOL_IN_SCALE）
    local mlrev = reg_get_f(221) or 0
    mlrev = math.floor(mlrev * 1000 + 0.5) / 1000
    if vol <= 0.001 then
        print("[pump] 容积运行：设定容积无效（" .. tostring(vol) .. " ml）")
        return -1
    end
    if mlrev <= 0.0005 then
        print("[pump] 容积运行：未设置 ml/圈（4x221=0）")
        return -1
    end
    local tubes = pump_tubes()                         -- ★输送管数量（4x214）
    local revs = vol / mlrev / tubes
    local rpm, ms = pump_start_revs(revs, "容积运行")
    if not rpm then return -1 end
    print("[pump] 容积运行：" .. tostring(vol) .. " ml ÷ " .. tostring(mlrev) .. " ml/圈 ÷ " ..
          tostring(tubes) .. " 管 = " ..
          string.format("%.1f", revs) .. " 圈 @" .. tostring(rpm) .. "rpm ≈ " ..
          string.format("%.1f", ms / 1000.0) .. "s")
    return 1
end

-- ★按圈数运行（HMI：4x211 圈数 float32、4x213 写 1 启动；2026-09-28）：
--   运行时间 = 圈数 ÷ 转速（rpm，取 4x204，未设则用泵读回）× 60s；到时自动发停止
pump_rev_run = function()
    local p = pump_state
    if (p.rev_end or 0) ~= 0 then
        print("[pump] 圈数运行进行中，忽略重复启动")
        return -1
    end
    if (p.conn or 0) ~= 1 or p.ok ~= 1 then
        print("[pump] 圈数运行：泵未连接/无通讯，忽略")
        return -1
    end
    local revs = reg_get_f(211) or 0
    if revs <= 0.001 then
        print("[pump] 圈数运行：圈数无效（" .. tostring(revs) .. "），忽略")
        return -1
    end
    if revs > 10000 then revs = 10000 end
    local rpm, ms = pump_start_revs(revs, "圈数运行")
    if not rpm then return -1 end
    local mlrev = reg_get_f(221) or 0
    local tubes = pump_tubes()
    print("[pump] 圈数运行：" .. tostring(revs) .. " 圈 @" .. tostring(rpm) ..
          "rpm ≈ " .. string.format("%.2f", ms / 1000.0) .. "s（约 " ..
          string.format("%.3f", revs * mlrev * tubes) .. " ml，×" .. tostring(tubes) .. " 管）")
    return 1
end

-- ★4x211 泵状态码（多状态屏显用；2026-09-27）：
--   0 未连接 / 1 已连接·未使能485 / 2 就绪 / 3 运行中 / 4 通讯超时 / 5 泵错误
pump_state_code = function(p)
    if (p.conn or 0) ~= 1 then return 0 end
    if (p.errcode or 0) ~= 0 or (p.exc or 0) ~= 0 then return 5 end
    if p.ok ~= 1 then return 4 end
    if (p.ctl or 0) ~= 2 then return 1 end
    if p.run == 1 then return 3 end
    return 2
end

local function pump_step()
    local p = pump_state

    -- 连接管理（与称重同套路：必须先 PORT_STATUS=1 才收发）
    p.conn = PORT_STATUS(PUMP_CH)
    if p.opened == 0 then
        local ok, err = pcall(OPEN, PUMP_CH, "TCP_CLIENT", PUMP_PORT, PUMP_IP)
        if ok then
            p.opened = 1
            p.reconn = now()
            p.try = 0
            print("[pump] 发起TCP连接 -> " .. PUMP_IP .. ":" .. PUMP_PORT)
        elseif elapsed(p.logt) > PUMP_LOG_MS then
            p.logt = now()
            print("[pump] 连接发起失败：" .. tostring(err))
        end
    end
    if p.conn ~= p.connold then
        p.connold = p.conn
        if p.conn == 1 then
            print("[pump] ★链路已建立(PORT_STATUS=1)，先写 485 使能并开始轮询")
            p.qn = 0; p.queue = {}
            pump_push(pump_w6(0, 1), 0x06, 0, 1)    -- ★每次上电/重连先使能 485 控制（掉电不存储）
        else
            print("[pump] 链路未连接(PORT_STATUS=0)，暂停收发")
        end
    end
    if p.conn ~= 1 then
        p.rx = ""
        p.send = 0
        p.ok = 0
        if elapsed(p.reconn) > PUMP_RECONN_MS then
            p.reconn = now()
            p.try = p.try + 1
            print("[pump] 连接未建立，第" .. p.try .. "次重试；请查: 网关是否TCP Server/10123，是否被占用，" ..
                  "控制器能否访问 " .. PUMP_IP)
            pcall(OPEN, PUMP_CH, "TCP_CLIENT", PUMP_PORT, PUMP_IP)
        end
        -- 状态镜像（离线态）
        reg_set(202, 0)
        return
    end

    -- 应答超时
    if p.send == 1 and elapsed(p.t0) > PUMP_TIMEOUT then
        p.send = 0
        p.ok = 0
        p.err = p.err + 1
        p.tocnt = p.tocnt + 1
        if elapsed(p.logt) > PUMP_LOG_MS then
            p.logt = now()
            print("[pump] 应答超时(累计" .. p.err .. "次) 缓冲内" .. #p.rx .. "字节未成帧")
            p.tocnt = 0
        end
        p.rx = ""
    end

    -- 接收（追加缓冲，天然支持粘包/分包）
    if #p.rx < PUMP_RXMAX then
        local chunk = recv_str(PUMP_CH, PUMP_RXMAX - #p.rx)
        if #chunk > 0 then p.rx = p.rx .. chunk end
    end

    -- 解析循环：按功能码推帧长（异常 5 / 0x04 = 5+n / 0x01 = 5+n / 写应答 8）
    local again = true
    while again and #p.rx >= 5 do
        again = false
        local b1, b2, b3 = string.byte(p.rx, 1, 3)
        local total = 0
        if b1 ~= PUMP_STATION then
            total = 0
        elseif b2 >= 0x80 then
            total = 5
        elseif b2 == 0x04 or b2 == 0x01 or b2 == 0x03 then
            total = 5 + (b3 or 0)
        elseif b2 == 0x05 or b2 == 0x06 or b2 == 0x10 then
            total = 8
        end
        if total == 0 or #p.rx < total then
            if total == 0 then                          -- 帧头非法：丢首字节重同步
                p.err = p.err + 1
                p.rx = string.sub(p.rx, 2)
                again = true
            end
        else
            local f = string.sub(p.rx, 1, total)
            p.rx = string.sub(p.rx, total + 1)
            p.send = 0
            pump_handle_frame(f)
            again = true
        end
    end

    -- HMI 命令位（4x203，0→1 沿触发；一次一个 bit）
    local cw = reg_get(203)
    if cw ~= 0 and cw ~= (p.cmdold or 0) and pump_cmd_apply then
        pump_cmd_apply(cw)                             -- 值边沿兜底（正常写入走写队列；同值重复由队列保证）
        reg_set(203, 0)                                -- ★处理完自动回 0（配合屏「写常数」按钮）
        cw = 0
    end
    -- ★4x213 圈数运行按钮（写 1 启动；值边沿兜底，正常写入走写队列）
    local c13 = reg_get(213)
    if c13 ~= 0 and (p.revold or 0) == 0 and pump_rev_run then
        pump_rev_run()
        reg_set(213, 0)
        c13 = 0
    end
    p.revold = c13
    -- ★4x219 回吸参数提交（值边沿兜底，正常写入走写队列）
    local c19 = reg_get(219)
    if c19 ~= 0 and (p.revcfg_old or 0) == 0 and pump_rev_cfg_apply then
        pump_rev_cfg_apply()
        reg_set(219, 0)
        c19 = 0
    end
    p.revcfg_old = c19
    -- ★4x229 容积运行（值边沿兜底，正常写入走写队列）
    local c29 = reg_get(229)
    if c29 ~= 0 and (p.volold or 0) == 0 and pump_run_by_volume then
        pump_run_by_volume()
        reg_set(229, 0)
        c29 = 0
    end
    p.volold = c29
    p.cmdold = cw

    -- 轮询（队列空时按周期补三条；归属随条目走）
    if p.qn == 0 and p.send == 0 and elapsed(p.lastpoll) > PUMP_POLL_MS then
        p.lastpoll = now()
        pump_push(pump_rd_inputs(0, 6), 0x04, 0, 6)     -- 控制方式/方向/转速/流速
        pump_push(pump_rd_coils(1, 1), 0x01, 1, 1)      -- 运行状态
        pump_push(pump_rd_inputs(50, 1), 0x04, 50, 1)   -- 业务错误码 reg50
        pump_push(pump_rd_hold(7, 2), 0x03, 7, 2)       -- ★回吸角度/速度（读回显示 4x215/4x217）
    end
    -- ★圈数运行：到时停止 / 断链中止（2026-09-28）
    if (p.rev_end or 0) ~= 0 then
        if (p.conn or 0) ~= 1 then
            p.rev_end = 0
            print("[pump] 圈数运行中止（链路断开）")
        elseif elapsed(p.rev_end) >= 0 then
            p.rev_end = 0
            pump_push(pump_coil(1, false), 0x05, 1, 1)
            print("[pump] 圈数运行完成 → 自动停止")
        end
    end
    -- ★485 使能自动重试（每 3s；手册：reg0 掉电不存储，泵重上电后须重新使能，2026-09-27）
    if p.ok == 1 and (p.ctl or 0) ~= 2 and p.qn == 0 and p.send == 0 and elapsed(p.ent0 or 0) > 3000 then
        p.ent0 = now()
        p.entn = (p.entn or 0) + 1
        pump_push(pump_w6(0, 1), 0x06, 0, 1)
        if p.entn == 1 or (p.entn % 5) == 0 then
            print("[pump] 自动重发 485 使能（第 " .. p.entn .. " 次，控制方式=" .. tostring(p.ctl) .. "）")
        end
    elseif (p.ctl or 0) == 2 then
        p.entn = 0
    end
    pump_flush()

    -- ★状态镜像（4x202 = 泵状态码，2026-09-27 由位域改为数值码，屏端按值配多状态显示：
    --   0 未连接 / 1 已连接·未使能485 / 2 就绪 / 3 运行中 / 4 通讯超时 / 5 泵错误）
    reg_set(202, pump_state_code(p))
    reg_set(205, math.floor((p.rpm or 0) + 0.5))        -- 读回转速（int16 rpm）
    reg_set_f(206, p.flow or 0)                         -- 读回流速（float，HMI 低字在前；泵侧已转字序）
    reg_set(208, p.dir or 0)                            -- 方向（1=左/2=右；0=未知）
    reg_set(209, (p.errcode or 0) & 0xFFFF)             -- 业务错误码（reg50）
    reg_set(210, p.ctl or 0)                            -- 控制方式（0/1/2）
    -- ★回吸角度/速度：仅泵内值**变化时**刷新 4x215/217（否则会覆盖操作员在屏上的输入，2026-09-28）
    if (p.rev_angle or 0) ~= (p.rev_angle_disp or -1) then
        reg_set(215, p.rev_angle or 0)
        p.rev_angle_disp = p.rev_angle or 0
    end
    if (p.rev_speed or 0) ~= (p.rev_speed_disp or -1) then
        reg_set(217, p.rev_speed or 0)
        p.rev_speed_disp = p.rev_speed or 0
    end
    -- ★预计出液量（ml）= 圈数(4x211) × ml/圈(4x221) × 输送管数量(4x214)，**保留 1 位小数**（2026-09-28；2026-09-30 ×N）
    --   ml/圈先按 3 位小数规整（消除 float32 表示噪声，如 0.65 实为 0.6499999… → 误舍）
    local mlrev = reg_get_f(221) or 0
    mlrev = math.floor(mlrev * 1000 + 0.5) / 1000
    local tubes = pump_tubes()                          -- ★输送管数量（4x214，2026-09-30）
    reg_set_f(223, math.floor((reg_get_f(211) or 0) * mlrev * tubes * 10 + 0.5) / 10)
    -- ★预计圈数（4x227/228）= 设定容积(4x225) ÷ ml/圈(4x221) ÷ 输送管数量(4x214)，保留 1 位小数（2026-09-30 ÷N）
    if mlrev > 0.0005 then
        reg_set_f(227, math.floor((((reg_get_f(225) or 0) * VOL_IN_SCALE) / mlrev / tubes) * 10 + 0.5) / 10)
    else
        reg_set_f(227, 0)
    end
end

