# 16 调试通道报文样例与 Mock 方案

> 定位：`13-调试通道与上位机通讯.md` 定义了控制器侧调试口（`DebugServer`）的**协议规范**；
> `15-VSCodium插件需求与开发任务.md` 定义了客户端（插件）的**需求与任务**。
> 本文补齐两者之间的**「可对接的报文样例（test vectors）+ 无控制器时的 Mock 方案」**，
> 使插件（`15` T-01~T-15）能在 `13` 服务端**尚未实施**时**独立开发与自测**。
>
> 边界：本文**只给报文样例与 Mock 设计描述，不提供可执行源码**（延续「只整理文档」）。
> 协议字段以 `13` §4.4 为准；若 `13` 实施时调整，须双向同步本文（单一事实来源 = `13`）。
> **尚未实施**。

---

## 0. 一页速览（TL;DR）

| 项 | 约定 |
|----|------|
| 分帧 | **JSON-Lines**：每行一条 JSON（UTF-8，`\n` 结尾）；一条连接内混跑 req/resp/event |
| 三类报文 | 请求 `{id,m,p}` / 应答 `{id,ok,r|err}` / 事件 `{e,t,…}`（无 `id`） |
| 关联 | 应答以 `id` 回关请求；事件无 `id`，客户端按 `e` 分发 |
| 错误模型 | `ok:false` + `err{code,msg[,line]}`；`line` 仅在编译/运行错误时出现 |
| 样例用途 | 作为插件集成测试的**冻结用例**；作为 Mock 服务端的**行为清单** |
| Mock 目标 | 无控制器时回放「连接→编译→运行→变量→面板→事件」全链路 |
| Mock 形态 | 推荐**状态机模拟**（非纯静态回放），见 §5.3 |

---

## 1. 传输与分帧约定

| 项 | 约定 | 依据 |
|----|------|------|
| 传输 | TCP；`<控制器业务网IP>:<DEBUG_PORT>`（现场如 `192.168.1.11:5000`；Mock 用 `127.0.0.1:5000`） | `13` §4.3 |
| 编码 | UTF-8；**每条报文一行**，以 `\n`（0x0A）结尾 | `13` §4.1 |
| 分帧 | 严格「一行一条」，**不接受**跨行 JSON；行内不得含裸换行 | 本文 |
| 方向 | 双向：客户端发请求，服务端回应答并**主动**推事件 | `13` §4.4 |
| 保活 | 可选心跳（建议空闲 5s 发一条 `{"m":"sys.ping"}` / `{"ok":true}`） | 待定（§7） |
| 限长 | 单行建议 ≤ 1MB（脚本源码上传行）；超限应报错而非截断 | 待定（§7） |

> ⚠️ 调试口**不复用** 4321 业务口（`13` §4.3）；两者分帧风格不同（本口是 JSON-Lines，4321 是 `;`/`/n`）。

---

## 2. 消息信封（Envelope）

### 2.1 请求（Client → Server）

```jsonc
{"id": 1, "m": "method.name", "p": { /* 参数，可省略 */ }}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | int | 是 | 客户端自增，用于关联应答；同连接内唯一 |
| `m` | string | 是 | 方法名（见 §2.4） |
| `p` | object | 否 | 参数对象；无参方法可省略 |

### 2.2 应答（Server → Client）

成功：

```jsonc
{"id": 1, "ok": true, "r": { /* 结果，可省略 */ }}
```

失败：

```jsonc
{"id": 1, "ok": false, "err": { "code": "BAD_PARAM", "msg": "axis out of range", "line": 0 }}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | int | 回关请求的 `id` |
| `ok` | bool | 成败 |
| `r` | any | 成功结果（对象/数组/标量） |
| `err.code` | string | 错误码（§3） |
| `err.msg` | string | 人话描述（可展示到 UI） |
| `err.line` | int | 仅编译/运行错误：出错行号（0=未知） |

### 2.3 事件（Server → Client，主动）

