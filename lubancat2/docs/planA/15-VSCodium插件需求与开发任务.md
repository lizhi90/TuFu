# 15 VSCodium 插件需求与开发任务（调试上位机 · `.vsix`）

> 定位：`14-调试软件客户端方案.md` 已拍板形态——「基于 VSCodium/VSCode 做**插件（Extension）**起步，
> 交付期再收敛为**定制发行版**」，并给出 DAP ↔ `IScriptEngine` 映射与 P1~P5 路线。
> 本文把该形态**落成可开工的需求规格（FR/NFR）+ 开发任务分解（T 编号）**。
>
> 三条前提/边界（先读）：
> 1. 控制器侧调试口 = `13-调试通道与上位机通讯.md` 的 C++ `DebugServer`（JSON-Lines over TCP），
>    该服务端**尚未实施**；本插件对其存在**前置依赖**（见 §6、§8）。
> 2. 业务协议（4321 / 502 / 称重 / 机器人）归**脚本层**，**不是**本插件职责（见 `12`）；插件只连**调试通道**。
> 3. 「控制器同一时刻只加载一种脚本语言」是硬规则（`src/script/engine_rule.h`），插件**必须在 UI 层强制**（见 §6.4）。
>
> 与 `13`/`14` 一致：本文**只做需求与任务，尚未实施**（无插件工程、无 Adapter 代码）。

---

## 0. 一页速览（TL;DR）

| 项 | 结论 |
|----|------|
| 交付物 | 一个 vsix：`kine-x.kinex-debug`（暂名），含 **扩展前端（TS）+ Debug Adapter（DAP）+ 语法资源** |
| 形态路线 | **A 插件起步**（`14` §3）→ P4 定制 VSCodium 发行版（内置插件） |
| 连接方式 | 插件（PC）──TCP JSON-Lines──▶ 控制器 `DebugServer`（`13`）；**默认直连控制器业务网 `192.168.1.11:5000`**（同网段）；跨网段才走 SSH 端口转发 |
| 关键依赖 | `13` 的 **D1~D3** 打通最小闭环；**D4 热更新 / D5 引擎 pause 钩子** 决定「真·断点/单步」 |
| 首版目标（P1） | 连上 → 打开/编辑 `.bas`/`.lua` → 下载并运行 → 看输出 |
| 本版不含 | 实时曲线（P3 可选）、LSP（先 TextMate 语法）、业务协议、VS Code Server 上板 |

---

## 1. 范围与非目标

### 1.1 范围内（In scope）

| # | 能力 | 说明 |
|---|------|------|
| 1 | 连接管理 | 配置/测试/断开会话到 `13` 调试口；状态栏可视化 |
| 2 | 脚本编辑 | `.bas`/`.lua` 语法高亮、注释/续行/折叠等行式语言辅助 |
| 3 | 下载与运行 | 上传当前文件、运行/停止/重启、查看状态与错误行 |
| 4 | 在线调试（DAP） | 断点、单步、变量、监视、调用栈、REPL（受 `13` D4/D5 约束，见 §6.3） |
| 5 | 设备终端 | 经 `13` 的 `cmd`（`MotionHost::call`）执行单条设备命令并看回显 |
| 6 | 状态面板 | 轴状态、总线、Modbus 4x 寄存器（`13` D3 订阅） |
| 7 | 日志 | `log` 订阅接入输出面板（`13` §5.3） |
| 8 | 参数视图 | 只读展示 `config/app.conf` 关键项，可选编辑下发 |
| 9 | 打包分发 | 产出 `.vsix`；可选发 Open VSX；可选内置进定制发行版 |
| 10 | （可选）实时曲线 | Webview + 事件订阅；高频采样按 `13` §9 单开通道 |

### 1.2 明确不做（Non-goals）

- **不**在控制器（RK3568）装 VS Code Server / Node 常驻（`14` §2.1）——控制器只暴露调试口。
- **不**自研编辑器/文本引擎/窗口框架（UI 白送，见 `14` §4）。
- **不**实现业务协议（4321/502/称重/机器人）——那是脚本层（`12`）；插件里**不得**出现业务端口逻辑。
- **不**做语言转换/转译（`.bas` ↔ `.lua`），仅按 `engine_rule.h` **提示与拦截**。
- **不**承诺 DAP 覆盖实时曲线（`14` §6.3）；波形单独立项。
- 首版**不**做 LSP，仅 TextMate 语法 + 复用引擎编译报错做诊断（`14` §6.4）。

---

## 2. 角色与典型场景（User Stories）

| 角色 | 场景 | 期望 |
|------|------|------|
| 控制器工程师 | S1 现场联调 | PC 上连板端调试口，改一行脚本即时下载运行看效果 |
| 控制器工程师 | S2 排错 | 脚本跑飞/报错时看错误行、变量值、调用位置，不必 SSH 敲命令 |
| 控制器工程师 | S3 观察状态 | 跑脚本同时看轴位置/使能/报警/寄存器面板与日志 |
| 现场调试员 | S4 交付使用 | 用定制发行版一键连、精简菜单，无需 VS Code 经验 |
| 维护者 | S5 协议演进 | 调试口 RPC 增方法时插件按 schema 增量支持，旧版不崩 |

**端到端主线（S1+S2）**：

```
打开工程(.bas/.lua) → 设置 host/port → 连接 → 下载(script.compile) → 运行(script.run)
   → 输出面板看 print / log → 出错跳到 error_line、变量面板看值 → 停止(script.stop)
```

---

## 3. 总体架构与组件划分

```
┌──────────────── PC（VSCodium / VS Code） ────────────────┐
│  Extension (TypeScript / Node)                            │
│   ├─ 命令 / 菜单 / 状态栏 / 快捷键                          │
│   ├─ 视图：状态面板、寄存器表、Webview(曲线/设置)           │
│   ├─ 输出通道：脚本输出 / 日志 / Problems(诊断)             │
│   └─ DebugSession（DAP 前端）+ 内嵌 Debug Adapter          │
│          │  DAP（stdio）                                   │
│          ▼                                                 │
│  Debug Adapter（DAP ⇄ `13` JSON-Lines 协议翻译）            │
│          │  TCP JSON-Lines                                 │
└──────────┼─────────────────────────────────────────────────┘
           ▼
   控制器 kine-x 进程（`13`）
     DebugServer 线程（普通优先级）→ IScriptEngine / Shared 只读快照 / MotionHost::call
     （RT 1ms 线程绝不参与调试 IO）
```

| 组件 | 语言 | 职责 | 备注 |
|------|------|------|------|
| Extension 前端 | TypeScript | 命令、视图、Webview、设置、状态栏、生命周期 | 必须 JS/TS（VS Code 约束） |
| Debug Adapter | **建议 TS** | DAP ⇄ JSON-Lines 翻译、断点解析、事件转发 | 语言自由（`14` §6.5）；TS 免第二运行时 |
| 语法资源 | JSON | `.tmLanguage.json` + `language-configuration.json` | 很轻（`14` §4） |
| （可选）LSP | — | 补全/跳转/诊断 | 首版不做（`14` §6.4） |
| 连接层 | TS | 单连接 TCP、心跳/超时、SSH 转发指引、重连 | PC 侧，与 `PortManager` 无关 |

**通信分层**：扩展↔Adapter 走 **DAP over stdio**；Adapter↔控制器走 `13` 的 **JSON-Lines RPC + 事件订阅**。
**协议冻结面** = `13` §4.4 的 schema；RPC 是「请求→应答」，其**事件推送**是面板/日志的关键差异点（`13` §1）。

---

## 4. 功能需求（FR）

> 优先级：**P0** 首版必须；**P1** 完整可用版；**P2** 增强/可选。
> `13 依赖` 指出该 FR 依赖 `13` 的哪个阶段（D1~D9，见 `13` §7）。

### 4.1 连接与工程（FR-1）

| ID | 需求 | 优先级 | `13` 依赖 | 验收要点 |
|----|------|--------|-----------|----------|
| FR-1.1 | 配置 `host`/`port`（默认 `192.168.1.11:5000`，控制器业务网 IP） | P0 | D1 | 设置页 + `launch.json` 可配 |
| FR-1.2 | 连接/断开/重连；状态栏显示连接态 | P0 | D1 | 断线可见、一键重连 |
| FR-1.3 | 连接首帧可选 token 鉴权（`13` §4.5） | P1 | D1 | token 错→明确报错，不静默 |
| FR-1.4 | `sys.info` 校验：版本/引擎(`basic`/`lua`)/轴数 | P0 | D1 | 连接后即展示 |
| FR-1.5 | SSH 端口转发辅助（生成/复制 `ssh -L` 命令） | P1 | — | 文档 + 一键复制 |
| FR-1.6 | 工作区识别 `.bas`/`.lua` 与 `.vscode/launch.json` | P0 | — | 打开即进入正确语言模式 |
| FR-1.7 | 多目标配置（多套 host/port profile） | P2 | D1 | 可保存多设备 |

### 4.2 脚本编辑与静态检查（FR-2）

