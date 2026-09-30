# 11 Lua 脚本 API 与开发指南

> 本文是 **Lua 引擎使用者视角的 API 参考 + 开发/运行手册**，与 `10-BASIC脚本API与开发指南.md`
> 一一对应：设备命令**同名同义**，差异只在「语言前端」与少量语法糖。
>
> 定位：**可选增强层**。标准 Lua 5.4 语言 + 本项目自定义 API；与 BASIC 引擎**互斥，二选一**。
> 评估与任务分解见 `09-Lua脚本引擎评估.md`；BASIC 版见 `10`。
> **命令名与语义的唯一事实来源是 `src/script/motion_host.{h,cpp}`**，命令清单由
> `src/script/command_table.h` 的 `motion_command_names()` 导出。

实现文件：

| 文件 | 作用 |
|------|------|
| `src/script/engine.h` | 语言无关接口 `IScriptEngine` |
| `src/script/script_host.h` | 设备宿主接口 `kx::ScriptHost`（**语言无关**，Lua 引擎唯一对接点，**不 include BASIC 头**） |
| `src/script/script_value.h` | 公共值类型 `kx::Value`（Lua 值桥接的输入/输出） |
| `src/script/lua_engine.h` / `lua_engine.cpp` | Lua 引擎：沙箱 + 绑定 + 预算 hook + 值桥接 |
| `src/script/command_table.h` | 命令名清单（两引擎共享的「单一事实来源」） |
| `src/script/motion_host.h` / `.cpp` | 设备宿主（与 BASIC **完全复用**，不改语义） |
| `third_party/lua/` | vendor 的标准 Lua 5.4.6（MIT） |
| `tools/lua_engine_test.cpp` | 假宿主纯逻辑单测（8 组） |

---

## 1. 快速上手（3 分钟）

**第一步：写脚本** `demo.lua`

```lua
print("BUS=" .. BUS())                 -- print 是 Lua 原生输出（终端）
if BUS() == 0 then
    print("总线未就绪，退出")
    return
end

ENABLE()
MOVEABS(55, 100, 1000)                 -- 绝对定位到 55mm，速度 100mm/s，加速度 1000mm/s^2
print("到位位置=" .. POS(0))
STOP()
```

**第二步：离线检查 + dry 试跑（不需要硬件）**

```bash
cd lubancat2/build
./script_run demo.lua             # .lua 扩展名自动选 Lua 引擎
./script_run demo.lua --check     # 只做语法检查
./script_run demo.lua --engine lua   # 也可显式指定引擎
```

**第三步：真机运行**（在控制器进程内）

```bash
# 方式 A：命令行临时指定
./kine-x --script /opt/kine-x/demo.lua --script-engine lua

# 方式 B：写进 config/app.conf
#   SCRIPT_ENABLE = 1
#   SCRIPT_FILE   = /opt/kine-x/demo.lua
#   SCRIPT_ENGINE = lua
```

> ⚠️ **两个引擎互斥（规则，已强制）**：控制器**同一时刻只加载一种脚本语言**。共用同一个
> `kx::Shared`，同一时刻只跑一个引擎线程（配置二选一，不是同时运行）。
> `SCRIPT_ENGINE` 只允许 `basic | lua`，**非法值在载入配置时明确报错**；引擎还须与脚本扩展名
> 一致（lua↔`.lua`）。代码上由 `src/script/engine_rule.h` 的 `ScriptEngineSlot` 保证"只装一个引擎"。

---

## 2. 语言与沙箱

- **语言**：标准 **Lua 5.4**（`third_party/lua/`，5.4.6，MIT）。
- **可用标准库（白名单）**：`base`（裁剪）、`string`、`table`、`math`。
- **多文件**（v0.6.0，方案 A）：提供白名单 `include("sub.lua")`（`include "sub.lua"` 亦可，独占一行）——
  由控制器在**编译前展开**为单文件源（与 BASIC 的 `INCLUDE` 同机制，见 `10` §7.5；Kine-X 控制器脚本自身已按此拆分为 `kx_*.lua` 模块，见同节「模块布局」），仍禁用
  `require`/`dofile` 等动态加载；被包含文件必须是 `DEBUG_SCRIPT_DIR` 内同语言纯文件名，
  嵌套 ≤ 8、循环明确报错；行号按展开后的合并源计。
