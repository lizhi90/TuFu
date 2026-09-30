# xCore 控制系统使用手册 V2.2（A）——学习笔记 / 整理

> **来源**：`xCore控制系统使用手册V2.2_A.pdf`（珞石机器人 ROKAE，xCore 控制系统 V2.2，文档版本 A，共 381 页）。
> **整理方式**：全文文本提取（`pdf_out/xcore_full.txt`，含页码标记）+ 按章归类；页码引用格式 `(p.N)` 为 **PDF 物理页**。
> **本笔记定位**：工程用速查与学习材料，不替代原手册；安全相关操作以原手册与现场规程为准。提取不清处以 `（提取不清）` 标注。
> **原始提取与分章切片**：`pdf_out/xcore_full.txt`、`pdf_out/xcore_outline.txt`、`pdf_out/xcore_s*.txt`。

## 它是什么（速览）

- **xCore = 珞石（ROKAE）机器人控制系统**，CS 架构：HMI 软件 **RobotAssist**（PC 端 / xPad2 示教器）+ 控制器软件 **RC**。
- 编程语言 **RL（Rokae Robot Language）**：ABB RAPID 风格（`PROC/FUNC`、`MoveJ/MoveL/MoveAbsJ`、`robtarget/pos/pose`、`speed/zone/tool/wobj` 等），HMI 内提供工程/任务/变量/点位/路径/IO/坐标系/工具/工件/视觉等列表化编辑。
- 机器人系列：**工业机器人**（XBC5 控制柜等）与**协作机器人 xMate ER / CR**（差异在手册中多处标注，如拖动、xPanel、末端工具）。
- 对外通信能力（第 10 章）：系统 IO、**TCP Socket 交互指令**、总线（Modbus TCP/RTU、CC-Link、EtherCAT、PROFINET）、**寄存器（含寄存器远程控制）**、IO 设备、RCI（默认端口 1337）、OPC-UA、串口（仅工业柜）等。
- 安全：软/硬急停、安全区域/安全位置、安全门、碰撞检测、外部/手持急停（mini 安全板固件 ≥1.0.8.7）等。

## ★ 与本项目（Kine-X）的对照要点

**本站机器人链路（`EtherCAT_SocketServer.lua` 的 `robot_step`）用的正是手册 10.3「外部通信」的 TCP Socket 交互指令**：控制器（鲁班猫2）作 **TCP 客户端** 连机器人 `192.168.1.221:4320`，指令以 `\r` 结尾；机器人侧须在 HMI「通信 → 外部通信」配置为**服务端**（监听端口 + 后缀 `\r`）并启用。

| 手册 10.3.3 | 本站脚本实现 | 备注 |
|---|---|---|
| 监控 `motor_on_state / robot_running_state / estop_state / operating_mode / home_state / fault_state / collision_state / task_state / cart_pos / jnt_pos / jnt_vel / jnt_trq` | `ROBOT_KEYTAB`（12 条，逐条一致） | 另可用 `*_name` 变体（带字段名前缀），本站未用 |
| 控制 `motor_on / motor_off / pp_to_main / start / stop / clear_alarm / switch_mode:auto / switch_mode:manual / open_drag / close_drag / list_prog / current_prog` | `ROBOT_CTLTAB` + 4321 短命令 `RON/ROFF/RMAIN/RSTART/RSTOP/RCLEAR/RAUTO/RMAN/RDRAG/RUDRAG/RLIST/RCUR` | 一一对应 |
| `load_prog: <工程名>`（**p.156 配置界面图示含冒号**；p.157 正文表格简写作 `load_prog + (工程名)`） | `RLOAD,<工程名>;` → 实际发送 `load_prog:<工程名>`（含冒号） | ✅ 与图示一致（2026-09-26 据手册 156/157 页核对，无需再现场验证） |
| `xCore::SocketInterface::Enable/Disable` | 未使用（现场在 HMI 侧启用） | 可选补充：断链时远程启停接口 |

现场注意事项（手册已明示、容易踩）：
- **`estop_state` 的 true/false 含义受机器人「急停触发电平类型」设置影响**（高低电平两种极性），上位机解析报警时不能想当然；
- **`task_state` 取值**：`ready/jog/load_identify/dynamic_identify/drag/program/demo/rci/debug`；
- `jnt_trq` 单位是**电机额定力矩的千分比**（非 N·m）；`jnt_pos` 单位 rad（导轨 m）；`cart_pos` 含四元数 q1~q4；
- 机器人侧「运动状态/程序运行状态」等系统输出与 socket 监控量一致地**不统计辨识/拖动/力控/拖动回放**；
- 有“暂停”功能绑定寄存器/系统 IO 未复位时，**任何方式都不能启动程序**。

**手册提供的其它可替代/增强路径（备查）**：
- **寄存器远程控制（10.5.6）**：用 Modbus/总线寄存器做 Jog、更新点位、运动到点、设工具/工件等（命令码 1~14 + 三级错误码），适合 PLC 深度集成；
- **系统 IO（10.2）** 硬接线上下电/启停/急停复位；**RCI（10.8，端口 1337）** 底层实时控制；**OPC-UA（10.13）** 标准化上位集成；
- 协作机型专属：拖动（socket `open_drag/close_drag` 或 HMI）、xPanel（CR）、末端工具/电爪吸盘（ER/CR）。

