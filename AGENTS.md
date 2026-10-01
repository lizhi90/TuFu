# AGENTS.md — Kine-X 项目工作指南（面向 AI 代理）

> 代理进入本仓库的入口文档：先读本文，再按需下钻 `lubancat2/docs/planA/`。
> 动态状态与最新决策在 `.codebuddy/memory/MEMORY.md`（长期）与 `2026-*.md`（按日期，先读最新）；两者冲突时以代码与 planA 文档为准。
> 本仓库已于 **2026-09-30** 建立本地 git 仓库并推送至**私有**远端 `lizhi90/TuFu`（分支 `main`）；
> 该日期之前无版本历史，改动前仍建议自行留备份。提交方式与四种方式对比见 `lubancat2/docs/GitHub提交指南.md`。

## 1. 项目一句话

用「鲁班猫2（RK3568, Debian 12）+ IgH EtherLab EtherCAT 主站 + 自研 C++17 应用」**1:1 替换**正运动 ZMC 控制器（原 ZBasic 程序 `EtherCAT_SocketServer.bas`），保留汇川 SV630 单轴伺服与全部对外接口。旧 ZMC 控制器现场不拆除，可随时回退。

## 2. 目录地图

```
Kine-X/
├── EtherCAT_SocketServer.bas / README.md / ZBasic文档.md   # ★ 旧 ZMC 系统：参考与回退基准，默认勿动
├── EtherCAT_SocketServer.lua / kx_*.lua                    # ★ 新控制器主文件 + 模块（kx_base/bus/socket/modbus/scale/pump/robot；
│                                                           #   include 编译期展开、顺序=依赖；2026-09-30 模块化，IP/端口/报文/寄存器不变）
├── SV630N_手册*.md、称重传感器_*.md、单路485说明书.pdf、ZBasic编程手册*.pdf   # 设备手册（唯一查阅依据）
├── pdf_out/                  # 手册提取的章节文本（配合 pdf_scan*.py 提取脚本）
└── lubancat2/                # ★ 新工程全部在这里
    ├── src/                  # C++17：common(配置/共享状态/网口) motion(内核/IgH) script(脚本引擎/DebugServer)
    ├── tools/                # 纯逻辑自测 *_test.cpp + 板端工具（ecat_*/script_run/netcfg_tool）
    ├── config/app.conf       # 运行配置：轴/EtherCAT/脚本/调试/日志
    ├── deploy/               # 板端部署与验证脚本（ssh_run.sh / push.sh / 18-push-src / 36-build-m2 / finalize-deploy ...）
    ├── docs/planA/           # 设计文档 01~17，README.md 是索引与里程碑
    ├── Extension/            # PC 侧 VSCodium 调试插件（TypeScript，产出 kinex-debug-<ver>.vsix）
    └── third_party/lua/      # vendor Lua 5.4（静态库）
```

## 3. 架构速览

- **进程 `kine-x`**（`src/main.cpp`）：单进程多线程 = RT 1ms 循环（绑 CPU3、SCHED_FIFO，**唯一触碰 ecrt 的地方**）+ SDO 线程（6081/6083/6084 阻塞下发）+ 脚本线程（BASIC 或 Lua）+ DebugServer 线程（非 RT）。
- **线程间只经 `src/common/state.h` 的 `Shared` 快照/命令槽**通信；RT 循环不感知脚本与协议细节。
- **运动内核**（`src/motion/`）：CiA402 状态机 + PP/PV + 梯型规划；编译期上限 8 轴、运行时 `AXIS_COUNT`（当前单轴=1），加轴只改配置。
- **脚本引擎**（`src/script/`）：BASIC 子集（`script.cpp`）与 Lua 5.4（`lua_engine.cpp`），**同一时刻只加载一种语言**（`engine_rule.h` 强制）；设备命令经 `MotionHost`、端口通道经 `PortManager`。
- **调试通道归 C++ 层**（planA `13`）：JSON-Lines TCP `5000`（`debug_server.cpp`），`sys.info.caps` 报 `d1`~`d6`；D5（断点/单步）仅 BASIC 引擎。
- **业务协议归脚本层**（planA `12`）：4321 ASCII / 502 Modbus / 称重 / 机器人 4 个原生服务**代码已移除**，计划由脚本程序实现；**不要新增 `src/svc_*`**。
- **Extension**：侧边栏 = 「控制台」（菜单栏 + 状态行）+ 「控制器文件」原生 TreeView（`kine-x.files`，行内/右键拉取、删除；v0.4.9）；「轴状态」「Modbus」是右侧独立面板（勿合并）；曲线面板；**面板可见才订阅**（引用计数），事件驱动不轮询。

### 3.1 控制器代码地图（`lubancat2/src/`）