- **明确禁用**（`_G` 中被置 `nil`）：`dofile`、`loadfile`、`load`、`loadstring`、`require`、
  `os`、`io`、`debug`、`package`。
- 原则：**能对文件/系统/进程产生副作用的 API 一律不给**。

因 `os` 被禁用，Lua 脚本**没有 `os.time()`/`os.clock()`**；需要计时用设备命令或由宿主提供。

> **`print(...)` 是 Lua 原生函数**，直接写 stdout（终端），**不经** `host->print`。
> 这一点与 BASIC 的 `PRINT` 不同（BASIC 的 `PRINT` 走宿主的 `print` 回调，可被调试软件接管）。

---

## 3. 设备 API（与 BASIC 同名同义）

命令表被批量注册为**同名全局函数**；调用即转调 `MotionHost::call`，**失败一律 `error()` 抛出**（绝不静默）。

```lua
ENABLE();  DISABLE();  STOP()               -- 使能 / 去使能 / 停
MOVE(5);   MOVEABS(55, 100);  JOG(50)       -- 直动
local p = POS(0)                            -- 查询：直接“函数返回值”
local s = AXISSTATUS(0)
MODBUS_REG(4);  BUS();  BUSY()
```

### 3.1 命令函数清单（相对 BASIC 的差异）

**同名注册的全局命令**（与 BASIC 完全同名同义）：
`EN ENABLE DIS DISABLE STOP RAPIDSTOP MOVE MOVR MOVEREL MOVEABS MOVE_ABS JOG VJOG VMOVE
CANCEL HOME DATUM DELAY SLEEP WAIT WAITIDLE BASE AXIS SPEED ACCEL DECEL ATYPE UNITS JOGLEAD SRAMP FASTDEC VP_SPEED
DRIVE_PROFILE AXIS_ADDRESS DRIVE_CONTROLWORD POS MPOS DPOS AXISSTATUS IDLE ISIDLE BUSY
BUS BUSOK ENABLED ALARM DISABLE_GROUP SLOT_SCAN SCAN SLOT_START SLOT_STOP
BUSSTOP SCAN_EVENT NODE_COUNT NODE_AXIS_COUNT NODE_STATUS NODE_IO NODE_AIO NODE_INFO
ETHERCAT ECUSTOM ETH_MODE OPEN CLOSE PORT_STATUS PORT_TARGET PORT_MAX
MODBUS_REG MODBUS_IEEE NVSET NVGET RUNTASK STOPTASK PROC_STATUS SDO_WRITE TICKS`。

> **脉冲当量（v0.8.1）**：`UNITS(轴, v)`（inc/mm）为**运行时生效**参数——控制器不再固定/默认脉冲当量，
> 未设置时定位/点动/回零/置零明确报错；脚本开机用 `UNITS(0, PULSE_EQUIV)` 按伺服+机械设置。
>
> **点动前视（2026-09-27）**：`JOGLEAD(轴[, s])`（秒，运行时生效）——PP 目标跟随的前视 = `s + v²/(2a)`
> （范围 0.05~5s；**固件不设默认值**，未设置时 PP 点动明确拒绝）；过小会让驱动器每 20ms「减速-加速」追近目标
> （一顿一顿/顿挫）。脚本开机设 `JOGLEAD(0, JOG_LEAD_S)`；现场可用插件「设备命令」热调。
>
> **速度曲线（2026-09-27）**：`SRAMP(轴, 0~250ms)`（**仅 CSP**，0=梯形；PP 下明确提示不支持）、
> `FASTDEC(轴, mm/s²)`（CSP 解析减速停机；PP 写 6084）、`VP_SPEED(轴)`（当前速度 mm/s 只读）。
> 脚本 `SPEED/ACCEL/DECEL` 写值会同步下发内核缺省（MOVE/JOG 未显式给参时使用）。

