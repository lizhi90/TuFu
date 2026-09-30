# 10 BASIC 脚本 API 与开发指南

> 本文面向两类读者，是**使用者视角的 API 参考 + 开发/运行手册**：
> - **脚本作者**：在控制器上写 `.bas`，做运动、端口 IO、Modbus、总线初始化等逻辑；
> - **集成开发者**：把脚本引擎嵌进调试软件/上位机（只对 `IScriptEngine` 接口编程）。
>
> 相关文档：语言实现细节与设计背景见 `08-ZBasic脚本引擎.md`；Lua 版本文档见 `11`，评估见 `09`。
> **命令名与语义的唯一事实来源是 `src/script/motion_host.{h,cpp}`**（本文与之一致）。

实现文件：

| 文件 | 作用 |
|------|------|
| `src/script/engine.h` | 语言无关接口 `IScriptEngine`（统一状态/变量/跟踪/中止） |
| `src/script/script_value.h` | 公共数据面 `kx::Value`（NUM/STR/NIL，**两套引擎共享**） |
| `src/script/script_host.h` | 设备宿主接口 `kx::ScriptHost`（**与语言无关**） |
| `src/script/script.h` / `script.cpp` | BASIC 引擎 `ScriptEngine`：词法 + 递归下降语法 + AST + 解释执行 |
| `src/script/motion_host.h` / `motion_host.cpp` | 设备宿主：命令 → `kx::Shared` |
| `src/script/port_manager.h` / `port_manager.cpp` | `OPEN #n` → 真实（非阻塞、单客户端）TCP 通道 |
| `tools/script_run.cpp` | 命令行工具：语法检查 + dry 试跑 |
| `tools/script_test.cpp` 等 | 引擎/宿主/端口的纯逻辑单测 |

---

## 1. 快速上手（3 分钟）

**第一步：写脚本** `demo.bas`

```basic
PRINT "BUS=" + STR(BUS)
IF BUS = 0 THEN
    PRINT "总线未就绪，退出"
    END
ENDIF

EN
MOVEABS 55, 100, 1000     ' 绝对定位到 55mm，速度 100mm/s，加速度 1000mm/s^2
PRINT "到位位置=" + STR(POS)
STOP                      ' 注意：STOP 是「停止运动」命令，不是结束语句
END
```

**第二步：离线检查 + dry 试跑（不需要硬件）**

```bash
cd lubancat2/build
./script_run demo.bas            # 语法检查 + 逻辑试跑（dry host 只打印命令，不驱动设备）
./script_run demo.bas --check    # 只做语法检查
```

**第三步：真机运行**（在控制器进程内）

```bash
# 方式 A：命令行临时指定（--script 会同时把 SCRIPT_ENABLE 视为 1）
./kine-x --script /opt/kine-x/demo.bas

# 方式 B：写进 config/app.conf
#   SCRIPT_ENABLE = 1
#   SCRIPT_FILE   = /opt/kine-x/demo.bas
```

> dry 试跑时**查询类命令一律返回 0**（上例会打印 `BUS=0`、`到位位置=0`），且**拼错的命令会报错**
> ——这正是它用来暴露笔误、做逻辑回归的价值。真正驱动设备请在控制器进程内运行。

---

## 2. 语言速查（脚本作者）

完整语言子集见 `08` §1，这里给出**高频要点**。

### 2.1 词法约定

