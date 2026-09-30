-- ============================================================
-- kx_robot.lua — 机器人 Modbus 桥接 / 轴0定位 / 安全联锁
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_robot.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ==================== 机器人 Modbus 桥接（502 多客户端；替代 4320，2026-09-28） ====================
-- 数据方向（xCore 手册 10.5.4.1/10.5.4.2；地址见 docs/planA/19）：
--   A 区 4x1000~1099：**机器人写**（sta_* 状态）→ 本桥镜像到屏幕状态区 4x140~150
--   B 区 4x1100~1139：**机器人读**（ctrl_* 控制）← 屏幕命令（4x151~160 / 4x60）经本桥整形为脉冲/电平
-- 约定：控制信号脉冲 ≥60ms（ROBOT_PULSE_MS）；sta_heartbeat 超时判离线；屏幕区地址/位义不变。
robot_hb = { last = nil, t = 0, alive = 0, cnt = 0, err = 0,
             pulses = {}, start_t = 0, start_stage = 0, estop_t = 0 }

-- B 区地址（控制信号；机器人读）——与 planA/19 §3.B 一一对应
local RB = {
    ext_cmd_set = 1100, ext_reset = 1101, ext_resp_get = 1102, ext_req = 1103,
    clear_alarm = 1111, estop_reset = 1112, jjwc_a = 1113,
    motor_off = 1114, motor_on = 1115, motor_on_off = 1116,
    motoron_pptomain_start = 1117, motoron_start = 1118, pause_motoroff = 1119,
    pptomain = 1120, program_start = 1121, program_start_stop = 1122,
    program_stop = 1123, set_program_speed = 1124, soft_estop = 1125,
    switch_auto_motoron = 1126, switch_auto = 1127, switch_auto_manu = 1128,
    switch_manu = 1129, safe_region = 1130,
}

-- ★轴0目标控制（机器人写 4x1080 位置 + 4x1082 触发；状态回 4x1140；2026-09-29）
--   位置 = **INT16 整数 mm（收到多少执行多少，±32767mm，支持负数）**；
--   状态 4x1140：**收到位置立即置 0（已受理/执行中）→ 到位 2 / 失败 4**（2026-09-29 现场定义）
local AX_TGT_ADDR  = 1080
local AX_TRIG_ADDR = 1082
local AX_STAT_ADDR = 1140
local AX_STAT_HOLD_MS = 3000   -- ★2/4 保持时长（2026-09-29 晚 用户要求 1s→3s：机器人轮询周期较慢，1s 窗口会错过）
local AX_TRIG_DEBOUNCE_MS = 100   -- ★4x1082 触发防抖：1082 先于 1080 到达时不误动旧目标（2026-09-29）
local AX_DISP_GUARD_MS = 50      -- ★下发防抖窗口：刚下发的命令先让内核启动（防"未启动就判空闲"提前置 2；2026-09-29 现场反馈）
local AX_POS_TOL_MM = 0.5        -- ★到位位置容差：必须 |MPOS-目标| ≤ 0.5mm 才算到位（idle 只代表"没在动"，中途去使能/超时会 idle=1）
local AX_OFF_CONFIRM_MS = 500    -- ★"停了但未到位"确认窗口：持续 500ms 未到位 → 判失败 4（防命令替换瞬间的假空闲）
local ROBOT_ESTOP_ACTIVE_HIGH = true  -- ★4x145 急停极性：true=非 0 表示急停（随机器人「急停触发电平类型」；如相反改 false）

-- 写 4x1140：0=默认/已受理；2=到位、4=失败（仅保持 AX_STAT_HOLD_MS 后自动回 0）
local function ax_stat_write(v)
    reg_set(AX_STAT_ADDR, v)
    if v ~= 0 then
        robot_axis.stat_t = now() - AX_STAT_HOLD_MS
    else
        robot_axis.stat_t = 0
    end
end
-- ★4x1082 触发排程（防抖 AX_TRIG_DEBOUNCE_MS；到期校验 1080 未变化，见 modbus_step 尾部）
robot_axis_sched = function()
    robot_axis.pend_t   = now() - AX_TRIG_DEBOUNCE_MS
    robot_axis.pend_tgt = reg_get(AX_TGT_ADDR)
end
robot_axis = { active = 0, ok = 0, trig = 0, armed = false, last_tgt = nil, stat_t = 0,
               safe = 0, safe_t = 0, last_ok = true, pend_t = 0, pend_tgt = nil, disp_t = nil, off_t = 0 }

