# 08 ZBasic 子集脚本引擎

> 目标：在鲁班猫2 侧提供一个**可跑 `.bas` 的 BASIC 子集解释器**，作为调试软件的脚本执行后端，
> 把原正运动控制器上的 ZBasic 使用习惯（命令行式运动控制）平移到本控制器。
>
> 关键约束（与整套方案一致）：
> - **不碰 EtherCAT / RT 线程**：脚本里的设备命令全部经 `kx::Shared` 下发，与内核运动语义一致；
> - **纯逻辑可单测**：引擎与「宿主」解耦，不需要 IgH 与硬件即可回归（`tools/script_test.cpp`）；
> - **绝不死循环**：语句级步数预算 + 宿主中止回调双保险。

> 📘 面向**脚本作者 / 集成开发者**的 API 参考与开发手册（语言速查、命令表、运行配置、`IScriptEngine` 集成）
> 见 [`10-BASIC脚本API与开发指南.md`](10-BASIC脚本API与开发指南.md)；本文侧重**引擎内部设计**。

实现文件：

| 文件 | 作用 |
|------|------|
| `src/script/script_value.h` | 公共数据面 `kx::Value`（NUM/STR/NIL）——**两套引擎共享**，与语言无关 |
| `src/script/script_host.h` | 设备宿主接口 `kx::ScriptHost`——**语言无关**，引擎唯一对接点 |
| `src/script/script.h` / `script.cpp` | BASIC 引擎 `ScriptEngine`：词法 + 递归下降语法 + AST + 解释执行（与硬件无关） |
| `src/script/motion_host.h` / `motion_host.cpp` | 设备宿主：把脚本命令翻译成 `kx::Shared` 的 `AxisCmd` 等 |
| `src/script/port_manager.h` / `port_manager.cpp` | 端口通道：`OPEN #n` → 真实（非阻塞、单客户端）TCP socket |
| `tools/script_test.cpp` | 引擎单测（假 Host） |
| `tools/motion_host_test.cpp` | 设备宿主单测（假 RT 线程，验证等待语义 + B 层寄存器/任务/总线） |
| `tools/port_manager_test.cpp` | 端口通道单测（回环 socket） |
| `tools/script_run.cpp` | 命令行工具：语法检查 + dry 试跑 |

---

## 1. 语言子集

### 1.1 基本词法

- **行式语言**：一条语句一行；一行内可用 `:` 分隔多条语句（如 `A = 1 : B = 2`）。
- **大小写不敏感**：关键字、变量名、命令名、标签统一按大写比较。
- **注释**：行首（允许前导空白）的 `REM ...`，或行内任意位置的 `' ...` 到行尾。
- **续行**：`\` 出现在**行尾**时表示下一行继续本行；`\` 出现在其它位置是**整除**运算符（注意区别）。
- **字符串字面量**：`"..."`，内部 `""` 表示一个字面双引号；字符串只用于 `PRINT`/`+` 拼接与 `LEN/VAL/STR`。
- **变量**：无需声明，首次使用即存在；**未赋值的数值变量默认 0**。

### 1.2 数组（一维）

- 形式 `A(i)`；读写作 `A(i)` / `A(i) = 值`。
- **不检查边界**：下标越界自动扩容，元素默认 0（便于脚本自增计数）。
- `DIM a(10)` 作为**兼容语句**接受，但仅作空操作（不做分配/不设上限）。
- 数组与同名的设备命令不冲突：引擎先问宿主「是不是命令」，宿主返回"不认识"时才按数组元素处理。

### 1.3 标签、跳转与子程序

- 标签写作 `名字:`（独占一行，或后面跟其它语句）。
- `GOTO 名字` 跳转；未定义标签在运行期报错。标签在**当前层**（顶层 / 子程序体内）解析。
- `GOSUB 名字` 调用**当前层**标签：压入返回地址（本层的返回地址栈），`RETURN` 弹栈回到调用点的下一条语句。
  返回地址栈**不跨层**（子程序体内的 `GOSUB` 只在子程序体内解析标签/返回）。
  需要复用的逻辑仍推荐 `SUB`。
- `RETURN` 的三种含义（按上下文自动区分）：① `GOSUB` 待返回 → 弹栈回到调用点下一条；
  ② 位于 `SUB` 内且无待返回地址 → 提前返回子程序（等价 `EXIT SUB`）；
  ③ 既无待返回地址、又不在子程序内 → 运行期报错（`RETURN 出现在子程序/GOSUB 之外`）。
- `GLOBAL SUB 名(形参, ...)` … `END SUB`：子程序定义（可前向引用，可嵌套调用，递归深度上限 32 层）。
  - 形参**按值**传入；`LOCAL a, b` 声明的局部量在调用期间**遮蔽**同名全局；调用结束自动恢复。
  - 结果通过**全局变量**回传（与原程序 `get_num` 写 `numval` 的习惯一致）。
  - `RETURN` / `EXIT SUB` 提前返回；`EXIT FOR` / `EXIT WHILE` 跳出本层循环。

### 1.4 语句一览

| 语句 | 说明 |
|------|------|
| `变量 = 表达式` | 赋值（`LET` 前缀可省） |
| `数组(下标) = 表达式` | 一维数组赋值；非 DIM 名字先试「设备参数写入」再退化为数组元素 |
| `PRINT 表达式[,|;] ...` | 输出，同 `?`；末尾为 `;` 时不换行，为 `,` 时换行（分隔符本身不输出）；`?,,` 空输出项容错 |
| `?* 表达式` | 按**字符串**打印（数组按 0 结尾字符串输出） |
| `PRINT #端口, 项...` | 向已 OPEN 的通道整包发送（**不附加换行**，一次 `PRINT #` = 一个 TCP 包） |
| `IF 条件 THEN 语句` | 单行 IF |
| `IF 条件 THEN` … `ELSEIF 条件 THEN` … `ELSE` … `ENDIF` | 块式 IF（`END IF` 亦可）；`ELSE`/`ELSEIF` 可省 |
| `WHILE 条件` … `WEND` | 前测循环（`END WHILE` 亦可） |
| `FOR v = a TO b [STEP s]` … `NEXT [v]` | 计数循环；`STEP` 可正可负 |
| `GOTO 标签` | 无条件跳转（当前层内） |
| `GOSUB 标签` | 调用当前层标签（压返回地址）；`RETURN` 回到调用点下一条 |
| `GLOBAL SUB 名(形参)` … `END SUB` | 子程序定义；`LOCAL` / `RETURN` / `EXIT SUB` |
| `GLOBAL CONST 名 = 值[, ...]` | 编译期常量（**只读**，赋值报运行错误） |
| `GLOBAL DIM a, b(n)` | 登记数组名（一维，不设容量上限） |
| `WAIT IDLE` / `WAIT UNTIL 条件` / `WAIT ms` | 等轴停止 / 条件轮询 / 延时（`WA` 为简写） |
| `EXIT FOR` / `EXIT WHILE` / `EXIT SUB` | 跳出本层循环 / 子程序 |
| `OPEN #端口, 类型, ...` | 打开通道（`TCP_SERVER`/`TCP_CLIENT`…），透传给宿主 |
| `GET #端口, 数组[(偏移)], n` | 非阻塞接收，字节写入数组并补 0 结尾；可写成 `rx = GET …` 取回字节数 |
| `PUTCHAR #端口, 数组(起点,长度)` | 按**原始字节**发送（含 0 字节），一帧一次发完 |
| `END` | 结束脚本（正常跑完即 `DONE`） |
| `REM ...` / `' ...` | 注释 |

