# 21 · Modbus 主站封装设计（设备 + 点位组态；P4）

> 状态：**M-P1 固件已部署验证（985880 B，caps d13，真机自环 4x699=2）；M-P2 插件「Modbus 主站」页已完成（v0.12.0，smoke 355/355）；M-P3 现场联调待做** ｜ 前置：`20`（P1~P3 已部署）、`13`
> 一句话：把"控制器作为 Modbus **主站**去读写外部设备"也做成**固件能力 + 插件组态**——
> 用户填**设备表**与**点位表**（点名 ↔ 功能码/地址/类型 ↔ 映射到 4x 或脚本点名），
> 设备数据自动落到 4x（屏/机器人直接用）或按名供脚本使用；不在脚本里手写帧。

---

## 1. 目标与边界

| # | 目标 | 说明 |
|---|------|------|
| G1 | **固件内建主站** | C++ 服务线程：连接/轮询/写回/超时重试/断线重连；脚本零协议代码 |
| G2 | **插件组态** | 「Modbus 主站」页：设备（链路/站号/周期）+ 点位（点名/方向/功能码/地址/类型/映射）；保存即热加载 |
| G3 | **两种使用面** | `map=reg`：值落到 4x 地址（触摸屏/机器人/PLC 按地址用）；`map=var`：仅按名供脚本用（`MB_READ/MB_WRITE`） |
| G4 | **状态可见** | 每设备 在线/最近错误/超时计数；插件在线状态列 + 脚本可查（`MBD_STATUS()`） |

**不做（边界）**：不做 Modbus 网关/路由；不迁移本工程既有业务（称重/泵保持现状，属项目脚本逻辑，将来可选用新能力重构）。

---

## 2. 配置模型（`/userdata/kine-x/config/modbus_master.json`；D13 `mbdev.get/set`）

```json
{
  "version": 1,
  "devices": [
    {
      "name": "变频器1",
      "link": { "kind": "tcp", "host": "192.168.1.50", "port": 502 },
      "unit": 1,
      "timeout_ms": 300, "retries": 1,
      "poll_ms": 200,
      "points": [
        { "name": "运行频率", "dir": "read",  "fc": 3,  "addr": 4096, "count": 2,
          "type": "f32", "map": { "kind": "reg", "addr": "4x600" } },
        { "name": "启动命令", "dir": "write", "fc": 16, "addr": 8192, "count": 1,
          "type": "u16", "map": { "kind": "reg", "addr": "4x601" }, "on_change": true },
        { "name": "故障码",   "dir": "read",  "fc": 4,  "addr": 16,   "count": 1,
          "type": "u16", "map": { "kind": "var" } }
      ]
    },
    {
      "name": "称重网关",
      "link": { "kind": "rtu-tcp", "host": "192.168.1.80", "port": 10123 },
      "unit": 1, "timeout_ms": 300, "retries": 1, "poll_ms": 200,
      "points": [ { "name": "重量", "dir": "read", "fc": 3, "addr": 0, "count": 1,
                    "type": "i16", "map": { "kind": "var" } } ]
    }
  ]
}
```

| 字段 | 取值 | 说明 |
|------|------|------|
| `name`（设备/点位） | 1~32 字节，唯一 | 脚本访问名：`设备名.点名`；点位名在设备内唯一，设备名全局唯一 |
| `link.kind` | `tcp` / `rtu-tcp` | `tcp`=标准 Modbus-TCP（MBAP）；`rtu-tcp`=RTU 帧经 TCP 透传网关（无 MBAP，含 CRC；与称重/泵同款链路） |
| `unit` | 1..247 | 从站号（RTU 必填语义；TCP 用于 MBAP uid 匹配） |
| `fc` | 1/2/3/4/5/6/15/16 | 读：01/02/03/04；写：05/06/0F/10（与 dir 匹配，校验） |
| `addr/count` | 0..65535 / 按 fc 限额 | 线圈按位、寄存器按字；`count` 与类型一致性校验 |
| `type` | `u16/i16/u32/f32/f32hi` | 编解码（同从站定义；`f32hi`=高字在前）；写点位用脚本/4x 值编码 |
| `map` | `{kind:"reg", addr:"4xN"}` / `{kind:"var"}` | 读：设备值 → 库存/变量；写：4x/变量变化 → 下发 |
| `on_change` | true/false（写点位） | true=4x 变化即下发（默认）；false=仅脚本 `MBD_WRITE` 显式下发 |
| `poll_ms` | 20..60000 | 读点位轮询周期（按设备节流；同设备请求串行） |

**校验**：设备/点位重名、fc↔dir 匹配、count 与类型一致、`reg` 映射地址不重叠且不与从站组态冲突（同一 4x 地址被主站写+从站组态只读 → 拒绝）、`rtu-tcp` 必须 `unit`、超时/周期范围、条目上限（设备 ≤32、点/设备 ≤256）。