- **行式语言**：一条语句一行；一行内用 `:` 分隔多条语句（`A = 1 : B = 2`）。
- **大小写不敏感**：关键字、变量名、命令名、标签均按大写比较。
- **注释**：行首 `REM ...`，或行内任意位置的 `' ...` 到行尾。
- **续行**：`\` 出现在**行尾**=续行；出现在其它位置是**整除**运算符。
- **字符串**：`"..."`，内部 `""` 表示一个字面双引号。
- **变量**：无需声明；**未赋值的数值变量默认 0**（`$` 前缀变量默认空串）。

### 2.2 语句一览

| 语句 | 说明 |
|------|------|
| `变量 = 表达式` | 赋值（`LET` 前缀可省） |
| `数组(下标) = 表达式` | 一维数组赋值（下标**从 0 起**；越界自动扩容） |
| `PRINT 项[,项…]` | 输出（同 `?`）；末尾 `;` 不换行、`,` 换行 |
| `?* 项` | 按**字符串**打印（数组按 0 结尾字符串输出） |
| `PRINT #端口, 项…` | 向通道整包发送（**不附加换行**，一次 `PRINT #` = 一个 TCP 包） |
| `IF 条件 THEN 语句` | 单行 IF |
| `IF … THEN` … `ELSEIF … THEN` … `ELSE` … `ENDIF` | 块式 IF（块内**不能**写 `END`，会提前结束脚本） |
| `WHILE 条件` … `WEND` | 前测循环 |
| `FOR v = a TO b [STEP s]` … `NEXT [v]` | 计数循环（`STEP` 可正可负） |
| `GOTO 标签` / `GOSUB 标签` | 当前层跳转 / 调用（`RETURN` 回到调用点下一条） |
| `GLOBAL SUB 名(形参)` … `END SUB` | 子程序（形参按值、`LOCAL` 局部、`RETURN`/`EXIT SUB`） |
| `GLOBAL CONST 名 = 值` | 编译期常量（**只读**） |
| `GLOBAL DIM a, b(n)` | 登记一维数组名（不设容量上限，仅登记） |
| `WAIT IDLE` / `WAIT UNTIL 条件` / `WAIT ms` | 等轴停止 / 条件轮询 / 延时（`WA` 简写） |
| `EXIT FOR` / `EXIT WHILE` / `EXIT SUB` | 跳出本层循环 / 子程序 |
| `OPEN #端口, 类型, …` | 打开通道（透传给宿主端口层） |
| `GET #端口, 数组[(偏移)], n` | 非阻塞接收（可 `rx = GET …` 取回字节数） |
| `PUTCHAR #端口, 数组(起点,长度)` | 按原始字节发送（含 0 字节） |
| `END` | 结束脚本（正常跑完 = `DONE`） |
| `REM ...` / `' ...` | 注释 |

> ⚠️ **`STOP` 不是结束语句**：脚本里的 `STOP` 是**设备命令「停止运动」**；结束脚本请用 `END`。

### 2.3 运算符（按位！）

按优先级从低到高：`AND` → `EQV` → `XOR` → `OR` → 比较（`= <> < > <= >=`）→
`+ -` → `* / \ MOD` → `^` → 一元 `-` / `NOT`。

- **`AND/OR/NOT/XOR/EQV` 一律按位运算**（与 ZBasic 一致）；逻辑取反请写 `(x = 0)`。
- 别名：`!=` = `<>`，`==` = `=`；十六进制字面量 `$FF` / `&H1F`。
- `+` 左右任一是字符串时按**字符串拼接**。

### 2.4 内置函数

`ABS INT SGN SQR SIN COS TAN ATAN MIN MAX`、
`LEN VAL STR`、`STRLEN STRFIND STRCOMP TOSTR HEX CHR ASC`、`CRC16(数组[,起点,数量])`。

> 除内置函数外的「函数调用」都交给宿主当**设备命令**处理（见第 3 节）。

### 2.5 数组与字符串（A3 数据模型）

- 数组是**一维、下标 0 起**，元素存字节，**不检查边界**（越界自动扩容、默认 0）。
- 数组可**整体当「以 0 结尾的字符串」读**：`PRINT 数组`、`STRFIND(数组,…)`、`STRLEN(数组)`、`VAL(数组)`；
  写 `数组(起点,长度)` 取**原始字节串**（不因 0 截断）。
- 数组名与设备命令不冲突：引擎先问宿主「是不是命令」，宿主返回「不认识」才按数组元素处理。

### 2.6 两个「系统变量」

- **`RETURN`**：保存**最近一次设备命令/函数的返回值**。`SLOT_SCAN(0)` 后用 `IF RETURN THEN` 判成败
  （非 0 = 真）。注意语句位置写 `RETURN` 才是「提前返回子程序」，读值用 `RETURN` 表达式。
- **`TICKS`**：**倒计时**毫秒计数器（赋值即重装初值）。「已流逝 ms = `t0 - TICKS`」。

---

