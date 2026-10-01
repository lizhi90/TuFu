-- ============================================================
-- kx_mbsync.lua — 脚本与固件 Modbus 库存的按区同步（P3b，2026-10-01）
-- 由主文件 EtherCAT_SocketServer.lua 以 include("kx_mbsync.lua") 编译前展开；须在 kx_base 之后。
--
-- 背景：P3b 后 502 从站由**固件** ModbusServer 独占（planA/20）；脚本业务（机器人桥接/
--   称重/泵/HMI 命令区）仍在本脚本中，但寄存器读写经本模块与固件库存同步：
--     · 每拍先 pull：固件区 → 本地缓存（覆盖非脏字；本拍脚本已改写的脏字保持本地值）
--     · 每拍末 push：脏字按**连续段**回推固件（只写脏段，绝不覆盖主站刚写的其它字）
--   同步区 = HMI 0~255 + 机器人 1000~1199（与业务使用的寄存器面一致）。
-- 依赖命令：MBREG_ZONE(start,count)（打包 u16 大端字符串）、MBREG_PUT(start,packed)（固件 ModbusServer）
-- ============================================================

local MBSYNC_ZONES = {
    { 0, MB_REGN },                        -- HMI/轴/称重/泵区
    { MB_ROBOT_BASE, MB_ROBOT_N },         -- 机器人区（含 Kine-X 轴0定位 1080/1082/1140）
}
for _, z in ipairs(MBSYNC_ZONES) do
    z.fmt = ">" .. string.rep("H", z[2])   -- 预生成解包格式（每拍直接用）
end

local mbsync_warned = false

-- 固件 → 本地（不覆盖本拍未推送的脏字）
mbsync_pull = function()
    if not MBREG_ZONE then
        if not mbsync_warned then
            mbsync_warned = true
            print("[mbsync] 固件库存通道不可用（MBREG_ZONE 缺失）——脚本按本地缓存运行")
        end
        return
    end
    for _, z in ipairs(MBSYNC_ZONES) do
        local s, n = z[1], z[2]
        local ok, packed = pcall(MBREG_ZONE, s, n)
        if ok and type(packed) == "string" and #packed == n * 2 then
            local vals = { string.unpack(z.fmt, packed) }
            for i = 1, n do
                local a = s + i - 1
                if not reg_dirty[a] then regs[a] = vals[i] end
            end
        end
    end
end

-- 本地脏字 → 固件（按连续段；只推脏段）
mbsync_push = function()
    if not MBREG_PUT then return end
    for _, z in ipairs(MBSYNC_ZONES) do
        local s, n = z[1], z[2]
        local i = 0
        while i < n do
            if reg_dirty[s + i] then
                local j = i
                while j < n and reg_dirty[s + j] do j = j + 1 end
                local t = {}
                for k = i, j - 1 do t[#t + 1] = string.pack(">H", regs[s + k] or 0) end
                pcall(MBREG_PUT, s + i, table.concat(t))
                for k = i, j - 1 do reg_dirty[s + k] = nil end
                i = j
            else
                i = i + 1
            end
        end
    end
end