| 文件 | 职责 / 关键点 |
|------|----------------|
| `common/state.h` | 跨线程唯一共享面 `Shared`：每轴 `AxisStatus` 快照 + `AxisCmd` 命令槽（`post` 覆盖式 / **`post_wait` 等槽位投递，脚本侧命令经 `MotionHost::post_cmd` 走它，快速连发不丢命令**）、`mb_regs[256]`、任务表、总线快照 `bus_seq`。**命令带轴号，越界丢弃** |
| `common/config.{h,cpp}` | `app.conf` 解析；键大小写不敏感、支持 `AXIS<n>_` 单轴覆盖；`SCRIPT_ENGINE` 非法值明确报错；`AXIS_COUNT` 越界夹紧并告警 |
| `motion/axis.{h,cpp}` | 单轴内核：`apply(AxisCmd)` + `tick(PdoIn,PdoOut)`；PP(6060=1)/PV(6060=3)、到位判定（bit12 ack + bit10 + settle + tol）、坐标系偏移 `pos_offset_inc_`、未使能命令挂起 `pend_` |
| `motion/cia402.{h,cpp}` | 使能时序 0x06→0x07→0x0F（逐级等状态位）、故障复位 0x80 脉冲、halt(bit8) 叠加；纯状态机不碰 ecrt |
| `motion/ethercat_master.{h,cpp}` | IgH 封装：`ecrt_request_master → 读 SII 身份 → 匹配档案 → slave_config(PDO/DC/WD) → reg_pdo_entry_list → activate`；**未映射偏移=kOffsetInvalid，读写前必判**；SDO 会阻塞，只能非 RT 线程调 |
| `motion/drive_profile.{h,cpp}` + `profiles/` | 驱动器档案库（厂商相关的唯一聚集处）：vid/pid/rev、PDO 档位、halt/错误码语义；新增伺服 = 加一个 `profiles/xxx.cpp` + `KX_REGISTER_DRIVE_PROFILE` + CMake 源文件；`sv630` 实测档位1(3Rx+2Tx)/档位3被拒 |
| `motion/rt_util.h` | 绑核/SCHED_FIFO/mlockall/`/dev/cpu_dma_latency`/绝对周期唤醒 |
| `script/engine.h` | 语言无关接口 `IScriptEngine`（compile/run/status/var/trace + D5 默认空实现=不支持） |
| `script/script.{h,cpp}` | BASIC 子集引擎（词法/语法/解释器，~1700 行）：数组即 0 结尾字符串、`TICKS` 倒数计数、`RETURN` 系统变量、`dbg_gate` 语句边界断点；`request_abort()` 必须 `notify_all` 防唤醒丢失 |
| `script/lua_engine.{h,cpp}` | Lua 5.4 沙箱（去 `io/os/debug/require` 等）、`lua_sethook` 指令级预算+中止、命令同名注册、`ww.call/ret`；GC 不承诺硬实时 |
| `script/engine_rule.{h,cpp}` | 单引擎槽 `ScriptEngineSlot`：重复 load 拒绝；D4 `create_staged/replace` 唯一 new 引擎处 |
| `script/script_host.h` / `motion_host.{h,cpp}` | 设备命令宿主：`call()` 返回 0 成功 / 1 不认识 / 2 运行错误 / 3 请求中止；运动等待走 `move_result` 轮询；`SDO_WRITE` 明确不可用 |
| `script/command_table.h` | 命令名单一事实来源；改它必须同步 `MotionHost::call()` 分支 + `node tools/gen-commands.mjs` |
| `script/port_manager.{h,cpp}` | 非阻塞 TCP 通道（**TCP_SERVER 多客户端，每端口最多 4 个**，每客户端独立收发；`status/send/recv` 带 idx，默认 0=主客户端）；重复 OPEN=先关后开；禁用 22/80 监听 |
| `script/debug_server.{h,cpp}` | JSON-Lines RPC；`sys.info.caps` 据实（d2/d4 随 `allow_script`、d5 仅 basic）；D4 swap 判定先于 `join()`、旧实例 `retired_` join 后释放；D6 文件名白名单防穿越；事件推送限频 |
| `script/json_lite.h` | 零依赖 JSON（保序对象、完整转义含 `\uXXXX`） |
| `modbus/modbus_config.{h,cpp}` | **Modbus 组态模型**（planA/20）：条目解析/校验（重名/重叠/类型/权限/默认值/persist 范围）、值↔字编解码（u16/i16/u32/f32/f32hi，低/高字在先）、JSON 序列化 |
| `modbus/modbus_server.{h,cpp}` | **固件 Modbus-TCP 从站引擎**（产品内建，非 RT 线程）：502 多客户端 + FC 01/02/03/04/05/06/0F/10；4x 全空间库存（策略 A）+ 组态叠加（只读拦截 0x02/persist 落盘）；命名访问（`MB_READ/MB_WRITE/MB_LIST` 的落点）；热加载 |
| `modbus/modbus_master_config.{h,cpp}` | **主站组态模型**（planA/21）：设备（tcp/rtu-tcp、unit、超时/重试/轮询）+ 点位（方向/功能码/地址/类型/映射 reg 或 var/on_change）；解析校验 + 与从站只读交叉校验 |
| `modbus/modbus_master.{h,cpp}` | **固件 Modbus 主站引擎**（独立线程）：轮询读点→4x/变量；写点（4x 变化即发 / 显式 `MB_WRITE`）；TCP/RTU-over-TCP；超时重试、退避重连、同设备串行；`MBD_STATUS/MBD_LIST`、D13 |
| `script/nvram_store.{h,cpp}` | **4x 寄存器持久化**（NVSET/NVGET → 脚本目录 `.nvram`，文本键值+原子写；屏输入参数掉电保存） |
| `src/main.cpp` | 装配 + 线程编排：RT 循环（先 `take` 命令再 `tick`，参数经 `ParamsMailbox` 交 SDO 线程）、SDO 线程、可选脚本线程、DebugServer；100ms 发布 `Shared`/`Snap`；退出先去使能 20 拍 |

调试/排查顺序：总线问题先 `tools/ecat_probe`/`ecat_sdo`，运动问题看 RT 日志（`[stat]/[axisN]`），脚本问题走 `--script`/`script_run`，协议问题用 `npm run mock` 或直连 5000。

## 4. 常用命令（均已在本机验证）

### 4.1 控制器 C++（PC 上编译纯逻辑部分）