## 3. 设备命令 API（完整参考）

脚本里的设备命令由 `MotionHost` 提供，全部经 `kx::Shared` 下发，**走与控制器内核一致的共享状态与运动语义**。
命令名大小写不敏感。**本控制器为多轴内核**：运行时轴数由 `AXIS_COUNT` 决定（合法 1..8，默认 1=单轴），带轴号的命令按轴选址，**越界轴号明确报错**。

### 3.1 运动 / 使能 / 回零

| 命令 | 参数 | 说明 |
|------|------|------|
| `EN` / `ENABLE` | — | 使能（阻塞等待生效，超时 `enable_timeout_ms`） |
| `DIS` / `DISABLE` | — | 去使能 |
| `STOP` / `RAPIDSTOP` | — | 停止当前运动 |
| `CANCEL` | `mode` | 取消运动（各 mode 一律停当前轴运动） |
| `MOVE` / `MOVR` / `MOVEREL` | `d[,spd[,acc[,wait]]]` | **相对**定位（距离 mm）；`wait` 默认 1=阻塞到到位/出错/超时，`0`=**异步下发立即返回**（长驻端口脚本用，自行用 `IDLE`/`BUSY` 观察） |
| `MOVEABS` / `MOVE_ABS` | `p[,spd[,acc[,wait]]]` | **绝对**定位（目标 mm），`wait` 语义同上 |
| `JOG` / `VJOG` | `spd` | 点动（mm/s，可负），**立即返回**，用 `BUSY`/`IDLE` 观察 |
| `VMOVE` | `dir` | 连续速度运动（`dir>0` 正转/`<0` 反转，速度取 `SPEED`），**立即返回**，直到 `STOP`/`CANCEL` |
| `HOME` | — | **软件回零**：绝对定位到约定零点（`AxisConfig::home_mm`，默认 0），阻塞到到位 |
| `DATUM` | `mode` | `0`=清除错误（→ `FAULT_RESET`）；`3`=当前位置置零（坐标系偏移，不动电机）；其余模式**明确报错** |
| `DELAY` / `SLEEP` / `WAIT` | `ms` | 等待毫秒（可被中止打断） |
| `TICKS`（命令形式，供 Lua） | — | 毫秒倒数计时：两次差值=经过毫秒（BASIC 侧是系统变量 `TICKS`，同语义） |
| `WAITIDLE`（由 `WAIT IDLE` 生成） | — | 等轴到位（轮询 `axis.idle`） |

> `MOVE`/`MOVR` 是**相对**、`MOVEABS` 是**绝对**；速度/加减速缺省时取当前轴的 `SPEED`/`ACCEL`/`DECEL`。

### 3.2 轴与轴参数

| 命令 | 参数 | 说明 |
|------|------|------|
| `BASE n` / `AXIS(n)` | `n` | 设置当前轴（命令后缀 `MOVEABS(x) AXIS(0)` 等效先 `BASE(0)`） |
| `SPEED` / `ACCEL` / `DECEL` | `(n)[=v]` | 轴参数：读（1 参）/ 写（2 参）；`MOVE` 未显式给速度时取其默认值 |
| `UNITS` | `(n)[=v]` | **脉冲当量（inc/mm）：运行时生效**（v0.8.1）。控制器无默认值——未设置时定位/点动/回零/置零明确报错；由脚本开机设置（如 `UNITS(0)=14043.41`） |
| `JOGLEAD` | `(n[,s])[=v]` | 点动 PP 跟随**前视**（秒，运行时生效）：实际前视 = `s + v²/(2a)`（范围 0.05~5s）。**固件无默认值**——未设置时 PP 点动明确拒绝；点动中也可改；脚本开机设置（如 `JOGLEAD(0)=0.5`） |
| `SRAMP` | `(n[,ms])[=v]` | **S 曲线时间**（0~250ms，0=梯形；ZBasic 同名）：**仅 CSP 生效**（PP 下写值/切 PP 会明确提示不支持）；脚本开机设置（如 `SRAMP(0)=100`） |
| `FASTDEC` | `(n[,v])[=v]` | **急停/停机减速度**（mm/s²，0=未设置=旧行为；ZBasic 同名）：CSP 按解析减速斜坡停机（距离 v²/2a），PP 写 6084 后 halt |
| `VP_SPEED` | `(n)` | 只读：当前运动速度 mm/s（内核实际位置差分估计+平滑；曲线/诊断用） |
| `ATYPE` / `DRIVE_PROFILE` / `AXIS_ADDRESS` | `(n)[=v]` | 轴参数读写；本控制器**仅记录**，不据其换算 |
| `DRIVE_CONTROLWORD` | `(n)[=v]` | CiA402 控制字 `6040h`：读恒 0；**写仅接受** `128`(清错→`FAULT_RESET`) / `6`(关机→去使能) / `15`(使能→`ENABLE`)，其余值**明确报错** |

