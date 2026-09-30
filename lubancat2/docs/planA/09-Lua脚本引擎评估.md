# 09 Lua 脚本引擎（评估 + 任务分解）

> 目标：在现有 **ZBasic 子集引擎**（`08`）之外，**并行**引入一个**独立的 Lua 脚本引擎**
> （标准 Lua 语法 + 本项目自定义 API），两套引擎**并存、由用户二选一**。
>
> 定位：**可选增强层**，非核心运行路径。**不替代、不改动**现有 BASIC 引擎；1:1 替换
> ZMC 控制器功能由 `motiond`（运动内核）与**脚本层**承担（原 4 个原生服务已从工程移除，见 `12`）。
>
> 不变量（与整套方案一致）：
> - **不碰 EtherCAT / RT 线程**：设备命令全部经 `kx::Shared` 下发，走与内核一致的运动语义；
> - **纯逻辑可单测**：假 Host，不需要 IgH 与硬件即可回归；
> - **绝不死循环**：指令级预算 + 中止回调双保险（Lua 用 `lua_sethook` 实现）；
> - **沙箱化**：默认禁用文件/系统/动态加载类标准库；
> - **绝不静默**：未绑定命令、越权操作、非法参数一律**明确报错**，不退化为静默行为。

> 📘 面向**脚本作者 / 集成开发者**的 Lua API 参考与开发手册见
> [`11-Lua脚本API与开发指南.md`](11-Lua脚本API与开发指南.md)；本文侧重**评估、决策与任务分解**。

---

## 0. 结论摘要

**可行，中等工作量（M）、低风险。** 核心依据：现有引擎的**设备宿主已与语言解耦**，
Lua 版可直接复用，**设备逻辑零重复**。

| 维度 | 评估 |
|------|------|
| 复用度 | **高**：`ScriptHost` 接口 + `MotionHost` 全套设备命令可直接复用 |
| 集成改动 | **小**：启动点单一（`script_loop`），配置加一个选择键 |
| 新增工作量 | **中**：Lua 依赖 + 绑定层 + 沙箱 + 预算 hook + 单测 + 文档 |
| 风险 | **低**：不碰 RT/EtherCAT；主要风险是"双语言双维护"与"API 语义取舍" |
| 依赖 | Lua 5.4（MIT，纯 C）；本机**当前无 Lua**，建议 **vendor 源码** |

> **实施状态**：决策 D-1/D-2/D-3 已定案并落码，**代码侧已全部完成**——
> `third_party/lua/`（5.4.6）、`src/script/{engine.h,command_table.h,lua_engine.{h,cpp}}`、
> 配置 `SCRIPT_ENGINE`、`script_loop` 分支、`--script-engine` CLI、`tools/lua_engine_test.cpp` 均已就位；
> `lua_engine_test`（8 组）与 `script_test` **全绿**。仅剩**实机对拍 L-19**（需硬件）。

---

## 1. 需求确认（用户诉求）

| # | 诉求 | 对应设计 |
|---|------|----------|
| R1 | 加一个**独立** Lua 引擎，**不考虑兼容 `.bas`** | 新增 `LuaEngine`，与 `BasicEngine` 并行 |
| R2 | 使用**标准 Lua 语言格式** | 标准 Lua 5.4 语法；官方解释器 |
| R3 | 视情况**删减原生库** | 白名单沙箱（见 §3.5） |
| R4 | 提供**本项目自定义 API / 语法** | 绑定层把 `MotionHost` 命令暴露为 Lua API（见 §3.3） |
| R5 | **用户可选择**用哪个脚本 | 配置 `SCRIPT_ENGINE = basic \| lua`（见 §3.7） |

---

## 2. 现状与复用基础

### 2.1 现有资产

| 文件 | 作用 | Lua 方案的用法 |
|------|------|----------------|
| `src/script/script.h` / `script.cpp` | BASIC 引擎（词法/语法/AST/解释执行） | 作为 `BasicEngine` 实现；不动 |
| `src/script/script_host.h` 的 `struct ScriptHost` | **语言无关**的宿主接口（已从 `script.h` 拆出） | Lua 绑定层的**唯一对接点** |
| `src/script/script_value.h` 的 `struct Value` | **语言无关**的值类型（NUM/STR/NIL，已从 `script.h` 拆出） | Lua 值桥接的输入/输出类型 |
| `src/script/motion_host.h` / `motion_host.cpp` | 设备宿主：运动/端口/Modbus/总线/寄存器命令 | **整套复用**，不改语义 |
| `src/script/port_manager.h` / `port_manager.cpp` | `OPEN #n` → 真实 TCP 通道 | 复用（由 `MotionHost` 持有） |
| `src/common/config.h` 的 `ScriptCfg` | 脚本配置项 | 扩展 `SCRIPT_ENGINE` 等 |
| `src/main.cpp` 的 `script_loop()` | 引擎启动点 | 加 Lua 分支 |
| `CMakeLists.txt` | 构建 | 加 Lua 目标/依赖 |
| `tools/script_test.cpp` / `motion_host_test.cpp` | 引擎/宿主的假 Host 单测 | 作为 `lua_engine_test` 模板 |

### 2.2 关键复用点（宿主已解耦）

`ScriptHost` 只认「命令名 + 值数组 + 返回值 + 错误」，**与调用它的语言无关**：

