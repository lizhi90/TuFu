# ZBasic 编程文档

> 适用：正运动 ZMC 控制器（ZMC3xx/4xx）+ EtherCAT 总线，本项目主程序 `EtherCAT_SocketServer.bas`。
> 手册：《ZBasic编程手册V3.3.0.pdf》（pdf_out/ 内有已提取章节文本）。

---

## 1. 语言规则要点

### 1.1 条件语句
- **必须用块式 if**：
  ```basic
  if cond then
      ...
  elseif cond2 then
      ...
  else
      ...
  endif
  ```
- 禁止"单行 then + 行内 endif"混合写法（`if x > 0 then y = 1 endif`）。这种写法会导致运行期配对错乱，报
  `ENDIF not paired with IF` / `ELSE or ELSEIF not paired with IF` 警告，分支行为不可预测。
- `goto 标号` 可以从嵌套 if 块内跳出（程序内有 `again:` 先例），用于命令解析失败提前退出：
  `goto cmd_end` → `cmd_end:` 独立一行放在 `END SUB` 前。

### 1.2 TICKS 倒数计数器（重要）
- TICKS 是**可赋值的倒数计数器**（1 tick≈1ms，随 SERVO_PERIOD 变），赋值 N 后每 tick 自动减 1，不是递增时钟。
- 计耗时必须**先取值减后取值**：`t0 = TICKS` → 之后 `t0 - TICKS` = 已流逝 ms。
  官方 MODBUS 例：`LASTTICK=TICKS ... ?LASTTICK-TICKS`。
- 写反成 `TICKS - t0` 恒为负，导致监测/超时永不执行（曾致"永远 busy"事故）。
- 典型用法：
  - 空闲超时：`if (cmdlen > 0) and (last_rx - TICKS > CMD_IDLE_MS) then`
  - 运动超时：`mvt0 - TICKS > MV_TIMEOUT`

### 1.3 数值解析
- `VAL` 遇字母/符号字符停止（如 `2000,100,55` → VAL 取 2000）。
- 负号、小数点必须手工解析（见程序 `get_num` 子程序模式）：
  - `get_num(0)` = 整串转数值；`get_num(idx)` = 第 idx 个逗号之后的字段
  - 负整数：跳负号取整数部分 `VAL` 后 `× -1`；小数：整数/小数部分拆开合并
- `STRFIND` 返回 0 起始索引，未找到返回 -1。

### 1.4 字符串与数组
- 字符串 `+` 拼接合法：`sta = "STA," + TOSTR(x,12,3)`。
- 数组即字符串，0 结尾：`CMDSTR(cmdlen) = 0`。
- 输出格式化：`TOSTR(值, 总宽, 小数位)`。

### 1.5 运动状态
- `IDLE(轴)`：运动中 = 0，结束 = -1。**IDLE=-1 时伺服静差可能未收敛，不要立即判位置**（见 §4.3）。
- `RAPIDSTOP(2)` = 全部轴停止。
- `AXISSTATUS(轴)` 位表见手册；本项目错误掩码：
  - `MV_ERRMASK = 6575932` = 通讯(4)+驱动器(8)+硬限位(16+32)+随动(256)+软限位(512+1024)+超频(4096)+机械手(16384)+电源(262144)+指令失败(2097152)+告警输入(4194304)

### 1.6 通讯（TCP 服务端）
- `OPEN #通道, "TCP_SERVER", 端口`；自定义网口通道 ZMC3xx/4xx 一般为 10/11（`?*PORT` 在线确认）。
- `GET #通道, 数组, 字节数` 非阻塞接收；`PRINT #通道, 字符串` 回发。
- `PORT_STATUS` 查连接状态；客户端断开后通道保持监听可直连。
- 任务控制：`STOPTASK n` + `RUNTASK n, 子程序`。

### 1.7 编码
- `.bas` / `.txt` 统一 **UTF-8 无 BOM**，勿转 GBK。

---

## 2. 单位与运动参数语义（关键，曾出双重换算事故）