> ⚠️ **`STOP` 不是结束语句**：脚本里的 `STOP` 是**设备命令「停止运动」**（等价 `RAPIDSTOP(2)`）；
> 结束脚本请用 `END`。这与「正运动 ZBasic 里 `STOP` 停运动」的习惯一致。

> 数组/字符串数据模型（A3）：数组元素存字节，`PRINT 数组`、`STRFIND(数组,…)`、`STRLEN(数组)`、
> `VAL(数组)` 都把数组当**以 0 结尾的字符串**读；`数组(起点,长度)` 取原始字节串（不因 0 截断）。

### 1.5 运算符

按优先级从低到高排列（左侧最先结合）：

| 优先级 | 运算符 | 说明 |
|--------|--------|------|
| 1 | `AND` | **按位**与 |
| 2 | `EQV` | **按位**同或（`~(a^b)`） |
| 3 | `XOR` | **按位**异或 |
| 4 | `OR` | **按位**或 |
| 5 | `=` `<>` `<` `>` `<=` `>=` | 比较；两串比较按字典序，返回 1/0 |
| 6 | `+` `-` | 加/减（二元）；`+` 左右任一是字符串时按字符串拼接 |
| 7 | `*` `/` `\` `MOD` | 乘、除、整除、取模（除数为 0 报运行错误） |
| 8 | `^` | 幂（右结合） |
| 9 | `-`（一元）、`NOT`、`*`（`?*` 字符串形式） | 取负、**按位取反**（`NOT 0 = -1`）、按字符串打印 |

- 别名：`!=` 等价 `<>`，`==` 等价 `=`。
- **`AND/OR/NOT/XOR/EQV` 一律按位运算**（与 ZBasic 一致）；布尔场景下用 1/0 时与逻辑等价，
  逻辑取反请写 `(x = 0)`。
- 进制字面量：`$FF`（`$` 前缀）与 `&H1F`（`&H` 前缀）表示十六进制。
- 逻辑真/假用 1/0 表示；`truthy()` 判据为「数值非 0 或字符串非空」。

### 1.6 内置函数

| 函数 | 说明 |
|------|------|
| `ABS(x)` / `INT(x)` / `SGN(x)` | 绝对值 / 向下取整 / 符号 |
| `SQR(x)` / `SIN/COS/TAN(x)` / `ATAN(x)` | 平方根（负数报错）、三角、反正切 |
| `MIN(a,b,...)` / `MAX(a,b,...)` | 可变参数的最小/最大值 |
| `LEN(s)` / `VAL(s)` / `STR(x)` | 字符串长度 / 转数 / 数转字符串 |
| `STRLEN(a)` | 数组（=0 结尾字符串）长度 |
| `STRFIND(a, 子串[, 起点])` | 查找下标（**0 起**，找不到 -1） |
| `STRCOMP(a, b)` | 字符串比较（0 = 相等） |
| `TOSTR(x, 位数, 小数位)` | 定宽格式化（如 `TOSTR(w,10,3)`） |
| `HEX(x[, 字节数])` | 十六进制大写（**字节对齐**） |
| `CHR(x)` / `ASC(s)` | 码值转字符 / 字符转码值 |
| `CRC16(数组[, 起点, 数量])` | Modbus CRC16（初值 `$FFFF`/多项式 `$A001`）；返回值**按发送顺序打包**：高 8 位 = 帧内先发的低字节（实测定案） |

> 除内置函数外的「函数调用」都交给宿主当**设备命令**处理（见第 2 节）。
> `TICKS` 是**倒计时**毫秒计数器（赋值即重装初值），故「已流逝 ms = `t0 - TICKS`」。
> `RETURN` 是**系统变量**（不是普通变量）：每次设备命令/函数执行成功后，其返回值写入 `RETURN`；
> 语句位置写 `RETURN` 才是「提前返回子程序」（见 1.3）。因此遗留脚本的
> `SLOT_SCAN(0)` → `IF RETURN THEN` 判成败可直接工作（`RETURN` 非 0 = 真）。

---

## 2. 设备命令（宿主导出）

脚本里的设备命令由 `MotionHost` 提供（`src/script/motion_host.h`），全部经 `kx::Shared` 下发，
**走与控制器内核一致的共享状态与运动语义**。命令名大小写不敏感。

| 命令 | 参数 | 说明 |
|------|------|------|
| `EN` / `ENABLE` | — | 使能（阻塞等待使能生效，超时 `enable_timeout_ms`） |
| `DIS` / `DISABLE` | — | 去使能 |
| `STOP` | — | 停止当前运动（对应 `AxisCmd::STOP`） |
| `CANCEL` | `mode` | 取消运动（停止当前轴定位/点动）；各 mode 一律停当前轴运动（对应 `AxisCmd::STOP`） |
| `MOVE` / `MOVR` / `MOVEREL` | `p[,spd[,acc]]` | **相对**定位（距离，mm），阻塞到到位/出错/超时 |
| `MOVEABS` / `MOVE_ABS` | `p[,spd[,acc]]` | **绝对**定位（目标位置，mm），阻塞语义同上 |
| `JOG` / `VJOG` | `spd` | 点动（mm/s，可负），**立即返回**，用 `BUSY`/`IDLE` 观察 |
| `VMOVE` | `dir` | 连续速度运动（`dir>0` 正转 / `<0` 反转，速度取当前轴 `SPEED`），**立即返回**，直到 `STOP`/`CANCEL`；落到内核 `JOG` |
| `HOME` | — | **软件回零**：绝对定位到约定零点（内核 `AxisConfig::home_mm`，默认 0），阻塞到到位/出错/超时。绝对编码器系统（SV630N 支持）下即“回到机械零点”；驱动器内部回零（`6060=6` + `0x6098` 方式）为独立后续项 |
| `DATUM` | `mode` | 手册语义：`DATUM(0)`=清除控制器所有轴错误（不改坐标）→ 下发内核 `AxisCmd::FAULT_RESET`（真实触发 cia402 `6040=0x80` 复位脉冲）；`DATUM(3)`=**当前位置置零**（内核 `AxisCmd::SET_POS` 做坐标系偏移，**不动电机**）→ 阻塞到完成返回 0；其余模式未定案，**明确报运行错误** |
| `DELAY` / `SLEEP` / `WAIT` | `ms` | 等待毫秒；等待期间可被中止 |
| `WAITIDLE`（由 `WAIT IDLE` 生成） | — | 等轴到位（轮询 `axis.idle`，可被中止/超时） |
| `POS` / `MPOS` | — | 实际位置 mm（返回数值） |
| `DPOS` | — | 指令位置 mm |
| `BUS` / `BUSOK` | — | 总线是否就绪（1/0） |
| `BUSY` | — | 是否在运动（1/0） |
| `IDLE` / `ISIDLE` | — | 运动是否结束（**`IDLE` 返回 -1=到位 / 0=运动中**；`ISIDLE` 返回 0/1） |
| `ENABLED` | — | 是否已使能（1/0） |
| `ALARM` | — | 是否报警（1/0） |
| `AXISSTATUS` | `轴` | 轴状态字（ZBasic 手册 6.3 位表）；由内核按 CiA402 状态字组装，脚本用 `AXISSTATUS(n) and MV_ERRMASK` 判报警（位映射见 §8.2） |
| `SLOT_SCAN(0)` / `SCAN` | — | 总线扫描：触发重扫并等 `Shared::bus` 刷新；返回值 = 扫到的从站数（`0`=没扫到），供 `IF RETURN THEN` 判成败 |
| `SLOT_START(0)` | — | 总线开启：等总线进入 OP（`bus_ok`）且已有从站；返回值 = `1` 成功 / `0` 失败 |
| `SLOT_STOP(0)` / `BUSSTOP` | — | 总线停止（软停，语义同 `05` 的 `BUSSTOP;`），返回值 = `1` |
| `RAPIDSTOP` | — | 急停 |
| `BASE n` / `AXIS(n)` | `n` | 设置当前轴（命令后缀 `MOVEABS(x) AXIS(0)` 等效先 `BASE(0)`） |

**轴参数读写**（按 `05` 约定的参数语义，写为 `参数(轴)=值`，读为 `参数(轴)`）：
`SPEED` `ACCEL` `DECEL` `ATYPE` `UNITS` `DRIVE_PROFILE` `AXIS_ADDRESS`。

> ⚠`DRIVE_CONTROLWORD(轴)=值`（CiA402 控制字 `6040h`）是**赋值形式**，引擎会先按「设备参数写入」提交宿主；
> 宿主若不认识则**静默退化为数组赋值**（见 §2.2 返回 1）。故宿主**显式处理**：只接受脚本用到的标准序列
> `128`(0x80 清错→`FAULT_RESET`) / `6`(0x06 关机→去使能) / `15`(0x0F 使能)，其余值**明确报错**（不直接写控制字，
> 避免与内核使能状态机冲突）；读（1 参）无数据源恒 0。

> ⚠**轴号边界**：本控制器为**多轴内核**，运行时轴数由 `AXIS_COUNT` 决定（合法 1..8，默认 1=单轴）。
> `BASE`/`AXIS`、轴参数、`POS`/`MPOS`/`DPOS`/`IDLE`/`ISIDLE`/`AXISSTATUS`、`DISABLE_GROUP`
> 等带轴号的命令，**轴号 ∈ [0, 实际轴数-1] 作用于对应轴**；越界轴号一律报运行错误，
> 不会静默回读轴 0 的数据（否则脚本会读到错误的位置/状态，见 §8.4）。单轴配置（`AXIS_COUNT=1`）
> 下，轴 0 为唯一合法轴，行为与旧单轴版完全一致。

> `MOVE`/`MOVR` 是**相对**、`MOVEABS` 是**绝对**，与 ZBasic 一致；速度/加减速度缺省时取
> 当前轴的 `SPEED`/`ACCEL`/`DECEL` 参数。

### 2.1 端口 / 任务 / 总线（B 层，宿主导出）

| 命令 | 参数 | 说明 |
|------|------|------|
| `OPEN`（由 `OPEN #端口,…` 生成） | `端口, 类型[, 端口号][, IP]` | 打开通道：`TCP_SERVER`（监听）/ `TCP_CLIENT`（主动连远端） |
| `CLOSE` | `端口` | 关闭通道 |
| `PRINT`（由 `PRINT #端口,…` 生成） | `端口, 文本` | 向通道整包发送（不加换行） |
| `PUTCHAR`（由 `PUTCHAR #端口,…` 生成） | `端口, 字节串` | 原始字节发送（含 0 字节） |
| `GET`（由 `GET #端口, 数组…` 生成） | `端口, 数组名, 偏移, 最大字节数` | 非阻塞接收；宿主回传**字节串**，引擎写入数组并补 0；返回字节数 |
| `PORT_STATUS` | `通道` | 通道状态（1=已连接，0=未连接/未打开） |
| `PORT_TARGET` | `通道` | 通道对端（`ip:port` / `listen:port`，诊断用） |
| `PORT_MAX` | — | 端口通道号上限（脚本里通常是 `GLOBAL CONST`） |
| `PROC_STATUS` | `任务号` | 子任务是否处于「运行中」（1/0），供 `RUNTASK` 重投 |
| `RUNTASK` / `STOPTASK` | `任务号, 子程序名` | 登记/注销脚本子任务（子任务名在编译期转为字符串常量） |
| `MODBUS_REG` | `寄存器` | 16 位 4x 寄存器读写（写为 `MODBUS_REG(n)=值`） |
| `MODBUS_IEEE` | `寄存器` | 32 位浮点 4x 读写（占 n/n+1，低字在前，与 `05` 触摸屏字序约定一致） |
| `SCAN_EVENT` | `输入口` | 输入口扫描事件（本控制器无 IO 事件源，恒 0） |
| `NODE_COUNT` / `NODE_AXIS_COUNT` / `NODE_STATUS` / `NODE_IO` / `NODE_AIO` / `NODE_INFO` | `槽位[, 设备…]` | 总线设备/IO/AD-DA 信息（未扫描时返回 0） |
| `ETHERCAT` | `[节点]` | 总线诊断字符串（`?*ETHERCAT(ii)`） |
| `ECUSTOM` | `通道` | 自定义网口信息（本实现为其 `PORT_TARGET`） |
| `ETH_MODE` | `n` | 网口工作模式（本实现恒 0=传统模式） |
| `SDO_WRITE` | `节点, …, 数据` | **脚本层不可用**：需 EtherCAT 主站通道，脚本宿主不碰 `ecrt`；调用即**明确报错**（不静默退化，见 §8.4） |

