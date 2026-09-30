# Kine-X 项目长期记忆

## 项目概览
- 目标：用「鲁班猫2 (RK3568, Debian12) + IgH EtherCAT 主站 + 自研 C++17 应用」**1:1 替换**正运动 ZMC 控制器（ZBasic `EtherCAT_SocketServer.bas`），保留汇川 SV630 单轴伺服。
- 仓库根：`/home/ubuntu20/Kine-X/`（**非 git 仓库**，无版本历史可查）。
- 旧程序保持不动（仅参考/回退）：`EtherCAT_SocketServer.bas`、根 `README.md`、`ZBasic文档.md`；设备手册唯一查阅依据（SV630 学习笔记、称重、xCore 学习笔记、蠕动泵学习笔记）。
- 新工程全部在 `lubancat2/`：`src/`(C++17)、`tools/`、`config/`、`deploy/`、`docs/planA/`(01~17)、`Extension/`(VSCodium 插件, TypeScript)。
- 每日明细在 `.codebuddy/memory/2026-*.md`（先读最新）；本文件只留长期有效的事实。

## 对外不变量（兼容目标）
- 4321 ASCII 命令字/应答（含字面 `/n` 后缀、`beat/n` 心跳）；502 Modbus-TCP 从站寄存器映射（站号 1）。
- ★ 502 32 位浮点字序 = **低字在前**（2026-09-26 触摸屏实测定案，与旧 ZMC `MODBUS_IEEE` 一致）：脚本 `MB_FLOAT_ORDER=1`、`MotionHost::MODBUS_IEEE`、文档 05/08/10 已统一；字序不一致时非零位置显示 `####`。
- 称重：TAS-LAN-869 网关 `192.168.1.80:10123` **纯透传 Modbus-RTU**；机器人：**Modbus TCP 直连**（机器人作主站连控制器 `502` 多客户端；**2026-09-28 起，原 4320 Socket 已摘除**，见 `planA/19`）；轴单位 `1 unit = 1mm`（当量由脚本 `UNITS` 运行时设置，控制器无默认）。
- ★ 创锐蠕动泵（RS485 Modbus-RTU 从站，8N1/9600/地址 1~31）：规约依据 = 根 `保定创锐蠕动泵通讯规约_学习笔记.md`；FLOAT **高字在前**（与 502 低字在前相反，镜像必换字序）；业务错误统一 Modbus 异常 04 → 读输入寄存器 50；保持 reg0（485 使能）**掉电不存储**，每次上电须重发 `06 reg0=1`。
- 业务网 `eth1` 出厂固定静态 `192.168.1.11/24`；`eth0` 独占 EtherCAT（NM unmanaged、无 IP）。

## 机器人（珞石 xCore）链路（2026-09-26 定案）
- 协议依据 = xCore 手册 **10.3.3 交互指令**（根 `xCore控制系统使用手册V2.2_学习笔记.md`，原始提取 `pdf_out/xcore_*`）：本站 12 条监控 + `RON/ROFF/RMAIN/RSTART/RSTOP/RCLEAR/RAUTO/RMAN/RDRAG/RUDRAG/RLIST/RCUR` 逐条对应。
- 结束符 **`\n`**（脚本常量 `ROBOT_SUFFIX`，须与机器人侧「外部通信→后缀」一致）；`load_prog` **含冒号**已定案（p.156 图示）；`estop_state` 极性受机器人设置影响（脚本 `ROBOT_ESTOP_HIGH`）；`jnt_trq` 单位=额定力矩千分比。
- 新增 4321 命令 **`RSOCK,<0|1>;`** → `xCore::SocketInterface::Enable/Disable`（无返回值，发送后不等应答）。
- 备选路径（笔记已整理）：10.5.6 寄存器远程控制（7 个 ext_* 寄存器、命令码 1~14）、系统 IO(10.2)、RCI(1337)、OPC-UA(10.13)。

## 端口语义（2026-09-26 拍板）
- 编号 0..15 仅为进程内**槽位句柄**（`PORT_MAX()` 返回 15），无 ECUSTOM/通道规划（ZMC 遗留结论）；**不要用 `local PORT_MAX` 覆盖同名命令**。
- 静态容量 64、运行期上限默认 16，可经 **D9** `port.max.set`（插件「控制器→修改端口数量」）调整并持久化 `.portmax`；占用时拒绝收缩回 `BUSY`。