```bash
cmake -S lubancat2 -B lubancat2/build
cmake --build lubancat2/build -j"$(nproc)"
ctest --test-dir lubancat2/build --output-on-failure     # 12 个测试，应 100% 通过
```

- 无 IgH（`ecrt.h`）时配置**不会失败**：依赖 ecrt 的 `kine-x` / `ecat_*` 目标自动跳过，纯逻辑测试照常。
- 单跑某个测试：`./lubancat2/build/debug_server_test`（同类还有 `cia402_test`、`motion_host_test`、`port_manager_test`、`lua_engine_test` 等）。

### 4.2 Extension（VSCodium 插件）

```bash
cd lubancat2/Extension
npx tsc -p ./            # 必须 0 error（npm run lint 同义）
npm run test:smoke       # 冒烟测试，当前 221/221 全绿
npx @vscode/vsce package --no-dependencies   # 产出 kinex-debug-<ver>.vsix
npm run mock             # 无真机时启动 Mock DebugServer（tools/mock-debug-server.mjs）
node tools/gen-commands.mjs   # ★ 改过 src/script/command_table.h 后必须重生成 data/commands.json
node tools/d45-real-verify.mjs [host] [port]   # D4/D5 真机联调（planA 17 清单自动化，勿在有生产任务时跑）
```

回归口径（planA `15` §8.5）：`tsc` 0 error → smoke 全绿 → 打包 VSIX。

### 4.3 板端（鲁班猫2，`cat@192.168.1.11`）

- 所有板端操作走 `lubancat2/deploy/ssh_run.sh`（免交互，`BOARD`/`PASS` 环境变量可覆盖，默认值见脚本）；交互式命令用 `deploy/push.sh` 同步 `deploy/` 到 `/opt/kine-x/deploy`。
- **脚本推送用 `deploy/17-push-script.sh`**（主文件→板端 `demo.lua` + `kx_*.lua` 全部，逐文件 md5；`RESTART=1` 可带重启——重启前须确认）。
- 板端无 cmake，用 g++ 直编。部署流水线：
  1. `deploy/18-push-src.sh`（推 `src/ tools/ config/ third_party/ CMakeLists.txt` 到 `/opt/kine-x/app`）；
  2. 板上 `36-build-m2.sh`（编 `kine-x` 与自测，日志 `/tmp/kx_remote_build.log`）；
  3. `deploy/ssh_run.sh deploy/finalize-deploy.sh`（拷二进制/配置 → 装并重启 `kine-x.service`，日志 `/tmp/kx_finalize.log`）；
  4. 验证：`deploy/12-verify.sh`、`?*TASK` 等价物（服务状态 + 5000 端口监听）。
- 关键路径：二进制 `/opt/kine-x/bin/kine-x`、配置 `/opt/kine-x/config/app.conf`、D6 脚本目录 `/userdata/kine-x/scripts`、服务 `kine-x.service`、调试口 `5000`。
- ★ **重启 `kine-x.service` 会中断板上正在跑的脚本/电机任务**：部署/重启前必须取得用户确认。

## 5. 硬约束 / 不变量（对外接口，禁止破坏）

| 约束 | 内容 |
|------|------|
| 4321 ASCII | 命令字/参数/应答字面量完全一致，统一以字面 `/n`（两字符）结尾，心跳 `beat/n`；上位机按报文读，**不能用 readline** |
| 502 Modbus-TCP | 站号 1、4x 寄存器地址与字序（32 位"低字在前/1234"）与位定义一致 |
| 称重 | 经 TAS-LAN-869 网关 `192.168.1.80:10123` **纯透传**，控制器自己组 Modbus-RTU 帧（含 CRC 字节序），无 MBAP |
| 机器人 | **Modbus TCP 直连（2026-09-28）**：机器人作主站连控制器 502（多客户端）；A 区 4x1000~1099 机器人写 / B 区 4x1100~1139 机器人读，语义与绑定见 `planA/19`（原 4320 Socket 链路已取消） |
| 单位 | 1 unit = 1 mm（`INC_PER_MM=14043.41` / 原 `PULSE_EQUIV`），换算只做一次 |
| 网络 | `eth1` 业务口出厂固定静态 `192.168.1.11/24`（不用 DHCP）；`eth0` 独占 EtherCAT（NM unmanaged、无 IP） |
| 脚本语言 | **同一时刻只加载一种**（basic 或 lua）；引擎不匹配必须明确拒绝（`ENGINE_MISMATCH`），不得静默转换。调试口语言由 `DEBUG_SCRIPT_DIR` 内现有脚本推导（2026-09-25 拍板）：空目录=`auto` 两种都收、首次带 `name` 落盘即绑定、并存=`mixed` 拒绝编译。**多文件/开机（2026-09-26 拍板，方案 A）**：开机只跑 `.boot` 清单指定的**一个主文件**（D8，插件设主文件），其它脚本由主文件 `INCLUDE` 编译期展开为子程序；`SCRIPT_FILE`+`SCRIPT_ENGINE` 仅为主文件未设置时的回退；**Lua 模块化（2026-09-30）**：主文件 `EtherCAT_SocketServer.lua` 拆为 `kx_base/bus/socket/modbus/scale/pump/robot.lua`（include 顺序=依赖顺序），本地自测 harness 自动展开 |

## 6. 过程铁律（工程纪律）