**端口落地（`PortManager`）**：

- `OPEN #n,"TCP_SERVER",port`：`bind`+`listen`，`port=0` 时由内核分配（`listen_port()` 查询）；**单客户端**，
  断开后允许新客户端接入（脚本用 `PORT_STATUS` 守卫并自愈）。
- `OPEN #n,"TCP_CLIENT",port,"ip"`：非阻塞 `connect`，连接完成由 `PORT_STATUS`/`send`/`recv` 顺带推进。
- 全部 fd 非阻塞，`PortManager` 内**无线程、无 select 循环**——谁调用谁推进（与单线程解释器一致）。
- 通道**未 OPEN** 时：`send` 失败、`recv` 返回 -1、`GET/PRINT` 报运行错误（明确报错，不静默）。
- 通道已 OPEN 但**未连接/瞬时断开**：`GET` 返回 0、`PRINT/PUTCHAR` 静默丢弃（与 ZBasic 一致，脚本靠守卫重连）。

**Modbus 落地**：`MODBUS_REG`/`MODBUS_IEEE` 直接读写 `kx::Shared::mb_regs`（`recursive_mutex mb_mtx` 保护）。
脚本可与触摸屏共享同一份 4x 寄存器区：脚本写 `MODBUS_REG(4)`，屏在 4x4 即可读到（具体映射由 502 协议脚本实现，见 `05` §2）。

