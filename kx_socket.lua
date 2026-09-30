-- ============================================================
-- kx_socket.lua — 4321 命令解析与执行 + 服务端步进
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_socket.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== 4321 命令解析与执行 ========================
local function send(s)
    if conn_old == 1 then
        PORT_PRINT(COM_PORT, s .. "/n")
    end
end

-- D10：声明端口标签（用途/主从）——供插件「工具 → 通讯状态」显示；用途/角色取自本文件各 step 的实际语义。
-- 老固件无 PORT_INFO 命令 → pcall 忽略（面板显示无标签，不伪装）。
port_tag_declare = function()
    pcall(PORT_INFO, COM_PORT, "4321 ASCII 服务端", "—")
    pcall(PORT_INFO, SCALE_CH, "称重网关 TCP 客户端", "Modbus 主站")
    pcall(PORT_INFO, MB_CH,    "Modbus-TCP 从站(502)", "从站")
end


local function cmd_exec(cmd)
    if DBG == 1 then print("[sock] CMD:" .. cmd) end

    local c0 = string.byte(cmd, 1) or 0
    -- ---- 纯数值命令 ----
    if (c0 >= 48 and c0 <= 57) or c0 == 45 or c0 == 46 then
        if #cmd == 1 and (c0 == 45 or c0 == 46) then
            send("move_error")
        elseif bus_ok ~= 1 then
            send("move_error")
        elseif mvmode == 1 then
            send("busy")
        else
            local mvacc, mvspd = MV_ACCEL, MV_SPEED
            local pos
            if string.find(cmd, ",", 1, true) then
                local c2 = string.find(cmd, ",", (string.find(cmd, ",", 1, true) or 1) + 1, true)
                if not c2 then
                    send("move_error")                    -- 只有一个逗号=格式错误
                    return
                end
                local a = field_num(cmd, 0)
                if a > 0 then mvacc = a end
                local v = field_num(cmd, 1)
                if v > 0 then mvspd = v end
                pos = field_num(cmd, 2)
            else
                pos = field_num(cmd, 0)
            end
            if DBG == 1 then
                print(string.format("[sock] 数值命令 acc=%s spd=%s pos=%s", tostring(mvacc), tostring(mvspd), tostring(pos)))
            end
            SPEED(MVAXIS, mvspd)
            ACCEL(MVAXIS, mvacc)
            DECEL(MVAXIS, mvacc)
            local ok, err = pcall(MOVEABS, pos, mvspd, mvacc, 0)
            if not ok then
                print("[sock] 定位下发失败：" .. tostring(err))
                send("move_error")
                return
            end
            mvt0 = now()
            mvwait = 0
            mvpos = pos          -- 到位判定要用目标位置（v0.8.1 修复：此前漏赋值 → 差 0 → 误报 move_done）
            mvmode = 1
        end
    elseif cmd == "HELLO" then
        send("HELLO ZMC")
    elseif cmd == "EN" then
        BASE(0)
        local ok, err = pcall(ENABLE)
        if not ok then print("[sock] EN 失败：" .. tostring(err)) end
        wdog = 1
        send("OK")
    elseif cmd == "DIS" then
        wdog = 0
        BASE(0)
        local ok, err = pcall(DISABLE)
        if not ok then print("[sock] DIS 失败：" .. tostring(err)) end
        send("OK")
    elseif cmd == "STOP" then
        RAPIDSTOP(0)   -- Kine-X: 参数是轴号（0=本机唯一轴）；ZBasic 的 mode 2=全停
        send("OK")
    elseif string.sub(cmd, 1, 4) == "VJOG" then
        local spd = field_num(cmd, 2)
        if bus_ok == 1 then
            SPEED(0, math.abs(spd))
            local ok, err
            if spd < 0 then ok, err = pcall(VMOVE, -1) else ok, err = pcall(VMOVE, 1) end
            if not ok then print("[sock] VJOG 失败：" .. tostring(err)) end
            send("OK")
        else
            send("ERR:NOBUS")
        end
    elseif string.sub(cmd, 1, 2) == "MA" then
        local pos = field_num(cmd, 2)
        local c3 = string.find(cmd, ",", (string.find(cmd, ",", 1, true) or 0) + 1, true)
        if bus_ok == 1 then
            if c3 then
                SPEED(0, math.abs(field_num(cmd, 3)))
            end
            local ok, err = pcall(MOVEABS, pos, 0, 0, 0)
            if not ok then print("[sock] MA 失败：" .. tostring(err)) end
            send("OK")
        else
            send("ERR:NOBUS")
        end
    elseif string.sub(cmd, 1, 2) == "MR" then
        local dist = field_num(cmd, 2)
        local c3 = string.find(cmd, ",", (string.find(cmd, ",", 1, true) or 0) + 1, true)
        if bus_ok == 1 then
            if c3 then
                SPEED(0, math.abs(field_num(cmd, 3)))
            end
            local ok, err = pcall(MOVE, dist, 0, 0, 0)
            if not ok then print("[sock] MR 失败：" .. tostring(err)) end
            send("OK")
        else
            send("ERR:NOBUS")
        end
    elseif cmd == "STA" then
        local mpos = pcall(MPOS, 0) and MPOS(0) or 0
        local dpos = pcall(DPOS, 0) and DPOS(0) or 0
        local idle = (pcall(BUSY) and BUSY() == 1) and 0 or -1
        send("STA," .. fmt(bus_ok, 1, 0) .. ",0," .. fmt(mpos, 12, 3) .. "," ..
             fmt(dpos, 12, 3) .. "," .. fmt(idle, 3, 0))
    elseif cmd == "SCAN" then
        rescan_flag = 1
        send("OK")
    elseif cmd == "BUSSTOP" then
        SLOT_STOP(0)
        bus_ok = 0
        send("OK")
    elseif cmd == "W" then
        if scale_state and scale_state.valid == 1 then
            send("WEIGHT:" .. fmt(scale_state.w, 10, 3))
        else
            send("ERR:NOSCALE")
        end
    elseif cmd == "PSTA" then                                  -- 蠕动泵状态（框架，2026-09-27）
        local p = pump_state or {}
        local function pn(v) return (v or 0) end
        send("PSTA," .. fmt(pn(p.conn), 1, 0) .. "," .. fmt(pn(p.ok), 1, 0) .. "," ..
             fmt(pn(p.ctl), 1, 0) .. "," .. fmt(pn(p.run), 1, 0) .. "," ..
             fmt(pn(p.rpm), 10, 1) .. "," .. fmt(pn(p.flow), 10, 2) .. "," ..
             fmt(pn(p.errcode), 5, 0) ..
             ",REVS," .. fmt(reg_get_f(211) or 0, 8, 3) .. ",SET," .. fmt(reg_get(204), 4, 0) ..
             ",ANG," .. fmt(reg_get(215), 4, 0) .. ",SPD," .. fmt(reg_get(217), 4, 0) ..
             ",MLREV," .. fmt(reg_get_f(221) or 0, 8, 3) .. ",VOL," .. fmt(reg_get_f(223) or 0, 10, 3) ..
             ",TVOL," .. fmt(reg_get_f(225) or 0, 10, 3) .. ",PREV," .. fmt(reg_get_f(227) or 0, 10, 3) ..
             ",TUBES," .. fmt(pump_tubes and pump_tubes() or 1, 4, 0))
    elseif cmd == "WSTA" then
        local s = scale_state or {}
        send("WSTA," .. fmt(s.ok or 0, 1, 0) .. "," .. fmt(s.valid or 0, 1, 0) .. "," ..
             fmt(s.conn or 0, 1, 0) .. "," .. fmt(s.result or 0, 1, 0) .. "," ..
             fmt(s.err or 0, 5, 0) .. "," .. fmt(s.w or 0, 10, 3) .. "," ..
             fmt(s.raw or 0, 12, 0) .. "," .. fmt(s.ad or 0, 12, 0) .. "," ..
             fmt(s.zero or 0, 12, 0))
    elseif cmd == "WT" then
        if scale_do_cmd then scale_do_cmd(1) end
        send("OK")
    elseif cmd == "WTC" then
        if scale_do_cmd then scale_do_cmd(2) end
        send("OK")
    elseif cmd == "WZERO" then
        if scale_do_cmd then scale_do_cmd(3) end
        send("OK")
    elseif cmd == "WCLR" then
        if scale_do_cmd then scale_do_cmd(4) end
        send("OK")
    elseif string.sub(cmd, 1, 5) == "WDIV," then
        local v = field_num(cmd, 1)
        if v > 0 and scale_state then
            scale_state.div = v
            scale_state.divsave = v
            send("OK")
        else
            send("ERR:ARG")
        end
    elseif string.sub(cmd, 1, 4) == "WSA," then
        local v = field_num(cmd, 1)
        if v >= 1 and v <= 247 and scale_state then
            scale_state.station = v
            scale_state.stasave = v
            send("OK")
        else
            send("ERR:ARG")
        end
    else
        send("ERR:UNKNOWN")
    end