- `UNITS(轴)` = 用户单位与脉冲的桥梁。设 `units(轴) = 脉冲当量(14043.41)` 后，**1 unit = 1 mm**。
- 因此：`MOVEABS / MOVE / SPEED / ACCEL / DECEL` 的参数**直接写 mm 数值**，`MPOS / DPOS` 返回值**直接就是 mm**。
- **禁止再乘或除 `PULSE_EQUIV`**——那是双重换算。
  双重换算的报错特征：`speed:19721736192 over max`（= 1404341 × 14043.41）。
- 历史教训：早期"蜗牛速"（SPEED=100 走 0.007mm/s）的真因是**总线未初始化（ATYPE=3）时 units() 映射段没执行、UNITS=1**，
  不是"需要乘换算"。排查单位问题先在线确认 `?UNITS(0)` 实际值。

---

## 3. ATYPE 掉电残留与自动运行

- 控制器重启后程序不自动运行时，ATYPE 停留在手动调试的旧值（如 3=脉冲轴），总线轴映射不生效。
- 解决：ZDevelop 中**下载程序到控制器并设置自动运行任务**，上电自动执行总线初始化。
- 程序内防御：扫描总线前清残留（手册例程范式）：
  ```basic
  for i=0 to 10
      ATYPE(i)=0
  next
  slot_scan(0)
  ```
- 扫描成功后逐轴设置：`ATYPE=65`、`units=PULSE_EQUIV`、`DRIVE_PROFILE=0`、`disable_group`、默认 SPEED/ACCEL/DECEL。

---

## 4. 事故案例与排查方法论

### 4.1 双重换算超限（move_error + over max）
- 现象：`MOVE:100` 后报 `speed/pulses over max`，数值 ≈ 1.97e10。
- 定位：报错数字反推 → `19721736192 = 1404341 × 14043.41`，即 mm×PULSE_EQUIV 后控制器内部又 ×UNITS。
- 修复：删除所有运动指令/位置读取处的 ×、÷ PULSE_EQUIV，只保留 `units(num)=PULSE_EQUIV` 一处设置。
- 方法论：**报错数字先做数值分析**（乘除关系），常能直接指出换算链。

### 4.2 ATYPE 变 3
见 §3。手动调试改过轴参数后，必须靠"下载+自动运行"保证上电状态一致。

### 4.3 到位误判 move_error（静差未收敛）
- 现象：控制器实际走到位，但返回 move_error。
- 定位：到位判断处加诊断打印
  `?"到位检查:MPOS:",MPOS(0),"DPOS:",DPOS(0),"目标:",mvpos,"位置差:",...,"STATUS:",AXISSTATUS(0),"ERR位:",(AXISSTATUS(0) and MV_ERRMASK)`
  → 数据显示 ERR位=0，位置差 0.336mm（DPOS 已到 100，MPOS=99.64）。
- 原因：IDLE 变 -1 瞬间伺服静差尚未收敛，立即读 MPOS 必然偏小。
- 修复：停止后进入 **200ms 到位确认期**（`mvwait` 记 TICKS），确认期满再读 MPOS 判断；确认期内新命令回 busy。
- 若确认期满仍有稳态静差：放宽容差 MV_TOL 或查驱动器增益/刚性。

### 4.4 if 配对警告
见 §1.1。块式 if 是唯一正确写法。

### 4.5 局部变量与全局同名（2052 Redim conflict）
- 现象：`init error:2052:Redim conflict` + 同语句后续名报 `2033:Unknown name`。
- 原因：`LOCAL a,b,c,spd,mode` 中 `spd` 已在全局 `GLOBAL DIM spd` 声明，ZBasic 不允许局部重定义全局名；
  该 `LOCAL` 语句解析中断，后面的 `mode` 被判为未定义（连带警告）。
- 修复：局部名加前缀避开全局名（如 `spd→mbmvspd`、`mode→mbmvmode`），并同步该任务内全部引用。
- 方法论：看到 `Redim conflict` 先查该名字是否已在 `GLOBAL DIM` 出现过；`Unknown name` 常是**前一个名字冲突的连带结果**。

---

## 5. 项目协议（EtherCAT_SocketServer.bas，192.168.1.11:4321）