**速度曲线 / 运动参数速查（用法示例，2026-09-27）**

| 命令 | 用法示例（BASIC） | 说明 |
|---|---|---|
| `MOTION_MODE` | `MOTION_MODE(1)` | 0=PP（驱动器规划，默认）/ 1=CSP（控制器每拍规划，需 DC+UNITS）；**S 曲线仅 CSP 生效** |
| `SRAMP` | `SRAMP(0)=100`（S 曲线 100ms）；`SRAMP(0)=0` 回梯形 | S 曲线时间 **0~250ms**；PP 下写值/切 PP 会明确提示“不支持”（不伪装） |
| `FASTDEC` | `FASTDEC(0)=800` | 停机减速度 mm/s²（0=未设置=旧行为）；CSP 解析减速停（距离 v²/2a，完成不报 move_done），PP 改 6084 后 halt |
| `VP_SPEED` | `PRINT VP_SPEED(0)` | 只读：当前运动速度 mm/s（实际位置差分+平滑）；曲线/诊断用 |
| `SPEED` / `ACCEL` / `DECEL` | `SPEED(0)=50`、`ACCEL(0)=800`、`DECEL(0)=650` | 缺省速度/加减速：写值**同步下发内核缺省**；MOVE/JOG 未显式给参时使用；DECEL 亦作 PP 的 6084 |
| `JOGLEAD` | `JOGLEAD(0)=0.5` | 点动 PP 跟随前视（秒，0.05~5；实际 = 值 + v²/2a）；**固件无默认**，未设置拒绝 PP 点动 |
| `MOVEABS` / `MOVE` | `MOVEABS(100, 50)`（绝对 100mm @50mm/s）；`MOVE(10, 50)`（相对 10mm） | CSP 下由规划器生成曲线（梯形/三角自动/S 曲线）；`wait=0` 异步下发（长驻脚本用） |
| `JOG` / `VJOG` / `VMOVE` | `JOG(50)`；`VJOG(1)` / `VJOG(-1)`；`VMOVE(-1)` | 点动（立即返回，用 `BUSY`/`IDLE` 观察）；VJOG 沿用当前 SPEED；VMOVE 方向持续运动（`CANCEL` 停止） |
| `STOP` | `STOP`（或 `STOP(0)`） | 按参数减速停止（halt）；CSP+FASTDEC 时按设定减速度平缓停机 |

### 3.3 状态查询

| 命令 | 参数 | 说明 |
|------|------|------|
| `POS` / `MPOS(n)` | — | 实际位置 mm |
| `DPOS(n)` | — | 指令位置 mm |
| `BUS` / `BUSOK` | — | 总线是否就绪（1/0） |
| `BUSY` | — | 是否在运动（1/0） |
| `IDLE` / `ISIDLE` | — | 运动是否结束 ⚠`IDLE` 返回 **-1=到位 / 0=运动中**（与 ZBasic 一致） |
| `ENABLED` | — | 是否已使能（1/0） |
| `ALARM` | — | 是否报警（1/0） |
| `AXISSTATUS(n)` | `n` | 轴状态字（位表见 `08` §8.2）；`(AXISSTATUS(0) and MV_ERRMASK) <> 0` 判报警 |
| `DISABLE_GROUP n` | `n` | 解除轴分组（no-op，保留语法兼容） |

### 3.4 总线初始化 / 扫描