**速度曲线 / 运动参数速查（用法示例，2026-09-27）**

| 命令 | 用法示例（Lua） | 说明 |
|---|---|---|
| `MOTION_MODE` | `MOTION_MODE(1)` | 0=PP（默认）/ 1=CSP（每拍规划，需 DC+UNITS）；**S 曲线仅 CSP 生效** |
| `SRAMP` | `SRAMP(0, 100)`（100ms）；`SRAMP(0, 0)` 回梯形 | S 曲线时间 0~250ms；PP 下明确提示不支持 |
| `FASTDEC` | `FASTDEC(0, 800)` | 停机减速度 mm/s²（0=未设置=旧行为）；CSP 解析减速停、PP 写 6084 |
| `VP_SPEED` | `print(VP_SPEED(0))` | 只读：当前速度 mm/s |
| `SPEED` / `ACCEL` / `DECEL` | `SPEED(0, 50)`、`ACCEL(0, 800)`、`DECEL(0, 650)` | 写值同步下发内核缺省（MOVE/JOG 未给参时使用；DECEL 亦作 PP 6084） |
| `JOGLEAD` | `JOGLEAD(0, 0.5)` | 点动前视（秒；实际 = 值 + v²/2a；未设置拒绝 PP 点动） |
| `MOVEABS` / `MOVE` | `MOVEABS(100, 50)`；`MOVE(10, 50)` | CSP 规划曲线（梯形/三角自动/S 曲线） |
| `JOG` / `VJOG` / `VMOVE` | `JOG(50)`；`VJOG(1)`；`VMOVE(-1)` | 点动（立即返回）；`CANCEL()` 停止 |
| `STOP` | `STOP()` | 减速停止（CSP+FASTDEC 时按设定减速度平缓停机） |


命令语义、参数、返回值、错误行为**全部对齐 BASIC**（见 `10` 第 3 节），要点：

- `MOVE` 的参数是**距离**（相对）；`MOVEABS` 是绝对目标；两者默认**阻塞到到位/出错**；
  第 4 参 `wait=0`（或 `false`）时**异步下发立即返回**——长驻端口服务脚本（如
  `EtherCAT_SocketServer.lua`）用它自己监测 `IDLE()`/`BUSY()` 完成状态。
- `TICKS()`：毫秒倒数计时（沙箱无 `os.time/os.clock` 时的唯一时间源，`t0 - TICKS()`=经过毫秒）。
- `IDLE` 返回 **-1=到位 / 0=运动中**（与 BASIC 一致，不“改良”）。
- `SPEED(n[,v])` 读/写轴参数；`DRIVE_CONTROLWORD` 仅接受 `128`/`6`/`15`。
- 轴号边界：带轴号的命令按轴选址生效，**越界轴号（≥ `AXIS_COUNT`）明确报错**（默认单轴配置下仅轴 0 合法）。
- `SDO_WRITE` 脚本层**不可用**，调用即报错。

**BASIC 语法糖 → Lua 显式函数**（因 BASIC 用 `#`/数组语法，无法字面同名，属**显式声明的差异**）：

| BASIC 形式 | Lua API | 说明 |
|------------|---------|------|
| `PRINT #n, s` | `PORT_PRINT(n, s)` | 整包发送，**不附加换行** |
| `PUTCHAR #n, 数组(起点,长度)` | `PORT_PUTCHAR(n, tbl_or_str[, start[, len]])` | 按原始字节发送（含 0 字节） |
| `GET #n, 数组, n` | `PORT_GET(n, tbl[, max])` | 非阻塞接收，返回字节数；内容写入 `tbl` |
| `OPEN #n, 类型, …` | `OPEN(n, type, …)` | 通道号显式传参 |
| `RUNTASK n, 子程序名` | `RUNTASK(n, fn)` | 只登记状态（单线程） |