### 5.1 命令表
| 命令 | 功能 |
|---|---|
| `HELLO;` | 应答 `HELLO ZMC` |
| `EN;` / `DIS;` | 全部轴使能 / 去使能 |
| `VJOG,<axis>,<spd>;` | 速度模式点动（spd 单位 mm/s，可负） |
| `STOP;` | 全轴急停 RAPIDSTOP(2) |
| `MA,<axis>,<pos>,<spd>;` | 绝对定位（mm，速度可省略） |
| `MR,<axis>,<dist>,<spd>;` | 相对定位（mm，速度可省略） |
| `STA;` | 应答各轴 MPOS/DPOS/IDLE（mm 数值） |
| `SCAN;` / `BUSSTOP;` | 重扫总线 / 停止总线 |
| `<数值>;` | 绝对运动：单值 = 位置 mm（默认速度/加速度） |
| `<acc>,<spd>,<pos>;` | 绝对运动：加速度 mm/s²，速度 mm/s，绝对位置 mm（如 `2000,100,55`） |
| `RSTA;` | 查询机器人状态：`RSTA,通道连接,通讯正常,报警,错误计数,报文计数,最近报文` |
| `RCMD,<内容>;` | 把命令原样透传给机器人（末尾补 LF 即 `\n`）；链路未连接应答 `ERR:ROBOT` |
| `RMON,<序号>;` | 单步测试：立即发监控表第 n 条（0~11），应答按监控路径解析并打印 |
| `RON;` / `ROFF;` | 机器人电机上电 / 下电（`motor_on` / `motor_off`） |
| `RSTART;` / `RSTOP;` | 程序启动 / 程序停止（`start` / `stop`） |
| `RMAIN;` | 程序指针回 main（`pp_to_main`） |
| `RCLEAR;` | 清除伺服报警（`clear_alarm`） |
| `RAUTO;` / `RMAN;` | 切自动 / 切手动模式（`switch_mode:auto` / `switch_mode:manual`） |
| `RDRAG;` / `RUDRAG;` | 打开拖动 / 关闭拖动（`open_drag` / `close_drag`） |
| `RLIST;` / `RCUR;` | 获取工程列表 / 当前工程（`list_prog` / `current_prog`，结果在报文里） |
| `RLOAD,<工程名>;` | 切换工程（`load_prog:<工程名>`） |

- 结束符：`;` 或 回车/换行；无结束符时 200ms 空闲超时按完整命令执行。
- 命令字后第一个逗号前是命令名（`MA`/`MR`/`VJOG`），数值命令无命令字。

### 5.2 数值命令状态机
```
收到数值命令 → bus_ok? → mvmode=1(busy) ? → 解析(单值/三段) → 设 SPEED/ACCEL/DECEL → MOVEABS
监测循环(每2ms): 发起50ms后开始看 IDLE
  IDLE=-1 → 记 mvwait → 200ms确认期 → 判: AXISSTATUS&掩码≠0 或 |MPOS-目标|>0.05mm → move_error，否则 move_done
  超时(10s) → RAPIDSTOP(2) → move_error
```
- 应答只有三种：`move_done`（运动完成后）、`move_error`、`busy`。不回显已解析的命令内容。

### 5.3 关键常量
```
COM_PORT=10  SVR_PORT=4321  PULSE_EQUIV=14043.41(脉冲/mm)
MVAXIS=0  MV_SPEED=100(mm/s)  MV_ACCEL=500(mm/s²)  MV_TOL=0.05(mm)  MV_TIMEOUT=10000(tick)
MV_SPD_MAX=3276.7(mm/s)  'HMI速度上限:int16上限32767÷10(屏端按1位小数存储)
'---- 机器人Socket(任务4):控制器=TCP客户端, 机器人=TCP服务端 ----
ROBOT_CH=12  ROBOT_PORT=4320  机器人IP=192.168.1.221
ROBOT_BOOT_WAIT=20000(ms,开机先等机器人启动)  ROBOT_RECONN_MS=5000(重连间隔)  ROBOT_POLL_MS=500(状态查询间隔)  ROBOT_TIMEOUT=2000(tick)
ROBOT_MON_N=12(监控量条数,须与ROBOT_KEYTAB段数一致)  ROBOT_ALARM_KEY="fault"(报警关键字)  ROBOT_DBG=1(协议测试期逐条打印收发,定案后改0)
ROBOT_KEYTAB="motor_on_state/robot_running_state/estop_state/operating_mode/home_state/fault_state/collision_state/task_state/cart_pos/jnt_pos/jnt_vel/jnt_trq"(★监控指令表,/分隔,按序轮询)
ROBOT_CTLTAB="motor_on/motor_off/pp_to_main/start/stop/clear_alarm/switch_mode:auto/switch_mode:manual/open_drag/close_drag/list_prog/current_prog"(★控制指令表,与RON/ROFF...一一对应)
```