| 命令 | 参数 | 说明 |
|------|------|------|
| `SLOT_SCAN(0)` / `SCAN` | — | 触发重扫并等 `Shared::bus` 刷新；**返回值=扫到的从站数**（0=没扫到），用 `IF RETURN THEN` 判 |
| `SLOT_START(0)` | — | 等总线进入 OP 且已有从站；返回值 `1` 成功 / `0` 失败 |
| `SLOT_STOP(0)` / `BUSSTOP` | — | 总线软停；返回值 `1` |

### 3.5 端口 / Modbus / 任务 / 总线信息（B 层）

| 命令 | 参数 | 说明 |
|------|------|------|
| `OPEN`（由 `OPEN #端口,…` 生成） | `端口, 类型[, 端口号][, IP]` | `TCP_SERVER`（监听）/ `TCP_CLIENT`（主动连远端） |
| `CLOSE` | `端口` | 关闭通道 |
| `PRINT`（由 `PRINT #端口,…` 生成） | `端口, 文本` | 整包发送（不加换行） |
| `PUTCHAR`（由 `PUTCHAR #端口,…` 生成） | `端口, 字节串` | 原始字节发送（含 0 字节） |
| `GET`（由 `GET #端口, 数组…` 生成） | `端口, 数组名, 偏移, 最大字节数` | 非阻塞接收；返回字节数，收到内容写入数组（补 0） |
| `PORT_STATUS` | `通道` | 1=已连接，0=未连接/未打开 |
| `PORT_TARGET` | `通道` | 通道对端（`ip:port` / `listen:port`，诊断用） |
| `PORT_MAX` | — | 端口通道号上限（脚本里常用 `GLOBAL CONST`） |
| `MODBUS_REG` | `寄存器` | 16 位 4x 寄存器读写（写为 `MODBUS_REG(n)=值`） |
| `MODBUS_IEEE` | `寄存器` | 32 位浮点 4x 读写（占 n/n+1，低字在前） |
| `NVSET` | `寄存器, 值` | 持久化：把 4x 寄存器值存到脚本目录 `.nvram`（立即落盘；掉电/重启后 `NVGET` 恢复） |
| `NVGET` | `寄存器` | 持久化：读回 `.nvram` 保存的值（无记录返回 0） |
| `RUNTASK` / `STOPTASK` | `任务号, 子程序名` | 登记/注销脚本子任务（**只登记状态，不并发执行**） |
| `PROC_STATUS` | `任务号` | 子任务是否登记为「运行中」（1/0） |
| `SCAN_EVENT` / `NODE_COUNT` / `NODE_AXIS_COUNT` / `NODE_STATUS` / `NODE_IO` / `NODE_AIO` / `NODE_INFO` | `…` | 总线设备信息（数据来自 `Shared::bus`，未扫描返回 0） |
| `ETHERCAT` / `ECUSTOM` / `ETH_MODE` | `…` | 总线诊断 / 自定义网口 / 网口模式（占位，见 `08` §8.4） |
| `SDO_WRITE` | `…` | **脚本层不可用**：需 EtherCAT 主站通道，调用即**明确报错** |

### 3.6 三条硬性纪律

1. **绝不死循环**：语句级步数预算（默认 5e6）+ 宿主中止回调双保险。
2. **绝不静默**：总线未就绪下发 `MOVE/JOG`、寄存器/任务号越界、通道未打开等
   一律返回**运行错误并带原因**，脚本随即以 `RUNTIME_ERROR` 结束。
3. **轴号边界**：`BASE`/`AXIS`、轴参数、`POS`/`MPOS`/`DPOS`/`IDLE`/`ISIDLE`/`AXISSTATUS`/`DISABLE_GROUP`
   等带轴号的命令，**越界轴号（≥ `AXIS_COUNT`）一律报运行错误**，绝不静默回读轴 0 的数据（默认单轴配置下仅轴 0 合法）。

---

## 4. 运行与配置

### 4.1 dry-run 工具 `script_run`（离线，不需要硬件）

```bash
./build/script_run demo.bas            # 语法检查 + 逻辑试跑（dry host 打印命令，不驱动设备）
./build/script_run demo.bas --check    # 只做语法检查
./build/script_run demo.bas --trace    # 单步跟踪
./build/script_run demo.bas --steps N  # 语句级预算（默认 5,000,000）
./build/script_run demo.bas --quiet    # 不回显 dry 命令
./build/script_run demo.bas --sim-bus  # 模拟「总线已连上」，走通整条总线初始化逻辑
```