## 关键约定
- 改 `EtherCAT_SocketServer.bas` 必须同步根 `README.md` + `ZBasic文档.md` 并在变更记录登记。
- planA 文档链：`13`(DebugServer, JSON-Lines/TCP:5000) → `14`(选型) → `15`(插件任务) → `16`(报文样例+Mock) → `17`(D4/D5 验收)。
- Extension 回归口径：`npx tsc -p ./` 0 error → `npm run test:smoke` 全绿 → `vsce package` 出 VSIX。
- ★ 控制器文件一致性（v0.8.4）：`file.list` 带 `hash`（FNV-1a 64，`debug_server.cpp fnv1a64_hex`）；插件比对 `controller-sync/<同名>` → 一致/不一致/本地无副本/未比较；demo.lua 更新后插件显示「不一致」，**重新拉取即恢复**。
- 硬纪律「**降级不伪装**」：`caps` 无 `dX` 时 UI/Adapter 必须明确报不支持。
- Extension 边界：纯逻辑模块不 import vscode（可 Node 单测）；UI 文案集中 `strings.ts`；命令名从 `command_table.h` 经 `gen-commands.mjs` 生成（改 kDocs/命令后必须重跑）。
- 插件形态铁律：侧边栏控制台只放菜单栏+状态行（高频表格会冲掉菜单交互）；轴状态/Modbus/通讯状态均为右侧独立面板；**面板可见才订阅**（引用计数）；`extension.ts onEvent` 的 switch 新增事件主题必须显式 `case`（v0.8.8 空表根因）。

## 当前状态（2026-09-27）
- ★ **M3（CSP）+ 多轴直线插补已完成（2026-09-27）**：`motion/interp.h`（Trapezoid+LinInterp）+ `Axis` CSP 子模式（6060=8 每拍 607A，完成=规划完+容差）+ DC SYNC0 1ms 真机启用 + 脚本 `MOTION_MODE(0|1)`/`BASE(n1,n2,...)` 多轴/`MOVEABS(p1,p2,...,spd,acc)` 直线插补（阻塞）；真机 CSP 定位 1mm 精确（MPOS=DPOS=1.000）；**现场默认仍 MOTION_MODE=pp**（csp 通路保留，config/脚本可切）；多轴 LIN 真机待 ≥2 轴硬件（单测覆盖：interp_test 6 组 + axis_test 用例14/15 + motion_host_test 用例7，ctest 16/16）。详见 planA 04 §3.3/§3.4。
- **未做（拍板 2026-09-27）**：**圆弧插补（MoveC 类）暂不实施**——需求出现时在 LinInterp 基础上扩展参数化路径（s(t) 线性映射→弧长映射），DC/CSP 链路复用；多轴插补真机联调待加第二根轴。清单见 planA README §五.5。
- ★ **PREEMPT_RT 内核已部署为默认（2026-09-27）**：板上运行 `6.1.99-rt36-rk356x`（野火官方 `lbc-develop-6.1-rt36` 分支，最小差异配置，PC 交叉编译）；绑隔离核 CPU3 cyclictest **Max=12µs/Avg=1µs**（旧内核 ~500µs，40 倍改善）；IgH 模块已按新内核重编，EtherCAT OP/502/4321/称重/机器人全链路正常。**内核切换入口 = `/boot/Image` 符号链接**（uEnv 的 uname_r 仅 fallback；实际生效 uEnv 是 `uEnvLubanCat2-V2.txt`）；回退 = `ln -sf Image-6.1.99-rk356x /boot/Image && reboot`。详见 planA `02` §2.5、`06` M0 验收（❌→✅）。
- **板端已部署最新固件（23:15）**：caps = d1~d10（d5 BASIC/Lua 均支持；d7 重启；d8 主文件；d9 端口上限；d10 conn 订阅）；`demo.lua`（md5 `5343f9f9…`）为主文件开机运行，`PORT_INFO` 声明 4 条连接（10=4321 服务端、11=称重主站、12=机器人客户端、13=502 从站）；真机验证：conn 4 连接 + 从站 SV630 明细（vid=0x100000/pid=0xC0112/AL=8）、502/4321/称重/机器人全通、总线自恢复（`AUTO_REENABLE=1`）。
- **控制器协议能力**：D1~D10 全实装并有单测（`debug_server_test` 237/237）；`mb` 事件带 `used` 位图（v0.8.6，脚本 `mirror_used()` 100ms 镜像读写地址）；`PortManager` 进程级连接快照 + `PORT_INFO(端口,用途[,主从])` 标签。
- **插件当前版本 0.8.8**（`Extension/kinex-debug-0.8.8.vsix`，smoke 313/313）：0.8.5 修改端口数量、0.8.6 Modbus 已用寄存器高亮+仅显示已用、0.8.7 工具→通讯状态（conn 订阅）、0.8.8 修 conn 事件漏 case。VSIX 升级后须 Reload Window。
- 脚本 `EtherCAT_SocketServer.lua`（根目录）= 旧 .bas 的 1:1 Lua 移植 + 现场增强：502 先于总线监听、Modbus/4321 应答优先于 bus_step、机器人日志限频 3s、`mirror_used()`、`RSOCK`、`AUTO_REENABLE`；自测 `tools/port_script_selftest.lua` 52/52（ctest 15/15）。
- 触摸屏现场要点：重启触摸屏用它自身重启（整柜断电会连带伺服/机器人/称重复位并伴随 ECAT 掉线→内核安全去使能，恢复后靠 `AUTO_REENABLE` 自动回使能）；TCP 半开靠 SO_KEEPALIVE（10/5/3≈25s 判死）释放。
- 待办：按 `planA/17` 跑 D4/D5 实机联调（`d45-real-verify.mjs` 待重跑）；调试口**单客户端串行**（插件连着时其它客户端排队，排查前先断开插件）。

