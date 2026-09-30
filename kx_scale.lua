-- ============================================================
-- kx_scale.lua — 称重采集（Modbus-RTU over TCP 透传）
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_scale.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== 称重采集（Modbus-RTU over TCP 透传） ========================
scale_state = {
    opened = 0, conn = 0, ok = 0, valid = 0, err = 0,
    send = 0, t0 = 0, lastpoll = 0, reconn = 0, connold = 0, try = 0,
    logt = 0, rxlogt = 0, tocnt = 0, badcnt = 0,
    raw = 0, ad = 0, zero = 0, w = 0, div = 100.0, divsave = 100.0,
    station = 1, stasave = 1, cmdold = 0, busy = 0, result = 0, res_t = 0,
    cal_idx = 0, cal_result = 0,
    poll = SCALE_POLL_MS, queue = {}, qn = 0, rx = "",
}

-- 写命令入队（地址/值以字节给出，等价旧 scale_push）
local function scale_push(ah, al, vh, vl)
    local s = scale_state
    if s.qn >= 15 then
        print("[scale] 命令队列满,丢弃本次命令")
        return
    end
    s.qn = s.qn + 1
    s.queue[s.qn] = string.char(ah, al, vh, vl)
end

-- 1=去皮 2=取消去皮 3=零点校准 4=清错误计数（均先写 0x0017=1 关写保护）
function scale_do_cmd(c)
    local s = scale_state
    if c == 1 then
        scale_push(0, 23, 0, 1)
        scale_push(0, 21, 0, 1)
        s.result = 1
        print("[scale] 命令:去皮")
    elseif c == 2 then
        scale_push(0, 23, 0, 1)
        scale_push(0, 21, 0, 2)
        s.result = 1
        print("[scale] 命令:取消去皮")
    elseif c == 3 then
        s.zero = s.ad
        scale_push(0, 23, 0, 1)
        scale_push(0, 22, 0, 1)
        s.result = 1
        print("[scale] 命令:零点校准")
    elseif c == 4 then
        s.err = 0
        s.result = 2
        print("[scale] 命令:清除错误计数")
    end
end

-- ★折线校准提交（HMI：4x28=本点重量、4x30=提交；Kine-X 增补，2026-09-28）
--   v=1：首次提交先“关写保护 + 设 10 点折线”，随后逐点写模块 0x0019+i；v=2：重置计数重新开始
--   4x29=已提交点数（0~10）、4x31=结果码（0 空闲 / 1 成功 / 2 重量无效 / 3 已满 / 4 未连接）
scale_calib_submit = function(v)
    local s = scale_state
    if v == 2 then
        s.cal_idx, s.cal_result = 0, 0
        print("[scale] 折线校准：计数已重置（可重新提交 10 点）")
    elseif v == 3 then
        -- ★关闭折线（0x0018=0，回单点/线性校准；现场低端跳变救援，2026-09-28）
        scale_push(0, 0x17, 0, 1)                     -- 关闭写保护
        scale_push(0, 0x18, 0, 0)                     -- 校准方式 = 砝码校准（关闭折线）
        s.cal_idx, s.cal_result = 0, 1
        print("[scale] 折线校准：已关闭折线（0x0018=0），回单点/线性校准")
    elseif v == 4 then
        -- ★单点砝码校准（写 0x0006 = 4x28×比例系数；砝码须已在秤上）
        if s.conn ~= 1 then
            s.cal_result = 4
            print("[scale] 单点砝码校准：未连接，忽略")
        elseif reg_get(28) < 1 then
            s.cal_result = 2
            print("[scale] 单点砝码校准：重量无效（最小 1）")
        else
            local wv4 = reg_get(28)
            local dv4 = s.div or 100.0
            if dv4 < 0.000001 then dv4 = 100.0 end
            local c4 = math.floor(wv4 * dv4 + 0.5)
            if c4 < 20 or c4 > 65535 then
                s.cal_result = 2
                print("[scale] 单点砝码校准：重量 " .. tostring(wv4) .. " → 模块值 " .. tostring(c4) .. " 超范围")
            else
                scale_push(0, 0x17, 0, 1)             -- 关闭写保护
                scale_push(0, 0x06, (c4 >> 8) & 0xFF, c4 & 0xFF)
                s.cal_idx, s.cal_result = 0, 1
                print("[scale] 单点砝码校准：写入 0x0006 = " .. tostring(c4) .. "（" .. tostring(wv4) .. "）")
            end
        end
    elseif s.conn ~= 1 then
        s.cal_result = 4
        print("[scale] 折线校准：未连接，提交忽略")
    elseif reg_get(28) < 1 then
        s.cal_result = 2
        print("[scale] 折线校准：重量无效（" .. tostring(reg_get(28)) .. "，最小 1（1g 输 1））")
    else
        local i = s.cal_idx or 0
        if i >= 10 then
            -- ★已满 10 点后再提交（重量有效）：自动开始新一轮（覆盖旧点）——现场“校准完第二次做不了”修复
            s.cal_idx, i = 0, 0
            print("[scale] 折线校准：已满 10 点 → 自动重新开始（将覆盖旧点；也可写 4x30=2 显式重置）")
        end
        local wv = reg_get(28)
        local div = s.div or 100.0
        if div < 0.000001 then div = 100.0 end
        local counts = math.floor(wv * div + 0.5)       -- 显示值 → 模块计数（默认 ×100，见 4x132/133 比例系数）
        if counts < 20 or counts > 65535 then
            s.cal_result = 2
            print("[scale] 折线校准：重量 " .. tostring(wv) .. " → 模块值 " .. tostring(counts) ..
                  " 超范围（20~65535），请检查比例系数（4x132/133）或重量")
        else
            if i == 0 then
                scale_push(0, 0x17, 0, 1)                 -- 关闭写保护（0x0017=1）
                scale_push(0, 0x18, 0, 10)                -- 校准方式 = 10 点折线
                print("[scale] 折线校准：关闭写保护 + 设为 10 点折线")
            end
            scale_push(0, 0x19 + i, (counts >> 8) & 0xFF, counts & 0xFF)
            s.cal_idx = i + 1
            s.cal_result = 1
            print("[scale] 折线校准：提交第 " .. s.cal_idx .. " 点 = " .. tostring(wv) ..
                  "（模块 " .. tostring(counts) .. "）")
        end
    end
    reg_set(30, 0)                                    -- 自动回 0（HMI 连按也能产生 0→1 写入）
    reg_set(29, s.cal_idx or 0)                       -- 立即上报（镜像在扫描之前，避免滞后一拍）
    reg_set(31, s.cal_result or 0)
    return 1