dry host 约定：查询类返回 0，动作类只打印；**拼错的命令会报错**（暴露笔误）。
`--sim-bus` 让 `SLOT_SCAN`/`SLOT_START` 成功、`NODE_COUNT`/`NODE_AXIS_COUNT`>0，
可离线走通遗留脚本「扫描 → 映射轴 → 开总线 → 清错 → 使能 → 主循环」的整条主程序逻辑。

### 4.2 控制器进程内运行

`config/app.conf`：

```ini
SCRIPT_ENABLE      = 1                 # 1=启动后在独立线程自动跑 SCRIPT_FILE
SCRIPT_FILE        = /opt/kine-x/demo.bas
SCRIPT_ENGINE      = basic             # basic(默认) | lua（见 11）
SCRIPT_WAIT_BUS    = 1                 # 先等总线上电就绪再执行
SCRIPT_WAIT_BUS_MS = 15000             # 等总线超时
SCRIPT_TRACE       = 0                 # 1=逐条打印执行语句
SCRIPT_MAX_STEPS   = 5000000           # 语句级步数预算
```

命令行覆盖（`--script` 会同时把 `SCRIPT_ENABLE` 视为 1）：

```bash
./kine-x --script /opt/kine-x/demo.bas --script-engine basic --script-trace
```

行为要点：

1. `kine-x` 起完 RT/SDO 线程后，另起**脚本线程**；
2. `SCRIPT_WAIT_BUS=1` 时先等 `bus_ok`（轮询 100ms，超时则跳过并告警）；
3. 读文件 → `compile` → `run`；`PRINT` 输出带 `[script]` 前缀写 stdout；
4. 脚本只经 `kx::Shared` 下发 `AxisCmd`（控制器进程内**只有脚本线程**主动下发运动命令）；
5. 进程收到 `SIGINT/SIGTERM` 时 `host->aborted()` 立即生效，脚本以 `ABORTED` 结束，不卡关机。

> ⚠️ **协议对接全部由脚本线程承担**（4 个原生服务已从工程移除）；同一时刻只加载一种脚本语言，
> 故不存在多个使用者争抢「覆盖式」命令槽的问题。

> 📌 **规则（已强制）：控制器同一时刻只加载一种脚本语言。** BASIC 与 Lua 引擎**互斥**：
>   * `SCRIPT_ENGINE` 只允许 `basic | lua`（大小写不敏感，空 = `basic`），**非法值在载入配置时明确报错**；
>   * 引擎必须与脚本扩展名一致（`basic`↔`.bas`、`lua`↔`.lua`），不一致时脚本线程**报错跳过**；
>   * 代码上由 `src/script/engine_rule.h` 的 `ScriptEngineSlot` 保证"只装一个引擎"，
>     已装载时拒绝再装第二种语言（见 `09` §3.1 / §3.7，自测 `engine_rule_test`）。

---

## 5. 状态、错误与安全

### 5.1 结束状态（`IScriptEngine::Status`）

| 状态 | 含义 |
|------|------|
| `READY` | 已编译、尚未运行 / 运行中 |
| `DONE` | 正常跑完（遇到 `END` 或语句耗尽） |
| `COMPILE_ERROR` | 语法错误（`error()` 带行号与原因） |
| `RUNTIME_ERROR` | 运行期错误（未定义标签、除零、命令报错…） |
| `ABORTED` | 被 `request_abort()` 或 `host->aborted()` 中止 |
| `BUDGET_EXCEEDED` | 超出语句步数预算（防死循环） |

### 5.2 双保险防死循环

- **步数预算**：`run(max_steps)`，默认为 5e6 条语句；超出置 `BUDGET_EXCEEDED`。
- **可中止**：每执行 1024 条语句询问一次 `host->aborted()`；`MotionHost::sleep_ms` 以 10ms 粒度响应中止，
  故长循环、长 `DELAY` 都能被及时叫停。

---

## 6. C++ 集成 API（调试软件后端）