```
ScriptHost::call(name, args, ret, err)
   ↑ 语言无关
   ├── BasicEngine  → 解析 "MOVEABS(10)" → call("MOVEABS", {10}, ...)
   └── LuaEngine    → 解析 "MOVEABS(10)" → call("MOVEABS", {10}, ...)   ← 新增
                                                        │
                                                        ▼
                                                   MotionHost ──▶ kx::Shared → motiond / EtherCAT
```

**结论：Lua 绑定层不需要知道任何运动/协议细节，只做"参数转换 + 调用 + 结果回填"。**

### 2.3 启动点现状

```cpp
// src/main.cpp  现有逻辑（简化）
void script_loop(Shared& sh, const AppConfig& cfg, std::atomic<bool>& running) {
    /* 1) 可选等总线就绪  2) 读文件
       3) MotionHost host(sh, hcfg); ScriptEngine eng(&host);
          eng.compile(...); eng.run(max_steps); */
}
```

Lua 分支只需在此处按 `cfg.script.engine` 选择实现即可。

---

## 3. 方案设计

### 3.1 总体架构

```
                      ┌────────────────────── kine-x（单进程）──────────────────────┐
 config/app.conf ───▶ │  main.cpp                                                    │
   SCRIPT_ENGINE=     │    └─ script_loop(...)      ← 按 SCRIPT_ENGINE 二选一（互斥）  │
   basic | lua        │         │                                                    │
   SCRIPT_FILE=       │         ├─ BasicEngine  ( src/script/script.cpp ，现状 )      │
   *.bas / *.lua      │         └─ LuaEngine    ( 新增 src/script/lua_engine.* )     │
                      │                   │              │                           │
                      │          ┌────────┴──────────────┘                          │
                      │          ▼   IScriptEngine（统一接口，见 §3.2）              │
                      │      ScriptHost::call(name,args,ret,err)  ← 语言无关宿主     │
                      │          │                                                  │
                      │      MotionHost ( src/script/motion_host.cpp ，复用不改 )     │
                      │          │  运动 / 端口 / Modbus / 总线 / 寄存器             │
                      │          ▼                                                  │
                      │      kx::Shared ──▶ motiond / EtherCAT（不碰 RT）            │
                      └────────────────────────────────────────────────────────────┘
```

> ⚠ **互斥（规则，已强制）**：两个引擎共用同一个 `kx::Shared`，**同一时刻只能跑一个引擎线程**，
> 否则命令相互竞争（单槽命令会被覆盖）。"用户可选" = 配置二选一，**不是同时运行**。
>
> 📌 该规则（**控制器同一时刻只加载一种脚本语言**）已落到 `src/script/engine_rule.h`：
>   * `parse_script_language()` 归一/校验 `SCRIPT_ENGINE`（大小写不敏感；非法值明确拒绝，绝不静默）；
>   * `script_file_matches_language()` 校验脚本扩展名与引擎一致（`.lua`↔lua、`.bas`↔basic）；
>   * `ScriptEngineSlot` 是**唯一创建引擎的地方**，已装载时拒绝再装（含同语言重复装载），
>     必须先 `unload()`——从代码上保证"同一时刻只装一种脚本语言"。
>   自测见 `tools/engine_rule_test.cpp`（`engine_rule_test`，纯逻辑）。

### 3.2 引擎抽象接口 `IScriptEngine`

抽取统一接口，让调试软件**只对接口编程**，两种语言共用一套调试入口：

| 方法 | 说明 | BASIC（现状） | Lua 对应实现 |
|------|------|---------------|--------------|
| `compile(src, err)` | 编译，失败带行号/原因 | 词法+语法 | `luaL_loadbuffer` |
| `run(max_steps)` | 执行，返回终态 | 解释执行 | `lua_pcall` + hook 预算 |
| `status()` / `status_name()` | 终态枚举 | 已有 | 映射（见下表） |
| `error()` / `error_line()` | 错误文本/行号 | 已有 | 从 Lua 消息解析 `file:line` |
| `request_abort()` | 线程安全中止 | 置位 | 置位 |
| `get/set/list/clear_var` | 变量内省 | 符号表 | 遍历 `_ENV`/`_G` |
| `labels()` | 标签/断点列表 | 标签表 | Lua 无标签；返回函数/行号表（见 §3.8） |
| `set_trace()/trace()` | 单步跟踪 | 已有 | `lua_sethook(LUA_MASKLINE)` |
| `steps()` | 已执行步数 | 已有 | hook 计数 |

**状态枚举映射**（与 BASIC 一致，便于统一 UI）：

| `IScriptEngine::Status` | BASIC 触发 | Lua 触发 |
|--------------------------|-----------|----------|
| `READY` | 已编译未跑 | 已编译未跑 |
| `DONE` | `END`/预算耗尽 | 脚本正常返回 |
| `COMPILE_ERROR` | 语法错 | `luaL_loadbuffer` 失败 |
| `RUNTIME_ERROR` | 运行期错误 | `lua_pcall` 非 `OK`（非中止类） |
| `ABORTED` | 外部中止 | hook 检测到 abort → 抛中止 |
| `BUDGET_EXCEEDED` | 超步数 | hook 超指令预算 |

### 3.3 绑定层设计（D-2 已定案：② 扁平全局 + 与 BASIC 同名）