1. **降级不伪装**：控制器未声明能力位（`caps` 无 `dX`）时，插件/适配器必须**明确报不支持**，不得用假象掩盖。
2. **D4 热更新规则**：`script.compile{p.swap:true}` 走 `create_staged` 旁路编译 + 原子替换；失败不碰运行中实例；`swap` 判定**先于** `run_th_.join()`；旧实例 `retired_` 必须 join 后才能释放。
3. **Extension 分层**：`engineRule/dapPure/statusBits/store/subscribe/panelModel/curvePure/commandsPure/config/axisPanelPure/modbusPanelPure/filesPanelPure` 等纯逻辑模块**不得 import vscode**（要能被 Node 直接单测）；UI 文案集中在 `src/strings.ts`（`S` + `setLocale()`），其余源码不含中文字面 UI 文案；命令名不得硬编码，从 `command_table.h` 经 `gen-commands.mjs` 生成。
4. **控制台形态**：侧边栏控制台只放菜单栏+状态行，**不得承载高频推送表格**（20Hz 重建 DOM 会冲掉菜单交互——v0.4.1 前"连接后菜单点不动"的根因）；轴状态与 Modbus 必须两个独立面板。
5. **同步语义**：插件「同步」= 控制器 → 工作区（拉取，落 `controller-sync/`）；「下载」= 工作区 → 控制器（上传编译）。勿混淆方向。
6. **长文件编辑**：大段改动优先整文件重写；编辑 `tools/mock-debug-server.mjs` 等长文件后必须 `node --check` 验证语法。
7. **文档同步纪律**：
   - 改根目录 `EtherCAT_SocketServer.bas` → 必须同步根 `README.md` + `ZBasic文档.md` 并在 README §十一变更记录登记；
   - 改控制器行为/配置/调试协议 → 同步 `docs/planA` 对应文档（调试协议 `13`、报文样例 `16`、插件契约 `15` §8.5、验收 `17`、`config/app.conf` 注释）；
   - 改 Extension → 同步 `15` §8.5 进度表与形态说明，升版本号；
   - 新决策/状态 → 追加 `.codebuddy/memory/MEMORY.md` 与当日记忆文件。

## 7. 已知坑速查

- **机器人链路 = Modbus 直连（2026-09-28 起；原 4320 Socket 已摘除）**：机器人作 **Modbus TCP 主站**直连控制器 **502**（多客户端，与触摸屏共享）；A 区 `4x1000~1099` 机器人写（`sta_*`）→ 桥接镜像到屏幕 `4x140~150`；B 区 `4x1100~1139` 机器人读（`ctrl_*`）← 屏幕 `4x151~157`/`4x60` 经桥接整形脉冲（100ms，手册 ≥60ms）；启动时序=上电→2s→`ctrl_pptomain`→2s→`ctrl_program_start`；心跳 `4x1000` 超时 1.5s 判离线；`4x151/155/158~160/168~200/201` 已停用；**★安全联锁（2026-09-29）**：机器人急停（`4x145` 非 0）→ 轴0 强制去使能，未复位期间每秒复查保持关闭、复位后**不自动使能**（极性常量 `ROBOT_ESTOP_ACTIVE_HIGH`）；**`sta_alarm`（`4x147`）不参与联锁**（当晚定案：其闪动会反复去使能，仅镜像显示）；地址/语义/绑定见 `planA/19`。
- **板端内核参数入口是 `/boot/uEnv/uEnv.txt`**，改 `extlinux.conf` 无效；内核无 `NO_HZ_FULL`，`nohz_full=` 是无效参数。
- `eth0` down 时看不到其 IRQ，绑亲和前必须先 `ip link set eth0 up`；批量 IRQ 迁移必须排除 EtherCAT 口。
- `sched_rt_runtime_us` 必须设 `-1`，否则 RT 线程被节流（表现为偶发大延迟）。
- `finalize-deploy.sh` 会先删除旧 `kine-x` 产物，防止"旧固件秒过部署"。
  - **★固件 502 客户端断开回收已修（2026-10-01）**：旧版 `recv()==0` 未回收 → CLOSE-WAIT 占死 8 槽（触摸屏/机器人偶发重连会"再也连不上"）且 poll 空转（实测 61% CPU）；修复后 EOF/硬错误即回收。探测 502 避免同一 IP 快速连打；编译完成后**手动拷装**（`cp app/kine-x bin/` + restart），别在编译完成后跑 finalize（会误删新产物）。