调试软件只需对 `IScriptEngine` 编程，无需关心底下是 BASIC 还是 Lua。

```cpp
#include "script/script.h"        // ScriptEngine（BASIC 引擎）
#include "script/script_host.h"   // ScriptHost（语言无关宿主接口，自定义宿主时用）
#include "script/script_value.h"  // Value（共享值类型）
#include "script/motion_host.h"   // 真机设备宿主（离线可换假宿主）
#include "script/engine.h"        // IScriptEngine

kx::MotionHost host(shared);                 // 设备宿主（或自定义 ScriptHost）
std::unique_ptr<kx::IScriptEngine> eng(new kx::ScriptEngine(&host));

std::string err;
if (!eng->compile(src, &err)) {              // err 带「第 N 行: 原因」
    fprintf(stderr, "编译失败: %s\n", err.c_str());
    return;
}
eng->set_trace(true);                        // 单步跟踪（输出到 host->print）
eng->set_var("SPD", kx::Value::number(80));  // 预置变量
auto st = eng->run(5000000);                 // 语句级预算
if (st != kx::IScriptEngine::Status::DONE)
    fprintf(stderr, "%s 第 %d 行: %s\n",
            kx::IScriptEngine::status_name(st), eng->error_line(), eng->error().c_str());

for (auto& v : eng->list_vars()) printf("%s\n", v.c_str());  // "NAME = 3"
for (auto& l : eng->labels())    printf("label %s\n", l.c_str());
eng->request_abort();                        // 线程安全，仅置位
```

### 6.1 自定义宿主接口 `ScriptHost`

想脱离硬件单测或接别的后端，实现 `ScriptHost` 即可（引擎完全不知道设备细节）：

```cpp
int call(const std::string& name, const std::vector<Value>& args,
         Value* ret, std::string* err) override;   // 见下方返回约定
void print(const std::string& line) override;       // PRINT 的落点
bool aborted() override;                            // 周期性询问是否中止
```

| 返回 | 含义 |
|------|------|
| `0` | 成功；返回值写入 `ret`（查询类命令用） |
| `1` | **不认识这个命令** → 引擎退化为「一维数组访问」（如 `A(i)`） |
| `2` | 运行错误（填 `err`），脚本以 `RUNTIME_ERROR` 结束 |
| `3` | 请求中止脚本（脚本以 `ABORTED` 结束） |

### 6.2 `IScriptEngine` 接口速览

| 方法 | 说明 |
|------|------|
| `name()` | 引擎标识（BASIC 引擎返回 `"basic"`） |
| `set_host(host)` | 运行前必须设置设备宿主 |
| `compile(src, err)` | 编译；失败 `err` 带行号与原因 |
| `run(max_steps)` | 执行；返回终态 `Status` |
| `status()` / `error()` / `error_line()` / `steps()` | 终态 / 错误文本 / 错误行号 / 已执行步数 |
| `request_abort()` | 线程安全中止（仅置位） |
| `get_var` / `set_var` / `list_vars` / `clear_vars` | 变量内省（预置/查看/列举/清空） |
| `set_trace(bool)` / `trace()` | 单步跟踪开关 |
| `labels()` | 脚本出现过的标签（做断点/跳转列表） |
| `status_name(s)` | 静态便捷入口：状态 → 字符串 |

---

## 7. 完整示例

```basic
' server.bas —— TCP 服务端 + 总线初始化 + 定位，演示端口 IO 与 RETURN 系统变量
GLOBAL CONST PORT_MAX = 16
GLOBAL DIM RX(256)

OPEN #10, "TCP_SERVER", 4321          ' 监听 4321（端口号按实际修改）
IF PORT_STATUS(10) = 0 THEN
    PRINT "等待客户端接入…"
ENDIF

' 总线初始化：扫描 -> 判成败 -> 开总线
IF SLOT_SCAN(0) = 0 THEN
    PRINT "未扫到从站，退出"
    END
ENDIF
PRINT "扫到从站数=" + STR(RETURN)

IF SLOT_START(0) THEN
    PRINT "总线已开启"
ENDIF

DATUM 0                               ' 清错
EN                                    ' 使能
MOVEABS 55, 100, 1000
PRINT "位置=" + STR(POS)

' 回显：把收到的字节整包发回（含 0 字节用 PUTCHAR）
WHILE 1
    rx = GET #10, RX, 64
    IF rx > 0 THEN
        PUTCHAR #10, RX(0, rx)
    ENDIF
    DELAY 20
WEND
```