**任务落地**：本引擎是**单线程解释器**，`RUNTASK` 只做「登记 + 状态」（`Shared::task[8]`），
**不并发执行任务体**——并发处理（socket/Modbus/称重/机器人）由**脚本自行组织**（端口轮询 + `DELAY` + 状态机）。
原 4 个原生服务（`svc-tcp`/`svc-modbus`/`svc-scale`/`svc-robot`）**已从工程移除**。
`PROC_STATUS` 反映登记状态，供遗留脚本的「任务掉线则重投」逻辑使用。

**总线落地**：`NODE_*`/`SCAN_EVENT`/`ETHERCAT` 读取 `kx::Shared::bus` 快照。该快照由 **RT 线程每 ~100ms 调用
`EthercatMaster::scan_bus()`**（见 `main.cpp` 发布块）填充，取自**真实总线扫描结果**：

- `node_count` = 总线上**实际响应的从站数**（`BusState::slaves_responding`）；
- 逐节点读 SII 身份取 `NODE_STATUS`（从站 AL 状态，`0x08`=OP）；
- 本控制器为**多轴内核**（`AXIS_COUNT` 决定实际轴数，默认 1）：各轴从站位置由 `ECAT_SLAVE_POS_<n>`
  （或全局 `ECAT_SLAVE_POSITION`=轴0）给出，命中任一轴从站的节点 `NODE_AXIS_COUNT=1`，其余节点为 0；
- 无 IO/模拟量从站，故 `NODE_IO`/`NODE_AIO` 与 `NODE_INFO(...,10/11/12/13)`（IN/OUT/AD/DA 个数）恒 0；
- 总线无响应从站时 `node_count=0`（脚本侧 `NODE_*` 即返回 0）。

`?*ETHERCAT[(ii)]` 返回节点的 `axis/status/io` 摘要（诊断用）。扫描频率（~100ms）远低于 1ms RT 周期，
与 RT 循环既有 `ecrt_master_state`/`send` 等主站 ioctl 同量级，不影响实时性。

**等待语义（与 `05` 协议约定一致）**：定位命令先确认本轮命令生效（`move_result==RUNNING`），
再等终态 `DONE`/`ERROR`；未见到 `RUNNING` 时对上一轮遗留终态设 50ms「stale grace」。
超时取 `axis.timeout_ms + 2000`（应大于内核定位超时）。

**不静默原则**：总线未就绪下发 `MOVE/JOG`、`HOME`、寄存器/任务号越界、通道未打开等，
一律返回运行错误并带原因，脚本随即以 `RUNTIME_ERROR` 结束。

### 2.2 宿主接口约定（`ScriptHost::call`）

`int call(命令名, 参数, 返回值*, 错误*)`：

| 返回 | 含义 |
|------|------|
| `0` | 成功；返回值写入 `ret`（查询类命令用） |
| `1` | **不认识这个命令** → 引擎退化为「一维数组访问」（如 `A(i)`） |
| `2` | 运行错误（填 `err`），脚本以 `RUNTIME_ERROR` 结束 |
| `3` | 请求中止脚本（脚本以 `ABORTED` 结束） |

