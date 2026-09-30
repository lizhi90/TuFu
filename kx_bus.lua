-- ============================================================
-- kx_bus.lua — 总线初始化（分步，不阻塞 4321）
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_bus.lua") 编译前展开；
-- ★include 顺序 = 依赖顺序（模块间按展开后同一 chunk 共享 local），勿调换。
-- 模块化拆分：2026-09-30（只搬运不改逻辑）。
-- ============================================================

-- ======================== 总线初始化（分步，不阻塞 4321） ========================
local bus_phase = 0        -- 0=复位/重扫 1=扫描 2=启动 3=清错/使能 4=运行
local bus_retry_at = 0

local function bus_reset()
    bus_phase = 0
    bus_ok = 0
    mvmode = 0
    mvwait = 0
end

local lost_was_enabled = 0     -- 掉线前是否处于使能（AUTO_REENABLE=1 时用）

local function bus_step()
    -- v0.8.1：同步内核真实总线状态——掉线时内核会自动去使能，脚本/HMI 必须据实显示
    local hw_bus = (pcall(BUSOK) and BUSOK() == 1) and 1 or 0
    if hw_bus ~= bus_ok then
        bus_ok = hw_bus
        mvmode = 0
        mvwait = 0
        if hw_bus == 0 then
            lost_was_enabled = wdog
            wdog = 0
            print("[bus] 总线掉线：内核已自动去使能" ..
                  (lost_was_enabled == 1 and "（恢复后需重新使能）" or ""))
        else
            print("[bus] 总线已恢复" ..
                  (AUTO_REENABLE == 1 and lost_was_enabled == 1 and "：自动重新使能"
                   or "（等待操作员使能）"))
            if AUTO_REENABLE == 1 and lost_was_enabled == 1 then
                local ok, err = pcall(ENABLE)
                if ok then
                    wdog = 1
                else
                    print("[bus] 自动重使能失败：" .. tostring(err))
                end
            end
            lost_was_enabled = 0
        end
    end

    if bus_phase == 0 then
        RAPIDSTOP(0)   -- Kine-X: 参数是轴号（0=本机唯一轴）；ZBasic 的 mode 2=全停
        -- 等停稳：仅使能状态下才等（Kine-X 未使能时 idle 恒 0，直接等会白等超时）
        if pcall(ENABLED) and ENABLED() == 1 then pcall(WAITIDLE) end
        -- Kine-X 轴数由配置固定（本项目单轴）；旧程序的 ATYPE 清残留循环针对 ZMC 动态映射，
        -- 这里只对轴 0 做等价设置（ATYPE/UNITS 在新控制器仅记录，不参与换算）。
        ATYPE(0, 0)
        bus_phase = 1
        bus_retry_at = now()
        return
    end

    if bus_phase == 1 then
        if elapsed(bus_retry_at) < 0 then return end      -- 重试等待中
        local n = SLOT_SCAN(0)
        if (n or 0) < 1 then
            print("[bus] 未扫到从站，2s 后重试")
            bus_retry_at = now() - 2000
            return
        end
        print("[bus] 总线扫描成功，从站数=" .. tostring(n))
        bus_phase = 2
        return
    end

    if bus_phase == 2 then
        local ok = SLOT_START(0)
        if (ok or 0) == 0 then
            print("[bus] 总线开启失败，2s 后重试")
            bus_phase = 1
            bus_retry_at = now() - 2000
            return
        end
        print("[bus] 总线开启成功")
        bus_phase = 3
        return
    end

    if bus_phase == 3 then
        -- 轴参数（等价旧程序映射段；单轴固定轴 0）
        ATYPE(0, 65)
        UNITS(0, PULSE_EQUIV)
        DRIVE_PROFILE(0, 0)
        DISABLE_GROUP(0)
        SPEED(0, 100)
        ACCEL(0, 1000)
        DECEL(0, 1000)

        -- 清驱动器错误：等价旧程序控制字 128(清错) -> 6(关机) -> 15(使能)
        pcall(DRIVE_CONTROLWORD, 0, 128)
        DELAY(10)
        pcall(DRIVE_CONTROLWORD, 0, 6)
        DELAY(10)
        pcall(DRIVE_CONTROLWORD, 0, 15)
        DELAY(10)
        -- 清控制器错误（Kine-X: DATUM(0) = 6040h 0x80 故障复位）
        pcall(DATUM, 0)
        DELAY(100)
        -- 使能
        local ok, err = pcall(ENABLE)
        if not ok then
            print("[bus] 使能失败：" .. tostring(err) .. "；2s 后重试")
            bus_phase = 0
            bus_retry_at = now() - 2000
            return
        end
        wdog = 1
        bus_ok = 1
        bus_phase = 4
        print("[bus] 轴使能完成，进入运行")
        -- ★运行态补发（2026-09-30）：init 阶段命令投递可能未被 RT 取走而丢失
        --   （实证：JOGLEAD 读回 0、4x71 速度未进内核）——在“进入运行”时刻补发一次（幂等）：
        --   SPEED/ACCEL/DECEL（内核缺省）+ SRAMP（4x73 曲线）+ FASTDEC（停机减速度）
        if rail_apply_saved then rail_apply_saved() end
        pcall(FASTDEC, 0, FASTDEC_MM)
    end
end