robot_axis_apply = function()
    local traw = reg_get(AX_TGT_ADDR)
    if traw > 32767 then traw = traw - 65536 end     -- 符号还原（INT16，支持负数）
    local tgt = traw                                  -- 整数 mm（收到多少执行多少）
    local bok = (bus_ok == 1)
    if BUSOK then bok = (BUSOK() == 1) end          -- 实时判定（自测环境 bus_ok 未跑 bus_step）
    if not bok then
        ax_stat_write(4)
        robot_axis.active = 0
        robot_axis.last_ok = false
        print("[axis0] 机器人定位拒绝：总线未就绪")
        return 1
    end
    if tgt < -1e6 or tgt > 1e6 then
        ax_stat_write(4)
        robot_axis.active = 0
        robot_axis.last_ok = false
        print(string.format("[axis0] 机器人定位拒绝：目标 %.3f 超范围", tgt))
        return 1
    end
    if robot_axis.safe == 1 then
        ax_stat_write(4)
        robot_axis.active = 0
        robot_axis.last_ok = false
        print("[axis0] 机器人定位拒绝：安全联锁中（机器人急停 4x145 未复位）")
        return 1
    end
    if ENABLED and ENABLED() ~= 1 then
        ax_stat_write(4)
        robot_axis.active = 0
        robot_axis.last_ok = false
        print("[axis0] 机器人定位拒绝：轴未使能（请先按 4x75 重新使能）")
        return 1
    end
    BASE(0)
    ax_stat_write(0)                                 -- ★收到位置即置 0（已受理；到位 2 / 失败 4 各保持 AX_STAT_HOLD_MS=3s）
    robot_axis.active = 1
    -- ★显式带 4x71/72（2026-09-30 现场反馈修复）：此前用 MOVEABS(tgt,0,0,0) 依赖
    --   「SPEED 参数表 → 内核缺省」两级回落——开机 init 阶段命令投递可能未被 RT 取走
    --   （实证：JOGLEAD 读回 0、地轨速度未生效），机器人定位按 app.conf 缺省(100/500)跑。
    local spd = (reg_get(71) or 0)                   -- 地轨速度（直接 mm/s；2026-09-30 晚改）
    local acc = (reg_get(72) or 0)                   -- 地轨加减速度
    if spd < 0 or spd > MV_SPD_MAX then spd = 0 end  -- 非法/未配置 → 0：交内核缺省链
    if acc < 0 then acc = 0 end
    local ok = pcall(MOVEABS, tgt, spd, acc, 0)      -- 异步下发（显式速度/加速度）
    if not ok then
        ax_stat_write(4)
        robot_axis.active = 0
        robot_axis.last_ok = false
        print("[axis0] 机器人定位下发失败")
        return 1
    end
    robot_axis.last_ok = true
    robot_axis.off_t = 0                             -- 新命令：清除"停在目标外"确认计时
    robot_axis.disp_t = now()                        -- ★下发时刻（完成监视的启动防抖窗口）
    robot_axis.last_tgt = tgt
    print(string.format("[axis0] 机器人定位：目标 %.3f mm，速度 %.1f mm/s（4x71/72；异步；状态见 4x%d）",
                        tgt, spd, AX_STAT_ADDR))
    return 1
end

robot_bridge_init = function()
    reg_set(RB.soft_estop, 1)                        -- 默认不触发软急停
    robot_hb.last = reg_get(1000)                    -- 以当前值起步：首个 0 不算心跳（避免开机误判）
    robot_hb.alive, robot_hb.cnt = 0, 0
    robot_axis.armed = false                         -- 位置未写前，1082 沿不触发（开机防误动）
end