```jsonc
{"e": "axis",   "t": 12345, "mpos": 12.5, "dpos": 12.5, "inc_per_mm": 14043.41, "idle": 1}
{"e": "script", "t": 12346, "status": "DONE", "steps": 8123}
{"e": "log",    "t": 12347, "s": "[script] loop=3", "lvl": "info"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `e` | string | 主题：`axis` / `script` / `log`（可扩展） |
| `t` | int | 毫秒时间戳（单调时钟），供曲线对齐 |
| 其余 | — | 随 `e` 而定，见 §4.4/§4.5 |

### 2.4 方法名总表（与 `13` §4.4、`15` §6.1 一致）

| 方法 | 参数 `p` | 结果 `r` | 服务端依赖 |
|------|----------|----------|------------|
| `auth` | `{token}` | `{}` | `13` §4.5（**已冻结**：连接首帧可选鉴权） |
| `sys.info` | — | `{ver,engine,axis_count}` | `13` D1 |
| `sys.ping` | — | `{}` | 本文建议（保活） |
| `sys.restart` | — | `{restarting:true}` | **`13` D7**（**已实装**，v0.5.0）：异步执行 `DEBUG_RESTART_CMD`；`DEBUG_RESTART_CMD` 为空 → `NOT_SUPPORTED` 且 `caps` 无 `d7` |
| `boot.get` | — | `{dir,name,valid,reason?}` | **`13` D8**（**已实装**，v0.6.0）：主文件（开机运行）清单，`.boot` 单行文件名；引用失效 → `valid:false` + `reason` |
| `boot.set` | `{name}` | `{name,valid:true}` | **`13` D8**：文件必须已存在且扩展名可判语言；目录 mixed / 已绑定其它语言 → `ENGINE_MISMATCH`；不存在 → `NOT_FOUND` |
| `boot.clear` | — | `{name:""}` | **`13` D8**：取消主文件（开机回退 `SCRIPT_FILE`） |
| `mb` 事件（订阅） | — | `{e:"mb",t,start,regs:[…],used:[4×16 位十六进制]}` | **`13` v0.8.6**：`used`=脚本访问过的寄存器位图（已用寄存器监视）；旧固件无该字段 |
| `conn` 事件（订阅） | — | `{e:"conn",t,conns:[…],bus:{…,slaves:[…]}}` | **`13` D10**（**已实装**，v0.8.7）：端口连接快照 + EtherCAT 主/从明细；`conns[].peer` 为对端 IP（server 为客户端 IP）、`tag/role` 来自脚本 `PORT_INFO` |
| `port.max.get` | — | `{max,slots,default,dir,used:[…]}` | **`13` D9**（**已实装**，v0.8.5）：运行期端口上限（静态容量 64，默认 16；`used`=当前占用端口号） |
| `port.max.set` | `{max}` | `{max,prev,saved:true,file}` | **`13` D9**：1..64 整数；有端口占用（`>= max`）→ `BUSY`（msg 指出占用端口）；落盘 `.portmax` 失败 → `RUNTIME_ERROR` 并回滚；重启保留 |
| `script.compile` | `{src,engine,swap?,name?}` | `{labels:[…],swapped?,saved?,save_error?}` | `13` D2；`swap:true` 需 **`13` D4**；`name` 需 **`13` D6**（编译成功后源码落盘到 `DEBUG_SCRIPT_DIR`，失败不阻断编译，回 `saved:false`+`save_error`） |
| `script.run` | — | `{status}` | `13` D2 |
| `script.stop` | — | `{status}` | `13` D2 |
| `script.status` | — | `{status,steps,error_line,line}` | `13` D2（`line` 供 D5 挂起时定位） |
| `script.pause` / `script.resume` | — | `{status}` | **`13` D5** |
| `script.step` | — | `{status}` | **`13` D5**（**行级单步**；DAP `next`/`stepIn`/`stepOut` 同语义，见 `15` §8.5 ⑪） |
| `breakpoint.add` / `list` | `{line}` / — | `{breakpoints:[{line:N},…]}` | **`13` D5**（**已实装**；`{label}` 形式本期不支持） |
| `breakpoint.del` | `{line}` 或 **无参 = 清空全部** | `{breakpoints:[{line:N},…]}` | **`13` D5**（**已实装**；DAP `setBreakpoints` 为**全量替换**：先清空再逐行 add） |
| `file.list` | — | `{dir,files:[{name,size,hash},…]}` | **`13` D6**（**已实装**；列 `DEBUG_SCRIPT_DIR`，不递归；目录不存在回空清单；`hash`=内容 FNV-1a 64 十六进制，v0.8.4 起，供插件一致性标识） |
| `file.get` | `{name}` | `{name,src}` | **`13` D6**（**已实装**；缺失 → `NOT_FOUND`；非法 name → `BAD_PARAM`） |
| `file.del` | `{name}` | `{deleted:true}` | **`13` D6**（**已实装**；非法 name → `BAD_PARAM`） |
| `var.list` | — | `["NAME = 3", …]` | `13` D1~D2 |
| `var.get` | `{name}` | `{name,type,value}` | `13` D1~D2 |
| `var.set` | `{name,v}` | `{}` | `13` D2 |
| `axis.snapshot` | `{axis}` | `{bus_ok,mpos,dpos,idle,enabled,alarm}` | `13` D1 |
| `cmd` | `{line}` | `{ret,out:[…]}` | `13` D1 |
| `subscribe` | `{topics,hz}` | `{sub:[…]}` | `13` D3 |
| `unsubscribe` | `{topics}` | `{sub:[…]}` | `13` D3 |

> 说明：`p`/`r` 的字段为**草案**，`13` 实施时以其实译为准则并回写本文。
>
> T-16/T-17 新增字段（**已按 Mock 假设实装并冻结**，见 §9.6）：
> `p.swap` / `r.swapped`、`script.status.line`、`script PAUSED` 事件的**当前行字段名**（插件按 `line` 读取）、
> `breakpoint.del` 的**无参清空**语义。

---

## 3. 错误模型（错误码）

`err.code` 建议为**稳定字符串**（便于插件做分支与 i18n）：

| code | 含义 | 触发 | UI 处理 |
|------|------|------|---------|
| `BAD_REQUEST` | JSON 语法错/缺字段 | 分帧或信封非法 | 报协议错误（FR-7.4） |
| `UNKNOWN_METHOD` | 方法不存在 | `m` 未注册 | 提示版本不符/降级 |
| `BAD_PARAM` | 参数非法/越界 | 轴号越界、类型不符 | 高亮错误参数 |
| `AUTH_FAILED` | 鉴权失败 | 首帧 token 错（`13` §4.5） | 提示检查 token |
| `ENGINE_MISMATCH` | 引擎与文件不符（`15` §6.4） | `lua` 引擎配 `.bas` | 拦截并提示 |
| `COMPILE_ERROR` | 编译失败 | `script.compile` | 落到 `err.line`（FR-2.4） |
| `RUNTIME_ERROR` | 运行期错误 | `script.run`/`cmd` | `stopped`(exception) |
| `BUSY` | 资源忙 | 运行中重复 run | 提示稍后 |
| `NOT_SUPPORTED` | 功能未实现 | 命中 `13` D5 未就绪项 | **明确提示不支持**（`15` §4.4 降级） |
| `TIMEOUT` | 超时 | 内部等待超时 | 重试/报错 |

> `err.msg` 供人读；**插件分支只看 `code`**，不解析 `msg`（避免文案化耦合）。

---

## 4. 会话样例（Test Vectors）

> 约定：`C→` 为客户端发送，`S←` 为服务端回送。为便于阅读，行内用注释标出说明（**真实报文不含注释**）。

### 4.1 握手：`sys.info`

```
C→ {"id":1,"m":"sys.info"}
S← {"id":1,"ok":true,"r":{"ver":"0.9.0","engine":"basic","axis_count":1}}
```

> `engine` ∈ `basic` / `lua` / `auto` / `mixed`（2026-09-25 拍板：语言由控制器 `DEBUG_SCRIPT_DIR`
> 内现有脚本推导，见 `13` §4.4）。空目录 → `auto`，两种语言都可编译：
>
> ```
> S← {"id":1,"ok":true,"r":{"ver":"0.9.0","engine":"auto","axis_count":1}}   // 空目录
> C→ {"id":2,"m":"script.compile","p":{"src":"x=1","engine":"lua","name":"demo.lua"}}
> S← {"id":2,"ok":true,"r":{"labels":["init","loop"],"saved":true}}
> S← {"id":3,"ok":true,"r":{"ver":"0.9.0","engine":"lua","axis_count":1}}    // 落盘后绑定 lua
> ```

失败（未鉴权，若启用 token）：

```
C→ {"id":1,"m":"sys.info"}
S← {"id":1,"ok":false,"err":{"code":"AUTH_FAILED","msg":"token required"}}
```

### 4.2 编译 → 运行 → 状态 → 停止

```
C→ {"id":2,"m":"script.compile","p":{"src":"PRINT \"hi\"\nEN\nMOVEABS 55\nEND","engine":"basic"}}
S← {"id":2,"ok":true,"r":{"labels":["main"]}}