end

-- ======================== 4321 服务端步进 ========================
local function socket_step()
    -- 4321 监听兜底：启动时 OPEN 失败则每 5s 重试（总线重试期间端口必须始终在线）
    if svr_open == 0 then
        if elapsed(svr_logt) > 5000 then
            svr_logt = now()
            local ok, err = pcall(OPEN, COM_PORT, "TCP_SERVER", SVR_PORT)
            if ok then
                svr_open = 1
                print("[sock] 4321 服务端已开启")
            else
                print("[sock] 4321 监听失败：" .. tostring(err))
            end
        end
        return
    end

    -- 连接状态
    local conn = PORT_STATUS(COM_PORT)
    if conn ~= conn_old then
        conn_old = conn
        if conn == 1 then
            print("[sock] 客户端已连接")
            PORT_PRINT(COM_PORT, "HELLO ZMC SERVER 4321/n")
        else
            print("[sock] 客户端断开")
            cmd_buf = ""
        end
    end

    -- 心跳
    if conn_old == 1 then
        if elapsed(beat_last) > HB_MS then
            beat_last = now()
            PORT_PRINT(COM_PORT, "beat/n")
        end
    else
        beat_last = now()
    end

    -- 接收与拆帧
    local n = 0
    repeat
        local chunk = recv_str(COM_PORT, 64)
        if #chunk > 0 then
            last_rx = now()
            for i = 1, #chunk do
                local ch = string.byte(chunk, i)
                if ch == 59 or ch == 13 or ch == 10 then      -- ; CR LF
                    if #cmd_buf > 0 then
                        local c = cmd_buf
                        cmd_buf = ""
                        local ok, err = pcall(cmd_exec, c)
                        if not ok then
                            print("[sock] 命令执行异常: " .. tostring(err))
                            send("ERR:UNKNOWN")
                        end
                    end
                else
                    if #cmd_buf < CMDMAX - 1 then
                        cmd_buf = cmd_buf .. string.char(ch)
                    end
                end
            end
            n = n + 1
        end
    until #chunk == 0 or n > 8

    -- 无结束符空闲超时
    if #cmd_buf > 0 and elapsed(last_rx) > CMD_IDLE_MS then
        local c = cmd_buf
        cmd_buf = ""
        local ok, err = pcall(cmd_exec, c)
        if not ok then
            print("[sock] 命令执行异常: " .. tostring(err))
            send("ERR:UNKNOWN")
        end
    end

    -- 数值命令运动完成监测（异步定位 -> 到位判定）
    if mvmode == 1 and elapsed(mvt0) > 50 then
        if BUSY() ~= 1 then
            if mvwait == 0 then
                mvwait = now()
            elseif elapsed(mvwait) >= 200 then
                mvmode = 0
                local st  = AXISSTATUS(MVAXIS)
                local errmask = (st & MV_ERRMASK) ~= 0
                local alarm = (ALARM() ~= 0)
                local diff = math.abs(MPOS(MVAXIS) - mvpos)
                if DBG == 1 then
                    print(string.format("[sock] 到位检查 MPOS=%.3f 目标=%.3f 差=%.3f STATUS=%d", MPOS(MVAXIS), mvpos, diff, st))
                end
                if errmask or alarm or diff > MV_TOL then
                    print(string.format("[sock] 定位失败: MPOS=%.3f 目标=%.3f 差=%.3f STATUS=0x%X ALARM=%d",
                                        MPOS(MVAXIS), mvpos, diff, st, alarm and 1 or 0))
                    send("move_error")
                else
                    print(string.format("[sock] 定位完成: MPOS=%.3f 目标=%.3f", MPOS(MVAXIS), mvpos))
                    send("move_done")
                end
            end
        elseif elapsed(mvt0) > MV_TIMEOUT then
            mvmode = 0
            mvwait = 0
            print(string.format("[sock] 定位超时(%dms): MPOS=%.3f 目标=%.3f", MV_TIMEOUT, MPOS(MVAXIS), mvpos))
            RAPIDSTOP(0)   -- Kine-X: 参数是轴号（0=本机唯一轴）；ZBasic 的 mode 2=全停
            send("move_error")
        end
    end
end