> **决策（D-2=②）**：Lua API 采用**扁平全局函数**，且**命令名与 BASIC 完全同名**；
> **同名必须同义**——凡与 BASIC 共享名字的命令，其参数含义、返回值含义、错误行为
> 一律**对齐 BASIC 现状**（见 §10 的逐条对齐表与 §5 风险 #2）。
> 命名空间前缀统一为 **`ww`**（本项目自有品牌，**不是** `kx`/正运动），仅用于**少数
> 通用/辅助入口**，避免污染 `_G`；命令函数本身直接注册为全局，不加前缀。

**主形态 — 命令表驱动的同名全局函数**：

```lua
ENABLE();  DISABLE();  STOP()
MOVEABS(10, 200);  MOVE(-5);  JOG(50)
local p = POS(0);  local s = AXISSTATUS(0)
OPEN(ch, "TCP_SERVER", 4321);  PORT_PRINT(ch, "hello");  local n = PORT_GET(ch, buf)
MODBUS_REG(0);  BUS();  BUSY()
```

因**命令表本就集中在 `motion_host.cpp`**，用「表驱动 + 宏」批量注册，避免手写上百个绑定：

| 来源 | 注册方式 |
|------|----------|
| `motion_host.cpp` 的 `name == "XXX"` 分支 | 抽出一份**命令名清单**（常量表），循环注册**同名** Lua 全局函数，统一转调 `MotionHost::call` |
| 需特殊处理的（`OPEN`/`PRINT #`/`GET #`/`PUTCHAR #`/`RUNTASK`） | 单独手写绑定（见下方备注） |

**辅助形态 — `ww` 命名空间（少量通用入口）**：

```lua
local p, err = ww.call("POS", 0)   -- 通用转调入口（逃生舱：调未具名命令/内部自查）
ww.version()                       -- 引擎/固件版本
```

`ww.call` 仅作为**通用入口 + 未具名命令的兜底**，不替代同名函数；同名函数的语义以 BASIC 为准。

绑定实现：Lua 参数（number/string/table）→ `std::vector<Value>` → `MotionHost::call` → `Value` → Lua。

> 📌 **模块解耦（已落地）**：`Value` 与 `ScriptHost` 原先定义在 `script.h`（BASIC 引擎的头）里，
> 使得 Lua 引擎为了取这两个类型而被迫 include「BASIC 的头」。现已**拆分**为两个语言中立头：
> `src/script/script_value.h`（`Value`）与 `src/script/script_host.h`（`ScriptHost`）；
> `script.h` 只保留 BASIC 的 `ScriptEngine` 并 include 上述两表头。于是 `lua_engine.cpp` /
> `motion_host.h` / `lua_engine_test.cpp` **不再依赖 `script.h`**，两套引擎在实现与链接层面彻底独立
> （仅经这两个中立头共享数据面；`engine_rule.cpp` 作为工厂是唯一同时聚合两者的点）。

> 备注：BASIC 的 `PRINT #n` / `GET #n` 依赖「通道 + 数组」语法糖；Lua 版改为**显式函数**：
> `OPEN(ch, type, ...)` / `PORT_PRINT(ch, "text")` / `PORT_GET(ch, table)`，并以 `PORT_STATUS(ch)` 作守卫。
> 这几处**名字无法与 BASIC 字面一致**（BASIC 是语句+语法糖），属**显式声明的必要差异**，须在 §10 标注。

### 3.4 值 / 类型桥接

| 共享值 `kx::Value`（`script_value.h`） | Lua | 说明 |
|---------------------|-----|------|
| `Value::NUM` | `number` | 直接映射 |
| `Value::STR` | `string` | 直接映射 |
| 一维数组 `A(i)` | `table`（1 基下标） | BASIC 无边界、自动扩容；Lua table 天然动态 |
| `RETURN` 系统变量 | **同名 `ww.ret()` 读取 + 函数返回值** | D-2 要求同名同义：BASIC 用全局 `RETURN` 承载命令返回值，Lua 侧保留 `ww.ret()` 读取最近一次命令返回值，**同时**命令函数本身返回该值（便于 Lua 习惯写法）；两者取同一份数据 |
| 未赋值数值默认 0 | Lua `nil` | **已知语义差异**：BASIC 符号默认 0，Lua 原生未赋值 = `nil`。同名**命令函数**语义对齐 BASIC；**变量**语义遵循 Lua 原生，须在 §5/§10 显式标注 |

### 3.5 沙箱与安全策略（白名单）

只用官方 API + `luaL_requiref` / 手工注册，**默认白名单**：

| 库 / 函数 | 默认 | 说明 |
|-----------|------|------|
| `base` | 部分 | 保留 `print/type/tonumber/tostring/pairs/ipairs/next/select/pcall/error/assert/rawget/rawset`；**移除 `dofile/loadfile/load/loadstring/require`**（`collectgarbage` 视需要保留） |
| `string` | 保留 | 文本处理 |
| `table` | 保留 | 数据结构 |
| `math` | 保留 | 数学 |
| `coroutine` | 待定 | 若开启需确认与 hook 预算的交互（协程切换仍受同一 hook） |
| `os` | **禁用**（可选仅放 `os.clock`/`os.time`） | **必须移除 `os.exit`/`os.execute`/`os.remove`/`os.rename`** |
| `io` | **禁用** | 脚本不允许文件 IO |
| `debug` | **禁用** | 可被用作逃逸沙箱 |
| `package` / `require` | **禁用** | 不允许加载外部模块 |