---

## 7.5 多文件与 `INCLUDE`（v0.6.0，方案 A）

控制器脚本目录可存多个 `.bas`；开机只运行**一个主文件**（D8，插件「控制器文件 → 设为主文件」），
其余文件由主文件**编译前展开**并入，等价 ZBasic「一个工程多个文件一起编译」：

```
INCLUDE "robot_lib.bas"     ' 独占一行；关键字大小写不敏感；可跟 ' 注释
```

规则：
- 只能包含 `DEBUG_SCRIPT_DIR` 内的**纯文件名**（`字母/数字/._-`，不以 `.` 开头；防路径穿越），
  扩展名必须同为 `.bas`；嵌套 ≤ 8 层，循环包含明确报错；
- 展开发生在编译前（调试口 `script.compile` 与开机脚本路径一致），失败按 `COMPILE_ERROR`
  明确报错（缺文件/跨语言/循环/超深），不静默跳过；
- **行号按展开后的合并源计算**：报错行号、`script` 事件 `line`、D5 断点都对应合并源；
  建议被包含文件只放子程序/常量（执行语句越少，行号越稳定）；
- 子程序之间用**全局变量**通信（ZBasic 子程序无返回值，与旧程序一致）。

**Kine-X 控制器脚本模块布局（2026-09-30，Lua）**：
- 主文件 `EtherCAT_SocketServer.lua`（仓库根）＝ 头部说明 + `include(...)` 列表 + 主循环 + `KX_TEST` 钩子；
- 模块（平铺同目录、`kx_` 前缀）：`kx_base`（常量/寄存器映射/工具）、`kx_bus`（总线初始化）、
  `kx_socket`（4321）、`kx_modbus`（502 从站 + HMI 轴命令区）、`kx_scale`（称重）、`kx_pump`（蠕动泵）、
  `kx_robot`（机器人桥接/轴0定位/安全联锁）；
- **include 顺序 = 依赖顺序**（合并后同一 chunk 共享 local），勿调换；
- 部署：`lubancat2/deploy/17-push-script.sh`（主文件 → 板端 `demo.lua`；`kx_*.lua` 原样；逐文件 md5 校验）；
- 本地自测 `tools/port_script_selftest.lua` 已内置同规则 include 展开；报错行号 / D5 断点按**合并源**计。

## 8. 限制与后续

| 限制 | 说明 / 后续 |
|------|-------------|
| `GOTO`/`GOSUB` 只在本层生效 | 从循环体内跳出会丢失该层循环状态；返回地址栈不跨层 |
| 仅一维数组、不检查边界 | 下标 0 起，越界自动扩容，元素默认 0 |
| 无 `INPUT` / 文件 IO | 输入由调试软件提供；**多文件经 `INCLUDE` 编译期展开（见 §7.5）** |
| `RUNTASK` 不并发执行任务体 | 引擎单线程：只「登记 + 状态」；并发处理由脚本自行组织（端口轮询 + `DELAY` + 状态机） |
| `DRIVE_CONTROLWORD` 只接受标准序列 | 仅 `128`/`6`/`15`，其余明确报错（不裸写 `6040h`） |
| `SDO_WRITE` 脚本层不可用 | 需 `ecrt` 通道；调用即明确报错 |
| 驱动器内部回零（`0x6098`）未定案 | `HOME` 现为**软件回零**；内部回零为独立后续项 |
| 多轴语法 | 本控制器为**多轴内核**（`AXIS_COUNT` 1..8，默认 1）：轴选择真正生效，越界轴号明确报错 |

**测试**：

```bash
cd lubancat2/build && cmake .. && make -j4
ctest -R 'script_test|motion_host_test|port_manager_test|config_test|engine_rule_test' --output-on-failure
```

以上测试均**不需要 IgH 与硬件**（假宿主 + 回环 socket）。