-- 脉冲输出：置 v（默认 1）并在 ROBOT_PULSE_MS 后自动回 0
robot_bridge_pulse = function(addr, v)
    v = v or 1
    reg_set(addr, v & 0xFFFF)
    robot_hb.pulses[#robot_hb.pulses + 1] = { addr = addr, t = now() - ROBOT_PULSE_MS }
end

-- 机器人 ← 屏幕命令映射（写队列调用；e=命令表项，v=0/1）
robot_bridge_cmd = function(e, v)
    local reg = e.reg
    if reg == 151 then
        print("[robot] 4x151 SocketInterface 已随 4320 取消，忽略")
        return 1
    elseif reg == 152 then
        if v == 1 then
            reg_set(RB.motor_on_off, 1)              -- 上电（保持）
            robot_hb.start_stage = 1
            robot_hb.start_t = now()
            print("[robot] 启动时序：电机上电，" .. (ROBOT_START_GAP_MS / 1000) .. "s 后 ctrl_pptomain")
        else
            robot_hb.start_stage = 0
            robot_bridge_pulse(RB.program_stop)
            print("[robot] 停止：ctrl_program_stop")
        end
        return 1
    elseif reg == 153 then
        reg_set(RB.motor_on_off, v & 1)              -- 电平：1 上电 / 0 下电
        print("[robot] 电机" .. (v == 1 and "上电" or "下电") .. "（ctrl_motor_on_off=" .. v .. "）")
        return 1
    elseif reg == 154 then
        reg_set(RB.switch_auto_manu, v & 1)          -- 电平：1 自动 / 0 手动
        if v == 1 then robot_bridge_pulse(RB.switch_auto) else robot_bridge_pulse(RB.switch_manu) end
        print("[robot] 模式切换：" .. (v == 1 and "自动" or "手动"))
        return 1
    elseif reg == 155 then
        print("[robot] 4x155 拖动已停用（寄存器链路无 drag；请用机器人示教器拖动）")
        return 1
    elseif reg == 156 then
        robot_bridge_pulse(RB.clear_alarm)
        print("[robot] 清报警（ctrl_clear_alarm）")
        return 1
    elseif reg == 157 then
        robot_bridge_pulse(RB.pptomain)
        print("[robot] 程序指针回 main（ctrl_pptomain）")
        return 1
    else
        print("[robot] 4x" .. tostring(reg) .. " 已随 4320 取消（程序列表/载入），忽略")
        return 1
    end
end

-- 软急停（4x60 bit2 急停 → ctrl_soft_estop=0，ROBOT_SOFT_ESTOP_MS 后回 1）
robot_bridge_estop = function()
    reg_set(RB.soft_estop, 0)
    robot_hb.estop_t = now() - ROBOT_SOFT_ESTOP_MS
    print("[robot] 软急停触发（ctrl_soft_estop=0，" .. ROBOT_SOFT_ESTOP_MS .. "ms 后恢复）")
end

-- 桥接步进（主循环每拍；非 RT）
local function robot_bridge_step()
    local h = robot_hb
    -- ① 心跳：4x1000 变化 → 在线；超时 → 离线（4x140 位据实）
    local hb = reg_get(1000)
    if hb ~= h.last then
        h.last = hb
        h.t = now()
        h.alive = 1
        h.cnt = h.cnt + 1
    elseif h.alive == 1 and elapsed(h.t) > ROBOT_HB_TIMEOUT_MS then
        h.alive = 0
        h.err = h.err + 1
        print("[robot] 心跳超时（" .. ROBOT_HB_TIMEOUT_MS .. "ms）→ 判离线")
    end

    -- ② A 区 → 屏幕状态区 4x140~150（位义与旧 4320 时代一致）
    local rst = 0
    if h.alive == 1 then rst = rst + 3 end           -- bit0 连接 + bit1 通讯
    if reg_get(1001) ~= 0 then rst = rst + 4 end     -- bit2 报警（sta_alarm）
    reg_set(140, rst)
    reg_set(141, h.err)
    reg_set(142, h.cnt)
    reg_set(143, reg_get(1005))                      -- sta_motor（上电）
    reg_set(144, reg_get(1010))                      -- sta_robot_moving
    reg_set(145, reg_get(1003))                      -- sta_estop
    reg_set(146, reg_get(1006))                      -- sta_operation_mode（1 自动）
    reg_set(147, reg_get(1001))                      -- sta_alarm
    reg_set(148, reg_get(1013))                      -- sta_collision
    reg_set(149, reg_get(1008))                      -- sta_program_full
    reg_set(150, reg_get(1016))                      -- sta_home

    -- ③ 脉冲清理（到期回 0）
    for i = #h.pulses, 1, -1 do
        local p = h.pulses[i]
        if elapsed(p.t) >= 0 then
            reg_set(p.addr, 0)
            table.remove(h.pulses, i)
        end
    end

    -- ④ 启动时序状态机：上电 → (GAP) ctrl_pptomain → (GAP) ctrl_program_start
    if h.start_stage == 1 and elapsed(h.start_t) >= ROBOT_START_GAP_MS then
        robot_bridge_pulse(RB.pptomain)
        h.start_stage = 2
        h.start_t = now()
        print("[robot] 启动时序：已发 ctrl_pptomain，" .. (ROBOT_START_GAP_MS / 1000) .. "s 后 ctrl_program_start")
    elseif h.start_stage == 2 and elapsed(h.start_t) >= ROBOT_START_GAP_MS then
        robot_bridge_pulse(RB.program_start)
        h.start_stage = 0
        print("[robot] 启动时序：已发 ctrl_program_start")
    end

    -- ④b 轴0机器人定位状态：执行中(bit0) → 到位(bit1)/失败(bit2)
    if robot_axis.active == 1 then
        local idle_now = false
        if ISIDLE then idle_now = (ISIDLE(0) == 1) elseif IDLE then idle_now = (IDLE(0) ~= 0) end
        local busy_now  = (BUSY ~= nil) and (BUSY(0) ~= 0)
        local alarm_now = (ALARM ~= nil) and (ALARM(0) ~= 0)
        local en_now = true
        if ENABLED then en_now = (ENABLED() == 1) end
        -- ★下发防抖：命令刚下发时内核可能尚未启动运动（idle 仍是上一次的 1）→ 不判到位
        local guard_ok = robot_axis.disp_t ~= nil and elapsed(robot_axis.disp_t) >= AX_DISP_GUARD_MS
        -- ★位置核对（2026-09-29 现场反馈）：idle 只代表"没在动"——中途去使能/超时停止时 idle=1 但离目标很远
        local mpos_v = nil
        do
            local pok, mv = pcall(MPOS, 0)
            if pok and type(mv) == "number" then mpos_v = mv end
        end
        local pos_ok = true
        if mpos_v ~= nil and robot_axis.last_tgt ~= nil then
            pos_ok = math.abs(mpos_v - robot_axis.last_tgt) <= AX_POS_TOL_MM
        end
        if robot_axis.safe == 1 then
            robot_axis.active = 0
            robot_axis.last_ok = false
            ax_stat_write(4)                         -- 安全联锁 → 中止（不再静默挂起）
            print("[axis0] 机器人定位中止：安全联锁（机器人急停）")
        elseif alarm_now and not busy_now then
            robot_axis.active = 0
            robot_axis.last_ok = false
            ax_stat_write(4)                         -- 失败（报警）优先于到位（保持 3s）
            print("[axis0] 机器人定位失败（报警）")
        elseif idle_now and guard_ok then
            if pos_ok then
                robot_axis.active = 0
                robot_axis.off_t  = 0
                robot_axis.last_ok = true
                ax_stat_write(2)                     -- 到位（位置核对通过；保持 3s 后自动回 0）
                print("[axis0] 机器人定位到位（4x" .. AX_STAT_ADDR .. "=2）")
            elseif (robot_axis.off_t or 0) == 0 then
                robot_axis.off_t = now() - AX_OFF_CONFIRM_MS   -- 首次见"停了但未到位"：进入确认窗口
            elseif elapsed(robot_axis.off_t) >= 0 then
                robot_axis.active = 0
                robot_axis.off_t  = 0
                robot_axis.last_ok = false
                ax_stat_write(4)                     -- 停在目标外 → 失败（不再误报 2）
                print(string.format("[axis0] 机器人定位失败：停在目标外（实际 %.3f / 目标 %.3f）",
                                    mpos_v or 0, robot_axis.last_tgt or 0))
            end
        elseif not en_now and not busy_now then
            robot_axis.active = 0
            robot_axis.last_ok = false
            ax_stat_write(4)                         -- 掉使能（去使能/掉线）→ 中止
            print("[axis0] 机器人定位中止：轴未使能")
        end
    end

    -- ★安全联锁（2026-09-29）：机器人急停(4x145) 非 0 → 轴0 去使能，未复位期间保持关闭
    --   ★2026-09-29 晚 用户定案：sta_alarm（4x147）闪动会误去使能 → **故障信号不再参与联锁**（仅镜像显示）
    do
        local r_estop = reg_get(145)
        local estop_act = (r_estop ~= 0) == ROBOT_ESTOP_ACTIVE_HIGH
        if estop_act then
            if robot_axis.safe ~= 1 then
                robot_axis.safe   = 1
                robot_axis.safe_t = now()
                pcall(DISABLE)
                print(string.format("[axis0] 安全联锁：机器人急停信号（4x145=%d）→ 伺服去使能", r_estop))
            elseif elapsed(robot_axis.safe_t or 0) > 1000 then
                robot_axis.safe_t = now()
                if ENABLED and ENABLED() == 1 then
                    pcall(DISABLE)
                    print("[axis0] 安全联锁：信号未复位，保持去使能")
                end
            end
        elseif robot_axis.safe == 1 then
            robot_axis.safe = 0
            print("[axis0] 安全联锁解除：机器人急停已复位（伺服需重新使能）")
        end
    end

    -- ④c 轴0状态字 1s 保持到期 → 回默认 0（2/4 为瞬时结果）
    if (robot_axis.stat_t or 0) ~= 0 and elapsed(robot_axis.stat_t) >= 0 then
        robot_axis.stat_t = 0
        reg_set(AX_STAT_ADDR, 0)
    end

    -- ⑤ 软急停脉冲恢复（0 触发 → 回 1）
    if (h.estop_t or 0) ~= 0 and elapsed(h.estop_t) >= 0 then
        reg_set(RB.soft_estop, 1)
        h.estop_t = 0
    end
end