> 原则：**能对文件/系统/进程产生副作用的 API 一律不给**；确有需要（如计时）单独按函数白名单放行。

### 3.6 预算与中止（对齐"绝不死循环"不变量）

- 用 `lua_sethook(L, hook, LUA_MASKCOUNT, N)`；**实现取 `N = 100`**（原规划建议 10 000，
  但 10 000 粒度太粗会让短脚本 `steps()` 恒为 0；100 兼顾预算精度与开销——脚本跑在
  **非 RT 线程**，hook 开销不影响 1ms 实时循环）；
- 回调里：① 累计指令数，超 `SCRIPT_MAX_STEPS` → 抛中止错误；② 检查 abort 标志 → 抛中止错误；
- `lua_pcall` 捕获错误后，按约定的错误标记/消息区分「预算耗尽」「外部中止」「真运行错误」，
  映射到 §3.2 的状态枚举（实现用不可打印前缀标记：`\001zm:budget` / `\001zm:abort`）；
- **`steps()` 按 hook 周期近似统计**（粒度 = N），故极短脚本可能报 0 或偏小；仅用于预算与观测，
  不承诺精确指令数；
- **BASIC 侧 `max_steps` 是"语句级"**，Lua 侧是"VM 指令级"，数值不可直接比较，文档需注明换算关系。

### 3.7 配置与启动

在 `ScriptCfg` / `config.cpp` / `config/app.conf` 增加选择项：

| 键 | 类型 | 默认 | 说明 |
|----|------|------|------|
| `SCRIPT_ENGINE` | 字符串 | `basic` | `basic`=现有 ZBasic 引擎；`lua`=新 Lua 引擎；**只允许这两个值**（空=basic），非法值载入时明确报错 |
| `SCRIPT_FILE` | 字符串 | 空 | 复用；`basic` 期望 `.bas`，`lua` 期望 `.lua` |
| `SCRIPT_WAIT_BUS` / `_MS` | bool / int | 1 / 15000 | 复用 |
| `SCRIPT_TRACE` | bool | 0 | 复用（Lua 侧 = line hook） |
| `SCRIPT_MAX_STEPS` | int | 5000000 | 复用（语义见 §3.6） |

CLI 覆盖：`--script-engine lua`（与 `--script <file>` 配合）✅ 已实现（`src/main.cpp`）。
`tools/script_run.cpp` 亦支持：`--engine basic|lua`，且 `.lua` 扩展名自动选 Lua ✅。

启动分支（`script_loop`）：

```
SCRIPT_ENGINE → parse_script_language()（只允许 basic | lua）
   ├─ script_file_matches_language()（扩展名 .lua/.bas 必须与引擎一致，否则明确报错）
   └─ ScriptEngineSlot::load(lang, host)  ← 唯一创建引擎处
          basic → ScriptEngine（现状）
          lua   → LuaEngine（装载沙箱 → 注册 API → luaL_loadbuffer → run）
同一进程内只启动一个引擎线程（互斥，槽已装载则拒绝再装）
```

### 3.8 调试接口对齐（供调试软件）

| 能力 | BASIC（`08` §4.3） | Lua 实现 | 差异 |
|------|--------------------|----------|------|
| 单步/跟踪 | 语句级 | `lua_sethook(LUA_MASKLINE)` 行级 | Lua 为"行级" |
| 断点 | 标签 | 行号断点（line hook 命中即停） | BASIC 用标签，Lua 用行号 |
| 变量窗口 | `get/set/list_vars`（符号表） | 遍历 `_ENV`/`_G`（过滤内部键） | 值是 Lua 类型，需序列化 |
| 中止 | `request_abort` | 置位 + hook 检查 | 一致 |
| 错误行号 | `error_line()` | 从 `file:line:` 解析 | 需归一为 int |

---

## 4. 依赖与构建

### 4.1 依赖现状（已实测）

| 检查 | 结果 |
|------|------|
| `pkg-config --exists lua5.4` / `lua5.3` | **no** |
| `/usr/include/lua*` 头文件 | **无** |
| `lua` / `lua5.4` / `luajit` 可执行 | **无** |

### 4.2 两种来源

| 方案 | 优点 | 缺点 | 建议 |
|------|------|------|------|
| **A. vendor Lua 5.4 源码**（`third_party/lua/`，保留 MIT LICENSE） | 无外部依赖；交叉/原机都易编；版本锁定 | 仓库多 ~300KB 源码 | **✅ 推荐** |
| **B. 链接系统 `liblua5.4-dev`** | 仓库干净 | 依赖目标板 apt；交叉需 arm64 包 | 备选 |

### 4.3 CMake 变更点（✅ 已落地）

- 新增 `lua_engine_test`（假 Host，纯逻辑，进 `ctest`）✅；
- `kine-x` 主目标增加 `src/script/lua_engine.cpp` 与 Lua 运行库（`target_link_libraries(... lua)`）✅；
- vendor 方式：`add_library(lua STATIC ${LUA_SRC})`，`LUA_SRC=GLOB third_party/lua/*.c`，`target_link_libraries(lua PUBLIC m)` ✅；
- `script_run` 亦链接 Lua（双引擎 CLI）✅；
- 板端无 cmake：`deploy/36-build-m2.sh` 用 `gcc -c` + `ar` 现场编 `liblua_zm.a` 并链接 ✅。

---

## 5. 风险与权衡