| ID | 需求 | 优先级 | `13` 依赖 | 验收要点 |
|----|------|--------|-----------|----------|
| FR-2.1 | `.bas` 语法高亮（关键字/命令名/字符串/注释/标签） | P0 | — | 命令名来自 `command_table.h` |
| FR-2.2 | `.lua` 语法高亮（Lua 5.4 子集 + 沙箱差异提示） | P0 | — | 见 `11` 的 `PORT_*`/`ww` 差异 |
| FR-2.3 | 行式语言辅助：`REM`/`'` 注释、`\` 续行、`:` 多语句可见性 | P1 | — | 与 `10` §2.1 一致 |
| FR-2.4 | **编译即诊断**：`script.compile` 的 `err.line/msg` → Problems | P0 | D2 | 语法错落到编辑器对应行 |
| FR-2.5 | 命令名补全（数据源：`command_table.h`） | P1 | — | 与两引擎名称一致（同名同义） |
| FR-2.6 | 语言/引擎一致性校验（见 §6.4） | P0 | — | 引擎 `lua` 配 `.bas` 明确拦截 |
| FR-2.7 | （可选）LSP：跳转/重命名/语义诊断 | P2 | D2 | 非首版；以编译报错替代 |

### 4.3 下载与运行控制（FR-3）

| ID | 需求 | 优先级 | `13` 依赖 | 验收要点 |
|----|------|--------|-----------|----------|
| FR-3.1 | 下载（`script.compile` 编译到新实例） | P0 | D2 | 失败不污染运行中实例（`13` §5.1） |
| FR-3.2 | 运行（`script.run`）；运行中显示 steps/status | P0 | D2 | 状态随 `script` 事件刷新 |
| FR-3.3 | 停止（`script.stop` → `request_abort`） | P0 | D2 | 长循环能被及时叫停 |
| FR-3.4 | 热更新：运行中原子替换脚本，不中断总线 | P1 | **D4** | 旧实例 abort + 原子换句柄（`13` §5.1） |
| FR-3.5 | 运行前「等总线就绪」开关（对应 `SCRIPT_WAIT_BUS`） | P1 | D2 | 可配超时 |
| FR-3.6 | 显示 `Status`：`READY/DONE/COMPILE_ERROR/RUNTIME_ERROR/ABORTED/BUDGET_EXCEEDED` | P0 | D2 | 映射见 §6.3 |
| FR-3.7 | 步数预算（`SCRIPT_MAX_STEPS`）；`BUDGET_EXCEEDED` 提示 | P1 | D2 | 防死循环；展示 `steps` |

### 4.4 在线调试 / DAP（FR-4）

> 与 `14` §2.3 的 DAP↔接口映射一一对应。
> **断点命中挂起与 `next/stepIn/stepOut/continue` 依赖 `13` D5（引擎 pause 钩子）**；
> D5 落地前这些项只能提供「跟踪打印式」降级体验（`13` §5.2）。

| ID | 需求 | 优先级 | `13` 依赖 | DAP 对应 |
|----|------|--------|-----------|----------|
| FR-4.1 | 启动调试会话 | P0 | D1~D2 | `initialize`/`configurationDone` |
| FR-4.2 | 断点增/删/列（源：`labels()`；BASIC 标签 / Lua 函数·行号表） | P1 | **D5** | `setBreakpoints` |
| FR-4.3 | 单步 `next`/`stepIn`/`stepOut`/`continue` | P1 | **D5** | 同名请求 |
| FR-4.4 | 中断/恢复 | P1 | **D5** | `pause` |
| FR-4.5 | 变量视图（`list_vars()` 解析 `NAME = 3`；`get_var` 取类型/值） | P0 | D1~D2 | `scopes`/`variables` |
| FR-4.6 | 变量写入（`set_var()`） | P1 | D2 | `setVariable` |
| FR-4.7 | 监视/REPL 求值（表达式 + `MotionHost::call()` 执行命令） | P1 | D2 | `evaluate` |
| FR-4.8 | 调用栈（BASIC `GOSUB/SUB` 链；Lua 真实调用栈） | P2 | D5 | `stackTrace` |
| FR-4.9 | 输出事件转发（`print()` 落点 / `log` 订阅） | P0 | D3 | `output` |
| FR-4.10 | 终止原因回传（Status→DAP，见 §6.3） | P0 | D2 | `stopped`/`exited` |

**降级策略（D5 未就绪）**：FR-4.2/4.3/4.4/4.8 显示「当前调试口暂不支持」，并在日志给出 `13` D5 说明；
UI **不得**伪装成已支持（避免误导现场）。

**变量视图在 D5 缺失下如何可用**：VS Code 只在「已停止」状态请求变量视图，而 D5 未就绪时没有真正的暂停点。
因此插件提供 `stopOnEntry`（默认开）——脚本**尚未运行**时停一次（DAP 标准的 entry stop，**真实状态**，非伪造），
用户先查看/预置变量（FR-4.5/4.6）再按「继续」启动；脚本启动后进入运行态，变量视图暂不可用（需 D5）。
运行期间读写变量仍可用 **REPL/监视**（FR-4.7）：`NAME` 读、`NAME = 值` 写；REPL 中非赋值表达式回退为设备命令
`cmd`，但 **hover/watch 上下文绝不执行命令**（避免悬停产生副作用）。

### 4.5 设备终端与命令执行（FR-5）

| ID | 需求 | 优先级 | `13` 依赖 | 验收要点 |
|----|------|--------|-----------|----------|
| FR-5.1 | 命令终端：执行单条设备命令（`cmd` → `MotionHost::call`） | P1 | D1 | 与脚本**同一条路**（`13` §4.4） |
| FR-5.2 | 回显与错误（返回码 0/1/2/3 语义，见 `script_host.h`） | P1 | D1 | 运行错误明确展示 |
| FR-5.3 | 命令历史/快捷片段（`EN`/`STOP`/`STA` 等） | P2 | — | 名称取自 `command_table.h` |
| FR-5.4 | 轴参数读写（`SPEED/ACCEL/…`）经终端或参数视图 | P2 | D1 | 与 `MotionHost` 语义一致 |

> ⚠️ 终端**统一走 `cmd`**，**不**复用 4321 业务口（`13` §9 已倾向统一入口）。

### 4.6 状态面板与寄存器（FR-6）

| ID | 需求 | 优先级 | `13` 依赖 | 数据来源 |
|----|------|--------|-----------|----------|
| FR-6.1 | 轴状态：`bus_ok/enabled/idle/alarm`、`mpos/dpos`、`axis_status` 位、`err_code` | P1 | **D3** | `Shared::AxisStatus`（`state.h`） |
| FR-6.2 | 总线：`node_count` 及每节点 `axis_count/status/io/aio` | P2 | D3 | `Shared::BusInfo` |
| FR-6.3 | Modbus 4x 寄存器表（`mb_regs[256]`，可读；写可选） | P2 | D3 | `Shared::mb_regs` |
| FR-6.4 | 订阅管理：`axis/bus/log` + `hz` 限频 | P1 | **D3** | `13` §4.4 ② |
| FR-6.5 | 面板用**事件推送而非轮询**（默认 ≤20Hz） | P1 | D3 | `13` §4.4 ③ |
| FR-6.6 | （可选）实时曲线/示波器 Webview | P2 | D3（高频另开） | `14` §6.3；高频按 `13` §9 |

**`AXISSTATUS` 位映射**（面板须与 `state.h` `AxisStatusBit` 一致）：
`bit1` 随动误差告警、`bit2` 通讯错、`bit3` 驱动器报错、`bit8` 随动误差出错、`bit22` 告警输入；
其余位本控制器无数据源（恒 0），面板应**标注空数据源**而非显示为正常。

### 4.7 日志与诊断（FR-7）

| ID | 需求 | 优先级 | `13` 依赖 | 验收要点 |
|----|------|--------|-----------|----------|
| FR-7.1 | 日志订阅接入输出面板（限频、环形缓冲） | P1 | **D3** | `13` §5.3 的 log hook |
| FR-7.2 | 输出分栏：脚本 `print` / 系统日志 / 调试器消息 | P1 | D2~D3 | 不混淆 |
| FR-7.3 | 协议原始报文查看（开关，便于排查 JSON-Lines） | P2 | D1 | 默认关 |
| FR-7.4 | 连接/协议错误人话提示（超时/拒绝/鉴权/版本不符） | P1 | D1 | 明确错误码与建议 |

### 4.8 打包、分发与（可选）发行版（FR-8）

| ID | 需求 | 优先级 | 说明 |
|----|------|--------|------|
| FR-8.1 | 产出 `.vsix`（`vsce package`） | P0 | 装官方 VS Code 与 VSCodium 皆可（`14` §3） |
| FR-8.2 | 支持在 VSCodium（**Open VSX**）安装/发布 | P1 | 不依赖微软 Marketplace（`14` §6.1） |
| FR-8.3 | 许可证与再分发合规 | P1 | 自研插件本身无约束；发行版才涉及（`14` §6.2） |
| FR-8.4 | （可选）定制 VSCodium 发行版：内置插件+品牌+预置连接+锁设置 | P2 | 对应 `14` §3 形态 B |
| FR-8.5 | 快捷安装脚本 / 离线 `.vsix` 交付（现场无外网） | P2 | 与 `deploy/` 风格一致 |

---

## 5. 非功能需求（NFR）

| ID | 类别 | 要求 |
|----|------|------|
| NFR-1 | 兼容性 | 目标 **VSCodium（Open VSX）+ VS Code**；声明 `engines.vscode` 下限，避免锁死旧版（`14` §6.7） |
| NFR-2 | 运行时 | 前端用 VS Code 内置 Node；**不在控制器侧部署 Node**（`14` §2.1） |
| NFR-3 | 性能 | 事件订阅默认 ≤20Hz（`13` §4.4）；高频不卡 UI；日志环形缓冲有上限 |
| NFR-4 | 安全 | 调试口绑**业务网**（`DEBUG_BIND`），供局域网 PC 直连；token 可选；跨网段/加固时走 **SSH 转发**（`13` §4.5） |
| NFR-5 | 健壮性 | 断线自动重连；协议未知字段**忽略不崩**（向前兼容）；`id` 关联超时兜底 |
| NFR-6 | 可用性 | 关键失败有人话提示；D5 未就绪的功能**明确标注不支持**（§4.4 降级策略） |
| NFR-7 | 可维护性 | RPC 方法/命令名数据表**单一来源**（对齐 `command_table.h`），避免硬编码漂移 |
| NFR-8 | 国际化 | 首版中文；UI 文案集中，预留 i18n（现场工程师中文环境） |
| NFR-9 | 许可证 | 插件自身选宽松许可（如 MIT/Apache-2.0）；发行版基座遵守 `14` §6.2 |
| NFR-10 | 可测试性 | 提供 **mock DebugServer**（回放 JSON-Lines），使插件可在无控制器时自测（§10.1） |

---

## 6. 与控制器调试口（`13`）的接口契约

> 本节是插件与 `13` 的**接口冻结面**；`13` 服务端实现须与本节一致，插件按此对接。
> （`13` 尚未实施，以下方法名/字段以 `13` §4.4 为准，实施时可微调，但须同步本文。）

### 6.1 插件调用的 RPC 方法

| 分类 | 方法 | 参数 | 返回 |
|------|------|------|------|
| 系统 | `sys.info` | — | `ver` / `engine` / `axis_count` |
| 脚本 | `script.compile` | `src`,`engine` | `labels[]`；失败 `err.line/msg` |
| 脚本 | `script.run` / `script.stop` | — | `status` |
| 脚本 | `script.status` | — | `status`/`steps`/`error_line` |
| 调试 | `script.pause` / `script.resume` / `script.step` | — | 状态（**需 `13` D5**） |
| 断点 | `breakpoint.add` / `del` / `list` | 行号/标签 | 断点集（**需 `13` D5**） |
| 变量 | `var.list` / `var.get` / `var.set` | `name`,`v` | 值/清单 |
| 状态 | `axis.snapshot` | `axis` | `bus_ok/mpos/dpos/idle/enabled/alarm` |
| 终端 | `cmd` | `line` | 返回值 + 输出（经 `MotionHost::call`） |
| 订阅 | `subscribe` / `unsubscribe` | `topics[]`,`hz` | 生效主题 |

### 6.2 事件（服务端主动推送）

| 事件 `e` | 载荷 | 用途 |
|----------|------|------|
| `axis` | `t`,`mpos`,`dpos`,`idle` … | 状态面板/曲线 |
| `script` | `t`,`status`,`steps` | 运行状态、终止原因 |
| `log` | `t`,`s`,`lvl` | 输出面板 |

### 6.3 调试语义映射（`Status` → DAP）

| 引擎 `IScriptEngine::Status` | DAP 终止/状态 |
|------------------------------|----------------|
| `READY` | 运行中（发 `continued`） |
| `DONE` | 正常结束（`exited`, code 0） |
| `COMPILE_ERROR` / `RUNTIME_ERROR` | `stopped`(exception) + `error_line` 定位 |
| `ABORTED` | 用户停止（`terminated`） |
| `BUDGET_EXCEEDED` | `stopped`(error)，附 `steps` 供诊断 |

> 状态名字符串取 `IScriptEngine::status_name()`（`engine.h`），插件**不得**自造状态名。

### 6.4 引擎互斥规则的 UI 强制（硬约束）

依 `engine_rule.h`（另见 `09` §3.1/§3.7、`13` §6）：

- 一次会话**只允许一种引擎**（`basic` XOR `lua`），由 `sys.info.engine` 揭示；
- **语言来源（v0.4.8 对齐 `13` §4.4）**：控制器调试口的语言由其 `DEBUG_SCRIPT_DIR` 内**现有脚本**推导——
  `basic`/`lua` = 已绑定；`auto` = 空目录（**两种语言都可编译**，由控制器在首次落盘时绑定）；
  `mixed` = 目录两种并存（控制器拒绝编译，提示先删除一种）。插件对 `auto`/`mixed` **不本地拦截**
  （交控制器裁决并展示明确错误），仅在 `basic`/`lua` 与本地文件扩展名冲突时**本地前置拦截**；
- **扩展名一致**：已绑定 `lua` 只跑 `.lua`、`basic` 只跑 `.bas`；不一致**明确拦截并报错**，不静默回退
  （对应 `script_file_matches_language()`）；
- 一个工作区内混合 `.bas`/`.lua` 时应**提示拆分为独立工程**，不提供「同会话双引擎」入口；
- 切换语言 = 切换会话/目标，而不是在运行中热切（控制器侧：`auto` 下编译即绑定；绑定后改名要删文件）。

---

## 7. 配置与文件约定

### 7.1 扩展设置（`settings.json`，`kine-x.*`）

| 键 | 默认 | 说明 |
|----|------|------|
| `kine-x.host` | `192.168.1.11` | 调试口地址（控制器**业务网 IP**；插件在 PC 上远程连它） |
| `kine-x.debugPort` | `5000` | 调试口端口（对应 `13` `DEBUG_PORT`） |
| `kine-x.token` | 空 | 可选鉴权 token（`13` §4.5） |
| `kine-x.engine` | `basic` | `basic`/`lua`（决定语言模式与校验） |
| `kine-x.sshForward` | 关 | 生成 `ssh -L` 提示/一键复制 |
| `kine-x.subscribeHz` | `20` | 面板订阅频率（NFR-3） |
| `kine-x.showRawProtocol` | 关 | 原始报文输出（FR-7.3） |

### 7.2 调试配置（`.vscode/launch.json`）

```jsonc
{
  "type": "kine-x",                 // 由本插件注册（DebugAdapterDescriptorFactory）
  "request": "launch",
  "name": "Kine-X 控制器调试",
  "host": "192.168.1.11",            // 控制器业务网 IP（插件在 PC 上远程连它）
  "port": 5000,
  "engine": "basic",                 // basic | lua（须与文件扩展名一致，见 §6.4）
  "program": "${workspaceFolder}/demo.bas",  // 留空则用当前打开的脚本文件
  "stopOnEntry": true,               // 仅插件侧：启动前先停一次，露出变量视图（见 §4.4 降级说明）
  "waitBus": true,                   // 对应 SCRIPT_WAIT_BUS
  "maxSteps": 5000000,               // 对应 SCRIPT_MAX_STEPS
  "trace": false                     // 对应 SCRIPT_TRACE（跟踪打印）
}
```

> `stopOnEntry` 与 `token` 是**插件侧**参数，无 `app.conf` 对应键；其余键名对齐见 D-04。

### 7.3 工程与文件约定

- 脚本文件：`.bas`（BASIC 子集）、`.lua`（Lua 5.4 子集），见 `10`/`11`。
- 运行方式对应关系（`10` §1）：插件「下载并运行」= 控制器侧等价于 `--script` / `SCRIPT_FILE`
  的**在线版**；`SCRIPT_ENGINE` 与 `kine-x.engine` 语义一致。
- 配置基准：`config/app.conf` 键名（`AXIS_COUNT`、`SCRIPT_*`、`ECAT_*`）——参数视图**只读展示**为主，
  写入须二次确认（避免误改现场标定）。

---

## 8. 开发任务分解（T 编号）

> 说明：任务面向**插件侧**；**控制器侧（`13` 服务端）任务不在本文范围**，但作为依赖标注。
> 每项给出产出、依赖、验收。**本文不实施**，仅作开工依据。

### 8.0 阶段 0：脚手架（无控制器依赖）

| ID | 任务 | 产出 | 依赖 | 验收 |
|----|------|------|------|------|
| T-01 | 建 Extension 工程（TS + 打包链路 `vsce`/`ovsx`） | 可 `F5` 调试的空插件 + `.vsix` | — | `vsce package` 出包、可在 VSCodium 装 |
| T-02 | 注册语言 `kx-basic` / `kx-lua` + `language-configuration.json` | 语言模式可用 | — | 注释/括号/折叠生效 |
| T-03 | `.bas`/`.lua` TextMate 语法（FR-2.1/2.2） | `syntaxes/*.tmLanguage.json` | — | 关键字/命令/字符串/注释着色正确 |
| T-04 | 命令名/关键字数据源生成（对齐 `command_table.h`，NFR-7） | 生成脚本 + 数据文件 | — | 命令清单与 `command_table.h` **逐名一致** |
| T-05 | 扩展设置项（§7.1）与配置读取 | `contributes.configuration` | — | 设置页可见、可持久化 |

### 8.1 阶段 1：连接与运行（对应 `14` P1，依赖 `13` D1~D2）

| ID | 任务 | 产出 | 依赖 | 验收 |
|----|------|------|------|------|
| T-06 | JSON-Lines 连接层（TCP、`id` 关联、超时、重连） | `Connection` 模块 | `13` D1 | 连/断/重连稳定；超时有人话提示 |
| T-07 | 连接命令与状态栏（FR-1.1/1.2/1.4） | 命令 + 状态栏项 | T-06 | 连上即显示 `sys.info` |
| T-08 | 输出通道（脚本输出/日志/原始报文分栏，FR-7.2/7.3） | OutputChannel | T-06 | 分栏正确、可开关原始报文 |
| T-09 | 下载与运行/停止（FR-3.1~3.3/3.6/3.7） | 命令 + 状态控制 | T-07,`13` D2 | 编译错落行、运行态实时、可停止 |
| T-10 | 编译即诊断（FR-2.4：`err.line/msg`→Problems） | Diagnostics 集成 | T-09 | 出错定位到具体行 |
| T-11 | 引擎一致性校验（FR-2.6、§6.4） | 前置校验 | T-05,T-07 | 不匹配**拦截并报错**，不静默 |
| T-12 | 设备终端（FR-5.1/5.2/5.3，走 `cmd`） | Terminal/Webview 面板 | T-06 | 命令执行与返回码语义正确 |

### 8.2 阶段 2：在线调试 DAP（对应 `14` P2，依赖 `13` D4~D5）

| ID | 任务 | 产出 | 依赖 | 验收 |
|----|------|------|------|------|
| T-13 | Debug Adapter 骨架（**DAP in-process**，见 D-02；`initialize`/`launch`/`setBreakpoints`/`configurationDone`/`disconnect`） | Adapter 模块 | T-09 | 会话可启停、握手正确 |
| T-14 | 变量/监视（FR-4.5/4.6/4.7；`list_vars` 解析） | `scopes`/`variables`/`setVariable`/`evaluate` | T-13 | 变量随运行刷新、可写、REPL 可用 |
| T-15 | 输出与终止事件（FR-4.9/4.10、§6.3） | `output`/`stopped`/`exited` 映射 | T-13 | 状态名取 `status_name()`，不造字 |
| T-16 | **热更新（FR-3.4）** | 原子替换流程 | `13` **D4** | 运行中换脚本不中断总线 |
| T-17 | **断点/单步/中断（FR-4.2/4.3/4.4/4.8）** | `setBreakpoints`/`next`/`pause`… | `13` **D5** | 命中挂起、单步、恢复；降级有提示 |

### 8.3 阶段 3：状态面板与曲线（对应 `14` P3，依赖 `13` D3）

| ID | 任务 | 产出 | 依赖 | 验收 |
|----|------|------|------|------|
| T-18 | 订阅管理（FR-6.4/6.5；`subscribe`/`unsubscribe` + 限频） | 订阅模块 | `13` D3 | 事件驱动更新，默认 ≤20Hz |
| T-19 | 轴状态面板（FR-6.1；位表见 §4.6） | 树/表视图 | T-18 | 位映射正确、空数据源有标注 |
| T-20 | 总线 + 寄存器面板（FR-6.2/6.3） | 表格视图 | T-18 | 数值与 `Shared` 快照一致 |
| T-21 | （可选）实时曲线 Webview（FR-6.6） | Webview | T-18（高频按 `13` §9） | 曲线流畅、不影响 RPC |
| T-26 | 面板连接入口（UX 补强，FR-6） | 视图标题栏 connect/disconnect 按钮 + 行内可点 | T-18 | 未连接时标题栏有「连接控制器」；行内「未连接控制器」点击即连 |

### 8.4 阶段 4：分发与发行版（对应 `14` P4~P5）

| ID | 任务 | 产出 | 依赖 | 验收 |
|----|------|------|------|------|
| T-22 | Open VSX 发布 / 离线 `.vsix`（FR-8.1/8.2/8.5） | 发布流程 + 安装脚本 | T-01 | VSCodium 内可装可更新 |
| T-23 | （可选）定制 VSCodium 发行版（FR-8.4） | 预置插件+品牌+一键连接 | T-19~T-21 | 现场开箱即用、精简菜单 |
| T-24 | 文档：安装/连接/调试/FAQ | 用户手册（可并入本文或单列） | 全部 | 新手按文档可完成 S1 |
| T-25 | 命令名补全（FR-2.5，P1） | `CompletionItemProvider`（数据源 `data/commands.json` ← `command_table.h`） | T-05 | `.bas`/`.lua` 内输入命令前缀出补全、两引擎同名同义 |

**任务依赖链（关键路径）**：
`T-01 → T-06 → T-09 → T-13 → T-14/T-15`（最小可用）→ `T-16/T-17`（真调试：插件侧已实装，`13` D4/D5 **控制器侧已实装**，待实机联调）→ `T-18→T-19/20/21`（面板 + 曲线）。

### 8.5 实施进度（插件侧 `Extension/`）

> 代码落位：`Extension/src/`（装配与 Adapter）、`Extension/tools/mock-debug-server.mjs`（Mock）、`Extension/test/connection.smoke.cjs`（冒烟）、`Extension/tools/vscodium/`（T-23 定制发行版配方）。
> **文案集中（NFR-8）**：UI 字符串统一收敛到 `src/strings.ts`（`S`，按模块分命名空间 + `setLocale()` 预留 i18n）；
> 除 `strings.ts` 外源码**不含中文字面 UI 文案**；协议逐字常量（AL 状态名、状态位名等）仍留在各自数据模块（`statusBits.ts` 等）。
> **口径补充（v0.4.7）**：Webview 骨架 HTML 模板内直接书写的界面文字视为**模板例外**（与该视图同文件维护、
> 不在逻辑分支里拼文案）；spec/逻辑层文案必须走 `S`。
> **依赖 vscode 的边界**：`engineRule.ts`（`engine_rule.h` 纯镜像）、`dapPure.ts`（DAP 纯逻辑）、
> `statusBits.ts`（位表）、`store.ts`（数据仓库）、`subscribe.ts`（订阅管理）、`panelModel.ts`（面板树模型）、
> `curvePure.ts`（曲线环形缓冲 + Webview 文档）
> **不依赖 vscode**，故可被 Node 直接 require 单测；`engineResolve.ts`（读设置做裁决）、`debugAdapter.ts`、
> `panels.ts`（`TreeDataProvider`）、`curve.ts`（Webview 面板）、`extension.ts` 属于 vscode 侧。
> 每项完成的回归口径：`npx tsc -p ./` 0 error → `npm run test:smoke` 全绿 → `npx @vscode/vsce package --no-dependencies` 出 VSIX。

| ID | 状态 | 落点 / 说明 |
|----|------|-------------|
| T-01~T-05 | ✅ | 脚手架、语言、语法、命令数据（`data/commands.json` / `tools/gen-commands.mjs`）、设置项 |
| T-06 | ✅ | `src/connection.ts`：JSON-Lines、`id` 关联、超时、`KxRpcError`（含 `code`/`line`） |
| T-07 | ✅ | `kine-x.connect` / `disconnect` / `sysInfo` + 状态栏 `render()` |
| T-08 | ✅ | 输出通道「Kine-X 调试」+「Kine-X 原始报文」分栏，`kine-x.showRawProtocol` 开关 |
| T-09 | ✅ | `download` / `downloadAndRun` / `run` / `stop`（`script.compile`/`run`/`stop`） |
| T-10 | ✅ | `handleCompileFailure()`：`COMPILE_ERROR`+`err.line` → Problems 并滚动定位；编辑即清过期诊断 |
| T-11 | ✅ | `resolveEngine()` 按 D-03 裁决（扩展名推断为主 + 显式设置冲突拦截）；`warnEngineMismatch` |
| T-12 | ✅ | `kine-x.cmd`（走 `cmd`，`ret`+`out[]`），命令历史 |
| T-13 | ✅ | `src/debugAdapter.ts` + `package.json` `debuggers`/`breakpoints` 贡献点 + `extension.ts` 注册 `DebugAdapterDescriptorFactory`；**`initialize` 据实声明能力**（`supportsSetVariable` 为 true，D5 相关为 false） |
| T-14 | ✅ | `scopes`/`variables`（`var.list` 解析 + `var.get` enrich）/`setVariable`（`var.set`）/`evaluate`（REPL 兜底 `cmd`）；纯逻辑抽到 `src/dapPure.ts` 并单测 |
| T-15 | ✅ | `script` 状态映射按 §6.3：`READY`→`continued`、`DONE`→`exited(0)`+`terminated`、`ABORTED`→`terminated`、`RUNTIME_ERROR`/`BUDGET_EXCEEDED`/`COMPILE_ERROR`→`stopped(exception)`+`error_line`；`log`→`output`（`lvl=error/warn`→stderr） |
| T-16 | ✅ | 热更新（`13` D4）：`download()` 检测脚本**运行中**（`READY`/`PAUSED`）→ 发 `script.compile {src,engine,swap:true}`（编译到新实例再原子替换，不中断总线）；控制器**未声明 `d4`** → 明示「请先停止再下载」并中止（不伪装）；`COMPILE_ERROR` 仍落到 Problems。`debugAdapter.launch` 缺 `d4` 时输出提示 |
| T-17 | ✅ | 断点/单步/暂停（`13` D5）：`setBreakpoints` **全量替换**（`breakpoint.del {}` + 逐行 `breakpoint.add`）；就绪回 `verified:true`，未就绪回 `verified:false` + 说明（不伪装）。`next`/`stepIn`/`stepOut`→`script.step`（行级，三者同语义）、`pause`→`script.pause`、`continue`→`script.resume`。`script PAUSED` 取事件 `line`：命中已登记断点 → `stopped(breakpoint)` + `hitBreakpointIds`，否则 `stopped(pause)`；`initialize` 的 D5 相关能力位据实声明 |
| T-18 | ✅ | `src/subscribe.ts`：引用计数 + 同 tick 合并 + `clampHz`（默认 ≤20Hz/上限 50）；`NOT_SUPPORTED`/`UNKNOWN_METHOD` → `unavailable` 降级；`sys.info.caps` 未声明 `d3` 时 `disable()`（**不试、不重试、不刷日志**）；由**面板可见性**驱动 retain/release |
| T-19 | ✅ | `src/statusBits.ts`（`state.h` 位表镜像）+ `src/axisPanelPure.ts` + `src/axisPanel.ts`（v0.4.2 起为编辑器区 WebviewPanel `kine-x.axisPanel`）：轴面板分「标志 / 位置与错误 / `AXISSTATUS` 位表」三层；**空数据源位显式标注**、位表外的位置位单独告警 |
| T-20 | ✅ | 总线面板与 Modbus 4x 面板（`mb_regs[256]` 分 16 组 × 16，`regsLoaded` 区分「全 0」与「没数据」）；v0.4.3 起 Modbus 拆为独立 WebviewPanel `kine-x.modbusPanel`（总线随轴面板展示）；树模型 `panelModel.ts`/`panels.ts` 已于 **v0.4.7 移除** |
| T-21 | ✅ | 实时曲线（FR-6.6）：`src/curvePure.ts`（`CurveRing` 定长环形缓冲 + `curveHtml` 自包含 Webview 文档，**纯逻辑可 Node 单测**）+ `src/curve.ts`（`KxCurvePanel`：Webview + Canvas，`mpos`/`dpos` 双序列、轴下拉）；命令 `kine-x.curve.open`；**面板可见才 retain `axis`、隐藏即 release**（引用计数，不额外占带宽）；**缺采样补 `null`（断线，不补 0、不插值）**；高频采样按 D-06「首版不做」，仍属 `13` §9 |
| T-22 | ✅ | 分发与打包（FR-8.1/8.2/8.5）：`npm run package`（`vsce package --no-dependencies`）产出 `kinex-debug-<ver>.vsix`；**离线安装脚本** `tools/install-vsix.sh`（自动探测 `codium`/`codium-insiders`/`code`/`code-insiders`，支持 `--cli` / `--uninstall` / `--list`，**全离线不联网**，找不到 CLI 或 `.vsix` 时**明确报错并给出下一步**）；**Open VSX 发布** `tools/publish-ovsx.sh`（前置 `OVSX_PAT`，先 `compile`+`package` 再 `npx ovsx publish`，缺 token 即中止）；`package.json` 增 `install:vsix` / `uninstall:vsix` / `publish:ovsx`。冒烟 `checkDistribution()` 校验脚本存在、离线（无网络命令）与 `bash -n` 语法 |
| T-23 | ○ | 定制 VSCodium 发行版（FR-8.4）：**本仓库不构建**（无 VSCodium 构建条件；D-07 定「先离线 `.vsix`」），配方**已固化为可执行脚本** `tools/vscodium/` → 见 §8.6。`build-vscodium.sh` 提供 `fetch`/`brand`/`workspace`/`inject`/`build`/`all`；`inject` 可**离线**把 `.vsix` 预置为内置插件。冒烟 `checkVscodiumRecipe()` 校验文件齐全 / 离线（无 `curl`·`wget`）/ `bash -n` / JSON 合法 |
| T-25 | ✅ | 命令名补全（FR-2.5，P1）：`src/commandsPure.ts`（纯逻辑：`parseCommandTable` 解析 `data/commands.json`、`buildCommandCompletions` 生成候选，**Lua 带 `call:true`**、分组标签取自 `S.commandGroup`）+ `src/completion.ts`（`registerCompletionItemProvider(SCRIPT_LANGUAGE_IDS)`，`.lua` 用 `SnippetString('NAME($0)')`、`.bas` 直插；数据缺失时**明确告警并跳过**，不伪装）。`extension.ts` 激活时注册。冒烟 `checkCommandsPure()` 15 条用例 |
| T-24 | ✅ | 用户手册 `Extension/README.md`：安装（离线 `.vsix` / 命令行 / Open VSX）、快速上手 5 步、设置项表、DAP `launch.json` 字段表、状态面板与实时曲线、**D1~D5 能力位与「降级不伪装」**、FAQ、开发与回归口径。冒烟校验必备章节（安装/连接/调试/FAQ）与降级语义 |
| T-26 | ✅ | 面板连接入口补强：未连接时面板标题栏显示「连接控制器」（view/title + !kine-x.connected）、连接后变「断开连接」；render() 同步 setContext(kine-x.connected)；未连接占位行携带 command: kine-x.connect（panelModel/panels 支持 PanelNode.command）。冒烟 +1 条 |

> 面板装配（`extension.ts`）：`KxStatusStore`（事件收敛，FR-6.5 **事件驱动、不轮询**）+ `SubscribeManager`
> （面板可见才订阅）+ 三个 `TreeView`；事件 → 仓库 → 按受影响面板定向刷新（`store.update()` 返回 `PanelId[]`）。
> 轴面板**首次可见**时取一次 `axis.snapshot`（FR-6.5 的「一次性快照」），此后全靠推送；手动刷新命令
> `kine-x.panel.refresh` 会重新对齐订阅并重取快照。
>
> 面板侧**待 `13` 冻结**的协议点（Mock 已按**插件侧假设**实现，联调时以 `13` 为准并回写 `16`）：
> ① 订阅主题名 `bus` / `mb`（`16` §4.5 样例只出现 `axis` / `log`）；
> ② `axis` 事件须带**轴号**（topic 只有 `axis`，多轴无法区分），Mock 用 `axis:0`；
> ③ `axis` 快照**与**事件须带 `axis_status` / `err_code`（FR-6.1 要求，`16` §4.4 样例未含）；
> ④ `mb` 事件分段格式 `{start, regs[]}`；⑤ `bus` 事件 `{node_count, nodes:[{index,axis_count,status,io,aio}]}`；
> ⑥ `16` §2.4 方法表**没有** `bus.snapshot` / `mb.read`，故总线与寄存器面板**只能靠事件**取首次数据
> （轴面板例外：有 `axis.snapshot`）。

> T-16/T-17 侧**待 `13` 冻结**的协议点（Mock 已按**插件侧假设**实现，联调时以 `13` 为准并回写 `16`）：
> ⑦ `script PAUSED` 事件携带**当前执行行**的字段名（插件按 `line` 读取；`16` §2.4/§5.3 待补）；
> ⑧ `script.compile` 的 `p.swap=true`（热更新开关：**编译到新实例再原子替换**，`13` §5.1）；未声明 `d4` 时回 `NOT_SUPPORTED`；
> ⑨ `breakpoint.del` **无参 = 清空全部**（DAP `setBreakpoints` 是**全量替换**，而控制器无「源文件」概念 →
>    断点定为**会话级行号集合**：先 `del {}` 清空，再逐行 `add`）；
> ⑩ `breakpoint.add/del/list` 的项 schema（行号 `line` 与标签 `label` 的取舍）；
> ⑪ 单步粒度：协议只有 `script.step`，故 DAP 的 `next`/`stepIn`/`stepOut` **三者同语义（行级）**。
>
> 逐条验收动作见 **`17-D4D5联调验收清单.md`**（控制器 D4/D5 交付后按该清单打勾并回写本文）。
>
> 待拍板：`15` §11.2 已全部收敛（D-01 定为 `kine-x`；D-07 已按「先离线 `.vsix`」落地）。

### 8.6 T-23 定制 VSCodium 发行版（可选·配方）

> 状态：**未构建**（本仓库无 VSCodium 构建条件，且 D-07 定「先离线 `.vsix`」）。
> **配方已固化为可执行脚本**：`Extension/tools/vscodium/`（`npm run dist:vscodium`），按需在具备工具链的机器上执行。

**合规前提**：不得再分发微软官方 VS Code 二进制（`14` §6.2），只能基于 **VSCodium / Code-OSS 源码**构建。

**落点**：

| 文件 | 作用 |
|------|------|
| `Extension/tools/vscodium/build-vscodium.sh` | 主驱动：`fetch` / `brand` / `workspace` / `inject` / `build` / `all` |
| `Extension/tools/vscodium/product.overrides.json` | 合并进 Code-OSS `product.json`：品牌 + Open VSX + `configurationDefaults` |
| `Extension/tools/vscodium/workspace-template/` | 工作区模板 `settings.json` + `launch.json` |
| `Extension/tools/vscodium/README.md` | 配方说明与验证清单 |

**步骤**（对应脚本子命令）：

1. `fetch` —— 取 VSCodium + Code-OSS 源码（`get_repo.sh`，满足 `engines.vscode` 下限 ≥1.85）。
2. `brand <code-oss-dir>` —— 合并 `product.overrides.json`：`nameShort`/`nameLong`/`applicationName` → 品牌名；
   `extensionsGallery` → 指向 **Open VSX**（VSCodium 默认，`14` §6.1）；`configurationDefaults` → 预置
   `kine-x.host` / `kine-x.debugPort` / `kine-x.engine` / `kine-x.showRawProtocol`（锁连接，减少现场配置）。
3. `workspace <target-dir>` —— 预置工作区模板：`settings.json`（锁连接参数、默认关 `kine-x.showRawProtocol`）
   + `launch.json`（一条 `kine-x` 配置，一键连接）。
4. `build [vscodium-dir]` —— 执行 VSCodium 构建（Linux x64 / arm64 免安装包 + `.deb`，对齐 FR-8.5）。
5. `inject <vscodium-install> [vsix]` —— **离线**把 `.vsix` 预置为**内置插件**（解包到
   `resources/app/extensions/kinex-debug`），打开即用；无需构建环境也可执行。
6. 精简菜单 / 欢迎页，弱化与本项目无关的入口（`14` §6.6，降低 ZDevelop→VS Code 的迁移学习曲线）—— **需人工按品牌调整，脚本不代劳**。

> 回归：冒烟 `checkVscodiumRecipe()` 校验配方文件齐全、脚本**离线**（无 `curl`/`wget`）、`bash -n` 语法通过、
> `product.overrides.json` 与工作区模板为**合法 JSON** 且含关键字段；`brand`/`workspace`/`inject` 已在临时目录**离线实证通过**。
> 本目录被 `.vscodeignore` 排除（`tools/**`），不进 VSIX 产物。

---

## 9. 里程碑与迭代计划

对齐 `14` §7 的 P1~P5 与 `13` §7 的 D1~D9：

| 迭代 | 插件侧（本文 T） | 控制器侧依赖（`13`） | 出口标准 |
|------|------------------|----------------------|----------|
| **P1** 最小可用 | T-01~T-12 | D1~D2 | 连上→编辑→下载运行→看输出/诊断 |
| **P2** 真调试 | T-13~T-17 | **D4、D5** | 断点/单步/变量/监视可用（插件侧 T-16/T-17 已实装；控制器 D4/D5 已实装，待实机联调） |
| **P3** 面板曲线 | T-18~T-21 | D3 | 事件驱动面板，无需轮询 |
| **P4** 发行版 | T-22 ✅、T-24 ✅、T-25 ✅（FR-2.5 补全）、T-23（可选，配方脚本已落地 §8.6） | — | 离线 `.vsix` 就绪；现场开箱即用（发行版按需） |
| **P5** 品牌化 | （可选 Theia） | — | 仅在需独立 IDE 时评估（`14` §3） |

> ⚠️ **P2 的 pace-maker 是控制器侧 D4/D5，不是插件工作量**（`14` §5 结论）。
> 若控制器先只交付 D1~D3，插件**先做 P1/P3、P2 降级**，避免空转。

---

## 10. 测试与验收

### 10.1 测试层次

| 层 | 手段 | 覆盖 |
|----|------|------|
| 单元 | TS 单测：JSON-Lines 编解码、`list_vars` 解析、Status→DAP 映射、引擎一致性校验 | FR-2.4/2.6、§6.3/6.4 |
| 集成（无硬件） | **mock DebugServer**（回放 `13` schema 的假应答/假事件） | 连接、运行、面板、DAP 全链路 |
| 端到端 | 真控制器 + 真伺服（实机） | S1/S2/S3 场景 |
| 兼容性 | VSCodium + VS Code 双端安装、`.vsix` 安装 | NFR-1、FR-8.1/8.2 |
| 健壮性 | 断网/杀进程/乱序/未知字段/超时 | NFR-5、FR-7.4 |

### 10.2 验收清单（对应上文 FR）

- [ ] 连接：`sys.info` 正确展示；断线可见、可重连（FR-1.2/1.4）
- [ ] 编辑+运行：`.bas` 改一行即时下载运行；语法错落到具体行（FR-2.1/2.4、FR-3.1）
- [ ] 停止：长循环脚本能及时停止（FR-3.3）
- [ ] 变量：运行中变量面板刷新、可写、REPL 可执行命令（FR-4.5/4.6/4.7）
- [ ] 引擎校验：`lua` 引擎配 `.bas` 被明确拦截（FR-2.6、§6.4）
- [ ] 面板：轴状态/寄存器由事件推送更新，≤20Hz（FR-6.1/6.4/6.5）
- [ ] 日志：订阅日志进输出面板（FR-7.1）
- [ ] 终端：设备命令执行与返回码语义正确（FR-5.1/5.2）
- [ ] 分发：VSCodium 内成功安装 `.vsix`（脚本与产物就绪并冒烟校验，真机安装待现场；FR-8.1/8.2/8.5）
- [ ] **P2 项**：断点/单步/热更新（依赖 `13` D4/D5；未就绪时须显示「不支持」而非假象）

---

## 11. 风险与待定项

### 11.1 风险（真坑，承自 `14` §6）

1. **强依赖 `13` 未实施**：D4（热更新）/D5（引擎 pause 钩子）是 P2 的前提；**控制器侧工作量 > 插件侧**。
   缓解：**mock DebugServer**（`tools/mock-debug-server.mjs`）已带 `d4`/`d5` 能力开关，插件侧
   「有 / 无能力」两条路径均已冒烟覆盖（`npm run test:smoke`），控制器未交付前也能验证 T-16/T-17（§10.1）。
2. **Open VSX ≠ 微软 Marketplace**：若依赖第三方插件须先确认 Open VSX 有替代（`14` §6.1）。
3. **DAP 不覆盖实时曲线**：波形须单开 Webview/高频通道，别指望 DAP（`14` §6.3）。
4. **VS Code 官方二进制不可再分发**：发行版只能基于 VSCodium/Code-OSS 源码构建（`14` §6.2）。
5. **现场迁移学习曲线**：ZDevelop 式 → VS Code 式；用 P4 发行版（预置+精简菜单）缓解（`14` §6.6）。
6. **接口漂移**：`13` 与本文的 RPC 名/字段须双向同步；以 `13` 为单一事实来源，插件按 schema 容错（NFR-5/7）。
7. **引擎互斥误用**：用户易在同工作区混放两语言；须在 UI 拦截（§6.4）。

### 11.2 开工决议表（原待定项收敛）

> 状态：**待拍板**。下表把原「待定项」收敛为**建议决议 + 理由 + 落点**，供开工前一次性确认；
> 标 ★ 者需你/相关方最终拍板，其余可按建议直接执行。

| # | 议题 | 建议决议 | 理由 | 落点 |
|---|------|----------|------|------|
| D-01 ★ | 插件 id / 显示名 / publisher | **已拍板**：`kine-x.kinex-debug` / 显示名「Kine-X 调试」/ publisher `kine-x` | 项目定名「Kine-X 控制器」，发布账号沿用该 publisher | `package.json` |
| D-02 | Adapter 落地形态 | **同包内 TS**（DebugSession + Adapter 同 vscode 扩展） | 免第二运行时、部署简单；语言自由是退路（`14` §6.5） | T-13 |
| D-03 | `engine` 来源 | **扩展名推断为主 + 设置可覆盖**；推断与覆盖冲突时**报错** | 减少配置；与 `engine_rule.h` 校验一致（§6.4） | T-11 |
| D-04 | `launch.json` ↔ `app.conf` 对齐 | 插件名→控制器键：`waitBus`→`SCRIPT_WAIT_BUS`、`maxSteps`→`SCRIPT_MAX_STEPS`、`trace`→`SCRIPT_TRACE`、`engine`→`SCRIPT_ENGINE` | 语义同源，避免两套概念 | §7.2/7.3 |
| D-05 | 是否引入 LSP | **首版不做**；仅 TextMate 语法 + 编译诊断（FR-2.4） | 成本低、先跑通；LSP 列为 P2 备选（`14` §6.4） | FR-2.7 |
| D-06 | 实时曲线实现 | **Webview + Canvas + 事件订阅**（T-21 已落地）；**高频采样首版不做**，需要时按 `13` §9 另开通道 | DAP 不覆盖曲线（`14` §6.3）；订阅速率（≤20Hz）已够看趋势 | FR-6.6 |
| D-07 | 分发方式 | **离线 `.vsix` 已落地**（`tools/install-vsix.sh`：自动探测 editor CLI、`--uninstall`/`--list`、全离线）；Open VSX 发布脚本 `tools/publish-ovsx.sh` 就绪（按需用）；定制发行版配方见 §8.6 | 现场常无外网（FR-8.5） | T-22/T-23 ✅ |
| D-08 | Mock 归属/语言 | **插件侧**维护，独立小服务，仅监听 `127.0.0.1`，带能力开关 | 服务于插件集成测试；控制器实装后留作回归 | `16` §5.5 |
| D-09 ★ | 协议字段冻结 | `err.code` 用**字符串码**；`cmd` 用 `out:[]` + `err`；`sys.info` 增**能力位** | 便于插件分支与启动定 UI；落实「降级不伪装」 | `16` §7 |
| D-10 | 组件命名/目录 | 遵循 §3 组件表；源码目录对齐 `15` §3（前端/Adapter/语法） | 与文档一致 | T-01 |

**开工前置条件（Checklist）**：

- [x] D-01 已拍板（publisher `kine-x`，插件 id `kine-x.kinex-debug`）；**D-07 已按建议落地**（离线 `.vsix`，见 T-22）；**D-09 已拍板 → 见 `16` §7.1**。
- [x] `16` §5 的 Mock 落地（能力开关 = D1~D5），作为 T-06 的对接对象 → `Extension/tools/mock-debug-server.mjs`。
- [ ] `13` 的 D1~D3 排期确认（插件 P1 的外部依赖）。

---

## 12. 与既有文档的关系

- `14-调试软件客户端方案.md`：本文的**上位文档**——`14` 定「用什么基座、走 DAP」，
  本文定「**具体需求与开发任务**」；`14` P1~P5 ↔ 本文 §9 迭代。
- `13-调试通道与上位机通讯.md`：**控制器侧**调试口；本文是它的**客户端消费方**，
  §6 的契约即对 `13` §4.4 的用例化。两文 + `14` 构成「服务端 / 形态选型 / 客户端需求」三件套。
- `12-范围与边界.md`：界定「协议对接归脚本层、调试通道归 C++ 层」；本文再落到「**客户端归 PC 侧 IDE 插件**」，
  并明确**不碰业务协议**。
- `08` / `10`（BASIC）、`09` / `11`（Lua）：脚本语言与设备命令 API；本文的语法高亮、补全、
  变量/诊断数据源均取自其命令表与引擎接口（`command_table.h`、`engine.h`）。
- `engine_rule.h`（代码）：§6.4「同一时刻只加载一种脚本语言」的**规则来源**，插件须强制。
- `README.md`：里程碑 **M7（调试软件后端）** 的**前端延伸**；本文属**可选增强**，不阻塞 M5/M6。
- `02-软件栈与环境搭建.md`：`systemd`/日志/部署；`DEBUG_*` 配置项并入 `config/app.conf` 风格（`13` §4.3）。
- `16-调试通道报文样例与Mock方案.md`：**报文样例（测试向量）+ Mock 方案**——为本文 §4 的协议层验证、
  §10.1 集成测试提供冻结用例与无控制器时的对接对象（Mock 归属与能力开关见本文 §11.2 D-08）。

---

> 状态：**实施中**——**T-01~T-15 ✅**（P1 最小可用 + Adapter 主链）、**T-16/T-17 ✅**（热更新 + 断点/单步/暂停：
> 插件侧已实装并冒烟覆盖「有 / 无 `13` D4/D5」两条路径；**控制器侧 `13` D4/D5 现已实装**
> （`src/script/debug_server.cpp`；v0.8.0 起 BASIC / Lua **两引擎**均报 `d5`）、
> **T-18~T-21 ✅**（订阅管理 + 轴状态/总线/寄存器面板 + 实时曲线 Webview，事件驱动、不轮询）、
> **T-22 ✅ / T-24 ✅**（离线 `.vsix` + 安装/发布脚本 + 用户手册）、**T-25 ✅**（FR-2.5 命令名补全）、
> **T-23 ○**（定制发行版：配方已固化为可执行脚本 `tools/vscodium/`，见 §8.6；本仓库不构建）——进度见 §8.5。
> NFR-8（文案集中/i18n 预留）：UI 字符串统一收敛到 `src/strings.ts`（`S` + `setLocale()`），
> 除 `strings.ts` 外源码不含中文字面 UI 文案。
> Mock DebugServer 已就位（`Extension/tools/mock-debug-server.mjs`；`--caps d1,d2,d3,d4,d5` 可模拟 D1~D5 就绪度；
> `npm run test:smoke` **131/131**），回归口径：`npx tsc -p ./` 0 error → smoke 全绿 → `vsce package` 出 VSIX。
> 插件侧 P1~P4 已落地（T-23 可选）；`13` 的 D4/D5 控制器侧已实装，
> **实机连接已打通（2026-09-25）**：插件 v0.3.4 ↔ 控制器 v0.1.0，`sys.info` 应答 caps=[d1,d2,d4,d5,d3]；
> 插件形态演进：侧边栏改为**单一「控制台」Webview**（菜单栏 + 轴/总线/寄存器表，v0.3.0 起），
> 「连接」改为**表单弹窗**（IP+端口+连接按钮，v0.3.2），新增**脚本语言切换**（Basic/Lua，v0.3.1）；
> **v0.4.2 形态修正**：连接后 axis/bus/mb 最高 20Hz 推送会把与表格同视图的菜单栏一并高频重建，
> 导致「连接后菜单点不动」——控制台自此**只承载菜单栏 + 状态行**（低频数据），
> 轴状态/总线/寄存器表格迁至右侧**「轴状态」面板**（`kine-x.axis.open`，菜单「工具 → 轴状态」打开，
> 与「工具 → 曲线」同形态；面板可见才 retain axis/bus/mb 订阅）。
> **v0.4.3 拆分**：Modbus 寄存器独立成**「Modbus」面板**（`kine-x.modbus.open`，菜单「工具 → Modbus」），
> 与轴状态面板分开存在；订阅随面板拆分——轴面板 retain axis/bus，Modbus 面板 retain mb。
> **v0.4.5 同步/文件管理**（依赖控制器 D6，见 `13` §7）：
> 「同步」（`kine-x.sync`）方向 = **控制器 → 工作区**（`file.list`+`file.get` 拉取到 `controller-sync/`）；
> 「下载」= 本地 → 控制器（`script.compile` 带 `name`，编译成功即保存到控制器）；
> 新增**「控制器文件」面板**（`kine-x.files.open`，工具菜单）：列出控制器保存的脚本并标明 Basic/Lua/其他，可逐个/全部拉取；
> 缺 `d6`（板端未升级）→ 面板与同步均明确降级提示（不伪装）。
> **v0.4.6**：控制器文件列表并入侧边栏控制台常驻（不再有 `kine-x.files.open` 独立面板；
> `file.list`/`file.get` 仍为 D6，缺 `d6` 明示降级）；控制台 = 菜单栏 + 状态行 + 文件列表（均低频数据）。
> **v0.4.7**：① 移除无运行引用的树形面板模型（`panelModel.ts`/`panels.ts`、`view/title` 死菜单与
> `kine-x.menu.*` 子菜单贡献全部删除；smoke 改为直接断言 store/statusBits/订阅）；② 引擎冲突**本地前置拦截**：
> 连接后按 `sys.info.engine`（= 板端 `SCRIPT_ENGINE`，**与控制器目录里有无文件无关**）与本文件扩展名比对，
> 不一致直接给出「换脚本 / 改板端 SCRIPT_ENGINE 并重启服务」两条出路，不发必然被 `ENGINE_MISMATCH` 拒绝的
> `script.compile`；③ `filesPanelPure` 文案收进 `strings.ts`；④ 旧 VSIX 归档、版本升 `0.4.7`。
> **v0.4.8**：对齐控制器「语言由脚本目录推导」（`13` §4.4，2026-09-25 拍板）——
> ① 识别 `sys.info.engine` 的 `auto`/`mixed`（状态栏/控制台显示「自动 / 混合」，`auto` 不再触发工作区语言不匹配告警）；
> ② `basic`/`lua` 绑定时保留本地前置拦截；③ 编译成功后自动重取 `sys.info`（`auto`→绑定、`d5` 能力位随语言刷新）；
> ④ DAP 启动不再把 `auto`/`mixed` 当冲突，编译后同样刷新 caps；⑤ Mock 支持 `--engine auto`，
> smoke 覆盖「空目录两种都收 → 落盘绑定 → 越界拒绝 → 删除回退」全流程。
> **v0.4.9**：控制器文件改用**原生 List/TreeView**（侧边栏独立视图 `kine-x.files`，用户拍板）——
> 语言图标（Basic 绿 / Lua 蓝 / 其他灰）+ 大小描述；行内按钮与右键菜单：拉取（`kine-x.files.pull`）、
> 删除（`kine-x.files.delete`，模态确认，删空目录会解绑语言）；视图标题栏：刷新（`kine-x.files.refresh`）、
> 全部拉取（`kine-x.files.pullAll`）；未连接 / 缺 `d6` / 拉取失败 / 空目录均以占位行明示（降级不伪装）；
> 控制台 Webview **移除**文件列表，回归「菜单栏 + 状态行」形态（`filesPanelPure` 提供行模型/图标/尺寸纯函数，
> `filesTree.ts` 为 vscode 适配层）。
> **v0.5.0**：新增「**控制器 → 重启控制器**」（`kine-x.restart` → `sys.restart`，协议 `13` D7）——
> 模态确认（明示会中断脚本/调试/电机任务）→ 请求重启 → 主动断开 → **自动重连**（每 1.5s，≤20s，
> 成功后恢复 `sys.info`/日志/面板订阅）；控制器未声明 `d7` 时菜单置灰且本地明确拒绝（降级不伪装）。
> Mock 恒含 `d7`（`sys.restart` 只记录不重启），smoke 覆盖菜单启用/禁用与接口应答。
> **v0.5.1**：「修改 IP 地址」升级为「**修改IP地址（含端口）**」——复用连接表单
> （`connectPanelPure` 新增 `mode: 'connect' | 'settings'`，同一面板运行期可切换模式；
> 设置模式按钮为「保存」，写入 `kine-x.host` / `kine-x.debugPort`，**不立刻连接**）；
> 相应清理两步 InputBox 时代的死文案（`setIpTitle`/`setIpPrompt`/`hostInvalid` 等）。
> **v0.6.0**（方案 A，用户拍板 2026-09-26）：**主文件（开机运行）**——「控制器文件」列表里 ★ 标记主文件，
> 行内/右键「设为主文件 / 取消主文件」（`kine-x.files.setMain` / `clearMain` → 控制器 D8 `boot.set/clear`）；
> 主文件失效（被删）时列表顶部占位明示原因；删除主文件时确认框加重提示；设置后提示可用「重启控制器」
> （D7）立即应用。控制器侧：`sys.info.boot`、`.boot` 清单、开机脚本优先主文件（语言按扩展名），
> 其它脚本由主文件 `INCLUDE` 编译期展开（`10` §7.5 / `11`）。缺 `d8` 的旧固件：只读列表，主文件交互明确降级。
> **v0.6.1**：轴状态面板「指令位置」列改为 **DPOS 保留 4 位小数**；修正该表**列与表头错位**的旧缺陷
> （表头写「指令位置/反馈位置」但依次显示 MPOS/DPOS）→ 现为 指令位置=DPOS、反馈位置=MPOS；
> 曲线图例标签同步纠正（DPOS=指令位置、MPOS=反馈位置）。
> **v0.6.2**：轴状态面板刷新体验优化——Webview 改**增量渲染**（表格只建一次、仅更新变化单元格，
> 取代每帧 `innerHTML=''` 全量重建）；扩展侧改**稳压刷新**并按新设置 `kine-x.axisRefreshHz`
> （默认 10Hz，2~20）重绘，等待期间事件只置脏、到点取最新快照；面板加「**暂停刷新**」按钮与
> **最后更新时间**（冻结读数/复制，数据仍在后台更新）。
> **v0.6.3**：轴状态面板「反馈位置」（MPOS）同样保留 4 位小数（与指令位置一致）。
> **v0.6.4**：Modbus 寄存器面板采用与轴状态面板同款刷新策略——增量渲染（16 组 details 与 256 个值单元格只在结构变化时重建，展开状态自动保留）+ 稳压刷新（新设置 `kine-x.modbusRefreshHz`，默认 10Hz）+「暂停刷新」与最后更新时间。
> **v0.7.0**（四项体验/能力补齐）：
> ① **调试会话单客户端交接**：DAP 启动时断开主连接让位、结束自动连回并恢复订阅（调试期间手动连接被拒）；
> ② **意外断线自动重连**（指数退避 2/5/10/20/30s；新设置 `kine-x.autoReconnect`）+ 状态栏「重连中」；
> ③ **轴面板在线调试**：使能/去使能/停止/按住点动（松开或 1.5s 心跳中断自动 CANCEL）/目标定位（绝对/相对+速度），
> 经 D1 `cmd` 下发、移动 `wait=0` 异步；
> ④ **曲线增强**：新增推算通道（跟随误差、速度=ΔMPOS/Δt）、图例点击显隐、**导出 CSV**、清空当前窗口
> （`curveToCsv` 纯函数可单测）。
> **v0.8.0**：
> ① **Lua 调试器（D5 扩展）**：Lua 引擎实装行级 hook + 断点/暂停/单步/挂起中中止唤醒
> （对齐 BASIC `dbg_gate` 语义；`enable_line_hooks` 仅调试装载时打开，生产脚本零开销）；
> 顺带修复「目录绑定语言与已装载引擎不一致时不重装引擎」的缺陷。
> ② **补全 / 语法高亮 / 命令表生成完善**：签名与说明上移到控制器 `command_table.h` 的 `kDocs[]`
> （单一事实来源），生成器同时产出 `data/commands.json`（names/groupList/docs）并**就地注入两份语法高亮**
> 的命令名表；补全按签名生成参数占位片段、Hover 显示签名+说明、签名帮助高亮当前实参；
> 新增 BASIC/Lua 代码片段；smoke 增加「生成器一致性」校验（改表不忘重跑）。
> **v0.8.2**：底部状态栏品牌项显示插件版本号（`Kine-X v0.8.2`，取 `package.json`，不硬编码）；smoke 增加版本号一致性校验。
> **v0.8.3**：`MODBUS_IEEE` 32 位字序定案同步（**低字在前**，2026-09-26 触摸屏实测）：控制器 `command_table.h kDocs` 文案更新 → `node tools/gen-commands.mjs` 重生成 `data/commands.json`（Hover/签名提示随之更新）；插件无功能改动。
> **v0.8.4**：控制器文件列表**一致性标识**——控制器 `file.list` 每条目新增 `hash`（FNV-1a 64，`13` D6），插件对 `controller-sync/<同名文件>` 算同款哈希并显示 **一致/不一致/本地无副本/未比较**（行 description + tooltip 说明，`filesPanelPure` 纯逻辑 + smoke 向量锁定）；拉取成功后自动重算；旧固件无 `hash` → 明示"未比较"（降级不伪装）。Mock 同步实现 `hash`。
> **v0.12.4**：「Modbus 主站」新增设备表单**字段功能文字置于输入框上方**（链路/名称/主机/IP/端口/站号/超时(ms)/重试/轮询(ms)；此前仅 placeholder，填值后不可见含义），并纳入 labels 分发与 smoke 锁定 → **373/373**；`kinex-debug-0.12.4.vsix`。
> **v0.12.3（评审修复）**：D12 校验 proto-aware（`w` 合法，宿主侧权威校验 `validateD12Text`）；D13 客户端校验补固件边界（timeout 50~5000/retries 0~5/poll 20~60000/设备非空/线圈类仅 u16·i16/fc6 不支持 2 字类型）；D11 相关回退保留给旧固件。smoke **372/372**；`kinex-debug-0.12.3.vsix`。
> **v0.12.1**：「工具 → **Modbus 配置**」更名为「**Modbus 从站**」（与「Modbus 主站」对称：从站=控制器对外暴露的寄存器表；主站=控制器去访问的外部设备），面板标题/提示同步；smoke 增两页文案不混淆锁定 → **356/356**；`kinex-debug-0.12.1.vsix`。
> **v0.12.0（M-P2）**：「工具 → **Modbus 主站**」（协议 **D13** `mbdev.get/set/status`，planA/21）——设备表（链路 tcp/rtu-tcp、站号、超时/重试/轮询）+ 点位表（名称/方向/功能码/地址/数量/类型/映射 4x·变量/变化即发）增删改 + 客户端校验（功能码↔方向、数量↔类型、写点 4x 映射不重叠）+ 保存下发 + **设备在线状态**（在线/err/timeouts/ok）；缺 `d13` 置灰。新增 **内联脚本可解析守卫**（smoke 抽取面板内联 JS 做语法解析，防"TS 模板吞转义"再犯——v0.11.2 两处缺陷的自动防线）。smoke **355/355**；`kinex-debug-0.12.0.vsix`。
> **v0.11.2**：修复「Modbus 配置 · 新增/保存无反应」——①生成 JS 的 `+ '\n'` 被 TS 模板转义成**真实换行**（脚本语法错误、整个脚本块失效）→ 转义修正；②校验正则 `\d` 被模板吞成 `d`（`/^4x(d+)$/` 永远匹配失败、保存被拦）→ 修正。验证：HTML 内联脚本 `node --check` 通过 + **假 DOM 全链路**（新增→填值→保存→产出正确 registers JSON）。smoke **342/342**；`kinex-debug-0.11.2.vsix`。
> **v0.11.1**：修复「Modbus 配置」对 **D12 组态文本的解析**——固件 `config/modbus.json` 用 `registers` 键，此前解析器只认 D11 `.mbmap` 的 `entries` → 误报「缺少 entries 数组」；现两者归一化兼容（真机 `mbreg.get` 文本解析通过）。smoke **342/342**；`kinex-debug-0.11.1.vsix`。
> **v0.11.0（P2）**：「Modbus 配置」切**固件组态 D12**（`mbreg.get/set`，planA/20）——有 `d12` 时**全表可编辑**（名称/地址/类型 u16·i16·u32·f32·f32hi/读写 r·w·rw/掉电保持/**默认值**/说明），保存经固件校验+热加载；无 `d12` 时自动回退过渡版（D11 用户寄存器编辑）或本地副本只读；菜单门控 = `d12‖d11`。smoke **341/341**、tsc 0 error；`kinex-debug-0.11.0.vsix`。
> **v0.10.0**：「Modbus 配置」升级为**可配置**——新增「用户寄存器（可编辑 · 真正生效）」编辑区：增/删/改（地址/名称/类型 u16·i16·u32·f32·f32hi/读写/掉电保持/说明）+ 客户端校验（越界/重叠/权限）+ **保存到控制器**（`mbmap.set` 合并下发，控制器 ~1s 热加载）；控制器侧新增 **4x300~999 用户寄存器区**（`kx_regmap.lua`：解析/校验/读写在 `kx_base` 叠加、RO 拦截、`persist` 走 `.nvram`、`REGMAP_GET` 轮询热加载）与 `NvramStore` 扩容 256→1024；固定区条目保持只读（已配置）。smoke **337/337**、ctest 17/17；`kinex-debug-0.10.0.vsix`。
> **v0.9.0**：「工具 → **Modbus 配置**」（协议 **D11** `mbmap.get/set`）——展示控制器 Modbus 寄存器表（地址/名称/类型/读写/★掉电保持/说明，按分区折叠式分组 + 搜索 + 刷新；来源=控制器 `.mbmap`，离线/未部署/读取失败时回退插件内置 `data/regmap.json` 并**明确标注来源**）；表格数据由 `tools/gen-regmap.mjs` 从 `planA/18`+`19` 生成（单一事实来源，改文档需重跑）；缺 `d11` 菜单置灰（降级不伪装）。新增纯逻辑 `mbmapPanelPure.ts`（解析/分组/文案/HTML）进 smoke → **331/331**；`kinex-debug-0.9.0.vsix`。
> **v0.8.17**：补齐菜单图标——「工具 → 通讯状态」加 **radio-tower**（无线电塔：塔身+双侧信号波，此前 `mk()` 传了 `'radio-tower'` 但 `ICON_SVG` 漏配 → 渲染为空）；顺带修复同类漏配：「控制器 → 重启控制器」**debug-restart**（refresh 环形箭头镜像、逆时针以示区分）、「控制器 → 修改端口数量」**server-process**（三层机架）。smoke 新增守卫「每个菜单项必须带内联 SVG（防漏配 ICON_SVG）」→ **321/321**；`kinex-debug-0.8.17.vsix`。
> **v0.8.16**：修复「通讯状态」同端口多客户端**行互相覆盖**——行 id 由 `p<端口>` 改为 `p<端口>-<对端IP>:<端口>`（502 同时显示触摸屏与机器人两条；对端 IP 列已有）。smoke 增同端口两行回归 → 320/320。
> **v0.8.14**：① 控制器侧**持久化命令 NVSET/NVGET**（`command_table.h` + `MotionHost::call` + 新模块 `nvram_store`）经 `gen-commands.mjs` 进入补全/帮助（81 条命令）；② smoke 319/319、tsc 0 error；插件无行为改动（数据同步）。
> **v0.8.13**：修复**拉取内容损坏**（根因级）——`connection.onData` 用 `chunk.toString('utf8')` 逐包解码，TCP 分片切在多字节中文中间时产生 `U+FFFD`（JSON 仍可解析 → “拉取成功”但本地文件与控制器哈希不符 → 永远显示「不一致」）。改用 `node:string_decoder` 的 `StringDecoder`（缓存半个序列到下一块）；smoke 增跨包中文回归用例（在“中”的 UTF-8 中间切包）。**安装后需重新拉取一次**以修复此前被损坏的 `controller-sync/` 副本。
> **v0.8.12**：修复「拉取到工作区」（单文件）体验——① 成功/失败此前只写输出通道（失败被静默）→ 现均弹信息/警告；② 连点/行点击与行内按钮并发触发 → 在途集合防抖（同名只跑一次）；③ `file.get` 超时 5s→**15s** 且**超时自动重试一次**（大文件/总线忙时 5s 易误判为无响应）；「全部拉取」共用同一助手同步受益。
> **v0.8.11**：修复“引擎校验”误报——`resolveEngine` 此前对**非脚本文件**（`.md`/`.txt`/无扩展名，如输出面板 `extension-output-…`）按 `engineFromFile` 一律推断为 `basic`，与显式 `kine-x.engine=lua` 冲突报警。现引入 `inferEngineName`（无法判断→`null`）与 `decideEngine`（**无法判断即放行**：显式设置优先、未设置按 basic；仅已知 `.lua`/`.bas` 与显式设置不符才拦截）；smoke 增 5 组裁决用例（含 `.md` 放行）。
> **v0.8.10**：命令表新增 `SRAMP`/`FASTDEC`/`VP_SPEED`（速度曲线 API）→ 重生成 `data/commands.json`（79 条）与语法高亮；插件无功能改动。
> **v0.8.9**：命令表新增 `JOGLEAD`（点动 PP 跟随前视，脚本可调）→ 重生成 `data/commands.json` 与两份语法高亮（补全/Hover 同步）；插件无功能改动。
> **v0.8.8**：修复「通讯状态」面板空表——`conn` 事件在扩展事件分发 `switch` 中落 `default`（只记日志）未进仓库；现与 `bus/mb` 同路（`store.update` → 定向刷新），并加源码级 smoke 守卫防回归。
> **v0.8.7**：「工具 → **通讯状态**」（协议 **D10** `conn` 订阅）——列表显示控制器对外连接（连接方式/用途、控制器角色、主/从、对方 IP、端口、状态四态）与 **EtherCAT 主站 + 每个从站明细**（从站号/身份 vid:pid rev/名称/轴/AL/在线）；面板可见才订阅（引用计数）；缺 `d10` 菜单置灰并明确提示。控制器侧新增进程级连接快照（含 accept 抓取的对端 IP）与脚本 `PORT_INFO` 标签。
> **v0.8.6**：Modbus 寄存器面板**「已用寄存器」监视**——控制器 `mb` 事件带 `used` 位图（脚本访问即标记），面板高亮已用单元格 + 工具栏「仅显示已用」过滤（已用计数、空组隐藏、无数据明示）；控制器侧新增 `Shared::mb_used` 与脚本 `mirror_used()`（100ms 全量镜像，含 HMI 输入区）。
> **v0.8.5**：「控制器 → 修改端口数量」（协议 **D9** `port.max.get/set`）——输入框校验 1..64、显示当前上限与占用端口；有占用导致不能收缩时明示原因（`BUSY`）；缺 `d9` 置灰并明确报需升级板端（降级不伪装）。控制器侧运行期上限持久化 `.portmax`（重启保留）。Mock 同步实现。
> P2 实机联调按 `17` 清单逐条进行。