### 5.4 机器人 Socket 任务（任务4）

- 链路方向与称重任务**相反**：机器人是 **TCP 服务端**、控制器是 **TCP 客户端**，即 `OPEN #12,"TCP_CLIENT",4320,"192.168.1.221"`；同样必须先 `PORT_STATUS(12)=1` 才收发。
- 上电流程：先等 `ROBOT_BOOT_WAIT`（20s，等机器人开机完毕）→ 按 `ROBOT_RECONN_MS`（5s）间隔重连 → 连上后每 `ROBOT_POLL_MS`（500ms）从 `ROBOT_KEYTAB` **按序取一条**监控指令发出（末尾补 `LF`，即 `\n`），12 条刷完一轮约 6s。
- ★**开机延迟必须"取差值"（TICKS 是倒数计数器）**：任务内 `robot_boott = TICKS` 记录启动时刻，判断 `robot_boott - TICKS < ROBOT_BOOT_WAIT` 表示"还在等"；写成 `robot_boott = ROBOT_BOOT_WAIT` 会让延迟直接失效。
- 拆包：应答按 `10(LF)/13(CR)/59(;)` 逐条成帧，写入 `ROBOT_MSG`（0 结尾）后交 `robot_parse()` 解析。
- 透传控制：PC 侧 `RCMD,<内容>;` → 逐字节拷入 `ROBOT_TXBUF` → `PRINT #12,ROBOT_TXBUF`（末尾补 `LF`）。**ZBasic 无 `MID$` 之类切片函数**，取 `CMDSTR` 第 5 字符起只能用 `for` 逐字符拷贝。
- 诊断：`RSTA;` 返回"连接/通讯正常/报警/错误计数/报文计数/最近报文"；`robot_lastmsg` 仅在报文**变化**时打印，避免周期刷屏。
- **监控（12 条轮询表，序号 0~11 对应 `robot_st(0..11)`）**：`motor_on_state` 上电 / `robot_running_state` 运行 / `estop_state` 急停 / `operating_mode` 模式 / `home_state` Home / `fault_state` 故障 / `collision_state` 碰撞 / `task_state` 任务 / `cart_pos` 笛卡尔位置 / `jnt_pos` 关节位置 / `jnt_vel` 关节速度 / `jnt_trq` 关节力矩。
- **解析（`robot_num()`）兼容两种应答**：①带字段名（先在报文里 `STRFIND` 到指令名，再解析其后的数值）②一问一答纯值（整条报文即数值）；`true`/`false` 也识别。数值提取**不能用 `VAL`**（遇符号即停），自行处理正负号与小数点。
- **控制（`ROBOT_CTLTAB` + 短命令）**：`RON/ROFF`、`RMAIN`、`RSTART/RSTOP`、`RCLEAR`、`RAUTO/RMAN`、`RDRAG/RUDRAG`、`RLIST/RCUR`、`RLOAD,<工程名>`；另有 `RCMD,<内容>` 通用透传。未连接时 `robot_pushcmd()` 拒发并应答 `ERR:ROBOT`。
- **报警汇总**：`fault_state(5)` / `estop_state(2)` / `collision_state(6)` 任一非 0，或报文命中 `ROBOT_ALARM_KEY`，即置 `robot_alarm`；**调整监控表顺序必须同步改这三个下标**。
- 协议适配只改四处：`ROBOT_KEYTAB`（监控指令表）、`ROBOT_CTLTAB`（控制指令表）、`ROBOT_ALARM_KEY`（报警关键字）、`robot_num()`（数值提取）；连接与重连机制无需改动。
- **协议测试（回复未知时）**：`ROBOT_DBG=1` 后下载，上电约 20s 连上即逐条打印三行一组：`机器人:★TX(监控n):<指令>` / `机器人:★RX <n>字节 HEX: ...`（`robot_dump_rx()` 打印原始字节，可看到 CR/LF 等不可见字符）/ `机器人:★报文[监控条n]=<报文> (解析值x 有效y)`。逐条对号入座用 `RMON,<序号>;`（0~11），发任意原始指令用 `RCMD,<内容>;`。