`print(行)` 是 `PRINT` 的落点；`aborted()` 供引擎周期性询问是否要停。

---

## 3. 运行方式

### 3.1 命令行工具 `script_run`（dry-run，不需要硬件）

```bash
./build/script_run demo.bas            # 语法检查 + 逻辑试跑（dry host 打印命令，不驱动设备）
./build/script_run demo.bas --check    # 只做语法检查
./build/script_run demo.bas --trace    # 单步跟踪
./build/script_run demo.bas --steps N  # 指定语句预算（默认 5,000,000）
./build/script_run demo.bas --quiet    # 不回显 dry 命令
./build/script_run demo.bas --sim-bus  # 模拟"总线已连上"，走通整条总线初始化逻辑
```

dry host 约定：查询类命令返回 0，动作类命令只打印不走设备。**拼错的命令会报错**（暴露笔误），
所以适合做脚本逻辑与语法回归；真正驱动设备请在控制器进程内运行（见下）。

`--sim-bus` 让 `SLOT_SCAN`/`SLOT_START` 返回成功、`NODE_COUNT`/`NODE_AXIS_COUNT`>0，
于是无需硬件即可离线走通「扫描 → 映射轴 → 开总线 → 清错 → 使能 → 主循环」的整条主程序逻辑：

```bash
./build/script_run ../../EtherCAT_SocketServer.bas --sim-bus --quiet --steps 600
# 输出应包含: 总线扫描成功…/ 轴号映射完成…/ 总线开启成功 / 驱动器错误清除完成 /
#             控制器错误清除完成 / 轴使能完成 …… 随后进入主循环（dry 下 PROC_STATUS 恒 0，
#             看门狗会反复"自动重启"任务，属预期）
```

### 3.2 主程序集成（真机执行）

在 `config/app.conf` 里配置：

```ini
# ---- ZBasic 子集脚本(可选) ----
SCRIPT_ENABLE      = 1                 # 1=启动后在独立线程自动跑 SCRIPT_FILE
SCRIPT_FILE        = /opt/kine-x/demo.bas
SCRIPT_WAIT_BUS    = 1                 # 先等总线上电就绪再执行
SCRIPT_WAIT_BUS_MS = 15000             # 等总线超时
SCRIPT_TRACE       = 0                 # 1=逐条打印执行语句
SCRIPT_MAX_STEPS   = 5000000           # 语句级步数预算
```

或命令行临时指定（`--script` 会同时把 `SCRIPT_ENABLE` 视为 1）：

```bash
./kine-x --script /opt/kine-x/demo.bas --script-trace
```

行为：

1. `kine-x` 起完 RT/SDO 线程后，另起**脚本线程**；
2. 若 `SCRIPT_WAIT_BUS=1`，先等 `bus_ok`（轮询 100ms，超时则跳过并告警）；
3. 读文件 → `compile` → `run`；`PRINT` 输出带 `[script]` 前缀写到 stdout；
4. 脚本只经 `kx::Shared` 下发 `AxisCmd`（控制器进程内**只有脚本线程**主动下发运动命令，无协议服务竞争命令槽）；
5. 进程收到 `SIGINT/SIGTERM` 时，`host->aborted()` 立即生效，脚本以 `ABORTED` 结束，不会卡住关机。

> 说明：**协议对接全部由脚本线程承担**（4 个原生服务已从工程移除）；同一时间只跑一个脚本引擎
> （BASIC / Lua 二选一），故不存在多个使用者争抢「覆盖式」命令槽的问题。

---

## 4. 状态、错误与安全

### 4.1 结束状态（`ScriptEngine::Status`）

| 状态 | 含义 |
|------|------|
| `READY` | 已编译、尚未运行 / 运行中 |
| `DONE` | 正常跑完（遇到 `END` 或语句耗尽） |
| `COMPILE_ERROR` | 语法错误（`error()` 带行号与原因） |
| `RUNTIME_ERROR` | 运行期错误（未定义标签、除零、命令报错…） |
| `ABORTED` | 被 `request_abort()` 或 `host->aborted()` 中止 |
| `BUDGET_EXCEEDED` | 超出语句步数预算（防死循环） |

### 4.2 双保险防死循环

- **步数预算**：`run(max_steps)`，默认 5e6 条语句；超出置 `BUDGET_EXCEEDED`。
- **可中止**：每执行 1024 条语句询问一次 `host->aborted()`；长循环、长 `DELAY` 都能被及时叫停。
  `MotionHost::sleep_ms` 以 10ms 为粒度响应中止。

### 4.3 调试支持（供调试软件扩展）

- `set_trace(bool)`：单步跟踪，输出到 `host->print`。
- `labels()`：脚本出现过的标签（做断点/跳转列表）。
- `get_var/set_var/list_vars/clear_vars`：查看/预置/列举变量（数组元素以 `名称[下标]` 呈现）。

---

## 5. 示例

```basic
' demo.bas —— 使能 -> 定位 -> 观察 -> 点动 -> 停止
PRINT "总线就绪=" + STR(BUS)

EN
MOVEABS 55, 100, 1000     ' 绝对定位到 55mm，速度 100mm/s，加速度 1000mm/s^2
PRINT "到位位置=" + STR(POS)

JOG 5                     ' 正向点动 5mm/s
DELAY 500
PRINT "点动中 BUSY=" + STR(BUSY)

STOP                      ' 停止运动（注意：这是设备命令，不是结束脚本）
DELAY 200
PRINT "停止后 IDLE=" + STR(IDLE)

FOR i = 0 TO 4
    PRINT "计数 " + STR(i)
NEXT i

END
```

> 想加「总线不就绪直接退出」的保护，用**单行 IF**：`IF BUS = 0 THEN END`。
> 注意块式 IF（`IF … THEN` 换行）**必须**以 `ENDIF` 收尾，且块内不能写 `END`（会提前结束脚本）——
> 与正运动 ZBasic「块式 if 必须配对 endif」的习惯一致。
>
> dry 试跑时查询类命令一律返回 0（上例输出 `总线就绪=0`、`到位位置=0`），这是 dry host 的正常行为。

---

## 6. 测试