## 部署/验证坑（反复踩，必读）
- `finalize-deploy.sh` 第 0 步删旧产物再等编译：**正确姿势 = 起 `36-build-m2.sh`（后台）后 1~2 分钟内就起 finalize**（不要等编译完成）；`/tmp/kx_remote_build.log` 可能是上一次的陈旧日志，编译结果看 run_bg 本地日志。
- `10-install-service.sh install` 的 `enable --now` 不会重启已运行服务；finalize 后需手工 `systemctl restart kine-x`（并核对 MainPID 变化）；重启前必须征得用户确认。
- 板端 `36-build-m2.sh` 是手写源文件清单，**加新 .cpp 必须同步**（曾漏 `port_config.cpp`）。


## 当前状态（2026-09-28，蠕动泵功能完善 + 屏侧校准/持久化）

### 蠕动泵（全流程可用，板端已部署）
- 链路：网关 `192.168.1.81:10123`（句柄 14/任务 5）；**★站号=4**（扫描 0~20 实测）；485 使能自动重试（`ctl≠2` 每 3s 重发）。
- 寄存器（4x202~229，屏端全按数值配；详见 planA/05 §8 与泵笔记 §0）：
  - 202 状态码（0~5）、203 命令码（1 启动/2 停止/3 写转速/4 重新使能/5 全速ON/6 全速OFF，写即触发、自动回 0）
  - 204 设定转速（INT16，1~300，越界本地拒绝）、205 读回转速、206/207 流速（float 低字在前）、208 方向、209 错误码、210 控制方式
  - 211/212 圈数（float）、213 圈数运行；215/217 回吸角度/速度（读回仅变化时刷新）、219 回吸提交
  - 221/222 ml/圈、223/224 预计出液量（1 位小数）、225/226 设定容积（**现场屏 ×10 → `VOL_IN_SCALE=0.1` 还原**）、227/228 预计圈数（1 位小数）、229 容积运行
  - **运行时先下发设定转速**再启动（保证计时与实际一致）；4x203=2 可中途停
- `PSTA;` 诊断：`conn,ok,ctl,run,rpm,flow,errcode,REVS,SET,ANG,SPD,MLREV,VOL,TVOL,PREV`。
- 掉电保存：204/211/212/221/222/225/226 → `.nvram`（C++ `nvram_store` + `NVSET/NVGET`，脚本 init 恢复）。
- TODO：分配模式/校准/方向/泵头泵管型号（保持寄存器 2~17 区）。

### 称重（现场实测入档）
- **折线校准教训**：用 1~25g 小砝码做的折线导致低端跳变（AD 噪声信噪比差）→ 救援路径 `4x30=3` 关闭折线、`4x30=4` 单点砝码校准；折线提交通道 4x28（重量输入，最小 1g，**按比例系数换算为模块计数**；满 10 点后再提交自动新一轮）。
- 4x136 称重状态码（0 空闲/1 未连接/2 出错/3 等数据/4 有效/5 执行中/6 成功/7 失败，命令态保持 2s）。
- 4x124/125 = 模块**原始计数**（= 显示值 × 比例系数），重量显示用 4x122/123。

### 机器人启动（现场定案）
- 屏按钮写成对 `1→0`：写队列（同批按序派发）+ 回弹 0 去抖（`ROBOT_IGNORE0_MS=1000`）+ 启动时序在途防重；`pp_to_main` → 延迟 `ROBOT_START_DELAY_MS=500ms` → `start`；报文最小间隔 `ROBOT_CMD_GAP_MS=50ms`。
- 4320 后缀 `\n`、`RSOCK` 映射、急停极性 `ROBOT_ESTOP_HIGH` 已定案；`start=false` 时查机器人侧（暂停绑定/工程/上电时序）。

### 插件
- **v0.8.14**（`Extension/kinex-debug-0.8.14.vsix`，smoke 319/319）：NVSET/NVGET 进补全/帮助（81 条命令）；0.8.13 起含 UTF-8 分片解码修复（拉取一致性问题）。