---

## 6. ModbusTCP 触摸屏（汇川 IT7000）通讯

- 控制器作 **Modbus-TCP 从站**（`PROTOCOL` 缺省=3），内置以太网口固定监听 502，无需 `OPEN`；
  站号由主程序 `ADDRESS=1` 设置，需与屏上"站号:1"一致。
- 处理任务：`GLOBAL SUB modbus_task()`，每循环约 5ms 扫描一次。
- 32 位数据字序**低字在前**（低字在低地址）；屏上该对象的字序项必须与此一致，
  否则位置非零时显示 `####` 或极大值（2026-09-26 实测定案）。
- **浮点对象占 2 个寄存器**：配置屏上地址时不能与其它对象重叠。

### 6.1 寄存器映射（4x 保持寄存器，编号从 0 起）

| 地址 | 类型 | 方向 | 说明 |
|---|---|---|---|
| `4x0` | int16 | 屏→控制器 | 输入值 → `hmi_val(0)` |
| `4x1~4x2` | float32 | 屏→控制器 | 输入值 → `hmi_val(1)` |
| `4x3` | int16 | 控制器→屏 | 状态字：bit0=有轴运动中 bit1=有轴报警 bit2=已使能 bit3=总线正常 |
| `4x4` | int16 | 控制器→屏 | 回写收到的 `4x0` |
| `4x5~4x6` | float32 | 控制器→屏 | 回写收到的 `4x1` |
| `4x7~4x8` | float32 | 控制器→屏 | **0 轴当前位置 `MPOS(0)`**（mm，可为负） |
| `4x9` | int16 | 控制器→屏 | `num`（总线轴数） |
| `4x10+4*i` / `4x12+4*i` | float32 | 控制器→屏 | MPOS / DPOS（各占 2 寄存器） |

命令写区（屏→控制器）：

| 地址 | 说明 |
|---|---|
| `4x60` | 命令字 bit0=使能 bit1=去使能 bit2=急停 bit3=重扫总线 |
| `4x61` | 目标轴号（0 开始） |
| `4x62` | 目标位置（float32 mm，占 62/63） |
| `4x64` | 定位速度：**int16，屏端按 1 位小数输入**（存值 = 显示值×10），程序 ÷10 还原 mm/s；上限 3276.7 |
| `4x65` | 模式 0=绝对 `MOVEABS` 1=相对 `MOVE` |
| `4x66` | 运动触发 0→1 上升沿执行一次 |
| `4x67` | 点动使能 1=点动 |
| `4x68` | 点动方向 1=正 -1=负 |
| `4x69` | 点动速度：同 `4x64`（1 位小数×10 存储，÷10 还原），上限 3276.7 |

机器人状态读区（控制器→屏）：

| 地址 | 说明 |
|---|---|
| `4x140` | 机器人状态字 bit0=通道已连接 bit1=通讯正常 bit2=报警 bit3=有命令待发 bit4=监控数据已建立 |
| `4x141` | 机器人错误计数（应答超时等累计） |
| `4x142` | 机器人累计收到报文数（诊断用） |
| `4x143` | 电机上电状态 `motor_on_state`（1=已上电） |
| `4x144` | 程序运行状态 `robot_running_state`（1=运行中） |
| `4x145` | 急停状态 `estop_state`（1=急停） |
| `4x146` | 工作模式 `operating_mode`（按机器人协议定义） |
| `4x147` | 故障状态 `fault_state`（0=正常） |
| `4x148` | 碰撞检测 `collision_state`（1=碰撞） |
| `4x149` | 运行任务状态 `task_state` |