> 端口**未 OPEN** 时报错；已 OPEN 但**未连接**时 `GET` 返回 0、`PORT_PRINT`/`PORT_PUTCHAR` 静默丢弃
> （与 ZBasic 一致，用 `PORT_STATUS(n)` 守卫并重连）。细节见 `port_manager.h`。
>
| `PORT_INFO` | `寄存器/网络` | 给端口打标签（用途/主从），供调试口 `conn`（D10 通讯状态）展示；用途 ≤48 字、主从 ≤24 字 |
>
> **端口编号语义（与 ZMC 不同）**：编号 0..上限-1 只是进程内**槽位句柄**——静态容量 64、运行期上限
> 默认 16（`PortManager::kDefaultMaxPorts`），可经调试口 **D9** `port.max.set` 调整并持久化 `.portmax`；
> 上限用引擎命令 `PORT_MAX()` 查询（返回 上限-1）；没有 ECUSTOM/「自定义网口只有 2 条」的限制，
> 不需要通道规划或抢占；重复 `OPEN` 同一编号 = 先关后开（重连惯例）。

### 3.2 `PORT_GET` / `PORT_PUTCHAR` 的字节表（**1 基**）

```lua
local buf = {}
local n = PORT_GET(10, buf, 64)     -- 收到 n 字节，写入 buf[1..n]（每个元素是 0..255）
if n > 0 then
    PORT_PUTCHAR(10, buf, 1, n)     -- 把 buf[1..n] 原样发回（含 0 字节）
end
```

- `PORT_GET(n, tbl[, max])`：默认最多收 `max` 字节（省略 `max` 视为 0，由宿主默认值兜底）；
  **写入 `tbl` 的下标从 1 开始**。
- `PORT_PUTCHAR(n, tbl|str[, start[, len]])`：表按 1 基取 `start`（默认 1）、`len`（默认 `#tbl`）；
  也可直接传字符串。

### 3.3 `ww` 命名空间（辅助入口）

`ww` 是**本项目自有前缀**（**不是**正运动的 `kx`），只放少量辅助入口：

| API | 说明 |
|-----|------|
| `ww.call(name, ...)` | **通用转调入口**：按字符串调用任意命令（逃生舱 / 动态命令名） |
| `ww.ret()` | 读**最近一次命令的返回值**（与 BASIC 的 `RETURN` 系统变量**同源同义**） |

```lua
local p = ww.call("POS", 0)      -- 等价 POS(0)
SLOT_SCAN(0)
if ww.ret() ~= 0 then            -- 判成败，等价 BASIC 的 IF RETURN THEN
    print("扫到从站")
end
```

> ⚠️ 除 `ww.call` / `ww.ret` 外，**没有其它 `ww.*` 函数**（例如没有 `ww.version`）。

---

## 4. 值 / 类型桥接

| 共享值 `kx::Value`（`script_value.h`） | Lua | 说明 |
|---------------------|-----|------|
| `Value::NUM` | `number` | 直接映射 |
| `Value::STR` | `string` | 直接映射（含 0 字节） |
| `Value::NIL` | `nil` | — |
| Lua `boolean` → 命令参数 | `1`/`0` | 传入设备命令时 `true`=1、`false`=0 |
| 一维数组 `A(i)`（0 基） | `table`（**1 基**） | 仅经 `PORT_*` 互通；下标基准不同（见下） |
| `RETURN` 系统变量 | `ww.ret()` + 命令函数返回值 | 二者取同一份「上一命令返回值」 |

**命令参数**仅接受 `number / string / boolean / nil`；其它类型（表/函数）传给设备命令会**明确报错**
（数组请走 `PORT_*`）。

### 4.1 三条**必须知道**的语义差异

1. **未赋值变量**：BASIC 数值变量默认 `0`；Lua 原生**未赋值 = `nil`**（同名**命令函数**语义对齐 BASIC，
   但**变量**遵循 Lua 原生语义）。