| # | 风险 | 影响 | 缓解 |
|---|------|------|------|
| 1 | **双语言 = 长期双维护** | 命令表/文档要同步两份 | 设备宿主只有一份（`MotionHost`）；差异仅在"语言前端 + 绑定 + 文档" |
| 2 | **API 语义陷阱（同名同义约束）** | 既定案 **D-2=②：与 BASIC 同名**，则必须**同义**——BASIC 怪癖（`RETURN` 系统变量、`MOVE` 是距离、`IDLE=-1`、数组无边界）**不得在 Lua 侧被"改良"成另一套含义** | 由 **§10 逐条对齐表**逐命令核对；无法同名的（`PRINT #n`/`GET #n` 等语法糖）**显式列为差异**并在文档标注；同义项写进 `lua_engine_test` 用例 |
| 3 | **确定性 / 实时** | Lua 有 GC 停顿 | 脚本跑在独立业务线程（**非 RT**），影响有限；**已定案（L-11）**：GC 用**分代模式** `lua_gc(L, LUA_GCGEN, 0, 0)`（内置默认 minor 20% / major 100%），并明确**"不承诺硬实时"**；运动时序由 RT 线程 + 参数信箱保证，脚本绝不进 1ms 循环 |
| 4 | **并发约束** | 两引擎共用 `kx::Shared`，可竞争 | **实现为互斥选择**，同一时刻只跑一个引擎线程；不设计"同时运行" |
| 5 | **安全面扩大** | Lua 能力强（表/闭包/元表），误用面大 | 白名单沙箱 + 指令预算 hook |
| 6 | **许可证** | — | Lua 为 MIT，商用无忧 |
| 7 | **调试语义差异** | BASIC 标签断点 vs Lua 行号断点 | 统一走 `IScriptEngine`，UI 按引擎类型适配 |

---

## 6. 工作量与成本估算

> 量级：S（< 0.5 人日）/ M（0.5~2 人日）/ L（> 2 人日），为粗估。

| 阶段 | 内容 | 量级 |
|------|------|------|
| 依赖接入 | vendor Lua + CMake | S |
| 引擎抽象 | `IScriptEngine` + `BasicEngine` 适配 | S |
| Lua 引擎骨架 | compile/run/status/error | M |
| 绑定层 | v1 通用 + v2 命令表 | M |
| 沙箱 + 预算 hook | 白名单 + `lua_sethook` | M |
| 调试接口 | trace/变量/行号 | M |
| 集成 | 配置 + CLI + `script_loop` | S |
| 测试 + 文档 | `lua_engine_test` + 本文档定稿 | M |
| **合计** | — | **M（中等，低风险）** |

---

## 7. MVP 落地路径（里程碑 M8，建议）

1. vendor Lua 5.4 + CMake 接入（**L-01/02**）✅；
2. 抽 `IScriptEngine`，`ScriptEngine` 适配、回归全绿（**L-03**）✅；
3. `LuaEngine` 骨架 + 值桥接（**L-04/05**）✅；
4. 绑定层：命令表批量注册**同名全局函数**（`MOVEABS/POS/…`）+ `ww.call` 通用入口（**L-06/07**）✅；
5. 沙箱白名单 + count hook 预算/中止（**L-09/10**）✅；
6. 配置 `SCRIPT_ENGINE` + `script_loop` 分支（**L-12/13**）✅；
7. **端到端验证**：`--script-engine lua` 跑 `ENABLE → MOVEABS → POS → STOP`，
   与 4321 同目标位置读数比对（**L-19**）⏳ 待实机。BASIC 版同义脚本读数应**一致**（同名同义验收）；
8. 补 `lua_engine_test` + 本文档定稿（**L-18/20**）✅。

> **代码侧 MVP 已闭环**（步骤 1–6、8 完成）：`lua_engine_test` 8 组用例 **ALL PASS**，
> BASIC 引擎 `script_test` **ALL PASS**（无回归）。仅剩步骤 7（**L-19 实机对拍**，需硬件）。

MVP 完成后，再做批 2（绑定 v2 全命令表、调试接口完善、GC 调优）。

---

## 8. 任务分解

> 说明：**本节是上面评估解析出的可执行任务清单**。状态图例：
> `✅ 完成` / `◐ 部分` / `⏳ 待开工`。量级 S/M/L 见 §6；批次：`批1`=MVP 必须，`批2`=增强。
> 当前进度：**L-01~L-18、L-20 全部完成**；仅剩 **L-19（实机端到端对拍，需硬件）**。

### 8.1 任务总表