- 旧 ZBasic 侧坑（TICKS 倒数计数、`PRINT #` 不换行、称重 CRC 字节序、块式 if 必须 `endif`、标识符不区分大小写等）见根 `README.md` 第八节——改 `.bas` 前必读。
- 板端：D6 文件管理与空闲超时/swap 快照重置已随 2026-09-25 18:03 的 `kine-x.service` 重启部署到位（`/userdata/kine-x/scripts` 已建）；`tools/d45-real-verify.mjs` 待重跑。
- **语言绑定已部署（2026-09-25 20:04）**：板端新固件 `sys.info.engine="auto"`（空目录两种语言都收）；`18-push-src → 36-build-m2 → finalize-deploy` 走完，手工 `systemctl restart kine-x.service` 生效（**注意 `10-install-service.sh install` 的 `enable --now` 不会重启已运行服务，finalize 验证可能验到旧进程**）。
- 部署坑：`finalize-deploy.sh` 第 0 步先删旧产物再**等并行编译**；若先跑完编译再跑 finalize，会把新产物删掉并空等 20 分钟。正确姿势：后台起 `36-build-m2.sh`，同时跑 `finalize-deploy.sh`。
- **`EtherCAT_SocketServer.lua` 已移植（2026-09-26）**：旧 `EtherCAT_SocketServer.bas` 的 4321/502/称重/机器人/总线功能 1:1 移植到 Lua（单线程协作步进；定位用新增的 `MOVEABS(...,wait=0)` 异步下发；计时用新增 `TICKS()` 命令；502 需 systemd `AmbientCapabilities=CAP_NET_BIND_SERVICE`）。离线自测 `lubancat2/tools/port_script_selftest.lua` 38 项（ctest `port_script_selftest`，总 15 个测试）。
- **使能项固件已部署（2026-09-26 09:58）**：板端真机验证 `TICKS()`/异步 `wait=0`/`OPEN(13,"TCP_SERVER",502)`（unit `AmbientCapabilities=cap_net_bind_service`）全部可用。
- **掉使能根因与总线自恢复（2026-09-26 16:43）**：日志证实触摸屏重启（或整柜断电）时 **EtherCAT 链路也断**（`[stat] BAD link=0`），内核按安全策略自动去使能，且总线恢复后不自动使能（原逻辑）。脚本改为：`BUSOK()`/`ENABLED()` 实时同步（HMI 状态位据实，不再残留“总线正常/已使能”）；新增 `AUTO_REENABLE` 常量（现场设为 **1**：掉线前使能则恢复后自动重使能，`ENABLE` 失败有日志）。实测：`BUSSTOP;` → 总线位=0 + 去使能；`SCAN;` → 自动回到 `en=1`。另注意：重启触摸屏请用它自身重启，别整柜断电（会连带伺服/机器人/称重复位）。
- **触摸屏通信时延优化已部署（2026-09-26 16:34）**：脚本 502 改在 `[init]` 阶段**先于总线初始化**监听（对齐 ZMC 固件行为）；主循环把 Modbus(502)/4321 应答排到阻塞性 `bus_step` 之前；机器人报文打印限频 3s（报警仍即时）。实测 502 与 4321 同期就绪、触摸屏 ~1s 连上。
- **TCP 短保活已部署（2026-09-26 16:22）**：对端断电/硬重启无 FIN 时，单客户端（502/4321）会抱着"半开"连接导致新连接进不来；`port_manager.cpp` 对 accept 与 client socket 统一启用 `SO_KEEPALIVE`（idle 10s / intvl 5s / cnt 3 ≈ 25s 判死）→ 释放后 `status()` 接受重连。重启实测：触摸屏 ~1s 自动重连、称重恢复取数。
- **脉冲当量改为脚本运行时设置（v0.8.1，2026-09-26 15:46 已部署）**：控制器不再固定/默认脉冲当量（`AxisConfig.inc_per_mm=0` 未设置；`app.conf` 的 `INC_PER_MM` 注释停用仅作兼容预置）。脚本 `UNITS(轴, v)` 经 `AxisCmd::SET_SCALE` **运行时生效**（空闲可设、运动中拒绝）；未设置时定位/点动/回零/置零**明确报错**。`axis` 事件/快照新增 `inc_per_mm`；插件轴面板加「脉冲当量」输入 + 「应用当量」（显示当前值）。`EtherCAT_SocketServer.lua` 的 `PULSE_EQUIV` 即现场配置点。ctest 15/15、smoke 274/274、插件 `kinex-debug-0.8.1.vsix`。真机验证：启动日志 `inc_per_mm=unset`、脚本 `UNITS` 生效后 1mm 往返正常（63→1.000→0.000）。
- **点动修复（2026-09-26 15:24；2026-09-27 追加两轮修复）**：点动（PV/60FF）在 SV630 默认档位无法工作（`tv=n/a`）——新增 **PP「目标跟随」点动**：目标=实际+dir·v·**前视**、每拍推进、bit4 每 20ms 脉冲；**新目标脉冲同时置 `6040.bit5`（立即更新）**（只发 bit4 时 SV630“单点模式”走完当前段才接受新目标 → “运行一段距离顿一下”）；**前视 = 脚本 `JOGLEAD(轴, s)`（固件无默认值，未设置拒绝 PP 点动；脚本设 0.5s）+ v²/(2a)**（运行时生效，可用插件「设备命令」热调）；点动时 **DPOS 对齐实际位置**，STOP 时对齐。
- **速度曲线 API（2026-09-27，待部署）**：`SRAMP(轴, 0~250ms)`（S 曲线，**仅 CSP**；PP 下明确提示）、`FASTDEC(轴, mm/s²)`（CSP 解析减速停机/PP 写 6084）、`VP_SPEED(轴)`（速度读回，mm/s）；脚本 `SPEED/ACCEL/DECEL` 同步下发内核缺省（`SET_DEFAULTS`）。内核：`interp.h` S 曲线（jerk 限幅，0=原梯形路径不变）、解析停机斜坡、速度估计。ctest 16/16；`motion_host_test` 增命令级用例。`axis_test` 断言前视 27.45mm/bit5/JOGLEAD 可调；ctest 16/16。
- **轴运动修复已部署（2026-09-26 14:30）**：三个真实缺陷——① SV630 档位1 不含 0x6061，轴内核却等模式回显 → PP/PV 500ms 超时静默失败（`wait=0` 异步下发吞错）；修复：`mode_disp` 未映射时不做模式门控。② 同档位不含 0x6062 → DPOS 恒 0；修复：`pos_demand` 未映射时 DPOS 回落为内核目标位置。③ `EtherCAT_SocketServer.lua` 数值命令漏赋值 `mvpos` → 到位判定差恒 0、把失败误报 `move_done`；已修并新增「定位完成/失败/超时」日志。真机验证：`1;` → `STA,1,0, 1.001, 1.000, -1`、回 0 → 双 0.000。`axis_test` 增两条回归；ctest 15/15。
- **boot.set 语言修复已部署（2026-09-26 14:02）**：`boot.set` 改为按**目录绑定**校验（新增 `scan_dir_bind()`），修复「目录里只有 .lua 却被判 basic、无法设主文件」；`main.cpp` 把运行中开机脚本的真实语言传给调试口，`sys.info.engine` 不再误报 basic。本地 `debug_server_test` 186/186。
- **Lua D5 固件已部署（2026-09-26 13:43）**：含行级调试钩子（断点/暂停/单步、挂起中可中止）与「目录绑定变化重装引擎」修复；开机主文件 `demo.lua` 仍在跑（占用引擎期间调试口按设计只 D1），要用 F5 调 Lua 需先在插件里**取消主文件→重启控制器**，调试完再设回。本地点验：`debug_server_test` 183/183、`lua_engine_test` 全绿、ctest 15/15。
- **中止标志修复 + 脚本上线（2026-09-26 12:09）**：`script_loop` 误把 `g_running`（运行中=true）当「请求中止」标志 → 主文件脚本一启动即 `ABORTED`（用户 11:42 触摸屏连不上的根因）；已改为独立 `g_abort` 并重新构建部署。控制器上 `demo.lua`（内容=最新 `EtherCAT_SocketServer.lua`，md5 一致）为主文件；真机验证：**502 已被触摸屏(192.168.1.3)轮询（>1400 帧 FC3 正常应答，非本站号 0）**、4321 应答正常、称重网关连上取数、机器人(192.168.1.221:4320)连上并轮询 12 条监控。脚本占用引擎期间调试口按设计只报 D1。
- **D8 主文件 + 多文件 INCLUDE 已部署（2026-09-26 02:08）**：板端 `caps` 含 `d8`；`boot.get/set/clear` 真机走通（临时文件设主→回读→清除→删除，无残留）；启动日志 `SCRIPT_ENABLE=1 但未配置开机脚本…跳过` = 尚未设主文件的正常状态。配置 `SCRIPT_ENABLE` 出厂改为 1（未设主文件且 `SCRIPT_FILE` 空则不跑）。本地点验：`debug_server_test` 167/167、ctest 14/14、插件 smoke 247/247。
- **D7 重启能力已部署（2026-09-25 21:03）**：板端 `sys.info.caps` 含 `d7`（`DEBUG_RESTART_CMD` 默认 `sudo -n systemctl restart kine-x.service`）；`sys.restart` 真机重启路径当时因有脚本在跑（`script.status=READY`）**未实测**，首次点击会中断该脚本（确认框已明示）；本地 `debug_server_test` 141/141 覆盖正/反例。
- 旧 VSIX 已归档到 `Extension/vsix-archive/`（0.1.1~0.4.9），根目录只保留当前版本。