```bash
cd lubancat2/build
cmake .. && make -j4
ctest -R 'script_test|motion_host_test|port_manager_test|config_test' --output-on-failure
```

- `script_test`：24 组用例，覆盖词法/语法/执行/数组/循环/IF/PRINT 尾分号/内置函数/位运算/`TICKS`/
  子程序（含 `LOCAL`/`EXIT SUB`/嵌套/递归防护）、**`GOTO`/`GOSUB`（本层返回地址栈、嵌套调用、顶层
  `RETURN` 明确报错）**、数组即字符串、**B 层端口 IO（`OPEN`/`PRINT#`/`GET#`/`PUTCHAR`、
  数组切片、`CRC16`、`?*`）**、`RETURN` 系统变量（命令返回值 → `IF RETURN THEN`）、步数预算/中止。
- `motion_host_test`：用假 RT 线程消费 `AxisCmd` 并刷新 `AxisStatus`，验证「`MOVEABS` 阻塞到到位」「`POS` 回读」
  「`MOVE`/`MOVR` 相对累加」「轴参数写读」「`IDLE` 到位返回 -1」，以及 **B 层**：「`MODBUS_REG`/`MODBUS_IEEE` 读写
  与 `Shared::mb_regs` 一致（含低字在前）」「`RUNTASK`/`STOPTASK`/`PROC_STATUS` 登记」「未扫描时 `NODE_COUNT`/`SCAN_EVENT`=0」
  「**填入 `Shared::bus` 快照后 `NODE_COUNT`/`NODE_AXIS_COUNT`/`NODE_STATUS`/`NODE_IO`/`NODE_AIO`/`NODE_INFO`/`?*ETHERCAT` 回读一致**」
  「**`SLOT_SCAN`/`SLOT_START`/`SLOT_STOP` 经 `RETURN` 判成败：扫到设备 `RETURN`=从站数、扫不到走 `ELSE` 分支**」
  「**`HOME` 软件回零到 0、`DATUM(0)`=清错（下发 `FAULT_RESET`，假 RT 计数验证真实生效）、
  `DATUM(3)`=当前位置置零（`POS()` 回读为 0）、其余模式明确报错（不静默退化）**」
  「**轴号边界明确报错**（轴数由 `AXIS_COUNT` 决定；`AXIS(n)`/`POS(n)`/`DPOS(n)`/`AXISSTATUS(n)`/`SPEED(n)=…`
  对越界 `n` 均 `RUNTIME_ERROR`，合法轴正常；默认单轴配置下仅轴 0 合法）」
  「**`VMOVE`/`CANCEL`/`DRIVE_CONTROLWORD`：`DRIVE_CONTROLWORD(0)=128` 真实触发 `FAULT_RESET`
  （不再静默退化为数组赋值）、非标准值与非 0 轴明确报错、`SDO_WRITE` 明确报错、总线未就绪 `VMOVE` 报错**」
  「寄存器/任务号越界明确报错」。
- `port_manager_test`：回环 socket 验证 `TCP_SERVER`（监听/接受/双向收发/含 0 字节原样发送/断开自愈/重连）
  与 `TCP_CLIENT`（非阻塞连接完成/双向收发），以及未打开通道 `send`/`recv` 的明确报错。
- 三个测试都不需要 IgH 与硬件。

整份遗留脚本的端到端验收：

```bash
./build/script_run ../../EtherCAT_SocketServer.bas --check     # 应输出 [编译通过]
```

---

## 7. 限制与后续

**已实现**：A1 语言核心、A1b 内置函数、A1c 设备宿主轴兼容、A2 子程序、A3 数组即字符串、
B1 端口 IO 语法、**B2 设备落地（端口=`PortManager` 真实 TCP；寄存器=`Shared::mb_regs` 与 502 共用；
任务=`Shared::task` 登记）、B3 总线落地（`Shared::bus` 由 RT 线程 `EthercatMaster::scan_bus()` 每 ~100ms
发布真实扫描结果）、B4 总线初始化命令（`SLOT_SCAN`/`SLOT_START`/`SLOT_STOP` + `RETURN` 系统变量，
使遗留脚本的 `slot_scan(0)`→`if return then` 分支按扫描结果正确走通）、
**B5 收尾（`DATUM` 显式处理；dry host 命令表与真实宿主对齐；新增 `--sim-bus` 使遗留脚本主程序
「扫描→映射轴→开总线→清错→使能→主循环」整条逻辑可离线试跑）**、
**C1 `AXISSTATUS` 真实位映射（内核 `Axis::tick()` 每拍按手册 6.3 组装，不再恒 0，见 §8.2）**、
**C2 `GOTO`/`GOSUB`/`RETURN`（本层返回地址栈；`RETURN` 三义自动区分）、
C3 `DATUM(3)` 位置置零 + C4 `HOME` 软件回零（内核新增 `AxisCmd::SET_POS` / `AxisCmd::HOME` 与坐标系偏移，
`POS`/`DPOS` 均随偏移回读，见 §8.1）、
多轴边界明确（`Shared::axis[]` 逐轴状态 + `MotionHost` `BASE`/`AXIS` 轴选择真正生效；`POS`/`AXISSTATUS`/轴参数/`AXIS` 等越界轴号报运行错误，不再静默回读轴 0 数据）、
**C5 控制字清错与速度命令真实化（内核新增 `AxisCmd::FAULT_RESET`；`DATUM(0)` 与
`DRIVE_CONTROLWORD(0)=128` 真实下发清错脉冲，`=6/15` 映射去使能/使能；`VMOVE`→`JOG`、`CANCEL`→`STOP`；
消除「`DRIVE_CONTROLWORD(0)=v` 赋值形式被静默退化为数组赋值」的缺口；`SDO_WRITE` 明确报错）**。
`tools/script_run ../../EtherCAT_SocketServer.bas --check` 已能**整份编译通过**（2020 行），
加 `--sim-bus` 可离线走通主程序启动路径。