2. **数组/表下标**：BASIC 数组**0 基**；Lua 表**1 基**（`PORT_GET` 也是 1 基）。
3. **控制台输出**：BASIC 的 `PRINT` 走宿主回调；Lua 的 `print` 是标准库，直接写 stdout。

---

## 5. 运行与配置

### 5.1 dry-run 工具 `script_run`（离线，不需要硬件）

```bash
./build/script_run demo.lua              # .lua 自动选 Lua 引擎
./build/script_run demo.lua --engine lua # 显式指定引擎（basic | lua）
./build/script_run demo.lua --check      # 只做语法检查
./build/script_run demo.lua --trace      # 单步跟踪（Lua 行级）
./build/script_run demo.lua --steps N    # VM 指令级预算（默认 5,000,000）
./build/script_run demo.lua --quiet      # 不回显 dry 命令
./build/script_run demo.lua --sim-bus    # 模拟「总线已连上」
```

### 5.2 控制器进程内运行

与 BASIC 共用同一组配置键（见 `10` §4.2），仅 `SCRIPT_ENGINE` 需为 `lua`：

```ini
SCRIPT_ENABLE      = 1
SCRIPT_FILE        = /opt/kine-x/demo.lua
SCRIPT_ENGINE      = lua               # basic(默认) | lua
SCRIPT_WAIT_BUS    = 1
SCRIPT_WAIT_BUS_MS = 15000
SCRIPT_TRACE       = 0
SCRIPT_MAX_STEPS   = 5000000           # ⚠ Lua 侧是 VM 指令级，见下
```

```bash
./kine-x --script /opt/kine-x/demo.lua --script-engine lua --script-trace
```

> `script_loop` 先 `parse_script_language(SCRIPT_ENGINE)`（只允许 `basic|lua`，非法值**明确报错**并跳过），
> 再校验脚本扩展名与引擎一致，最后经 `ScriptEngineSlot::load()` 装载 `LuaEngine`；
> 同一时刻**只装载一个引擎**（已装载则拒绝再装，见 `09` §3.1）。

### 5.3 `SCRIPT_MAX_STEPS` 语义差异

| 引擎 | 预算粒度 | 说明 |
|------|----------|------|
| BASIC | **语句级** | 每条语句计 1 |
| Lua | **VM 指令级** | 每 100 条 VM 指令回调一次（hook），按周期近似统计 |

两者**复用同一配置键但数值不可直接比较**；极短脚本的 `steps()` 可能为 0 或偏小（仅用于预算与观测）。

---

## 6. 状态、错误与安全

### 6.1 结束状态映射（与 BASIC 一致，便于统一 UI）

| `IScriptEngine::Status` | Lua 触发条件 |
|--------------------------|--------------|
| `READY` | 已编译、尚未运行 |
| `DONE` | 脚本正常返回 |
| `COMPILE_ERROR` | `luaL_loadbufferx` 失败 |
| `RUNTIME_ERROR` | `lua_pcall` 失败（非中止类） |
| `ABORTED` | hook 检测到中止标志 → 抛中止 |
| `BUDGET_EXCEEDED` | hook 超指令预算 |

**错误行号**：chunk 名固定为 `kx_lua`（`=kx_lua`），错误消息形如 `kx_lua:12: ...`，
`error_line()` 据此解析出整数行号。

### 6.2 防死循环（双保险）

- `lua_sethook(L, hook, LUA_MASKCOUNT, 100)`：每 100 条 VM 指令回调一次；
  ① 累计超 `SCRIPT_MAX_STEPS` → 抛「预算耗尽」；② 检测 `abort` 标志 → 抛「中止」。
- 预算/中止用**不可打印前缀标记**（`\001zm:budget` / `\001zm:abort`）区分，避免与脚本自发 `error()` 混淆。

### 6.3 确定性与实时