## 8. 当前状态（截至 2026-09-25）

- 控制器：M0 实时化调优（Max 抖动 ~1.1ms→~0.53ms）、M1 到 OP、M2 单轴使能/急停/点动/PP **均已实机通过**；M7 BASIC 引擎 ✅；M8 Lua 引擎代码侧 MVP ✅（仅剩 L-19 实机对拍）。
- DebugServer D1~D6 已全部实装并有单测（`tools/debug_server_test.cpp`）；D6 恒报 caps、D5 仅 BASIC。
- **蠕动泵（2026-09-27 起，2026-09-28 功能完善）**：保定创锐 M 基本型（RS485 Modbus-RTU 从站）经透传网关
  `192.168.1.81:10123`（与称重同款链路，端口句柄 14、任务 5）；**★现场实测站号=4**（脚本 `PUMP_STATION=4`）。
  已实现：连接/重连、**485 使能（含每 3s 自动重试）**、命令码 4x203（启停/写转速/重新使能/全速）、
  回吸参数读写（4x215/217/219，读回仅变化时刷新）、**按圈数运行**（4x211/213）、**按容积运行**
  （4x221 ml/圈、4x214 输送管数量（圈数↔容积换算 ×N/÷N；掉电保持）、4x225 设定容积、4x227 预计圈数、4x229 运行；运行前自动下发设定转速）、
  体积换算（4x223）、`PSTA;` 诊断（REVS/SET/ANG/SPD/MLREV/VOL/TVOL/PREV/TUBES）；
  屏输入参数（轴 64/65、69/70；称重 130/131/132/133；泵 204/211/212/214/221/222/225/226）经 `NVSET/NVGET` **掉电保持**
  （脚本目录 `.nvram`，清单=脚本 `NVRAM_REGS`，见 planA/18 §0）。
  TODO：分配模式/校准/方向/泵头泵管型号等（见 planA/05 §8）。