end

-- 解析 32 位有符号（低字在前）
local function scale_par32(s, idx)   -- idx: 数据区第 1 字节在字符串中的位置
    local b1, b2, b3, b4 = string.byte(s, idx, idx + 3)
    local lo = b1 * 256 + b2
    local hi = b3 * 256 + b4
    if hi >= 32768 then
        return (hi - 65536) * 65536 + lo
    end
    return hi * 65536 + lo
end

local function scale_step()
    local s = scale_state

    -- 连接管理：必须先 PORT_STATUS=1 才收发
    s.conn = PORT_STATUS(SCALE_CH)
    if s.opened == 0 then
        local ok, err = pcall(OPEN, SCALE_CH, "TCP_CLIENT", SCALE_PORT, SCALE_IP)
        if ok then
            s.opened = 1
            s.reconn = now()
            s.try = 0
            print("[scale] 发起TCP连接 -> " .. SCALE_IP .. ":" .. SCALE_PORT)
        else
            if elapsed(s.logt) > SCALE_LOG_MS then
                s.logt = now()
                print("[scale] 连接发起失败：" .. tostring(err))
            end
        end
    end

    if s.conn ~= s.connold then
        s.connold = s.conn
        if s.conn == 1 then
            print("[scale] ★链路已建立(PORT_STATUS=1)，开始Modbus-RTU轮询")
        else
            print("[scale] 链路未连接(PORT_STATUS=0)，暂停收发")
        end
    end

    if s.conn ~= 1 then
        s.rx = ""
        s.send = 0
        s.ok = 0
        if elapsed(s.reconn) > SCALE_RECONN_MS then
            s.reconn = now()
            s.try = s.try + 1
            print("[scale] 连接未建立，第" .. s.try ..
                  "次重试；请查: 网关是否TCP Server/10123，是否被上位机占用，控制器能否访问 " .. SCALE_IP)
            pcall(OPEN, SCALE_CH, "TCP_CLIENT", SCALE_PORT, SCALE_IP)
        end
        return
    end

    -- 应答超时
    if s.send == 1 then
        if elapsed(s.t0) > SCALE_TIMEOUT then
            s.send = 0
            s.ok = 0
            s.err = s.err + 1
            s.tocnt = s.tocnt + 1
            if s.result == 1 then s.result = 3 end
            if elapsed(s.logt) > SCALE_LOG_MS then
                s.logt = now()
                print("[scale] 应答超时(本窗口" .. s.tocnt .. "次/累计" .. s.err ..
                      "次) 缓冲内" .. #s.rx .. "字节未成帧")
                s.tocnt = 0
            end
            s.rx = ""
        end
    end

    -- 接收（追加到缓冲，天然支持粘包/分包）
    if #s.rx < SCALE_RXMAX then
        local chunk = recv_str(SCALE_CH, SCALE_RXMAX - #s.rx)
        if #chunk > 0 then
            s.rx = s.rx .. chunk
            if SCALE_DBG == 1 and elapsed(s.rxlogt) > SCALE_LOG_MS then
                s.rxlogt = now()
                print("[scale] 收到" .. #chunk .. "字节(缓冲共" .. #s.rx .. "字节)")
            end
        end
    end

    -- 解析：RTU 帧 = 站号 + 功能码 + 数据 + CRC16(低字节在前)
    local again = true
    while again and #s.rx >= 3 do
        again = false
        local b1, b2 = string.byte(s.rx, 1, 2)
        local bad = false
        if b1 ~= s.station then
            bad = true
        elseif b2 ~= 3 and b2 ~= 6 and b2 < 128 then
            bad = true
        end
        if bad then
            s.err = s.err + 1
            s.badcnt = s.badcnt + 1
            if SCALE_DBG == 1 and elapsed(s.logt) > SCALE_LOG_MS then
                s.logt = now()
                print("[scale] 帧头非法，本窗口丢弃" .. s.badcnt .. "字节(站号" .. hex(b1) ..
                      " 功能码" .. hex(b2) .. ")，丢首字节重同步")
                s.badcnt = 0
            end
            s.rx = string.sub(s.rx, 2)
        else
            local total = 0
            if b2 == 3 then
                total = 5 + (string.byte(s.rx, 3) or 0)
            elseif b2 == 6 then
                total = 8
            else
                total = 5
            end
            if total > SCALE_RXMAX then
                s.err = s.err + 1
                print("[scale] 报文长度非法，整包丢弃")
                s.rx = ""
            elseif #s.rx < total then
                -- 半包，等下一批
            else
                local body = string.sub(s.rx, 1, total - 2)
                local crc_calc = crc16(body)
                local crc_lo, crc_hi = string.byte(s.rx, total - 1, total)
                local crc_frame = crc_lo + crc_hi * 256
                if crc_calc ~= crc_frame then
                    s.err = s.err + 1
                    if SCALE_DBG == 1 and elapsed(s.logt) > SCALE_LOG_MS then
                        s.logt = now()
                        print(string.format("[scale] CRC错(算出%04X 帧内%04X)，丢首字节重同步", crc_calc, crc_frame))
                    end
                    s.rx = string.sub(s.rx, 2)
                else
                    if b2 >= 128 then                       -- 异常应答
                        s.ok = 0
                        s.err = s.err + 1
                        print("[scale] 模块异常应答，功能码" .. b2 .. " 异常码" .. (string.byte(s.rx, 3) or 0))
                        if s.result == 1 then s.result = 3 end
                    elseif b2 == 3 then
                        local n = string.byte(s.rx, 3) or 0
                        if n >= 4 then
                            s.raw = scale_par32(s.rx, 4)    -- 实时重量（32位有符号）
                            if n >= 8 then
                                local b8, b9, b10, b11 = string.byte(s.rx, 8, 11)
                                -- 内码：32 位无符号，低字寄存器在前；寄存器内高字节在前
                                s.ad = b8 * 256 + b9 + (b10 * 256 + b11) * 65536.0
                            end
                            if s.div < 0.000001 then s.div = 100.0 end
                            s.w = s.raw / s.div
                            s.ok = 1
                            if s.valid == 0 then
                                s.valid = 1
                                print("[scale] 数据接入成功，重量" .. string.format("%.3f", s.w) ..
                                      " 原始值" .. tostring(s.raw))
                            end
                        else
                            s.err = s.err + 1
                            print("[scale] 读应答数据长度不足")
                        end
                    elseif b2 == 6 then
                        if s.result == 1 then s.result = 2 end
                        s.ok = 1
                    end
                    s.send = 0
                    s.rx = string.sub(s.rx, total + 1)
                    again = true
                end
            end
        end
    end

    -- 优先下发写命令
    if s.qn > 0 and s.send == 0 then
        local p = s.queue[1]
        table.remove(s.queue, 1)
        s.qn = s.qn - 1
        local frame = rtu_frame(string.char(s.station, 6) .. p)
        PORT_PRINT(SCALE_CH, frame)
        s.send = 1
        s.t0 = now()
        s.lastpoll = now()
        print(string.format("[scale] 下发写命令 寄存器0x%02X%02X", string.byte(p, 1), string.byte(p, 2)))
    end

    -- 周期读请求（实时重量 + 内码；串行：一帧应答完再发下一帧）
    if s.send == 0 and s.qn == 0 then
        if elapsed(s.lastpoll) > s.poll then
            local frame = rtu_frame(string.char(s.station, 3, 0, 0, 0, 4))
            if SCALE_DBG == 1 and elapsed(s.logt) > SCALE_LOG_MS then
                s.logt = now()
                print("[scale] TX " .. #frame .. "字节: " .. (frame:gsub(".", function(c) return hex(string.byte(c)) .. " " end)))
            end
            PORT_PRINT(SCALE_CH, frame)
            s.send = 1
            s.t0 = now()
            s.lastpoll = now()
        end
    end
end