| ID | 任务 | 产出 / 触点 | 验收标准 | 依赖 | 量级 | 批次 | 状态 |
|----|------|-------------|----------|------|------|------|------|
| **L-01** | vendor Lua 5.4 源码入仓 | `third_party/lua/`（含 `LICENSE`=MIT） | 源码就位、版本标注 5.4.x | — | S | 批1 | ✅ 完成（5.4.6，61 文件 + LICENSE + README.kine-x.md） |
| **L-02** | CMake 集成 Lua | `CMakeLists.txt`（`add_library(lua STATIC ...)`） | 纯逻辑目标可编；不破坏现有目标 | L-01 | S | 批1 | ✅ 完成（`project(... C CXX)`、`GLOB third_party/lua/*.c`、链 `m`） |
| **L-03** | 抽 `IScriptEngine` 接口 + BASIC 适配 | 新 `src/script/engine.h`；`script.h`/`script.cpp` 适配 | `script_test`/`motion_host_test` 全绿、行为不变 | — | S | 批1 | ✅ 完成（`script_test` ALL PASS，无回归） |
| **L-04** | `LuaEngine` 骨架 | `src/script/lua_engine.h`/`.cpp` | `compile/run/status/error/error_line` 可用；状态映射对齐 §3.2 | L-02,L-03 | M | 批1 | ✅ 完成 |
| **L-05** | 值桥接 `Value` ↔ Lua | `lua_engine.cpp` 转换函数 | number/string/table 互转正确；含表→数组 | L-04 | S | 批1 | ✅ 完成（bool→1/0；表经 `PORT_*`） |
| **L-06** | 绑定层通用入口 `ww.call` | `lua_engine.cpp` 注册 `ww.call` / `ww.ret` | Lua 调 `ww.call("POS",0)` 得值；未绑定命令明确报错 | L-04,L-05 | S | 批1 | ✅ 完成 |
| **L-07** | 绑定层：命令表批量注册**同名全局函数** | `command_table.h` 导出命令名清单；批量注册 Lua 全局函数 | `MOVEABS/POS/SPEED…` 可独立调用；**名字与语义对齐 BASIC**；清单与 `MotionHost` 分支一致 | L-06 | M | 批1 | ✅ 完成（单一事实来源 `motion_command_names()`） |
| **L-08** | 命令失败语义定案与实现 | `lua_engine.cpp`（`return nil, err` 或 `error()`） | 文档记录取舍；失败**不静默** | D-2 | S | 批2 | ✅ 完成（一律 `luaL_error`，取 `error()`；§3.3 记录） |
| **L-09** | 沙箱白名单 | `lua_engine.cpp`（`luaL_requiref`/手工注册） | `io/os/debug/package/load/dofile` 均不可用；`os.exit` 必禁 | L-04 | M | 批1 | ✅ 完成（保留 base/string/table/math） |
| **L-10** | count hook 预算 + 中止 | `lua_engine.cpp`（`lua_sethook`） | 死循环脚本被 `BUDGET_EXCEEDED` 终止；abort 标志生效 | L-04 | M | 批1 | ✅ 完成（`\001zm:budget` / `\001zm:abort` 标记） |
| **L-11** | GC / 确定性参数与说明 | `lua_engine.cpp` + 文档 | GC 参数选定；文档标注"不承诺硬实时" | L-04 | S | 批2 | ✅ 完成（`LUA_GCGEN` 分代模式 + §5#3 说明） |
| **L-12** | 配置项 `SCRIPT_ENGINE` | `config.h`/`config.cpp`/`config/app.conf` | 解析 `basic\|lua`，非法值明确报错；默认 `basic` | — | S | 批1 | ✅ 完成（`config.cpp` 载入时即校验并规范化为小写，非法值报错；见 `engine_rule.h`） |
| **L-13** | `script_loop` 引擎选择分支 | `src/main.cpp` | 按配置选引擎；**同一时刻仅一个引擎线程** | L-04,L-12 | S | 批1 | ✅ 完成（经 `ScriptEngineSlot` 唯一装载；已装载则拒绝再装） |
| **L-14** | CLI `--script-engine` + `script_run` | `src/main.cpp`；`tools/script_run.cpp` | CLI 覆盖配置；`script_run` 支持 lua 语法检查（可选） | L-13 | S | 批2 | ✅ 完成（main `--script-engine`；script_run `--engine` + `.lua` 自动识别） |
| **L-15** | trace（line hook）对齐 | `lua_engine.cpp` | `set_trace(true)` 逐行打印 | L-04 | S | 批2 | ✅ 完成（`LUA_MASKLINE` → `host->print`） |
| **L-16** | 变量内省 `get/set/list_var` | `lua_engine.cpp`（遍历 `_ENV`） | 与 `IScriptEngine` 语义一致；过滤内部键 | L-04 | M | 批2 | ✅ 完成（`clear_vars` 按环境基线快照保留库/API） |
| **L-17** | 错误行号归一 | `lua_engine.cpp` | Lua `file:line:` → `error_line()` 返回 int | L-04 | S | 批2 | ✅ 完成（chunk 名 `=kx_lua` 保证前缀可解析） |
| **L-18** | `lua_engine_test`（假 Host 单测） | `tools/lua_engine_test.cpp` + CMake | 覆盖：命令调用/table/错误行号/预算/中止/沙箱禁用；进 `ctest` | L-04,L-06,L-09,L-10 | M | 批1 | ✅ 完成（8 组、ALL PASS；已进 `ctest`） |
| **L-19** | 端到端对拍 | 实机/仿真 | `--script-engine lua` 跑 `ENABLE→MOVEABS→POS→STOP`，与 4321 同目标读数一致 | L-13 | M | 批1 | ⏳ 待开工（需实机） |
| **L-20** | 文档定稿 + 索引/里程碑同步 | 本文档 + `docs/planA/README.md` | 本文档完成；README 文档索引/里程碑含 Lua 引擎项 | L-19 | S | 批2 | ◐ 部分（本文档已同步；README M8 已加） |
| **L-21** | **强制"同一时刻只加载一种脚本语言"规则** | `src/script/engine_rule.h`/`.cpp`；`config.cpp`/`main.cpp`/`script_run.cpp` | 单引擎槽只装一个引擎；引擎与脚本扩展名一致；非法值/混用明确报错 | L-12,L-13 | S | 批2 | ✅ 完成（`engine_rule_test` 全绿） |