- 插件：T-01~T-22、T-24~T-26 完成；**v0.12.1 已打包**（`Extension/kinex-debug-0.12.1.vsix`，smoke 356/356）；T-23 定制 VSCodium 仅配方脚本、本仓库不构建。
  - v0.4.6：控制器文件列表并入侧边栏控制台（不再有 `kine-x.files.open` 独立面板）。
  - v0.4.7：删除树形面板遗留（`panelModel.ts`/`panels.ts` 与 `view/title` 死菜单）；引擎不符时**本地前置拦截**并给出改法；`filesPanelPure` 文案收进 `strings.ts`；版本号 `0.4.7`。
  - v0.4.8：对齐控制器「语言由脚本目录推导」——识别 `sys.info.engine` 的 `auto`/`mixed`（显示「自动/混合」）、编译成功后自动重取 `sys.info`、DAP 不再把 auto 当冲突；Mock 支持 `--engine auto`，smoke 覆盖绑定全流程（225/225）。
  - v0.4.9：控制器文件改用侧边栏**原生 TreeView** `kine-x.files`（语言图标 + 大小；行内/右键拉取、删除；标题栏刷新/全部拉取），控制台 Webview 移除文件列表、回归「菜单栏 + 状态行」。
  - v0.5.0：新增「控制器 → 重启控制器」（`sys.restart`，协议 **D7**）：模态确认 → 重启板端服务 → 自动重连（≤20s）；缺 `d7` 置灰拒绝（降级不伪装）。**板端已部署 D7（21:03）**。
  - v0.5.1：「修改 IP 地址」升级为「修改IP地址（含端口）」——复用连接表单设置模式（`connectPanelPure` 的 `mode: connect|settings`），保存到 `kine-x.host`/`kine-x.debugPort`，不立刻连接。
  - v0.5.2：菜单/命令/表单标题统一为「修改IP地址」（通俗命名；端口在表单内与提示中说明）。
  - v0.6.0：主文件（开机运行）——「控制器文件」列表 ★ 标记 + 设/取消主文件（控制器 D8 `boot.get/set/clear`，`.boot` 清单）；主文件失效占位明示；多文件由主文件 `INCLUDE`/`include` 编译期展开（`10` §7.5 / `11`）。
  - v0.6.1：轴状态面板「指令位置」= DPOS 保留 4 位小数；修正该表「指令位置/反馈位置」与 MPOS/DPOS 错位的旧缺陷；曲线图例标签同步纠正。
  - v0.6.2：轴状态面板刷新优化——增量渲染（不再全量重建 DOM）+ 稳压刷新（`kine-x.axisRefreshHz`，默认 10Hz）+ 「暂停刷新」与最后更新时间。
  - v0.6.3：轴状态面板「反馈位置」（MPOS）同样保留 4 位小数。
  - v0.6.4：Modbus 寄存器面板同款刷新策略——增量渲染 + 稳压刷新（`kine-x.modbusRefreshHz` 默认 10Hz）+「暂停刷新」与最后更新时间。
  - v0.8.0：① **Lua 调试器（D5 扩展）**——Lua 引擎行级 hook + 断点/暂停/单步/挂起中中止唤醒（`enable_line_hooks` 仅调试装载时开，生产零开销），并修复「目录绑定语言与已装载引擎不一致不重装引擎」的缺陷；② **编辑体验完善**——签名/说明上移到控制器 `command_table.h` `kDocs[]`（单一事实来源），生成器产出 `data/commands.json`（names/groupList/docs）并**注入两份语法高亮**的命令名表，补全按签名生成参数占位片段、Hover + 签名帮助、BASIC/Lua 代码片段，smoke 增生成器一致性校验。
  - v0.7.0：① DAP 与主连接单客户端**交接**（会话开始断开主连接、结束自动连回；调试期间拒绝手动连接）；② 意外断线**自动重连**（指数退避，`kine-x.autoReconnect`）；③ 轴面板**在线调试**（使能/去使能/停止/按住点动+1.5s 看门狗/绝对相对定位，经 D1 `cmd`，移动 `wait=0`）；④ 曲线新增推算跟随误差/速度、图例显隐、导出 CSV、清空。
- v0.8.1：脉冲当量改为脚本运行时设置（`UNITS`/`SET_SCALE`），轴面板加「脉冲当量」输入 + 「应用当量」。
  - v0.8.2：底部状态栏品牌项显示插件版本号（`Kine-X v0.8.2`，取 `package.json`，不硬编码）；smoke 增版本号一致性校验。
  - v0.8.3：`MODBUS_IEEE` 字序定案同步（低字在前）——`kDocs` 文案更新并重生成 `data/commands.json`；插件无功能改动。
  - v0.8.4：「控制器文件」**一致性标识**——控制器 `file.list` 增 `hash`（FNV-1a 64，**板端已部署 17:43**），插件比对 `controller-sync/` 本地副本显示 一致/不一致/本地无副本/未比较；Mock 同步实现。
  - v0.8.7：「工具 → 通讯状态」（协议 **D10** `conn` 订阅）——连接快照（进程级，含对端 IP）+ `PORT_INFO` 标签 + EtherCAT 主站/每从站明细；缺 `d10` 置灰。**板端待部署**。
  - v0.9.0：「工具 → Modbus 配置」（协议 **D11** `mbmap.get/set`，脚本目录 `.mbmap`）——寄存器清单页（地址/名称/类型/读写/★保持/说明 + 搜索 + 刷新；来源=控制器，回退本地副本并标注）；数据由 `tools/gen-regmap.mjs` 从 `planA/18`+`19` 生成（改文档须重跑）。
  - v0.12.1：「工具 → **Modbus 配置**」更名「**Modbus 从站**」（与「Modbus 主站」对称）；smoke 增两页文案不混淆锁定。
  - v0.12.0（M-P2）：「工具 → **Modbus 主站**」（协议 **D13** `mbdev.get/set/status`）：设备表+点位表编辑、校验、保存、在线状态；门控 d13；smoke 增"内联脚本可解析"守卫（防模板转义缺陷）。
  - v0.11.0（P2）：Modbus 配置切**固件组态 D12**（`mbreg.get/set`）：有 d12 全表可编辑（含**默认值**列；r/w/rw）；无 d12 回退 D11 过渡或本地只读；门控 d12‖d11。
    - **v0.12.4**：「Modbus 主站」设备表单字段功能文字置顶（链路/名称/主机/IP/端口/站号/超时/重试/轮询）+ smoke 锁定（373/373）。
    - **v0.12.3（评审修复）**：D11 停报 caps、`mbmap.get/set` 明确 `NOT_SUPPORTED`（`kx_regmap.lua`/`.mbmap` 退役；推送脚本停推+前置检查 d12/enable）；D12 校验 proto-aware（w 合法）+ 宿主侧权威校验 `validateD12Text`；D13 校验补固件边界（timeout/retries/poll/非空/位类型/fc6 2字拒绝）。
  - v0.10.0：Modbus 配置**可编辑**——「用户寄存器（4x300~999，真正生效）」增删改 + 校验 + 保存（控制器 `kx_regmap.lua` 热加载；RO 异常 0x02；`persist` 走 `.nvram`（容量扩到 1024）；`REGMAP_GET` 通道）；固定区只读。