---

## 3. 运行时语义（固件 `ModbusMaster`，独立服务线程，非 RT）

- **连接**：每设备一条链路（`tcp`=长连接；`rtu-tcp`=长连接、帧按 RTU+CRC）；断线指数退避重连（1s 起、≤10s），重连后恢复轮询。
- **读**：按 `poll_ms` 节流轮询读点；成功后 `type` 解码 → `map=reg` 写库存（仅值变化才写，避免刷屏事件）/`map=var` 存变量表；失败按 `retries` 重试，再失败计错误、置离线（连续 N 次）。
- **写**：`map=reg` 的写点监听库存值变化（比对上次下发值）→ 下发（`on_change`）；`map=var` 写点由 `MBD_WRITE` 直接下发。
- **串行化**：同设备请求队列串行，避免交错；单请求超时 300ms 默认。
- **状态**：`{online, err_code, timeouts, last_ok_ms}`；日志限频。
- **线程安全**：值/状态表加锁；与 RT 循环零耦合；与 `ModbusServer` 库存共享一把锁或经接口写入。

## 4. 命令与调试协议

| 面 | 内容 |
|----|------|
| 脚本（两引擎同名） | `MB_READ("设备.点位")` / `MB_WRITE("设备.点位", v)`（沿用现有 MB_* 命令，**先查从站组态名，再查主站点位名**）；`MBD_STATUS("设备")`→在线/错误/超时；`MBD_LIST()`→设备与点位清单 |
| 调试 | **D13 `mbdev.get/set`**（组态读写 + 热加载，caps 增 `d13`）+ `mbdev.status`（在线状态，供插件轮询/面板） |
| 插件 | 「工具 → **Modbus 主站**」独立面板：设备列表 + 点位表格（新增/编辑/删除/校验/保存）+ 在线状态列（缺 `d13` 置灰） |

## 5. 实施分期

| 期 | 内容 | 验收 |
|----|------|------|
| **M-P1 固件** | `src/modbus/modbus_master.{h,cpp}`（链路/轮询/写回/重连 + 编解码复用 `modbus_config` 的 encode/decode + CRC16）+ `modbus_master_config`（解析/校验）+ `MBD_*` 命令 + D13 + `mbdev.status` | ctest：`modbus_master_test` —— **用现有 `ModbusServer` 当假从站**做环回（读/写/异常/超时重试/断线重连/reg 映射/var 映射/on_change） |
| **M-P2 插件** | 「Modbus 主站」面板（设备/点位编辑 + 校验 + 保存 + 状态） | tsc 0 error + smoke（纯逻辑）+ Mock 联调 |
| **M-P3 部署** | 固件部署（重启窗口）+ 真机联调（可用现场任意 Modbus 从站，如称重网关做只读验证） | 读点位落 4x 可被屏读到；写点位经脚本/4x 下发成功 |

## 6. 风险与开放问题（已按推荐定案，2026-10-01）

1. **TCP/RTU 按 kind 分支** ✅（已实现：TCP 无 CRC 按 tid 匹配；RTU 有 CRC 按 unit+fc）；
2. **写点与从站只读冲突 → 组态拒绝** ✅（`mb_master_cross_check`，D13 与启动均校验）；
3. **写回=变化才发** ✅（首观测建立基线不主动下发；`on_change=false`/变量映射走 `MB_WRITE`/`MBD_WRITE` 显式下发）；
4. **在线状态 v1 走 `MBD_STATUS`/`mbdev.status`/插件** ✅（寄存器化留作选项）；
5. **编解码复用** ✅（`mb_encode_value/mb_decode_value` 提为公共函数，从站/主站共用）。

## 7. 变更记录

| 日期 | 变更 |
|------|------|
| 2026-10-01 | 初稿：设备/点位组态 schema、运行时语义、`MB_READ` 统一点名、D13、M-P1~M-P3 分期 |
| 2026-10-01 | 定案（"按推荐来"）并完成 **M-P1**：`modbus_master_config`/`modbus_master` 固件实现；公共编解码；`MBD_STATUS/MBD_LIST`（90 条命令）；D13 `mbdev.get/set/status`；`modbus_master_test` 24/24（ModbusServer 假从站 + RTU 假从站）、ctest 19/19 |
| 2026-10-01 | **M-P2 插件「Modbus 主站」页**（v0.12.0）：设备+点位编辑/校验/保存（D13）+ 在线状态；smoke 355/355（含"内联脚本可解析"守卫）；`kinex-debug-0.12.0.vsix` |
| 2026-10-01 | **M-P1 部署并真机自环验证通过**：固件 985880 B；`mbdev.set` 组态「自测→127.0.0.1:502 读 4x3」→ 轮询落 **4x699=2**、`mbdev.status` online/ok=8；恢复空组态。板端 `deploy/36-build-m2.sh` 源清单已补 modbus_master 两文件 |