C→ {"id":3,"m":"script.run"}
S← {"id":3,"ok":true,"r":{"status":"READY"}}

S← {"e":"script","t":10233,"status":"READY","steps":0}     // 开始执行
S← {"e":"log","t":10240,"s":"hi","lvl":"info"}             // PRINT 落点（经 output/log）
S← {"e":"script","t":15980,"status":"DONE","steps":8123}   // 正常结束

C→ {"id":4,"m":"script.status"}
S← {"id":4,"ok":true,"r":{"status":"DONE","steps":8123,"error_line":0}}
```

编译失败（`err.line` 供 Problems 定位）：

```
C→ {"id":5,"m":"script.compile","p":{"src":"MOVEABS\nEND","engine":"basic"}}
S← {"id":5,"ok":false,"err":{"code":"COMPILE_ERROR","msg":"参数不足: MOVEABS","line":1}}
```

运行期错误 / 步数超预算：

```
S← {"e":"script","t":20411,"status":"RUNTIME_ERROR","steps":3210}
C→ {"id":6,"m":"script.status"}
S← {"id":6,"ok":true,"r":{"status":"RUNTIME_ERROR","steps":3210,"error_line":42}}

S← {"e":"script","t":30000,"status":"BUDGET_EXCEEDED","steps":5000000}
```

用户停止：

```
C→ {"id":7,"m":"script.stop"}
S← {"id":7,"ok":true,"r":{"status":"ABORTED"}}
S← {"e":"script","t":31500,"status":"ABORTED","steps":4200}
```

### 4.3 变量：list / get / set

```
C→ {"id":8,"m":"var.list"}
S← {"id":8,"ok":true,"r":["SPEED = 100","POS = 12.5","FLAG = 1"]}

C→ {"id":9,"m":"var.get","p":{"name":"SPEED"}}
S← {"id":9,"ok":true,"r":{"name":"SPEED","type":"num","value":100}}

C→ {"id":10,"m":"var.set","p":{"name":"SPEED","v":250}}
S← {"id":10,"ok":true,"r":{}}

C→ {"id":11,"m":"var.get","p":{"name":"NOPE"}}
S← {"id":11,"ok":false,"err":{"code":"BAD_PARAM","msg":"变量不存在: NOPE"}}
```

> `list_vars()` 原生返回 `"NAME = 3"` 字符串（`engine.h`）；插件解析为 `name/value`（`15` FR-4.5）。
> 具体类型（`num`/`str`/`nil`）由 `var.get` 给出，对应 `kx::Value`。

### 4.4 轴快照 + 终端 `cmd`

```
C→ {"id":12,"m":"axis.snapshot","p":{"axis":0}}
S← {"id":12,"ok":true,"r":{"bus_ok":1,"mpos":12.5,"dpos":12.5,"inc_per_mm":14043.41,"idle":1,"enabled":1,"alarm":0}}