- **固件 Modbus 主站（planA/21，P4）**：**M-P1 完成（本地 ctest 19/19；`modbus_master_test` 24/24）**——设备+点位组态、轮询/写回/重连、`MBD_STATUS/MBD_LIST`、D13 `mbdev.get/set/status`；板端部署与自环验证待做（M-P2 插件页随后）。
- **固件 Modbus 从站产品化（planA/20）**：**P1~P3 全部完成并部署（2026-10-01；现有 108 条寄存器已由 `gen-regmap` 自动生成 `deploy/modbus.json` 并经 D12 热加载进固件，只读拦截实测生效）**——固件引擎（502 多客户端 + FC 全集 + 4x 全空间 + 组态 + `MB_READ/MB_WRITE/MB_LIST`/`MBREG_ZONE/PUT` + persist + D12 热加载）；插件 v0.11.0 切 D12（smoke 341/341）；**固件 502 已独占 502（913224 B，`config/modbus.enable`）**；脚本 `MB_WIRE=false`（Lua 502 退役）；**v0.12.2 按名访问层**：业务 `reg_get/reg_set` 对已组态地址内部改走 `MB_READ/MB_WRITE`（名字由 `MB_LIST()` 装载、5s 自刷新；零调用点改动），未组态地址回退 `kx_mbsync` 同步（每拍 pull 非脏字 / push 仅脏段）；真机闭环验证通过（机器人心跳链 4x142 递增、固件写 4x75 触发脚本兜底使能）。固件 502 客户端已接入 D10 通讯状态（`clients_info` → `conn` 事件 `fw:true` 行，2026-10-01）；**4x1117 现场放开为 rw**（屏直写；生成器 `accessOverride`）。后续小项：M-P3 主站现场联调。
  - v0.8.6：Modbus 面板**「已用寄存器」监视**——控制器 `mb` 事件带 `used` 位图（`Shared::mb_used`，脚本 `MODBUS_REG/MODBUS_IEEE` 读/写即标记），面板高亮 + 「仅显示已用」过滤；脚本 `mirror_used()` 100ms 全量镜像（输出区 + HMI 输入区）。**板端已部署 21:04**，真机验证 `used = 0-6,9-13,60-69,120-135,140-149`。
  - v0.8.5：「控制器 → 修改端口数量」（协议 **D9**，**板端已部署 19:57**；`caps` 含 `d9`）——运行期端口上限（静态容量 64、默认 16、可设 1..64、持久化 `.portmax`；占用时拒绝收缩回 `BUSY`）；缺 `d9` 置灰。真机验证：`used=[10,11,12,13]` 回读、`set 24 → 重启后启动日志=24`（持久化）、`set 10` 因占用回 `BUSY`。
- 待办：按 `planA/17` 跑 D4/D5 实机联调 → `d45-real-verify.mjs` 全绿 → 插件装机验证「控制器文件」列表与同步流程。

## 9. 关键文档索引

| 主题 | 文档 |
|------|------|
| 里程碑 / 验收 / 风险 | `lubancat2/docs/planA/README.md` |
| 硬件网络 / 环境搭建 / IgH+SV630 / 运动内核 | `planA/01` ~ `04` |
| 旧协议 4321/502/称重/机器人 对照基准 | `planA/05` |
| 实施步骤与验收 / 操作实录与踩坑 | `planA/06`、`07` |
| BASIC 引擎 / API / Lua 引擎 / API | `planA/08`、`10`、`09`、`11` |
| 范围与边界（原生服务已移除） | `planA/12` |
| 调试通道协议（JSON-Lines, D1~D6） | `planA/13` |
| 插件选型 / 需求任务 / 报文样例 Mock / D4D5 验收 | `planA/14`、`15`、`16`、`17` |
| **Modbus 产品化设计（从站固件组态 + 按名访问；P1~P3 已部署）** | `planA/20-Modbus配置与固件封装设计.md` |
| **Modbus 主站封装设计（设备+点位组态；P4，设计评审中）** | `planA/21-Modbus主站封装设计.md` |
| **通用工业 I/O 与总线路线（PLC 级产品演进，方案评审稿）** | `planA/22-通用工业IO与总线路线.md` |
| **对屏 4x 寄存器总表（单一事实来源）** | `planA/18-触摸屏寄存器总表.md` |
| 机器人 Modbus 直连（502 多客户端；机器人主站，1000 区） | `planA/19-机器人Modbus从站(503)接口.md`（文件名沿用历史，内容为 502 方案） |
| 旧系统对外协议速查（命令表/寄存器表/踩坑） | 根 `README.md` |
| 机器人（珞石 xCore）手册整理：10.3.3 交互指令即本站机器人链路协议依据 | 根 `xCore控制系统使用手册V2.2_学习笔记.md`（原始提取 `pdf_out/xcore_*`） |