### 6.2 接收变量与用法

接收（`modbus_task` 内，变化时才更新并打印）：
```basic
rd = MODBUS_REG(0)      '4x0 int16
rf = MODBUS_IEEE(1)     '4x1 float32(占 4x1/4x2)
' 变化时: hmi_val(n) = 新值; hmi_flag(n)=1; hmi_cnt(n)+1; ?"HMI数据..."
```

| 变量 | 含义 |
|---|---|
| `hmi_val(0)` / `hmi_val(1)` | 最后一次接收值（保持） |
| `hmi_flag(0)` / `hmi_flag(1)` | 1=有新数据未处理，**用完须清 0** |
| `hmi_cnt(0)` / `hmi_cnt(1)` | **值变化**计数：收到与上次**不同**的值才 +1（屏上持续写同一个值**不增长**，因此**不能当心跳**用；验证连通性必须"在屏上改值 + 看计数"） |

其它任务取用（新数据只处理一次）：
```basic
if hmi_flag(0) = 1 then
    hmi_flag(0) = 0
    ' 使用 hmi_val(0) ...
endif
```

回写（把收到的原值写回屏上显示）：`MODBUS_REG(4) = hmi_val(0)`、`MODBUS_IEEE(5) = hmi_val(1)`。

轴位置下发：`if num > 0 then MODBUS_IEEE(7) = MPOS(0) endif`（0 轴当前位置，mm）。
- `MPOS` 单位是 mm，改轴号把 `MPOS(0)` 换成 `MPOS(n)`；要跟随屏上"目标轴号"可写 `MPOS(MODBUS_REG(61))`（需先限幅 `0 <= ax < num`）。
- **float32 是带符号的，负数完全支持**（如 `-12.5`）；屏上把该对象设为"32位浮点、低字在前"即可显示负号。

初始化：`hmi_val(0/1)` 上电置哨兵值 `-99999`，保证触摸屏**首次写 0 也能被识别为新数据**。