- GC 采用**分代模式** `lua_gc(L, LUA_GCGEN, 0, 0)`（内置默认 minor 20% / major 100%）。
- **不承诺硬实时**：GC 停顿本质不确定；脚本运行在独立的**非 RT** 线程，绝不进 1ms 循环；
  运动时序由 RT 线程 + 参数信箱保证。

---

> **调试支持（v0.8.0）**：D5 断点/暂停/单步已在 Lua 引擎实装（`lua_sethook` **行级**事件 + 协作式挂起，
> 语义对齐 BASIC 的语句级 `dbg_gate`）：`add_breakpoint/request_pause/request_step/resume` 由会话线程调用，
> worker 在行事件挂起；`request_abort()` 会唤醒挂起中的 worker，保证关机/停止可退出。
> 行级 hook 只在调试通道装载的引擎上开启（`enable_line_hooks(true)`），开机生产脚本零额外开销。

## 7. C++ 集成 API（调试软件后端）

与 BASIC **完全同一套接口** `IScriptEngine`——调试软件只对接口编程，切引擎无需改代码：

```cpp
#include "script/lua_engine.h"      // LuaEngine
#include "script/engine.h"          // IScriptEngine
#include "script/motion_host.h"     // 真机设备宿主

std::unique_ptr<kx::IScriptEngine> eng(new kx::LuaEngine(&host));
std::string err;
if (!eng->compile(src, &err)) { /* 编译失败：err 带 kx_lua:行号 */ }
auto st = eng->run(5000000);        // VM 指令级预算
if (st != kx::IScriptEngine::Status::DONE)
    fprintf(stderr, "%s 第 %d 行: %s\n",
            kx::IScriptEngine::status_name(st), eng->error_line(), eng->error().c_str());
```

### 7.1 与 BASIC 的接口差异

| 能力 | BASIC（`ScriptEngine`） | Lua（`LuaEngine`） |
|------|--------------------------|---------------------|
| `name()` | `"basic"` | `"lua"` |
| `labels()` | 脚本标签表 | Lua **无标签** → 返回**已注册 API/命令名清单** |
| `set_trace` | 语句级 | `LUA_MASKLINE` **行级**（写 `host->print`） |
| `get_var`/`set_var`/`list_vars` | 符号表 | 遍历 `_G`（函数/表不列入「变量」） |
| `clear_vars` | 清空变量 | 只删**环境基线之外**的键，**保留**沙箱库与 API（`type`/`string`/命令函数等） |
| `steps()` | 语句数 | VM 指令数（近似） |

### 7.2 `list_vars()` 形态

```
POS_VAR = 3
NAME = "abc"
FLAG = true
```

数字/字符串/布尔会被列出；函数、表等不列出（避免把 API 函数当成“变量”）。

---

## 8. 与 BASIC 的对照（同名同义对齐）

| BASIC 形式 | Lua API | 对齐结果 |
|------------|---------|----------|
| `EN` / `DIS` | `ENABLE()` / `DISABLE()` | ✅ 同义（阻塞到使能完成） |
| `STOP` | `STOP()` | ✅ 同义 |
| `MOVEABS p[,spd]` | `MOVEABS(p[, spd])` | ✅ 同义 |
| `MOVE d` | `MOVE(d)` | ✅ 同义（**参数是距离**） |
| `JOG spd` / `VMOVE dir` / `CANCEL m` | 同名函数 | ✅ 同义 |
| `DATUM m` | `DATUM(m)` | ✅ 同义（0=清错 / 3=位置置零） |
| `POS` / `DPOS(n)` / `AXISSTATUS(n)` | 同名函数 | ✅ 同义 |
| `SPEED(n)[=v]` | `SPEED(n[, v])` | ✅ 同义（读/写） |
| `BUS`/`BUSY`/`IDLE` | 同名函数 | ✅ 同义（**`IDLE` 返 -1/0 照搬**） |
| `MODBUS_REG(n)` | `MODBUS_REG(n)` | ✅ 同义 |
| `NVSET(r, v)` / `NVGET(r)` | `NVSET(r, v)` / `NVGET(r)` | ✅ 同义（**Kine-X 增补**：4x 寄存器持久化，文件=脚本目录 `.nvram`；每写立即原子落盘） |
| `RETURN`（系统变量） | `ww.ret()` + 函数返回值 | ✅ 同源同义 |
| `PORT_STATUS(n)` | `PORT_STATUS(n)` | ✅ 同义 |
| `PRINT #n, s` | `PORT_PRINT(n, s)` | ⚠ 差异（语法糖→显式函数） |
| `GET #n, arr` | `PORT_GET(n, tbl)` | ⚠ 差异（语句+数组→函数） |
| `OPEN #n,...` | `OPEN(n, type, ...)` | ⚠ 差异（通道号显式传参） |
| `PUTCHAR #n, c` | `PORT_PUTCHAR(n, c)` | ⚠ 差异（语法糖→显式函数） |
| `RUNTASK n, sub` | `RUNTASK(n, fn)` | ⚠ 差异（子程序→函数值） |
| `SDO_WRITE(...)` | — | ⚠ 不提供（调用即明确报错） |