C→ {"id":13,"m":"cmd","p":{"line":"STA"}}
S← {"id":13,"ok":true,"r":{"ret":0,"out":[]}}

C→ {"id":14,"m":"cmd","p":{"line":"POS"}}
S← {"id":14,"ok":true,"r":{"ret":0,"out":["12.500"]}}

C→ {"id":15,"m":"cmd","p":{"line":"MOVEABS"}}      // 参数不足
S← {"id":15,"ok":false,"err":{"code":"RUNTIME_ERROR","msg":"参数不足: MOVEABS"}}
```

轴号越界（`15`/`motion_host.h` 明确报错，绝不回读别的轴）：

```
C→ {"id":16,"m":"axis.snapshot","p":{"axis":3}}     // 本机 axis_count=1
S← {"id":16,"ok":false,"err":{"code":"BAD_PARAM","msg":"axis out of range: 3"}}
```

### 4.5 订阅 + 事件流

```
C→ {"id":17,"m":"subscribe","p":{"topics":["axis","log"],"hz":20}}
S← {"id":17,"ok":true,"r":{"sub":["axis","log"]}}

S← {"e":"axis","t":40001,"mpos":12.5,"dpos":12.5,"inc_per_mm":14043.41,"idle":1}
S← {"e":"log","t":40005,"s":"[script] loop=3","lvl":"info"}
S← {"e":"axis","t":40052,"mpos":12.6,"dpos":12.6,"idle":1}   // ≈20Hz
…

C→ {"id":18,"m":"unsubscribe","p":{"topics":["log"]}}
S← {"id":18,"ok":true,"r":{"sub":["axis"]}}
```

### 4.6 错误与降级样例

```
C→ {"id":19,"m":"bogus.method"}
S← {"id":19,"ok":false,"err":{"code":"UNKNOWN_METHOD","msg":"未知方法: bogus.method"}}

C→ {"id":20,"m":"script.step"}                       // 13 的 D5 未就绪
S← {"id":20,"ok":false,"err":{"code":"NOT_SUPPORTED","msg":"单步需引擎 pause 钩子(13 D5)"}}

C→ {"id":21,"m":"script.compile","p":{"src":"x=1","engine":"basic"}}   // 控制器已绑定 lua（目录内有 .lua）
S← {"id":21,"ok":false,"err":{"code":"ENGINE_MISMATCH","msg":"控制器脚本目录中为 lua 脚本，不能编译 basic"}}