### 8.2 前置决策（✅ 已定案，本轮拍板）

| ID | 决策 | 选项 | 结论 | 依据 / 影响 |
|----|------|------|------|-------------|
| **D-1** | Lua 依赖来源 | A. vendor 源码 / B. 系统 `liblua5.4-dev` | **✅ A（vendor Lua 5.4 源码）** | 目标板无 cmake、无 `liblua5.4-dev`，源码 push 后板上裸 `g++` 直编；版本锁定、零外部依赖 → 解阻塞 L-02 |
| **D-2** | Lua API 风格 | ① 仅通用 `ww.call` / ② 扁平全局 + 与 BASIC 同名 | **✅ ② 扁平全局 + 与 BASIC 同名，且同名同义** | 跨引擎符号表独立、本无冲突；同名可让用户"换语言不换命令名"→ 降学习成本。**硬约束：同名必须同义，不得同名不同义**（见 §10）→ 解阻塞 L-06/L-07 |
| **D-3** | 命名空间前缀 | `kx` / 自有前缀 | **✅ `ww`** | `kx` 是正运动（ZMotion）简称，本项目为自研控制器，故改用自有品牌 `ww`；仅用于 `ww.call`/`ww.ret` 等少量辅助入口，命令函数直接为全局、不加前缀 |

### 8.3 推荐执行顺序

```
D-1 ─▶ L-01 ─▶ L-02 ┐
                     ├─▶ L-04 ─▶ L-05 ─▶ L-06 ─┐
L-03 ────────────────┘                          ├─▶ L-13 ─▶ L-19 ─▶ L-20
                     L-04 ─▶ L-09 ─────────────┤
                     L-04 ─▶ L-10 ─────────────┤
L-12 ──────────────────────────────────────────┘
                                  └─▶ L-18（可与 L-13 并行）
批2：D-2 ─▶ L-07 ─▶ L-08 ✅；L-14/L-15/L-16/L-17 ✅
     仅剩：L-19（实机对拍，需硬件）
```

---

## 9. 待决事项

| # | 事项 | 选项 | 状态 / 结论 |
|---|------|------|-------------|
| 1 | Lua 依赖来源（D-1） | vendor 源码 / 系统库 | **✅ 已定案：vendor Lua 5.4 源码**（无外部依赖、版本锁定、裸 `g++` 可编） |
| 2 | API 风格（D-2） | 仅 `ww.call` / 扁平全局同名函数 | **✅ 已定案：② 扁平全局 + 与 BASIC 同名**，硬约束**同名同义**（不同义者须显式列为差异） |
| 3 | 命名空间前缀（D-3） | `kx` / 自有前缀 | **✅ 已定案：`ww`**（非正运动 `kx`），仅用于少量辅助入口 |
| 4 | 是否同时支持两引擎并行 | 互斥 / 并行 | **互斥**：共用 `kx::Shared`，并行会命令竞争；"用户可选"=配置二选一 |
| 5 | `SCRIPT_MAX_STEPS` 语义 | 复用同一数值 / 分别换算 | 复用数值 + 文档注明指令级/语句级差异 |
| 6 | 是否需要 `script_run` 支持 lua | 要 / 暂不要 | 批2 可选 |

> 决策定案记录见 §8.2；后续如调整 D-1/D-2/D-3，须回到本节与 §8.1 任务表同步修订。

---

## 10. 附录：BASIC 命令 → Lua API（**同名同义对齐表**）

> D-2=② 定案：Lua 侧采用**同名全局函数**，**同名必须同义**。本表逐条给出「对齐结果」，
> 凡出现 `⚠ 差异` 者为**无法字面同名/同形**的项，已**显式声明**，不算"同名不同义"。
> 完整命令表以 `src/script/motion_host.h` 为准；名称与语义以 `motion_host.cpp` 实现为准。

| BASIC 形式 | Lua API | 对齐结果 | 备注 |
|------------|---------|----------|------|
| `EN` / `DIS` | `ENABLE()` / `DISABLE()` | ✅ 同义 | 阻塞到使能完成 |
| `STOP` | `STOP()` | ✅ 同义 | 停止当前运动 |
| `MOVEABS p[,spd]` | `MOVEABS(p[, spd])` | ✅ 同义 | 阻塞到到位/出错 |
| `MOVE d` | `MOVE(d)` | ✅ 同义（**参数是距离**） | 相对定位，含义照搬 BASIC |
| `JOG spd` | `JOG(spd)` | ✅ 同义 | 立即返回 |
| `VMOVE dir` | `VMOVE(dir)` | ✅ 同义 | 连续速度运动 |
| `CANCEL m` | `CANCEL(m)` | ✅ 同义 | 取消运动 |
| `DATUM m` | `DATUM(m)` | ✅ 同义 | 0=清错 / 3=位置置零 |
| `POS` / `DPOS(n)` | `POS(n)` / `DPOS(n)` | ✅ 同义 | mm |
| `AXISSTATUS(n)` | `AXISSTATUS(n)` | ✅ 同义 | 状态字位表见 `08` §8.2 |
| `SPEED(n)[=v]` | `SPEED(n[, v])` | ✅ 同义 | 读/写 |
| `BUS` / `BUSY` / `IDLE` | 同名函数 | ✅ 同义（**`IDLE` 返 -1/0 照搬**） | 返回数值 |
| `MODBUS_REG(n)` | `MODBUS_REG(n)` | ✅ 同义 | 经 `Shared::mb_regs`（与 502 协议脚本共享同一份） |
| `RETURN`（系统变量） | `ww.ret()` + 函数返回值 | ✅ 同源同义 | 二者取同一份「上一命令返回值」 |
| `PRINT #n, s` | `PORT_PRINT(n, s)` | ⚠ 差异（语法糖→显式函数） | 整包发送、不附加换行 |
| `GET #n, arr` | `PORT_GET(n, tbl)` | ⚠ 差异（语句+数组→函数） | 返回字节数 |
| `OPEN #n,...` | `OPEN(n, type, ...)` | ⚠ 差异（语法糖→显式函数） | 通道号显式传参 |
| `PUTCHAR #n, c` | `PORT_PUTCHAR(n, c)` | ⚠ 差异（语法糖→显式函数） | 单字节 |
| `PORT_STATUS(n)` | `PORT_STATUS(n)` | ✅ 同义 | 1/0 |
| `RUNTASK n, sub` | `RUNTASK(n, fn)` | ⚠ 差异（子程序→函数值） | 只登记状态（单线程） |
| `SDO_WRITE(...)` | **不提供** | ⚠ 不提供 | 需 `ecrt`；调用即明确报错（同 `08` §8.4） |