**同名同义验收**：同一段业务逻辑（`ENABLE → MOVEABS → POS → STOP`）用 BASIC 与 Lua 各写一遍，
**读数必须一致**；不一致即视为「同名不同义」缺陷。

---

## 9. 完整示例

```lua
-- server.lua —— TCP 服务端 + 总线初始化 + 定位，演示端口 IO 与 ww.ret
-- 端口编号（0..15）只是句柄，无 ZMC 式通道规划；上限可用引擎命令 PORT_MAX() 查询
OPEN(10, "TCP_SERVER", 4321)          -- 监听 4321（端口号按实际修改）
if PORT_STATUS(10) == 0 then
    print("等待客户端接入…")
end

-- 总线初始化：扫描 -> 判成败 -> 开总线
if SLOT_SCAN(0) == 0 then
    print("未扫到从站，退出")
    return
end
print("扫到从站数=" .. ww.ret())       -- 等价 BASIC 的 RETURN

if SLOT_START(0) ~= 0 then
    print("总线已开启")
end

DATUM(0)                               -- 清错
ENABLE()                               -- 使能
MOVEABS(55, 100, 1000)
print("位置=" .. POS(0))

-- 回显：把收到的字节整包发回（含 0 字节用 PORT_PUTCHAR）
local buf = {}
while true do
    local n = PORT_GET(10, buf, 64)
    if n > 0 then
        PORT_PUTCHAR(10, buf, 1, n)
    end
    DELAY(20)
end
```

---

## 10. 限制与后续

| 限制 | 说明 / 后续 |
|------|-------------|
| 无 `os`/`io`/`debug`/`package`/`load` | 白名单沙箱；无文件 IO、无动态加载、无 `os.exit` |
| 无 `os.time`/`os.clock` | 需计时用设备命令或由宿主提供 |
| `print` 不经宿主 | 直接写 stdout；调试软件若要接管输出需改用宿主接口（后续可补 `LOG` 命令） |
| 与 BASIC **互斥**（规则，已强制） | 控制器同一时刻只加载一种脚本语言；`ScriptEngineSlot` 只装一个引擎（见 `09` §3.1） |
| 不承诺硬实时 | GC 停顿不确定；脚本跑在非 RT 线程 |
| `steps()` 为近似值 | VM 指令级，按 100 周期统计 |
| `labels()` 非「标签」 | Lua 无标签，返回已注册 API/命令名清单 |

**测试**：

```bash
cd lubancat2/build && cmake .. && make -j4
ctest -R 'lua_engine_test|script_test|config_test|engine_rule_test' --output-on-failure
```

`lua_engine_test`（8 组，假宿主）覆盖：命令调用 / 表桥接（`PORT_*`）/ 错误行号 / 预算 / 中止 /
沙箱禁用；与 `script_test` 一样**不需要 IgH 与硬件**。