| 限制 | 说明 / 后续 |
|------|-------------|
| `GOTO`/`GOSUB` 只在本层生效 | 从循环体内跳出会丢失该层循环状态；返回地址栈不跨层 |
| 仅一维数组、不检查边界 | 下标越界自动扩容，元素默认 0 |
| 无多文件 / `INCLUDE` | 单文件脚本 |
| 无 `INPUT` / 文件 IO | 由调试软件提供输入 |
| `STOP` 与「结束」语义区分 | 结束用 `END`；`END` 立即结束整个脚本（`Flow::F_STOP`） |
| 驱动器内部回零（`0x6098`）未定案 | `HOME` 已实现为**软件回零**（绝对定位到 `AxisConfig::home_mm`）；驱动器内部回零待 `0x6098` 定案后接入 |
| `DATUM` 支持模式 0 / 3 | `DATUM(0)`=清除控制器所有轴错误（显式接受；实际清错由 cia402 `6040=0x80` 承担，见 `04` §1.2）；`DATUM(3)`=当前位置置零（坐标系偏移）；其余模式未定案，明确报错 |
| `AXISSTATUS` 仅映射有数据源的位 | C1 已落地：bit2 通讯错 / bit3 驱动器报错 / bit8 随动超限 / bit22 告警输入；无数据源的位（bit1/bit4/bit5/bit9/bit10 等）恒 0，见 §8.2 |
| `RUNTASK` 不并发执行任务体 | 引擎单线程：只「登记 + 状态」；并发处理（socket/Modbus/称重/机器人）由脚本自行组织（端口轮询 + `DELAY` + 状态机）；原生服务已从工程移除，遗留脚本里同名任务体作为「参考实现」保留 |
| `SCAN_EVENT` 无事件源 | 本控制器无 IO 事件输入，恒返回 0 |
| 总线信息为多轴聚合快照 | `Shared::bus` 由 RT 线程每 ~100ms `scan_bus(cfg.ecat.slave_position, axis_count)` 发布：`node_count`=响应从站数，命中任一轴从站的节点 `axis_count=1`；本控制器无 IO/AD-DA，相关计数恒 0，未响应时 `NODE_*` 返回 0 |
| 多轴命令 | 本控制器为**多轴内核**（轴数由 `AXIS_COUNT` 决定，合法 1..8，默认 1）：`BASE`/`AXIS`、轴参数、`POS`/`MPOS`/`DPOS`/`IDLE`/`ISIDLE`/`AXISSTATUS`/`DISABLE_GROUP` 均按轴号选址生效；**越界轴号明确报错**，不静默回读轴 0 数据 |
| `ECUSTOM`/`ETH_MODE` 为占位 | `ECUSTOM(ch)` 回 `PortManager` 通道对端信息（= `PORT_TARGET`）；`ETH_MODE(n)` 恒 0。已核对 `netcfg`：只管理 eth1 的 **IP/掩码/网关**，**无「网口工作模式」数据源** |
| `DRIVE_CONTROLWORD` 只接受标准序列 | 脚本用它做「清错→关机→使能」（`128`/`6`/`15`），映射到内核 `FAULT_RESET`/`DISABLE`/`ENABLE`；其余控制字值**明确报错**——不直接写 `6040h`，避免与内核使能状态机冲突（见 §8.1 C5） |
| `SDO_WRITE` 脚本层不可用 | 需 EtherCAT 主站通道，脚本宿主不碰 `ecrt`；调用即明确报错（遗留脚本里它只在 `SCAN_EVENT` 恒 0 的调试分支），见 §8.4 |

后续若调试软件要做「脚本编辑器 + 断点/单步 + 变量窗口」，可直接复用本引擎的
`compile/run/request_abort/status/get_var/set_var/labels/set_trace` 接口，无需改动内核与 RT 线程。

---

## 8. 明确不做（C 层）

「C 层」指脚本引擎**刻意不实现、只做占位或不予承诺**的部分：要么属于外部工具/脚本层的职责，
要么依赖额外硬件/内核条件，要么与 ZBasic 全量语义冲突。集中登记于此，避免出现「看起来支持、实际静默」的假象。
需要从占位转为「要做」时，在本节登记并更新上文实现状态。

### 8.1 已落地的 C 层项

| 项 | 现状 |
|----|------|
| **C1 `AXISSTATUS(n)` 真实位映射** | **已实现**：内核 `Axis::tick()` 每拍按手册 6.3 组装状态字，`MotionHost` 直接回读（位表见 §8.2） |
| **C2 `GOTO` / `GOSUB` / `RETURN`** | **已实现**：`GOSUB` 压入本层返回地址栈，`Flow::F_RETURN` 弹栈回到调用点下一条；`RETURN` 三义（GOSUB 返回 / 子程序提前返回 / 层外报错）自动区分；顺带修复 `END`（`Flow::F_STOP`）此前未真正结束脚本的缺陷 |
| **C3 `DATUM(3)` 位置置零** | **已实现**：内核 `AxisCmd::SET_POS` 做**坐标系偏移**（不动电机），`POS`/`DPOS` 随偏移回读；宿主 `DATUM(3)` 下发并等轴空闲后返回 0 |
| **C4 `HOME` 软件回零** | **已实现**：内核 `AxisCmd::HOME` = 绝对定位到 `AxisConfig::home_mm`（默认 0）+ `home_speed`；宿主 `HOME` 下发并阻塞到到位。**驱动器内部回零（`6060=6` + `0x6098` 方式）仍为独立后续项** |
| **C5 控制字清错与速度命令真实化** | **已实现**：内核新增 `AxisCmd::FAULT_RESET`（发 CiA402 `6040h=0x80` 清错脉冲，瞬时完成供 `wait_move` 等到终态）。宿主 `DATUM(0)`、`DRIVE_CONTROLWORD(0)=128` 真实下发清错；`=6`→`DISABLE`、`=15`→`ENABLE`，其余值**明确报错**——由此消除 `DRIVE_CONTROLWORD(0)=v` 经 `ASSIGN_ARR` 静默退化为数组赋值的**唯一静默缺口**。同时 `VMOVE`→`JOG`（连续速度运动，方向由符号定）、`CANCEL`→`STOP`；`SDO_WRITE` 明确报错（需 `ecrt` 通道，见 §8.4） |

### 8.2 AXISSTATUS 位表（手册 6.3，仅列本控制器有数据源的位）