C→ {"not-json}}                                       // 分帧/信封非法
S← {"id":0,"ok":false,"err":{"code":"BAD_REQUEST","msg":"invalid JSON line"}}
```

> `NOT_SUPPORTED` 是 `15` §4.4「降级不伪装」在协议层的落点：插件据此把「断点/单步」标为**不可用**。

---

## 5. Mock DebugServer 方案

### 5.1 目标与边界

- **目标**：在控制器 `DebugServer`（`13`）尚未实施时，让插件（`15`）的**集成测试**与**功能演示**可跑通
  「连接→编译→运行→变量→面板→事件→错误/降级」全链路（对应 `15` §10.1 集成层）。
- **边界**：Mock **不是**控制器替代品——不接 EtherCAT、不驱动电机、不做真实运动；
  轴数据/寄存器为**设定值**。它只模拟**协议行为**，用于客户端联调。

### 5.2 形态选项

| 形态 | 说明 | 优点 | 缺点 | 建议 |
|------|------|------|------|------|
| A 静态回放 | 按预置脚本逐条回放 §4 的固定报文 | 实现最快 | 不响应客户端输入顺序 | 仅做「冒烟」 |
| B **状态机模拟** | 维护会话状态（未编译/运行中/已停止），按请求给合法应答 | **可交互**、覆盖分支 | 需实现小状态机 | **推荐** |
| C 录播 | 录制真机会话，按 `id`/方法检索回放 | 最接近真实 | 录制依赖真机（鸡生蛋） | 后期补 |

> 推荐 **B 状态机模拟**：它能让插件真正"用起来"（点运行有状态变化、点停止变 `ABORTED`、订阅后有事件流），
> 而 A 只能验证渲染。

### 5.3 必须模拟的行为清单

| # | 行为 | 关键状态 | 对应样例 |
|---|------|----------|----------|
| 1 | 握手返回 `sys.info`（`engine` 可配 `basic`/`lua`/**`auto`**） | — | §4.1 |
| 2 | `compile` 成功返回 `labels`；失败返回 `COMPILE_ERROR`+`line`（可注入错误） | 源码是否含"错误标记" | §4.2 |
| 3 | `run` 后进入 `READY`，按定时器依次发 `script` 事件到 `DONE` | 运行态 | §4.2 |
| 4 | `stop` → `ABORTED`；可注入 `RUNTIME_ERROR` / `BUDGET_EXCEEDED` | 终止原因可配 | §4.2 |
| 5 | 变量表：预置若干 `NAME = N`；`get/set` 读写同一内存表 | 变量内存 | §4.3 |
| 6 | `axis.snapshot` 返回**设定值**（可随 `run` 变化，演示曲线） | 假轴状态 | §4.4 |
| 7 | `cmd` 对已知命令返回 `ret/out`，未知命令报 `RUNTIME_ERROR` | 命令白名单 | §4.4 |
| 8 | `subscribe` 后按 `hz` **周期发事件**；`unsubscribe` 停 | 订阅集 | §4.5 |
| 9 | 未知方法 → `UNKNOWN_METHOD`；非法 JSON → `BAD_REQUEST` | — | §4.6 |
| 10 | D5 相关（`pause/step/breakpoint`）默认返回 `NOT_SUPPORTED`，**可用开关**模拟已支持 | 能力开关 | §4.6 |
| 11 | **D4（`--caps` 含 `d4`）**：`script.compile` 带 `p.swap=true` → 编译到新实例并**原子替换**，回 `{labels,swapped:true}`，运行态不中断（发 `[hotswap]` 日志）；未声明 `d4` → `NOT_SUPPORTED`。**板端已实装**（§9.1） | 能力开关 + 运行态 | §4.2 |
| 12 | **D5 运行态（`--caps` 含 `d5`）**：`run` 时若已登记断点 → 到该行推 `script PAUSED`（事件带 `line`）；`step` 行号 +1 再推 `PAUSED`；`resume` 推 `READY` 续跑；`pause` 在运行中推 `PAUSED`。**板端已实装**（BASIC 引擎，§9.2） | 运行态 + 断点集 | §4.2 |
| 13 | **语言绑定（`--engine auto`）**：`sys.info.engine` 报 `auto`，两种语言都可编译；带 `name` 落盘后按文件扩展名绑定（`.lua`→`lua` / `.bas`→`basic`）；绑定后另一种语言 `ENGINE_MISMATCH`；`file.del` 删空回 `auto`；两种并存 → `mixed`（一律拒绝编译）。**板端已实装**（§9.1） | 文件 Map + 绑定推导 | §4.1 |
| 14 | **D7 重启**：`--caps` 含 `d7`（Mock 恒含）时 `sys.restart` → `{restarting:true}` 并推 `[restart]` 日志，**不真的重启 Mock 进程**；缺 `d7` → `NOT_SUPPORTED`。**板端已实装**（§9.1，真机由 `DEBUG_RESTART_CMD` 执行） | 能力开关 | §4.1 |
| 16 | **D9 端口数量上限**（v0.8.5）：Mock 恒含 `d9`；`port.max.get` 默认 16（slots 64）；`port.max.set` 校验 1..64 整数，非整数/越界 → `BAD_PARAM`，只改内存（`sess.portMax`，不落盘）。**板端已实装**（含 `.portmax` 持久化） | 会话内 `portMax` 字段 | §4.1 |
| 15 | **D8 主文件**（v0.6.0）：Mock 恒含 `d8`；`boot.set` 校验文件名/存在性/语言绑定（mixed 拒绝），`boot.get` 在文件被删后回 `valid:false` + `reason`；`sys.info.boot` 回显；**只改内存清单，不重启**。**板端已实装**（§9.1） | 文件 Map + 绑定推导 | §4.1 |

> 第 10~12 条很关键：用一个「**能力开关**」模拟 `13` 的 D1~D5 就绪度，
> 插件即可**同时验证「有 / 无 D4·D5」两条路径**（`15` §4.4 降级策略）——
> 无能力走 `NOT_SUPPORTED` 降级，有能力走真实命中/热替换，两条路都要跑通（`Extension/test/connection.smoke.cjs` 已固化）。

### 5.4 假数据设定（默认值，供用例固定）

| 项 | 默认 | 备注 |
|----|------|------|
| `sys.info.engine` | `basic` | 可切 `lua` 测引擎校验；`auto` 模拟控制器语言绑定（按文件 Map 推导，空=auto） |
| `axis_count` | `1` | 测越界用 `axis=3` |
| 变量表 | `SPEED=100`、`POS=12.5`、`FLAG=1` | 与 §4.3 一致 |
| 轴快照 | `bus_ok=1,enabled=1,idle=1,alarm=0`；`mpos` 随时间缓增 | 演示面板/曲线 |
| `labels` | `["main"]`（basic）/`["init","loop"]`（lua） | 演示断点列表 |
| `cmd` 白名单 | `STA`/`POS`/`MPOS`/`EN`/`DIS`/`STOP` | 与 `command_table.h` 对齐 |

### 5.5 交付物与目录约定（建议）

| 项 | 建议 | 说明 |
|----|------|------|
| 归属 | **插件仓库**内 `Extension/tools/mock-debug-server.mjs`（**已落地**） | 与真机解耦；随插件版本走 |
| 备选归属 | 独立小工具（控制器仓库外） | 若控制器侧也想用则抽公共 |
| 形态 | 独立可执行 / 作为测试内嵌服务（起在本地端口）；实现为 Node 脚本 | 无第三方依赖；**仅监听 `127.0.0.1`** |
| 入口 | `--port 5000 --engine basic\|lua\|auto --caps d1,d2,d3[,d4][,d5] [--verbose]` | 用能力开关控制就绪度（§5.3 第 10~12 条）；`auto` 覆盖语言绑定（第 13 条） |
| 资产 | 本文章节即**行为规格**；§4 即**测试向量**；已用 `Extension/test/connection.smoke.cjs` 固化 | 无需另写文档 |

> 维护责任：建议**插件侧**维护（它服务于插件测试）；控制器侧实现 `13` 后，Mock 保留用于回归。

---

## 6. 用例 ↔ 需求映射（供 `15` §10 集成测试）

| 样例 | 覆盖 `15` 的 FR | 断言点 |
|------|------------------|--------|
| §4.1 | FR-1.4 / FR-1.3 | `sys.info` 展示；`AUTH_FAILED` 可见 |
| §4.2 | FR-2.4 / FR-3.1 / FR-3.2 / FR-3.3 / FR-3.6 / FR-4.9 / FR-4.10 | 编译诊断、运行态、停止、终止原因、输出 |
| §4.3 | FR-4.5 / FR-4.6 / FR-4.7 | 变量视图/写入/REPL |
| §4.4 | FR-5.1 / FR-5.2 / FR-6.1 | 终端、返回码、轴面板 |
| §4.5 | FR-6.4 / FR-6.5 / FR-7.1 | 订阅、限频、日志面板 |
| §4.6 | FR-2.6 / FR-7.4 / **§4.4 降级** | 错误提示、引擎拦截、`NOT_SUPPORTED` 不伪装 |

---

## 7. 待定与开放问题

### 7.1 已拍板（冻结，插件按此实现）

| 议题 | 决议 | 落点 |
|------|------|------|
| `err.code` 形态 | **字符串码**（§3 表），插件分支只看 `code` | `15` §11.2 D-09；`src/protocol.ts` `ErrorCodes` |
| `cmd` 返回 | 成功 `{"ret":0,"out":[…]}`；失败走 `err`（`RUNTIME_ERROR`/`BAD_PARAM`） | `15` §11.2 D-09 |
| 能力声明 | `sys.info` 一次性返回 `caps:["d1","d2","d3","d4","d5"]`（就绪几项报几项），插件启动即定 UI | `15` §11.2 D-09 |
| 首帧鉴权 | `{"m":"auth","p":{"token":"..."}}`；未实现→`UNKNOWN_METHOD`→按未启用放行 | `13` §4.5（已冻结） |
| `script` 事件 | **同时带** `error_line`，免二次查询 | §4.2 |
| Mock 归属/语言 | 插件仓库 `Extension/tools/mock-debug-server.mjs`，Node 无依赖，仅 `127.0.0.1` | §5.5 |

### 7.2 仍待定（与 `13` 联调时拍板）

- [ ] 保活：是否需要 `sys.ping` 心跳？空闲多久算断线？（§1；Mock 已实现 `sys.ping`）
- [ ] 单行长度上限与超限行为（报错 vs 分块）？（§1；脚本源码上传行是主要顾虑）
- [ ] 订阅能力：主题可扩展集与上限（`axis/bus/log` 之外）？（§4.5、`13` §9）
- [x] `script.pause/resume/step`、`breakpoint.*` 的**参数与返回**细节 —— **已冻结并实装**（见 §9.2 / §9.6）
- [x] `script PAUSED` 事件携带**当前执行行**的字段名 —— **已冻结为 `line`**（与 `script.status` 同名，见 §9.3）
- [x] `script.compile` 的 `p.swap` / `r.swapped` 命名与「编译到新实例再原子替换」语义 —— **已实装**（见 §9.1 / §9.6）

> 上述各项的**逐条验收动作与回写方式**见 **`17-D4D5联调验收清单.md`**（联调时对照打勾，拍板后回写本文 §2.3/§2.4/§3 与本节）。

---

## 8. 与既有文档的关系

- `13-调试通道与上位机通讯.md`：**协议规范来源**（本文是其**用例化 + 可测试化**）；字段以其为准。
- `15-VSCodium插件需求与开发任务.md`：**消费方**；本文 §6 直接映射其 FR，§5 支撑其 §10.1 集成测试。
- `10`/`11`（脚本 API）、`src/script/engine.h`、`src/script/motion_host.h`、`src/script/command_table.h`：
  方法语义与命令名/返回码的**事实来源**（变量格式、`cmd` 语义、`Status` 名）。
- `12-范围与边界.md`：再次确认本 Mock **只模拟调试通道**，不涉及业务协议。

---

> 状态：**板端 D1~D5 已实施且实机连通**（2026-09-25：服务 active、5000 监听、`sys.info` 应答 caps 全量；
> 实现见 `src/script/debug_server.cpp`，§9；D5 仅 BASIC 引擎）；
> §5 的 Mock 仍是 PC 侧离线联调用的**等价物**（能力开关 = D1~D5），二者报文须一致。

---

## 9. 板端实施回写（`13` D1~D5 已实装冻结，本文 §2.4 / §3 以此为准）

> 本节是 §2.4「以实译为准则并回写本文」的落实结果。实现：`src/script/debug_server.cpp`
> （TCP JSON-Lines，会话线程 + 事件推送）；自测：`tools/debug_server_test.cpp`（ctest `debug_server_test`）。
> 与本文样例的**差异处仅以下几点**，其余字段名与示例逐字一致。

### 9.1 能力与范围（D1~D6）

- `sys.info.r` 实际字段：`{ver, engine, axis_count, debug_port, caps}`。
  `caps` = `["d1","d2","d4","d5","d3","d6"]` 顺序下的就绪子集（**就绪几项报几项**）：
  * `d1` / `d3` 恒报；
  * `d2` 仅当调试口可独占脚本引擎时报（`allow_script`：`SCRIPT_ENABLE=1` 且已配置开机主文件/`SCRIPT_FILE`
    时被自动脚本占用，此时只报 `d1`；两者都未配置则独占，正常报 `d2`/`d4`/`d5`）；
  * `d4` 与 `d2` 同条件（热更新需可独占引擎）；
  * **`d5` 仅当引擎为 `basic`** 且 `d2` 就绪时报（断点/暂停/单步只在 BASIC 引擎实装；
    语言由目录绑定，`auto` 初始按 `basic` 声明，编译 `lua` 后刷新 `sys.info` 即不再报 `d5`）；
  * **`d7` 当 `DEBUG_RESTART_CMD` 非空时报**（重启控制器：`sys.restart`；插件菜单「控制器 → 重启控制器」）；
  * **`d8` 恒报**（主文件/开机运行：`boot.get/set/clear`；插件「控制器文件」列表设主文件）；
  * **`d9` 恒报**（端口数量上限：`port.max.get/set`，持久化 `.portmax`；插件「控制器 → 修改端口数量」）；
  * **`d10` 恒报**（通讯状态：`conn` 订阅；插件「工具 → 通讯状态」）；Mock 返回演示连接/从站（标签与板端脚本一致）；
  * **`d6` 恒报**（文件管理：`file.list/get/del` + `script.compile` 带 `name` 落盘）。
    落盘目录 `DEBUG_SCRIPT_DIR` **同时决定调试口语言**（2026-09-25 拍板）：
    `engine` 报 `basic`/`lua`（单一语言绑定）/`auto`（空目录，两种都收）/`mixed`（并存，拒绝编译）；
    绑定在「带 `name` 编译成功落盘」时确立，`file.del` 删空后回到 `auto`；自动脚本（`SCRIPT_ENABLE=1`）不受目录影响。
    落盘目录 `DEBUG_SCRIPT_DIR`（默认 `/userdata/kine-x/scripts`，板端 `mkdir_p` 逐级创建）；
    `name` 白名单 = 字母/数字/`._-` 且不以 `.` 开头（防路径穿越），非法一律 `BAD_PARAM`。
- 未实装方法（`breakpoint.remove/set/clear`、`watch.*`、`engine.swap`、`state.dump`、
  `machine.snapshot`、`trace.*`、`scope.*`、`axis.monitor`、`mb.write`、`io.read/io.write`、`task.list` 等）
  一律回 `NOT_SUPPORTED`——**不伪装可用**。D5 方法在 `lua` 引擎下也回 `NOT_SUPPORTED`（`msg` 说明仅 BASIC 实装）。
- `script.compile`：不带 `swap`（或未运行时）→ `{labels:[…], swapped:false}`；
  运行中带 `p.swap=true` → 编译到**新实例**，成功则**原子替换**并回 `{labels:[…], swapped:true}`，
  旧实例被 `request_abort()` 回收（旧脚本推出 `ABORTED` 事件）；**编译失败不影响运行中的旧脚本**。
- 与自动脚本**互斥**：`SCRIPT_ENABLE=1` 且已指定 `SCRIPT_FILE` 时脚本引擎归自动脚本，
  调试口的 `script.*` / `var.*` 回 `BUSY`（此时只剩 D1：状态/命令/订阅）。

### 9.2 各方法回包（实译）

| 方法 | 实际 `r` |
|------|----------|
| `sys.info` | `{ver, engine, axis_count, debug_port, caps:[…]}` |
| `sys.ping` | `{}` |
| `sys.restart` | `{restarting:true}`（先回应答，随后服务重启断开；命令为空 → `NOT_SUPPORTED`） |
| `boot.get` / `boot.set` / `boot.clear` | `{dir,name,valid[,reason]}` / `{name,valid:true}` / `{name:""}`（`.boot` 单行文件名；改动下次启动生效） |
| `auth` | `{}`（`DEBUG_TOKEN` 非空时生效；首帧后除 `sys.ping` 外均需先 `auth`） |
| `script.compile` | `{labels:[…], swapped:false}`（失败 `COMPILE_ERROR` + `err.line`；运行中 `swap:true` → `swapped:true`；带 `name` 成功落盘 → 追加 `saved:true`，`auto` 下该次编译确立语言绑定） |
| `script.run` / `script.stop` | `{status}`（运行中重复 run → `BUSY`） |
| `script.status` | `{status, steps, error_line, line}`；挂起中 `status="PAUSED"`、`line` = 当前执行行 |
| `script.pause` | `{status:"PAUSED"}`；未运行 → `BAD_PARAM`；非 BASIC 引擎 → `NOT_SUPPORTED` |
| `script.resume` | `{status:"READY"}`；未运行 → `BAD_PARAM` |
| `script.step` | `{status:"PAUSED"}`（行级单步：走一条语句后在下一行边界再停）；未挂起 → `BAD_PARAM` |
| `breakpoint.add` | `{breakpoints:[{line:N},…]}`；缺/非法 `line` → `BAD_PARAM`（**标签断点本期不支持**） |
| `breakpoint.del` | `{breakpoints:[…]}`；`{line}` 删单个，**无参 = 清空全部**（DAP `setBreakpoints` 全量替换语义） |
| `breakpoint.list` | `{breakpoints:[{line:N},…]}` |
| `var.list` | `["NAME = 3", …]`（原样透传引擎列表，供 UI 直接显示） |
| `var.get` | `{name, type, value}`；`type` ∈ `num`/`str`/`nil`；变量不存在 → `BAD_PARAM` |
| `var.set` | `{}`；**只允许写已存在变量**（拼错不静默新建）→ 否则 `BAD_PARAM` |
| `axis.snapshot` | `{bus_ok, mpos, dpos, idle, enabled, alarm, axis_status, err_code}`（比 §2.4 多后两项，为透传 CiA402 状态字） |
| `cmd` | `{ret:0, out:[…]}`；`out` 只在命令有返回值时非空（如 `POS`）；失败走 `err` |
| `subscribe` / `unsubscribe` | `{sub:[…]}`（顺序固定 `axis,bus,mb,log`；无参 `unsubscribe` = 全部退订） |

### 9.3 事件（`e` 字段）

- `script`：**仅状态跳变时**推（含 `steps`/`error_line`/挂起行变化），字段 `{e,t,status,steps,error_line,line[,error]}`；
  `status` 名与 `15` §6.3 映射一致（`READY/PAUSED/DONE/COMPILE_ERROR/RUNTIME_ERROR/ABORTED/BUDGET_EXCEEDED`）；
  **D5 挂起**时 `status="PAUSED"`、`line` = 当前执行行（断点命中 / `script.pause` / `script.step` 后均推一条）。
- `log`：`{e,t,s,lvl}`，**未订阅也推**（脚本调试必需；`s` 为 `PRINT`/命令回显单行）。
- `axis`：主题只有一个 `axis`，轴号在事件内 —— `{e,t,axis,mpos,dpos,idle,enabled,alarm,bus_ok,axis_status,err_code}`；
  多轴时**每轴一条**，按 `hz` 周期推。
- `bus`：`{e,t,node_count,nodes:[{index,axis_count,status,io,aio,in_count,out_count}]}`。
- `mb`：`{e,t,start,regs:[…]}`，每周期推一段（滚动，段长内置），覆盖全部保持寄存器。

### 9.4 板端配置（`config/app.conf`）

```
DEBUG_ENABLE    = 1          # 开机启用调试通道（随 kine-x.service 自启）
DEBUG_PORT      = 5000       # 本项目自定端口（非 ZDevelop 的 500）
DEBUG_BIND      = 0.0.0.0    # 绑业务网卡，局域网 PC 直连（也可写 192.168.1.11）
DEBUG_TOKEN     =            # 可选加固；非空则首帧须 auth
DEBUG_MAX_STEPS = 5000000    # script.run 步数预算兜底（超限 → BUDGET_EXCEEDED）
```

### 9.5 与 §4 样例的实测差异（自测已固化）

1. `sys.info` 多出 `debug_port` / `caps` 两字段（§7.1 能力声明决议）。
2. `axis.snapshot` 多出 `axis_status` / `err_code`。
3. `subscribe` / `unsubscribe` 回包为 **`{"sub":[…]}` 对象**（§2.4 草案行已按此对齐）。
4. 非法 JSON 回 `{"ok":false,"err":{"code":"BAD_REQUEST",…},"id":-1}`（解析不到 `id` 时用 `-1`）。
5. `cmd` 的 `out` 对无返回值命令为空数组 `[]`（不是 `null`）。

### 9.6 对 §7.2 待定项的收口

- `sys.ping` **保留**（实现为恒 `{}` 的保活），板端**不做空闲超时断开**（断开由客户端主动或 TCP 层发现）。
- `p.swap` / `r.swapped`：命名冻结如上；**已实装**——运行中 `swap:true` → `swapped:true`；
  未运行时 `swap` 等价普通编译（`swapped:false`）。
- `script.pause/resume/step`、`breakpoint.*`、`script PAUSED` 当前行字段名：**已冻结并实装**——
  `PAUSED` 事件与 `script.status` 均用 `line` 承载当前执行行；`breakpoint.del` 无参 = 清空全部。
- **D5 状态前置不满足时的错误码**：本固件复用 `BAD_PARAM`（`msg` 说明原因）——
  协议 §3 错误码表未设「状态类」码，若后续 `13` 增补（如 `BAD_STATE`）在此同步。
- **D5 断点粒度**：仅**行号**（BASIC 语句行）；`breakpoint.add` 的 `{label}` 形式本期**不支持**（回 `BAD_PARAM`）。


> 驱动 `15` 的 T-06~T-15；待 `13` 的 D1~D3 实装后，用 §4 向量做**Mock↔真机一致性**回归。