**通用入口（辅助，非同名）**：`ww.call(name, ...)` 转调任意命令；`ww.ret()` 读最近返回值。
`ww` 为本项目自有前缀（**非** `kx`/正运动）。

**同名同义验收**：§7 步骤 7 的端到端对拍中，**同一段业务逻辑**（`ENABLE→MOVEABS→POS→STOP`）
用 BASIC 与 Lua 各写一遍，**读数必须一致**；不一致即视为"同名不同义"缺陷，须修复或改判为 `⚠ 差异`。

---

## 11. 决策记录（本轮拍板）

| 项 | 决议 | 说明 |
|----|------|------|
| 命名空间前缀 | **`ww`** | 弃用 `kx`（正运动简称）；本控制器为自研，用自有品牌。仅用于 `ww.call`/`ww.ret` 等辅助入口，命令函数为全局、不加前缀 |
| D-1 依赖来源 | **vendor Lua 5.4 源码** | 入仓 `third_party/lua/`（含 MIT LICENSE）；板端裸 `g++` 可编 |
| D-2 API 风格 | **② 扁平全局 + 与 BASIC 同名** | 硬约束：**同名必须同义**；无法同名/同形者（`PRINT #`/`GET #`/`OPEN #`/`PUTCHAR #`/`RUNTASK`/`SDO_WRITE`）在 §10 显式标注为 `⚠ 差异` |
| 引擎并发 | **互斥** | 共用 `kx::Shared`，同一时刻仅一个引擎线程 |

### 11.1 执行记录

| 时间 | 任务 | 结果 |
|------|------|------|
| 上轮 | 决策整理入档（`ww`/D-1/D-2/D-3） | ✅ 完成（§3.3/§3.4/§5/§8.2/§9/§10/§11） |
| 本轮 | L-01 vendor Lua 5.4.6 源码 | ✅ 完成（`third_party/lua/`，MIT，剔除 `lua.c`/`luac.c`） |
| 本轮 | L-02 CMake 接入 | ✅ 完成（`project C CXX` + `lua` 静态库） |
| 本轮 | L-03 `IScriptEngine` + BASIC 适配 | ✅ 完成（`engine.h`；`script_test` ALL PASS） |
| 本轮 | L-04~L-07 LuaEngine 骨架 + 桥接 + 绑定 | ✅ 完成（`lua_engine.{h,cpp}` + `command_table.h`） |
| 本轮 | L-09/L-10 沙箱 + 预算/中止 hook | ✅ 完成 |
| 本轮 | L-12/L-13 配置项 + `script_loop` 分支 | ✅ 完成 |
| 本轮 | L-14 CLI `--script-engine`（main）+ `script_run --engine` | ✅ 完成（`.lua` 自动识别；非法引擎明确报错） |
| 本轮 | L-15/L-16/L-17 trace/内省/错误行号 | ✅ 完成 |
| 本轮 | L-18 `lua_engine_test` | ✅ 完成（8 组 ALL PASS，已进 `ctest`） |
| 本轮 | 部署脚本同步（`18-push-src.sh` 带 `third_party`、`36-build-m2.sh` 编 `liblua_zm.a` 并链接两引擎） | ✅ 完成 |
| 本轮 | L-20 本文档同步 | ◐ 完成（README 见下条） |
| 本轮 | L-11 GC 参数（`LUA_GCGEN`）与"不承诺硬实时"说明 | ✅ 完成 |
| 待办 | L-19 端到端实机对拍（需硬件） | ⏳ |

---

## 附：状态与约定

- **本文档为评估 + 任务规划**，评估与决策（D-1/D-2/D-3）已定案；§8 任务表状态随执行更新。
- **代码侧已全部完成**（L-01~L-18、L-20）；仅 L-19 实机对拍待做（见 §8.1 状态列）。
- 执行中持续同步：任务表「状态」列、§11.1 执行记录、`docs/planA/README.md`（文档索引 + 里程碑 M8）。
- 遵循项目既定纪律：**绝不静默**、**同步文档**、**纯逻辑可单测**。