| 位 | 掩码 | 含义 | 数据源（内核 `Axis::tick`） |
|----|------|------|------------------------------|
| bit2 | `0x000004` | 与远程轴通讯出错 | 总线未就绪 / 从站不响应（`!bus_ok_`） |
| bit3 | `0x000008` | 远程驱动器报错 | CiA402 `6041.bit3` fault |
| bit8 | `0x000100` | 随动误差超限出错 | CiA402 `6041.bit13` following_error |
| bit22 | `0x400000` | 告警信号输入 | CiA402 `6041.bit7` warning |
| bit1 | `0x000002` | 随动误差超限告警 | 位定义保留；本控制器无独立告警源，恒 0 |
| bit4/5 | `0x10`/`0x20` | 正/反向硬限位 | 本控制器无硬限位输入，恒 0 |
| bit9/10 | `0x200`/`0x400` | 正/反向软限位 | 无软限位配置，恒 0 |

遗留脚本经 `GLOBAL CONST MV_ERRMASK = 6575932`（= `0x64573C`）筛位；脚本注释列出的是其中
「通讯(4)+驱动器(8)+硬限位(16+32)+随动出错(256)+软限位(512+1024)」（= `0x73C`）这一子集。
本控制器能置位的 bit2/bit3/bit8/bit22 **均落在该掩码内**，故 `(AXISSTATUS(0) and MV_ERRMASK) <> 0`
可正确感知通讯失败、驱动器报错、随动超限与告警输入；掩码内其余位（硬/软限位等）无数据源，恒 0，不影响判据。

### 8.3 语言 / 引擎不做项

| 不做项 | 处理方式 | 替代 |
|--------|----------|------|
| `INPUT` / 文件读写 / `INCLUDE` / 多文件 | 不实现 | 输入由调试软件提供 |
| 多维数组 / 下标边界检查 | 只支持一维，越界自动扩容 | 脚本自理 |
| `RUNTASK` 真正并发跑任务体 | 只「登记 + 状态」，解释器单线程 | 脚本自行组织（端口轮询 + `DELAY` + 状态机） |
| 动态字符串 / 结构体 / 指针 | 不支持 | 数组即字节串（A3），配 `CHR`/`ASC`/`VAL`/`STR` |

### 8.4 设备 / 现场条件占位项

> `HOME` 软件回零、`DATUM(3)` 位置置零与 C5 控制字清错**已落地**（见 §8.1 C3/C4/C5）；
> 其中 `DRIVE_CONTROLWORD` 仅支持标准清错/使能序列，`SDO_WRITE` 明确报错，二者仍部分受限，详见下表。
> 驱动器内部回零（`6060=6` + `0x6098` 回零方式）仍需 0x6098 定案，属**驱动器侧**后续项，不在脚本引擎内。

| 占位项 | 现状 | 转为「要做」的条件 |
|--------|------|--------------------|
| `SCAN_EVENT(in口)` | 恒 0 | 本控制器无 IO 事件输入源。脚本里 `SCAN_EVENT(in(n))` 是「面板输入口触发」调试分支；`in(n)` 在本控制器恒 0，故这些分支永不进入（安全，无副作用）。要做需真实 DI 硬件 |
| `ECUSTOM` / `ETH_MODE` | 占位：`ECUSTOM(ch)` 回通道对端信息（= `PORT_TARGET`）；`ETH_MODE(n)` 恒 0=传统模式 | 已核对 `src/common/netcfg.{h,cpp}`：它是 eth1 的 **IP 配置**后端（manual/dhcp），**没有「网口工作模式」这类数据源**；且 ZBasic 的 `ETH_MODE(0)` 指向 EtherCAT 口 eth0（受保护不可改）。要真实化需先定义「模式」语义并新增数据源 |
| `NODE_IO` / `NODE_AIO` / `NODE_INFO(…,10/11/12/13)` | 恒 0 | **代码路径已就绪**：直接读 `Shared::bus` 快照；本控制器实际无 IO/AD-DA 从站，故恒 0。接上此类从站即自动有值 |
| 多轴 `POS`/`DPOS`/`AXISSTATUS`/轴参数/`BASE`/`AXIS` | **已落地**：`Shared::axis[]` 逐轴状态 + `MotionHost` `BASE`/`AXIS` 轴选择真正生效；越界轴号宿主返回运行错误（不静默回读轴 0 数据） | 已扩展 `Shared` 为轴数组、`EthercatMaster` 支持多从站（单域多轴，`ECAT_SLAVE_POS_<n>`）；再增轴仅需加配置与从站 |
| `DRIVE_CONTROLWORD` 只接受标准序列 | **部分实现**：`DRIVE_CONTROLWORD(0)=v` 仅接受 `128`(清错)/`6`(关机)/`15`(使能)，分别映射 `FAULT_RESET`/`DISABLE`/`ENABLE`；其余控制字值 → 运行错误；读恒 0 | 要支持任意 `6040h` 位组合需先把内核使能状态机与「裸写控制字」解耦（当前直接写会与状态机冲突），或改由驱动器侧专用 SDO 通道 |
| `SDO_WRITE` | **脚本层不可用**：需 EtherCAT 主站通道，脚本宿主不碰 `ecrt`；调用即**明确报错**（遗留脚本里它只出现在 `SCAN_EVENT` 恒 0 的调试分支） | 若要支持，需为脚本引擎开放一条受控的 SDO 访问接口（含从站/索引/子索引/类型的校验），并明确其与内核状态机的交互边界 |

### 8.5 不予承诺（超出脚本引擎职责）

- **调试软件 UI**（脚本编辑器、断点/单步、变量窗口）：引擎已提供
  `compile` / `run` / `request_abort` / `status` / `get_var` / `set_var` / `list_vars` / `clear_vars` /
  `labels` / `set_trace` 接口（**已被 `script_test` 的变量/标签内省用例覆盖**），UI 由调试软件自行实现；
- **PREEMPT_RT / 硬实时**：脚本运行在独立业务线程，不做硬实时承诺；时间敏感逻辑应由 RT 内核承担；
- **ZBasic 全量命令**：只承诺本仓库脚本实际使用到的子集，未列出的命令**不会被碰巧执行**——写错命令会明确报错。