### 6.3 常见坑
- 浮点对象占 2 个寄存器，地址**不可与其它对象重叠**：曾把 int16 放在 `4x3`，与 `4x2` 浮点的低字互相覆盖 → 浮点显示错误。
- 屏上地址常按 `40001` 编号，对应控制器 `MODBUS_REG(0)`（确认偏移是 +1 还是相等）。
- 显示负数位置时，屏上类型必须是**有符号/浮点**；选成"无符号整数(UINT16)"会显示成大正数。
- **HMI 速度寄存器（`4x64`/`4x69`）是 int16 且按 1 位小数存储**：屏端"小数位数"必须保持 1，改成 0 会导致速度再被除以 10（慢 10 倍）。`MODBUS_REG` 读回的是**有符号** int16，屏端输入 > 3276.7 时存值溢出（如 40000 → -25536），程序已用 `MV_SPD_MAX` 把"超上限"和"回绕成负"统一按 3276.7 处理。
- 判断通讯是否成功：ZDevelop 变量监视 `hmi_val()/hmi_cnt()`，或用 PC 端 Modbus 工具连 控制器IP:502、从站号 1 读写。
- **`4x142`（机器人报文计数）是 int16**：长期运行会超过 32767 回绕，仅作诊断参考，不要用于累计统计。
- **机器人控制走 PC 侧短命令或 `RCMD` 透传**，触摸屏无控制命令字（`load_prog:<工程名>` 这类字符串命令不适合直接放 Modbus 寄存器）。
- **机器人监控值刷新慢**：12 条轮询 × `ROBOT_POLL_MS`(500ms) ≈ 6s 才刷完一轮，`4x143~4x149` 有数秒滞后；急停/故障等安全信号不能只依赖本监控通道。
- **`ROBOT_KEYTAB` 段数必须等于 `ROBOT_MON_N`**（现为 12）：增删监控量必须两处同步，否则轮询会取到空指令或漏项。
- **报警下标与监控表顺序绑定**：`robot_parse()` 用 5/2/6 号（fault/estop/collision）汇总报警，调整顺序必须同步改下标。
- **ZBasic 标识符不区分大小写**：全局变量 `robot_msglen` 与常量 `ROBOT_MSGLEN` 被编译器视为同名 → 下载报 `init error 2052: Redim conflict`（并打印 `ROBOT_MSGLEN is already Const.`）。命名变量/常量时禁止只靠大小写区分（已改为 `robot_msgn`）。
- **`PORT_STATUS(通道号)` 越界会让任务直接停机**：下标超过控制器实际通道数时不是返回 0，而是 `error 2024: Array index over max` 并**停止整个任务**（如 `PORT_STATUS(12)`，实测本控制器最大下标为 11）。访问前先用 `?*PORT`（在线命令或程序内打印）确认真实通道表；程序里用 `PORT_MAX` 常量兜底判断。
- **自定义网口通道（ECUSTOM）才是可用网口通道**：`?*PORT` 中只有标 `ECUSTOM` 的通道能 `OPEN`，串口 0/1 的 `PORT_STATUS` 恒为 1 不代表可当网口用（`ETH` 通道是控制器自身的网口协议栈，用于 ZDevelop 在线/Modbus-TCP，不能当自定义 TCP 用）；一个通道同一时刻只能承载**一条** TCP 连接（服务端或客户端）。★自定义网口**数量由型号参数 `max_ethcustom` 决定、不能靠配置增加**（本机 `max_ethcustom=2`，端口表只列出 `Port:10-ECUSTOM` / `Port:11-ECUSTOM`），通道不够只能替换链路方向或介质，方案见 `README.md` 第八节。
- **注释只能以单引号 `'` 开头**：写成双引号开头（如 `if rc = 45 then "'-'`）会被当成字符串起始 → 下载报 `init warn 2046: Quotation not ended, need CR`，且字符串会一直吞到本行末尾（严重时跨行连累一大片误报 `Label name is invalid`）。快速排查：逐行统计 ASCII 双引号（34）个数，奇数即缺引号；`'` 注释内部的 `"` 不影响解析。
- **子程序 `LOCAL` 变量名不能与主程序用到的私有(全局)变量同名**：ZBasic 里未声明的变量是"私有变量"（全程序可见，等同全局）。若主程序（任务0）用了 `i`，而多个子程序又写 `LOCAL ch, rxnum, i`，控制器启动时会打印 `Local and Private name:I is same.`——两者可能共用同一存储，多任务并发时循环变量互相踩（表现为设备信息打印错位、称重移帧 `for i = 0 to scale_rxlen-total-1` 偶发错乱）。**命名纪律：主程序循环变量用 `ii`（或其它专用名），`i/j/k` 只允许出现在声明了 `LOCAL` 的子程序内部**。本项目主程序已统一改为 `ii`。
- **端口 500 是 ZDevelop 在线，不是业务连接**：`?PORT_TARGET(n)` 出现 `IP:高端口号` 时，先确认该通道是 **`ETH` 通道**（控制器自身网口协议栈，承载 **ZDevelop 在线 500** 与 **Modbus-TCP 从站 502**，**不能用 `OPEN`**）还是 **`ECUSTOM` 通道**（自定义 TCP 专用，本机仅 10/11）。曾把 ZDevelop 电脑 `192.168.1.22:64762` 的**在线连接**误判为"外部设备连上了 502 从站"，据此得出"从站服务已正常"的错误结论。
- **验证触摸屏是否连上 502（`hmi_cnt` 不是心跳）**：①`?*PORT` 看通道表（分清 `ETH` / `ECUSTOM`）+ `?PORT_STATUS(n)` 看连接状态 + `?PORT_TARGET(n)` 看对端 IP，出现触摸屏 IP 即已连上；②硬证据 = 在屏上**改一下 `4x0` 的值**，`?hmi_cnt(0)` 应自增（原因见 6.2 表：`hmi_cnt` 只在值变化时 +1）。屏上数值刷新异常时，可短暂断开 ZDevelop 在线再看是否恢复，以排除"在线链路占满 ETH 通道 / 与 502 从站相互干扰"。
