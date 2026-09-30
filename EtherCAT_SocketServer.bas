'''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''
'ZMC EtherCAT + TCP Socket Server
'xjw
'手动运动调试 + 远程Socket控制
'
'功能(本项目单轴: 总线仅1个轴, 轴号固定0):
'  1. EtherCAT总线初始化(同 EtherCAT.txt)
'  2. 自定义网口通道开启TCP服务端，端口4321
'     控制器IP: 192.168.1.11 (控制器网络参数中设置，本程序不改IP)
'  3. 上位机(PC客户端)连接后发ASCII命令(以 ; 或 回车/换行 结尾,或发送后空闲200ms自动按完整命令执行)：
'     HELLO;                     -> 应答 HELLO ZMC
'     EN;  / DIS;                -> 轴0使能 / 去使能
'     VJOG,<axis>,<speed>;       -> 轴0速度模式点动 (speed单位mm/s,可负; axis段保留但忽略)
'     STOP;                      -> 全轴急停 RAPIDSTOP(2)
'     MA,<axis>,<pos>,<speed>;   -> 轴0绝对定位(位置mm,速度mm/s可省略; axis段保留但忽略)
'     MR,<axis>,<dist>,<speed>;  -> 轴0相对定位(距离mm,速度mm/s可省略; axis段保留但忽略)
'     STA;                       -> 应答轴0 MPOS/DPOS/IDLE
'     <数值>;                    -> 绝对运动(单值=位置mm,速度/加速度用默认值)
'     <acc>,<spd>,<pos>;         -> 绝对运动:加速度mm/s^2,速度mm/s,绝对位置mm(如2000,100,55)
'                                   到位应答 move_done;出错/超时应答 move_error
'                                   (作用于MVAXIS轴;运动中再发应答busy)
'     SCAN;                      -> 重新扫描并启动总线
'     BUSSTOP;                   -> 停止总线
'     W;                         -> 查询称重重量 应答 WEIGHT:<值> (未接入应答 ERR:NOSCALE)
'     WSTA;                      -> 查询称重完整状态 WSTA,通讯正常,数据有效,通道连接,命令结果,错误计数,重量,原始重量,内码,零点内码
'     WT;                        -> 称重去皮     WTC;   -> 取消去皮
'     WZERO;                     -> 称重零点校准(内部自动先写0x0017=1关写保护,再写0x0016=1)
'     WCLR;                      -> 清除称重错误计数
'     WDIV,<系数>;               -> 设置重量比例系数(显示重量=原始重量/系数,默认100)
'     WSA,<站号>;                -> 设置称重模块Modbus站号(1~247,默认1)
'     RSTA;                      -> 查询机器人状态:RSTA,通道连接,通讯正常,报警,错误计数,报文计数,最近报文,MOTOR,RUN,ESTOP,MODE,FAULT,COLL,TASK
'     RCMD,<内容>;               -> 把命令原样透传给机器人(链路未连接时应答 ERR:ROBOT)
'     RMON,<序号>;               -> 单步测试:立即发监控表第n条(0~11)并打印应答,便于逐条对协议
'     RON; ROFF;                 -> 机器人电机上电/下电                        (motor_on / motor_off)
'     RMAIN; RSTART; RSTOP;      -> 程序指针回main / 程序启动 / 程序停止        (pp_to_main / start / stop)
'     RCLEAR;                    -> 清除伺服报警                              (clear_alarm)
'     RAUTO; RMAN;               -> 切自动模式 / 切手动模式                   (switch_mode:auto / switch_mode:manual)
'     RDRAG; RUDRAG;             -> 打开拖动 / 关闭拖动                       (open_drag / close_drag)
'     RLIST; RCUR;               -> 获取工程列表 / 当前工程                   (list_prog / current_prog,结果在报文里)
'     RLOAD,<工程名>;            -> 切换工程                                 (load_prog:<工程名>)
'                                   以上控制命令均应答 OK / ERR:ROBOT(链路未连接时拒发)
'  4. 输入口调试功能保留(in0~in7,in22,in23 同原程序)
'
'     所有应答(HELLO/move_done/move_error/busy/OK/ERR:*/STA)均以字面"/n"结尾,与上位机分割约定一致
'     服务端心跳:连接期间每2s主动发一帧"beat/n",客户端超时收不到即判失联(应答解析时跳过beat帧)
'注意：
'  - 自定义网口通道号用 ?*PORT 在线命令确认(ZMC3xx/4xx 一般为10/11)
'  - 客户端断开后通道保持监听，可直接重连；服务端永远开启(无CLOSE指令,监听常驻)
'  - Socket服务端在总线初始化之前就开启：总线扫描/开启失败重试期间4321照常监听,
'    客户端随时可连上(此时运动类命令回ERR:NOBUS/move_error,STA等查询命令正常应答)
'  - 任务0持续看门狗任务1,Socket任务意外停止时自动重启,收发/心跳不中断
'
'  5. 汇川IT7000触摸屏ModbusTCP通讯(任务2):
'     控制器=Modbus-TCP从站(服务端),内置以太网口固定监听502端口(无需OPEN);
'     IT7000触摸屏=Modbus-TCP主站(客户端),站号1(控制器ADDRESS=1)主动轮询;
'     PROTOCOL缺省=3(控制器为从端)。寄存器映射见 modbus_task 注释。
'
'  6. 称重传感器采集(任务3):
'     链路: 控制器(TCP客户端) -> TAS-LAN-869串口服务器(192.168.1.80:10123 TCP Server,
'           ★纯透传,未开Modbus TCP/RTU转换) -> RS485数字称重模块(站号1,9600,8,N,1)
'     控制器侧: OPEN #11,"TCP_CLIENT",10123,"192.168.1.80", 手工收发★Modbus-RTU★报文
'              帧=站号+功能码+数据+CRC16(低字节在前); 网上跑的就是485总线的原始字节,无MBAP头
'     注意: ①网关须为TCP Server/端口10123/串口9600 8N1(关闭心跳包与注册包);
'           ②调试助手用过的连接必须先断开, 否则控制器连不上(网关并发连接数有限);
'           ③★控制器必须先确认 PORT_STATUS(11)=1(已连接)才收发, 见 scale_task 第1步;
'           ④排查: 连4321发 "WSTA" 命令, 应答第3个字段=通道连接(1=已连上网关)
'
'  7. 机器人Socket通讯(任务4):
'     链路: 控制器(TCP客户端) -> 机器人(TCP服务端, 192.168.1.221:4320)
'     流程: 上电后先延迟 ROBOT_BOOT_WAIT(默认20s)等机器人启动完毕 -> 间隔 ROBOT_RECONN_MS(默认5s)重连
'           -> 连上后每 ROBOT_POLL_MS(默认500ms)按"监控指令表"轮询一条(指令+LF)
'           -> 应答按 10/13/59 拆包, 缓存到 ROBOT_MSG, 解析后存入 robot_st(序号)
'     监控: ROBOT_KEYTAB(初始化段)按序轮询12条: 电机上电/程序运行/急停/工作模式/Home/故障/碰撞/任务/
'           笛卡尔位置/关节位置/关节速度/关节力矩; 刷完一轮约 12*ROBOT_POLL_MS
'     控制: ①PC短命令(见第3节 RON/ROFF/RSTART...) ②RCMD,<内容> 通用透传(自动补LF)
'     ★协议适配: ROBOT_KEYTAB(监控指令表)/ROBOT_CTLTAB(控制指令表)/ROBOT_ALARM_KEY(报警关键字)/
'              robot_num()(数值提取) 按机器人实际协议调整, 连接与重连机制无需改动
'     ★应答格式: 兼容两种-①带字段名(先在报文找指令名, 再取其后的数值)②一问一答纯值(整条报文即数值)
'     触摸屏: 4x140状态字(bit0连接 bit1通讯正常 bit2报警 bit3有命令待发 bit4监控已建立)
'             4x141错误计数 4x142报文计数 4x143电机上电 4x144运行 4x145急停 4x146工作模式
'             4x147故障 4x148碰撞 4x149任务状态
'''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''

'---------------- 全局定义 ----------------
ERRSWITCH = 3				'全部错误信息输出

GLOBAL CONST DBG = 0			'★总调试开关: 1=打印命令/数值解析等调试信息; 0=正式运行(只保留状态与故障提示)
GLOBAL CONST COM_PORT = 10		'自定义网口通道1(ZMC3xx/4xx,用?*PORT确认)
GLOBAL CONST SVR_PORT = 4321		'TCP服务端监听端口
GLOBAL CONST CMDMAX = 100		'命令缓冲区大小
GLOBAL CONST RXGETMAX = 64		'单次接收字节数
GLOBAL CONST CMD_IDLE_MS = 200		'无结束符空闲超时(ms),超时按完整命令执行
GLOBAL CONST PULSE_EQUIV = 14043.41	'脉冲当量(脉冲/mm)
GLOBAL CONST MVAXIS = 0			'数值命令运动的轴号
GLOBAL CONST MV_SPEED = 100		'数值命令运动速度(mm/s)
GLOBAL CONST MV_TIMEOUT = 10000		'数值命令到位超时(tick≈1ms,调试期10s,可调大)
GLOBAL CONST MV_ACCEL = 500			'数值命令运动加速度(mm/s^2)
GLOBAL CONST MV_SPD_MAX = 3276.7		'★HMI速度上限(mm/s):屏端int16按1位小数存储(值×10),上限=32767/10
GLOBAL CONST MV_TOL = 0.05		'到位判断位置容差(mm)
'MV_ERRMASK错误位: 通讯(4)+驱动器(8)+硬限位(16+32)+随动出错(256)+软限位(512+1024)
'                  +超频(4096)+机械手(16384)+电源(262144)+指令失败(2097152)+告警输入(4194304)
GLOBAL CONST MV_ERRMASK = 6575932
GLOBAL CONST HB_MS = 2000		'服务端心跳间隔(ms),连接期间每此间隔发一次beat/n

GLOBAL DIM RXBUF(RXGETMAX)		'接收缓冲
GLOBAL DIM CMDSTR(CMDMAX)		'命令字符串(数组即字符串,0结尾)
GLOBAL DIM PSTR(CMDMAX)			'参数字段提取
GLOBAL DIM IPSTR(32)			'数值整数部分
GLOBAL DIM FPSTR(32)			'数值小数部分
GLOBAL DIM sta(300)			'状态应答字符串

GLOBAL DIM num			'总线轴数
GLOBAL DIM cmdlen, have_cmd		'命令长度/命令就绪标志
GLOBAL DIM last_rx			'最后收到字节的TICKS(空闲超时判断用)
GLOBAL DIM svr_open, bus_ok		'服务端已开/总线正常标志
GLOBAL DIM conn_old, rescan_flag	'连接状态/重扫总线标志
GLOBAL DIM beat_last		'上次心跳发出的TICKS(服务端心跳计时)
GLOBAL DIM numval, numsign		'数值解析结果/符号
GLOBAL DIM spd, spdabs			'运动参数
GLOBAL DIM mvmode, mvt0, mvpos, mvwait	'数值命令运动:等待标志/起始TICKS/目标位置/到位确认起始TICKS
GLOBAL DIM posval			'命令中的轴号/位置

'---- ModbusTCP触摸屏(IT7000)任务变量 ----
GLOBAL DIM mb_state, mb_cmdold, mb_trigold	'状态字/命令字旧值/触发旧值
GLOBAL DIM mb_jog, mb_jogold, mb_dir, mb_spd	'点动使能/旧值/方向/速度
GLOBAL DIM mb_tgt, mb_ax, mb_mode		'HMI目标位置/轴号/运动模式
GLOBAL DIM hmi_val(2)			'HMI接收值: [0]=4x0(40001) [1]=4x1(40002),保持最后一次接收值
GLOBAL DIM hmi_flag(2)			'HMI新数据标志: 1=有新值待处理(使用后由用户任务清0)
GLOBAL DIM hmi_cnt(2)			'HMI接收计数: 每收到一次变化(新数据)自增1

'---- 称重传感器(485数字称重模块, 经TAS-LAN-869网关做 Modbus-RTU转Modbus-TCP 服务端) ----
GLOBAL CONST SCALE_CH = 11		'自定义网口通道2,用作 Modbus TCP 客户端(用?*PORT确认)
GLOBAL DIM SCALE_IP(16)			'网关(TAS-LAN-869)IP(字符串只能用数组,CONST不支持字符串)
GLOBAL CONST SCALE_PORT = 10123		'网关 TCP Server 监听端口
GLOBAL CONST SCALE_TXLEN = 8		'Modbus RTU 请求帧长(站号1+功能码1+数据4+CRC2)
GLOBAL CONST SCALE_RXMAX = 64		'接收缓冲大小
GLOBAL CONST SCALE_POLL_MS = 200		'轮询周期(ms)
GLOBAL CONST SCALE_TIMEOUT = 1000		'单次应答超时(tick≈1ms)
GLOBAL CONST SCALE_RECONN_MS = 5000	'未连接时的重连尝试间隔(ms);拉长避免打断正在建立的连接
GLOBAL CONST SCALE_DBG = 0		'1=打印收发报文(16进制)便于排查;0=正式运行(已定案,默认关)
GLOBAL CONST SCALE_LOG_MS = 3000	'★周期日志最小打印间隔(ms): 超时/非法帧等不再逐条刷屏,每3秒才输出一条完整快照

'---- 机器人Socket通讯(机器人=TCP服务端, 控制器=TCP客户端) ----
GLOBAL CONST PORT_MAX = 11		'★本控制器 PORT_STATUS 最大可用下标=11(端口表只到11;自定义网口 ECUSTOM 只有 10/11 两个)
GLOBAL CONST ROBOT_CH = 12			'★机器人通道:★本控制器不存在12号通道(ECUSTOM只有10/11且已被占用)→当前是占位值,先按第八节方案腾出通道后再改这里
GLOBAL CONST ROBOT_PORT = 4320		'机器人TCP服务端监听端口
GLOBAL CONST ROBOT_BOOT_WAIT = 20000	'★开机等待机器人启动完毕时间(ms):上电后先等这么久再发起首次连接
GLOBAL CONST ROBOT_RECONN_MS = 5000	'未连接时的重连尝试间隔(ms)
GLOBAL CONST ROBOT_POLL_MS = 500		'已连接时的状态查询间隔(ms)
GLOBAL CONST ROBOT_TIMEOUT = 2000	'单次状态查询的应答超时(tick≈1ms)
GLOBAL CONST ROBOT_RXMAX = 128		'接收缓冲大小
GLOBAL CONST ROBOT_MSGLEN = 120		'单条报文缓存长度
GLOBAL CONST ROBOT_TXMAX = 120		'透传命令缓冲长度
GLOBAL CONST ROBOT_DBG = 1			'★1=打印机器人收发报文(HEX+文本,协议测试期用);0=正式运行(协议定案后改回)
GLOBAL CONST ROBOT_LOG_MS = 3000		'★周期日志最小打印间隔(ms): 超时等不再逐条刷屏
GLOBAL CONST ROBOT_MON_N = 12			'★状态监控量个数(轮询表条数,必须与ROBOT_KEYTAB一致)
GLOBAL CONST ROBOT_STMAX = 16			'状态值数组长度(需 >= ROBOT_MON_N)
GLOBAL CONST ROBOT_CMDMAX = 40			'控制命令拼装缓冲长度
GLOBAL DIM SCALE_TX(16)			'发送缓冲(Modbus RTU 请求帧,实发8字节)
GLOBAL DIM SCALE_RX(SCALE_RXMAX)	'接收缓冲(粘包/分包重组用)
GLOBAL DIM scale_rxlen			'接收缓冲中已有效字节数
'注: TAS-LAN-869 是串口透传(网口字节==485上的Modbus-RTU帧), RTU无事务标识,
'    靠"站号+功能码+CRC16"配对请求与应答, 故不需要 TID
GLOBAL DIM scale_opened			'通道是否已OPEN
GLOBAL DIM scale_conn			'链路是否已连接(1=PORT_STATUS为1)
GLOBAL DIM scale_ok			'通讯正常(收到过合法应答且未超时)
GLOBAL DIM scale_valid			'数据有效(至少收到过一帧重量)
GLOBAL DIM scale_err			'错误计数(超时/异常应答/校验失败)
GLOBAL DIM scale_t0			'本帧发送时刻TICKS(超时判断)
GLOBAL DIM scale_send		'本轮已发出请求标志
GLOBAL DIM scale_lastpoll		'上次轮询时刻TICKS
GLOBAL DIM scale_reconn		'上次重连尝试时刻TICKS
GLOBAL DIM scale_connold		'上次链路连接状态(变化时打印提示)
GLOBAL DIM scale_try			'连接重试次数(未连上时周期性提示)
GLOBAL DIM scale_logt			'上次打印周期日志的TICKS(发送/超时侧限频用)
GLOBAL DIM scale_rxlogt			'★接收侧独立的日志限频基准: 与发送共用时,健康链路上RX打印总被先到的TX抢掉窗口
GLOBAL DIM scale_tocnt			'本日志窗口内累计超时次数(限频打印时才输出)
GLOBAL DIM scale_badcnt			'本日志窗口内累计丢弃的非法字节数(限频打印时才输出)
GLOBAL DIM scale_raw			'原始重量(int32, 称重模块内部计数)
GLOBAL DIM scale_ad			'内码值(int32, AD码, 诊断用)
GLOBAL DIM scale_zero		'零点内码(int32, 校准时记录)
GLOBAL DIM scale_w			'换算后重量(显示单位, = raw/scale_div)
GLOBAL DIM scale_div			'重量比例系数(默认100.0, 即读到10000表示100.00)
GLOBAL DIM scale_divsave		'比例系数上次下发值(检测HMI是否修改)
GLOBAL DIM scale_station		'称重模块站号(默认1)
GLOBAL DIM scale_stasave		'站号上次下发值
GLOBAL DIM scale_cmd			'称重命令字: 1=去皮 2=取消去皮 3=零点校准 4=清错误计数
GLOBAL DIM scale_cmdold		'称重命令字旧值(上升沿)
GLOBAL DIM scale_busy			'称重命令执行中(等待应答)
GLOBAL DIM scale_result		'称重命令结果: 0=空闲 1=执行中 2=成功 3=失败
GLOBAL DIM scale_wrreg		'待写入称重模块的寄存器地址(-1=无)
GLOBAL DIM scale_wrval		'待写入称重模块的寄存器值
GLOBAL DIM scale_tmp			'32位解析临时值
GLOBAL DIM SCALE_Q(64)		'待发写命令队列: 每条4字节(地址高,地址低,值高,值低)
GLOBAL DIM scale_qn			'队列中待发命令条数
GLOBAL DIM scale_poll		'轮询周期(ms,触摸屏可改)

'---- 机器人任务变量 ----
GLOBAL DIM ROBOT_IP(16)			'机器人IP(字符串只能用数组)
GLOBAL DIM ROBOT_KEYTAB(320)		'★状态监控指令表(/分隔,robot_task按序轮询,见初始化段)
GLOBAL DIM ROBOT_CTLTAB(220)		'★控制指令表(/分隔,与PC短命令RON/ROFF...一一对应)
GLOBAL DIM ROBOT_CMDBUF(ROBOT_CMDMAX)	'控制命令拼装缓冲(命令字写入后统一补LF发出)
GLOBAL DIM ROBOT_ALARM_KEY(16)		'★报警关键字(在应答报文中查找,命中则置报警位)
GLOBAL DIM ROBOT_RX(ROBOT_RXMAX)	'接收缓冲
GLOBAL DIM ROBOT_MSG(ROBOT_MSGLEN)	'最近一条完整报文(0结尾,供查询/解析)
GLOBAL DIM ROBOT_TXBUF(ROBOT_TXMAX)	'待发给机器人的命令缓冲(数组=可存LF等不可见字节)
GLOBAL DIM robot_msgn		'ROBOT_MSG中已存字节数(★勿写成ROBOT_MSGLEN: ZBasic不区分大小写,会与上面同名常量冲突)
GLOBAL DIM robot_rxlen			'ROBOT_RX中已存字节数
GLOBAL DIM robot_txlen			'ROBOT_TXBUF中待发长度(0=无)
GLOBAL DIM robot_opened		'通道是否已OPEN
GLOBAL DIM robot_conn			'链路是否已连接(1=PORT_STATUS为1)
GLOBAL DIM robot_connold		'上次链路状态(变化时打印)
GLOBAL DIM robot_ok			'通讯正常(收到过应答且未超时)
GLOBAL DIM robot_alarm		'机器人报警(报文命中ROBOT_ALARM_KEY)
GLOBAL DIM robot_err			'错误计数(超时等)
GLOBAL DIM robot_msgcnt		'累计收到报文条数
GLOBAL DIM robot_send			'本轮已发出查询(等应答)
GLOBAL DIM robot_t0			'本轮发出时刻TICKS
GLOBAL DIM robot_lastpoll		'上次轮询时刻TICKS
GLOBAL DIM robot_reconn		'上次重连尝试时刻TICKS
GLOBAL DIM robot_try			'连接重试次数
GLOBAL DIM robot_lastrx		'最后收到应答的时刻TICKS
GLOBAL DIM robot_logt			'日志限频基准TICKS
GLOBAL DIM robot_boott			'★任务启动时刻TICKS(开机延迟判断: robot_boott-TICKS < ROBOT_BOOT_WAIT 表示还在等)
GLOBAL DIM robot_lastmsg(ROBOT_MSGLEN)	'上一条报文(仅用于"状态变化才打印")
GLOBAL DIM robot_st(ROBOT_STMAX)		'★各监控量最新数值(序号对应ROBOT_KEYTAB)
GLOBAL DIM robot_stv(ROBOT_STMAX)		'各监控量是否已取到值(1=已取到)
GLOBAL DIM robot_curk			'当前正在查询/解析的监控量序号
GLOBAL DIM robot_numval			'robot_num()解析出的数值
GLOBAL DIM robot_numok			'1=本次解析到有效数值
GLOBAL DIM robot_moncnt			'已完成一轮轮询的次数(诊断)
GLOBAL DIM robot_espermon		'本轮发出的是监控查询(1)还是透传控制命令(0)
GLOBAL DIM robot_pushok			'控制命令入队结果(1=已入队, 0=机器人未连接未发)

cmdlen = 0
have_cmd = 0
svr_open = 0
bus_ok = 0
conn_old = 0
rescan_flag = 0
mvmode = 0
mvwait = 0
hmi_val(0) = -99999				'HMI接收初值(哨兵值):保证触摸屏首次写0也被当成新数据
hmi_val(1) = -99999
scale_rxlen = 0
scale_opened = 0
scale_conn = 0
scale_ok = 0
scale_valid = 0
scale_err = 0
scale_send = 0
scale_busy = 0
scale_result = 0
scale_raw = 0
scale_ad = 0
scale_zero = 0
scale_w = 0
scale_div = 100.0				'重量比例: 模块"10000"对应显示100.00
scale_divsave = 100.0
scale_station = 1				'称重模块站号(出厂默认1)
scale_stasave = 1
scale_wrreg = -1				'无待写命令
scale_qn = 0					'称重写命令队列为空
scale_poll = SCALE_POLL_MS		'轮询周期默认200ms
scale_connold = 0
scale_try = 0
scale_logt = TICKS				'发送/超时日志限频基准时刻
scale_rxlogt = TICKS			'接收日志限频基准时刻
scale_tocnt = 0
scale_badcnt = 0
SCALE_IP = "192.168.1.80"		'网关IP(仅用于打印;OPEN的IP参数直接用字符串字面量)

'---- 机器人通讯初始化(任务4) ----
ROBOT_IP = "192.168.1.221"		'机器人IP(仅用于打印;OPEN的IP参数直接用字符串字面量)
'★状态监控指令表(/分隔), 序号0~11与下列监控量一一对应, 顺序勿随意调整(前4条会映射到触摸屏):
'  0=motor_on_state 电机上电  1=robot_running_state 运行  2=estop_state 急停  3=operating_mode 工作模式
'  4=home_state Home输出  5=fault_state 故障  6=collision_state 碰撞  7=task_state 任务状态
'  8=cart_pos 笛卡尔位置  9=jnt_pos 关节位置  10=jnt_vel 关节速度  11=jnt_trq 关节力矩
ROBOT_KEYTAB = "motor_on_state/robot_running_state/estop_state/operating_mode/home_state/fault_state/collision_state/task_state/cart_pos/jnt_pos/jnt_vel/jnt_trq"
ROBOT_ALARM_KEY = "fault"		'★报警关键字: 报文命中即置报警位(现用故障字段名,按机器人实际协议改)
robot_curk = 0				'从监控表第0条开始轮询
robot_moncnt = 0
robot_pushok = 0
robot_espermon = 0
'★控制指令表(/分隔), 序号与PC短命令一一对应:
'  0=motor_on(RON) 1=motor_off(ROFF) 2=pp_to_main(RMAIN) 3=start(RSTART) 4=stop(RSTOP)
'  5=clear_alarm(RCLEAR) 6=switch_mode:auto(RAUTO) 7=switch_mode:manual(RMAN)
'  8=open_drag(RDRAG) 9=close_drag(RUDRAG) 10=list_prog(RLIST) 11=current_prog(RCUR)
ROBOT_CTLTAB = "motor_on/motor_off/pp_to_main/start/stop/clear_alarm/switch_mode:auto/switch_mode:manual/open_drag/close_drag/list_prog/current_prog"
robot_boott = TICKS				'★任务启动时刻(任务内会再取一次);开机后等 ROBOT_BOOT_WAIT ms 再连接
robot_opened = 0
robot_conn = 0
robot_connold = 0
robot_ok = 0
robot_alarm = 0
robot_err = 0
robot_msgcnt = 0
robot_send = 0
robot_txlen = 0
robot_msgn = 0
robot_rxlen = 0
robot_reconn = TICKS
robot_lastpoll = TICKS
robot_lastrx = TICKS
robot_logt = TICKS

wa 1000					'等待驱动器设备上电完成

'---------------- ModbusTCP从站参数(汇川IT7000触摸屏主站轮询) ----------------
ADDRESS = 1					'控制器Modbus站号,与IT7000"站号:1"对应(缺省即1)
'ETH_MODE(0) = 0				'首个网口传统模式,极少数触摸屏不兼容eth模式时可打开

'---------------- 开启TCP服务端(总线初始化之前,保证4321永远在线) ----------------
'总线扫描/开启失败重试期间端口照常监听,客户端可连上(此时命令回ERR:NOBUS/move_error)
if svr_open = 0 then
	'IP_ADDRESS = "192.168.1.11"	'如需程序内固定IP可打开(一般用控制器网络参数设置)
	OPEN #COM_PORT, "TCP_SERVER", SVR_PORT
	svr_open = 1
	?"Socket服务端已开启","端口："SVR_PORT
endif
STOPTASK 1
RUNTASK 1, socket_task			'启动Socket接收任务
?"Socket接收任务已启动"

STOPTASK 2
RUNTASK 2, modbus_task			'启动ModbusTCP触摸屏任务(502端口由固件自动监听)
?"Modbus触摸屏任务已启动"

STOPTASK 3
RUNTASK 3, scale_task			'启动称重采集任务(ModbusTCP客户端->网关->485称重模块)
?"称重采集任务已启动"

STOPTASK 4
RUNTASK 4, robot_task			'启动机器人通讯任务(TCP客户端->机器人4320, 延迟等待+间隔重连)
?"机器人通讯任务已启动"

again:

bus_ok = 0					'重扫期间屏蔽远程运动命令(任务1仍在线)

RAPIDSTOP(2)
wait idle

for ii=0 to 10				'清除轴类型残留(手册例程范式),防旧值干扰
	ATYPE(ii)=0			'★主程序(任务0)循环变量用ii: 不能再用i, 否则与各子程序的LOCAL i同名,
						'  控制器报"Local and Private name:I is same", 两者可能共用存储→跨任务互踩
next

slot_scan(0)				'开始扫描

if return then

	?"总线扫描成功","连接设备数："NODE_COUNT(0)
	?
	if NODE_COUNT(0) < 1 then		'扫描动作成功但一个从站都没扫到(驱动器未就绪),不设参数不放行
		?"未扫到任何设备,2s后重试"
		delay(2000)
		goto again
	endif
	?"开始映射轴号","(本项目单轴,仅轴号0)"
	num=0

	'---- 本项目单轴: 只映射轴0(取第一个带驱动器的从站设备) ----
	for ii=0 to NODE_COUNT(0)-1
		if (num = 0) and (NODE_AXIS_COUNT(0,ii) > 0) then
			AXIS_ADDRESS(0)=1				'映射轴号0
			ATYPE(0)=65
			units(0)=PULSE_EQUIV			'脉冲当量(脉冲/mm)
			DRIVE_PROFILE(0)=0				'设置功能
			disable_group(0)				'单独分组
			SPEED(0)=100					'缺省速度/加速度
			ACCEL(0)=1000
			DECEL(0)=1000
			num=1							'总轴数(本项目单轴,固定1)
		endif
	next

	?"轴号映射完成","连接总轴数："num
	?
	delay(100)

	SLOT_START(0)

	if return then
	?"总线开启成功"
	?

	?"开始清除驱动器错误(根据驱动器数据字典设置)"
	DRIVE_CONTROLWORD(0)=128				'根据驱动器数据字典
	delay(10)
	DRIVE_CONTROLWORD(0)=6
	delay(10)
	DRIVE_CONTROLWORD(0)=15
	delay(10)

	?"驱动器错误清除完成"
	delay(100)
	?

	?"清除控制器错误"
	datum(0)
	?"控制器错误清除完成"
	?

	delay(100)

	?"轴使能准备"
	base(0)
	AXIS_ENABLE=1
	wdog=1									'使能总开关
	?"轴使能完成"
	?

	bus_ok = 1

	else
	?"总线开启失败,2s后重试"
	delay(2000)
	goto again
	endif

	while 1
		'---- 远程命令请求重扫总线 ----
		if rescan_flag = 1 then
			rescan_flag = 0
			mvmode = 0
			goto again
		endif

		'---- 服务端看门狗:任务1意外停止(运行错误等)时自动拉起,保证4321永远开启 ----
		if PROC_STATUS(1) <> 1 then
			?"警告:Socket任务已停止,自动重启"
			STOPTASK 1
			RUNTASK 1, socket_task
		endif

		'---- Modbus任务看门狗:任务2意外停止时自动拉起,保证触摸屏通讯不中断 ----
		if PROC_STATUS(2) <> 1 then
			?"警告:Modbus任务已停止,自动重启"
			STOPTASK 2
			RUNTASK 2, modbus_task
		endif

		'---- 称重任务看门狗:任务3意外停止时自动拉起,保证重量采集不中断 ----
		if PROC_STATUS(3) <> 1 then
			?"警告:称重任务已停止,自动重启"
			STOPTASK 3
			RUNTASK 3, scale_task
		endif

		'---- 机器人任务看门狗:任务4意外停止时自动拉起,保证机器人监控不中断 ----
		if PROC_STATUS(4) <> 1 then
			?"警告:机器人任务已停止,自动重启"
			STOPTASK 4
			RUNTASK 4, robot_task
		endif

		'---- 输入口调试(同原程序) ----
		if SCAN_EVENT(in(0))>0 then			'in0口查看每个设备带驱动器个数
			?
			for ii=0 to NODE_COUNT(0)-1
			?"设备号："ii,"带驱动器个数："NODE_AXIS_COUNT(0,ii),"设备状态："NODE_STATUS(0,ii)
			next

		elseif SCAN_EVENT(in(1))>0 then			'in1口查看io及ad/da参数
			?
			?
			for ii=0 to 	NODE_COUNT(0)-1
			?"设备号："ii,"起始IO编号："NODE_IO(0,ii),"IN个数："NODE_INFO(0,ii,10),"OP个数："NODE_INFO(0,ii,11)
			?,,"起始AD/DA："NODE_AIO(0,ii),"AD个数："NODE_INFO(10,ii,12),"DA个数："NODE_INFO(10,ii,13)
			?
			next

		elseif SCAN_EVENT(in(2))>0 then
			?
			for ii=0 to NODE_COUNT(0)-1
				?*ETHERCAT(ii)
				?
			next

		elseif SCAN_EVENT(in(3))>0 then			'查看所有设备状态
			?
			?*ETHERCAT

		elseif SCAN_EVENT(in(4))>0 then
			vmove(1)axis(0)

		elseif SCAN_EVENT(in(5))>0 then
			vmove(-1)axis(0)

		elseif SCAN_EVENT(in(6))>0 then
			RAPIDSTOP(2)

		elseif SCAN_EVENT(in(7))>0 then
			SDO_WRITE (0,0,$6060,0,2,0)
			?11

		elseif SCAN_EVENT(in(22))>0 then
			goto again
			?"启动总线，等待1s"
			delay(1000)

		elseif SCAN_EVENT(in(23))>0 then
			SLOT_STOP(0)
			?"停止总线"

		endif

	wend

else
	?"总线扫描失败,2s后重试"
	delay(2000)
	goto again
endif

end

'=================== Socket接收任务 ===================
'任务1：非阻塞轮询接收，遇 ; 或 回车/换行 组成完整命令后执行
GLOBAL SUB socket_task()
LOCAL ch, rxnum, i
while 1
	'连接状态监测
	if PORT_STATUS(COM_PORT) <> conn_old then
		conn_old = PORT_STATUS(COM_PORT)
		if conn_old = 1 then
			?"Socket客户端已连接"
			PRINT #COM_PORT,"HELLO ZMC SERVER 4321/n"
		else
			?"Socket客户端断开"
			cmdlen = 0				'丢弃未完成的命令
		endif
	endif

	'服务端心跳:连接期间每HB_MS毫秒发一帧beat/n,客户端超时收不到即判服务端失联
	if conn_old = 1 then
		if beat_last - TICKS > HB_MS then
			beat_last = TICKS
			PRINT #COM_PORT,"beat/n"
		endif
	else
		beat_last = TICKS			'无连接时持续刷新,保证重连后先隔一个周期再发第一拍
	endif

	'非阻塞接收(GET语法4,返回本次读取的字节数)
	rxnum = GET #COM_PORT, RXBUF, RXGETMAX
	if rxnum > 0 then
		last_rx = TICKS
		for i = 0 to rxnum-1
			ch = RXBUF(i)
			if ch = 59 or ch = 13 or ch = 10 then		';或回车/换行=命令结束
				if cmdlen > 0 then
					CMDSTR(cmdlen) = 0			'字符串结尾
					cmd_exec()
					cmdlen = 0
				endif
			else
				if cmdlen < CMDMAX-1 then
					CMDSTR(cmdlen) = ch
					cmdlen = cmdlen + 1
				endif
			endif
		next
	endif

	'---- 无结束符空闲超时:一段时间无后续字节则按完整命令执行 ----
	'(兼容不加回车/换行的上位机;字面"/n"等非数字后缀由VAL解析自动忽略)
	if (cmdlen > 0) and (last_rx - TICKS > CMD_IDLE_MS) then
		CMDSTR(cmdlen) = 0
		cmd_exec()
		cmdlen = 0
	endif

	'---- 数值命令运动完成监测 ----
	'(MOVEABS后延时50ms才开始判断,等待运动启动;停止后再延时200ms让伺服稳态收敛)
	if (mvmode = 1) and (mvt0 - TICKS > 50) then
		if IDLE(MVAXIS) <> 0 then				'-1=运动结束
			if mvwait = 0 then
				mvwait = TICKS					'刚停止,进入200ms到位确认期
			elseif mvwait - TICKS >= 200 then
				mvmode = 0
				if conn_old = 1 then
					?"到位检查:MPOS:",MPOS(MVAXIS),"DPOS:",DPOS(MVAXIS),"目标:",mvpos,"位置差:",ABS(MPOS(MVAXIS) - mvpos),"STATUS:",AXISSTATUS(MVAXIS),"ERR位:",(AXISSTATUS(MVAXIS) and MV_ERRMASK)
					if ((AXISSTATUS(MVAXIS) and MV_ERRMASK) <> 0) or (ABS(MPOS(MVAXIS) - mvpos) > MV_TOL) then
						PRINT #COM_PORT,"move_error/n"
					else
						PRINT #COM_PORT,"move_done/n"
					endif
				endif
			endif
		elseif mvt0 - TICKS > MV_TIMEOUT then			'超时保护
			mvmode = 0
			mvwait = 0
			RAPIDSTOP(2)
			if conn_old = 1 then
				PRINT #COM_PORT,"move_error/n"
			endif
		endif
	endif
	delay(2)
wend
END SUB

'=================== 命令解析与执行 ===================
GLOBAL SUB cmd_exec()
LOCAL c1, c2, c3, c0, mvacc, mvspd, i, n, l, rctl
if DBG = 1 then
	?"SOCKET CMD:",CMDSTR,"len:",cmdlen			'调试:打印收到的命令
endif
c0 = CMDSTR(0)
'---- 纯数值命令: 单值=位置(默认速度/加速度); 三段=加速度,速度,绝对位置(如2000,100,55) ----
if ((c0 >= 48) and (c0 <= 57)) or (c0 = 45) or (c0 = 46) then
	if (cmdlen = 1) and ((c0 = 45) or (c0 = 46)) then	'只有"-"或"."=无效
		PRINT #COM_PORT,"move_error/n"
	elseif bus_ok <> 1 then
		PRINT #COM_PORT,"move_error/n"
	elseif mvmode = 1 then
		PRINT #COM_PORT,"busy/n"
	else
		'---- 解析参数 ----
		mvacc = MV_ACCEL
		mvspd = MV_SPEED
		c1 = STRFIND(CMDSTR,",")
		if c1 >= 0 then
			c2 = STRFIND(CMDSTR,",",c1+1)
			if c2 < 0 then
				PRINT #COM_PORT,"move_error/n"	'只有一个逗号=格式错误
				goto cmd_end
			endif
			get_num(0)					'整串,VAL遇逗号停=加速度(mm/s^2)
			if numval > 0 then
				mvacc = numval
			endif
			get_num(1)					'第2段=速度(mm/s)
			if numval > 0 then
				mvspd = numval
			endif
			get_num(2)					'第3段=绝对位置(mm)
			mvpos = numval
		else
			get_num(0)					'单值=绝对位置(mm)
			mvpos = numval
		endif
		if DBG = 1 then
			?"数值命令:轴",MVAXIS,"acc:",mvacc,"spd:",mvspd,"pos:",mvpos	'调试(仅控制台)
		endif
		SPEED(MVAXIS) = mvspd					'1unit=1mm,直接mm数值
		ACCEL(MVAXIS) = mvacc
		DECEL(MVAXIS) = mvacc
		MOVEABS(mvpos) AXIS(MVAXIS)				'勿乘PULSE_EQUIV,控制器内部按UNITS换算
		mvt0 = TICKS
		mvwait = 0
		mvmode = 1
	endif

elseif STRCOMP(CMDSTR,"HELLO") = 0 then
	PRINT #COM_PORT,"HELLO ZMC/n"

elseif STRCOMP(CMDSTR,"EN") = 0 then			'轴0使能
	base(0)
	AXIS_ENABLE=1
	wdog=1
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"DIS") = 0 then			'轴0去使能
	wdog=0
	base(0)
	AXIS_ENABLE=0
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"STOP") = 0 then			'全轴急停
	RAPIDSTOP(2)
	PRINT #COM_PORT,"OK/n"

elseif STRFIND(CMDSTR,"VJOG") = 0 then			'VJOG,<axis>,<speed>;
	get_num(2)						'速度(可负)
	spd = numval
	if spd < 0 then
		spdabs = -spd
	else
		spdabs = spd
	endif
	if bus_ok = 1 then
		SPEED(0) = spdabs				'1unit=1mm,直接mm/s
		if spd < 0 then
			VMOVE(-1) AXIS(0)
		else
			VMOVE(1) AXIS(0)
		endif
		PRINT #COM_PORT,"OK/n"
	else
		PRINT #COM_PORT,"ERR:NOBUS/n"
	endif

elseif STRFIND(CMDSTR,"MA") = 0 then			'MA,<axis>,<pos>,<speed>;
	get_num(2)
	posval = numval
	c1 = STRFIND(CMDSTR,",")
	c2 = STRFIND(CMDSTR,",",c1+1)
	c3 = STRFIND(CMDSTR,",",c2+1)
	if bus_ok = 1 then
		if c3 >= 0 then					'带速度参数
			get_num(3)
			if numval < 0 then
				spdabs = -numval
			else
				spdabs = numval
			endif
			SPEED(0) = spdabs			'1unit=1mm
		endif
		MOVEABS(posval) AXIS(0)				'位置直接mm
		PRINT #COM_PORT,"OK/n"
	else
		PRINT #COM_PORT,"ERR:NOBUS/n"
	endif

elseif STRFIND(CMDSTR,"MR") = 0 then			'MR,<axis>,<dist>,<speed>;
	get_num(2)
	posval = numval
	c1 = STRFIND(CMDSTR,",")
	c2 = STRFIND(CMDSTR,",",c1+1)
	c3 = STRFIND(CMDSTR,",",c2+1)
	if bus_ok = 1 then
		if c3 >= 0 then
			get_num(3)
			if numval < 0 then
				spdabs = -numval
			else
				spdabs = numval
			endif
			SPEED(0) = spdabs			'1unit=1mm
		endif
		MOVE(posval) AXIS(0)				'距离直接mm
		PRINT #COM_PORT,"OK/n"
	else
		PRINT #COM_PORT,"ERR:NOBUS/n"
	endif

elseif STRCOMP(CMDSTR,"STA") = 0 then			'状态应答
	sta = "STA," + TOSTR(bus_ok,1,0) + ","
	sta = sta + "0," + TOSTR(MPOS(0),12,3) + "," + TOSTR(DPOS(0),12,3) + "," + TOSTR(IDLE(0),3,0) + ","
	PRINT #COM_PORT,sta+"/n"

elseif STRCOMP(CMDSTR,"SCAN") = 0 then			'重扫总线
	rescan_flag = 1
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"BUSSTOP") = 0 then		'停止总线
	SLOT_STOP(0)
	bus_ok = 0
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"W") = 0 then				'查询当前重量
	if scale_valid = 1 then
		PRINT #COM_PORT,"WEIGHT:"+TOSTR(scale_w,10,3)+"/n"
	else
		PRINT #COM_PORT,"ERR:NOSCALE/n"			'称重未接入/未收到数据
	endif

elseif STRCOMP(CMDSTR,"WSTA") = 0 then			'查询称重完整状态
	'格式: WSTA,通讯正常,数据有效,通道连接,命令结果,错误计数,重量,原始重量,内码,零点内码
	PRINT #COM_PORT,"WSTA,"+TOSTR(scale_ok,1,0)+","+TOSTR(scale_valid,1,0)+","+TOSTR(scale_conn,1,0)+","+TOSTR(scale_result,1,0)+","+TOSTR(scale_err,5,0)+","+TOSTR(scale_w,10,3)+","+TOSTR(scale_raw,12,0)+","+TOSTR(scale_ad,12,0)+","+TOSTR(scale_zero,12,0)+"/n"

elseif STRCOMP(CMDSTR,"WT") = 0 then			'去皮(以当前重量为零点)
	scale_do_cmd(1)
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"WTC") = 0 then			'取消去皮
	scale_do_cmd(2)
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"WZERO") = 0 then			'零点校准(程序内部自动先关写保护再校准)
	scale_do_cmd(3)
	PRINT #COM_PORT,"OK/n"

elseif STRCOMP(CMDSTR,"WCLR") = 0 then			'清除称重错误计数
	scale_do_cmd(4)
	PRINT #COM_PORT,"OK/n"

elseif STRFIND(CMDSTR,"WDIV,") = 0 then			'WDIV,<比例系数> 显示重量=原始重量/系数
	get_num(1)					'★必须显式解析:numval是上一条命令的遗留值,不解析=用错值或永远ERR:ARG
	if numval > 0 then
		scale_div = numval
		scale_divsave = numval
		PRINT #COM_PORT,"OK/n"
	else
		PRINT #COM_PORT,"ERR:ARG/n"
	endif

elseif STRFIND(CMDSTR,"WSA,") = 0 then			'WSA,<站号> 修改称重模块Modbus站号
	get_num(1)					'★同上:不解析将沿用上一个命令的numval
	if (numval >= 1) and (numval <= 247) then
		scale_station = numval
		scale_stasave = numval
		PRINT #COM_PORT,"OK/n"
	else
		PRINT #COM_PORT,"ERR:ARG/n"
	endif

elseif STRCOMP(CMDSTR,"RSTA") = 0 then			'查询机器人状态
	'格式: RSTA,通道连接,通讯正常,报警,错误计数,报文计数,最近报文,MOTOR,RUN,ESTOP,MODE,FAULT,COLL,TASK
	sta = "RSTA," + TOSTR(robot_conn,1,0) + "," + TOSTR(robot_ok,1,0) + "," + TOSTR(robot_alarm,1,0) + "," + TOSTR(robot_err,5,0) + "," + TOSTR(robot_msgcnt,6,0) + ","
	sta = sta + ROBOT_MSG
	sta = sta + ",MOTOR," + TOSTR(robot_st(0),1,0) + ",RUN," + TOSTR(robot_st(1),1,0) + ",ESTOP," + TOSTR(robot_st(2),1,0) + ",MODE," + TOSTR(robot_st(3),1,0)
	sta = sta + ",FAULT," + TOSTR(robot_st(5),1,0) + ",COLL," + TOSTR(robot_st(6),1,0) + ",TASK," + TOSTR(robot_st(7),1,0)
	PRINT #COM_PORT,sta+"/n"

elseif STRFIND(CMDSTR,"RCMD,") = 0 then			'RCMD,<命令内容> 把命令原样透传给机器人
	if robot_conn = 1 then
		n = cmdlen - 5					'去掉"RCMD,"前缀
		if n > ROBOT_TXMAX-2 then
			n = ROBOT_TXMAX-2			'预留LF与结尾0
		endif
		if n <= 0 then
			PRINT #COM_PORT,"ERR:ARG/n"
		else
			for i = 0 to n-1
				ROBOT_TXBUF(i) = CMDSTR(i+5)
			next
			robot_txlen = n
			if DBG = 1 then
				?"透传机器人命令:",ROBOT_TXBUF," len:",n
			endif
			PRINT #COM_PORT,"OK/n"
		endif
	else
		PRINT #COM_PORT,"ERR:ROBOT/n"	'机器人链路未连接
	endif

elseif STRFIND(CMDSTR,"RMON,") = 0 then		'RMON,<序号> 单步测试:立即发监控表第n条(0~11), 应答按监控路径解析并打印
	get_num(1)
	n = numval
	if (n >= 0) and (n < ROBOT_MON_N) then
		if robot_conn = 1 then
			robot_curk = n
			robot_lastpoll = TICKS + ROBOT_POLL_MS + 100	'令下轮发送条件立即成立(立即发出该条)
			PRINT #COM_PORT,"OK/n"
		else
			PRINT #COM_PORT,"ERR:ROBOT/n"
		endif
	else
		PRINT #COM_PORT,"ERR:ARG/n"
	endif

else
	'---- 机器人控制短命令: 先定位指令序号, 再统一入队发送(RCMD 为通用透传) ----
	rctl = -1
	if STRCOMP(CMDSTR,"RON") = 0 then			'电机上电
		rctl = 0
	elseif STRCOMP(CMDSTR,"ROFF") = 0 then		'电机下电
		rctl = 1
	elseif STRCOMP(CMDSTR,"RMAIN") = 0 then		'程序指针回main
		rctl = 2
	elseif STRCOMP(CMDSTR,"RSTART") = 0 then		'程序启动
		rctl = 3
	elseif STRCOMP(CMDSTR,"RSTOP") = 0 then		'程序停止
		rctl = 4
	elseif STRCOMP(CMDSTR,"RCLEAR") = 0 then		'清除伺服报警
		rctl = 5
	elseif STRCOMP(CMDSTR,"RAUTO") = 0 then		'切自动模式
		rctl = 6
	elseif STRCOMP(CMDSTR,"RMAN") = 0 then		'切手动模式
		rctl = 7
	elseif STRCOMP(CMDSTR,"RDRAG") = 0 then		'打开拖动
		rctl = 8
	elseif STRCOMP(CMDSTR,"RUDRAG") = 0 then		'关闭拖动
		rctl = 9
	elseif STRCOMP(CMDSTR,"RLIST") = 0 then		'工程列表
		rctl = 10
	elseif STRCOMP(CMDSTR,"RCUR") = 0 then		'当前工程
		rctl = 11
	endif
	if rctl >= 0 then
		robot_getctl(rctl)
		robot_pushcmd()
		if robot_pushok = 1 then
			PRINT #COM_PORT,"OK/n"
		else
			PRINT #COM_PORT,"ERR:ROBOT/n"
		endif
	elseif STRFIND(CMDSTR,"RLOAD,") = 0 then	'RLOAD,<工程名> -> load_prog:<工程名>
		ROBOT_CMDBUF = "load_prog:"
		l = STRLEN(ROBOT_CMDBUF)
		n = cmdlen - 6						'去掉"RLOAD,"前缀
		if n > ROBOT_CMDMAX-2-l then
			n = ROBOT_CMDMAX-2-l				'预留结尾0
		endif
		if n <= 0 then
			PRINT #COM_PORT,"ERR:ARG/n"
		else
			for i = 0 to n-1
				ROBOT_CMDBUF(l+i) = CMDSTR(i+6)
			next
			ROBOT_CMDBUF(l+n) = 0
			robot_pushcmd()
			if robot_pushok = 1 then
				PRINT #COM_PORT,"OK/n"
			else
				PRINT #COM_PORT,"ERR:ROBOT/n"
			endif
		endif
	else
		PRINT #COM_PORT,"ERR:UNKNOWN/n"
	endif
endif

cmd_end:
END SUB

'=================== 取第idx个逗号字段并转数值 ===================
'输入: CMDSTR, idx(0=命令字,1/2/3=参数)  输出: numval
'说明: 手工处理负号与小数点(VAL遇符号字符停止)
GLOBAL SUB get_num(idx)
LOCAL k, f0, f1, il, fl, s0, fnum
if idx = 0 then					'idx=0=整串(数值命令用)
	f0 = -1
	f1 = cmdlen
else
	f0 = STRFIND(CMDSTR,",")
	for k = 1 to idx-1
		f0 = STRFIND(CMDSTR,",",f0+1)
	next
	f1 = STRFIND(CMDSTR,",",f0+1)
endif
if f1 < 0 then f1 = cmdlen			'无后续逗号=到串尾
fl = f1 - f0 - 1					'字段长度
if fl > CMDMAX-1 then fl = CMDMAX-1
for k = 0 to fl-1
	PSTR(k) = CMDSTR(f0+1+k)
next
PSTR(fl) = 0
'--- 负号 ---
numsign = 1
s0 = 0
if PSTR(0) = 45 then				'ASCII '-'
	numsign = -1
	s0 = 1
endif
'--- 小数点 ---
f0 = STRFIND(PSTR,".")
if f0 < 0 then
	if s0 = 1 then					'负整数:跳过负号再VAL
		for k = s0 to STRLEN(PSTR)-1
			IPSTR(k-s0) = PSTR(k)
		next
		IPSTR(STRLEN(PSTR)-s0) = 0
		numval = VAL(IPSTR) * numsign
	else
		numval = VAL(PSTR) * numsign
	endif
else
	il = f0 - s0					'整数部分长度
	fl = STRLEN(PSTR) - f0 - 1		'小数部分长度
	numval = 0
	if il > 0 then
		for k = 0 to il-1
			IPSTR(k) = PSTR(s0+k)
		next
		IPSTR(il) = 0
		numval = VAL(IPSTR)
	endif
	fnum = 0
	if fl > 0 then
		for k = 0 to fl-1
			FPSTR(k) = PSTR(f0+1+k)
		next
		FPSTR(fl) = 0
		fnum = VAL(FPSTR)
		for k = 1 to fl
			fnum = fnum / 10
		next
	endif
	numval = (numval + fnum) * numsign
endif
END SUB

'=================== 汇川IT7000触摸屏 ModbusTCP 通讯任务 ===================
'任务2：控制器=Modbus-TCP从站(服务端),IT7000触摸屏=主站(客户端)
'  控制器内置以太网口固定监听502端口(无需OPEN,固件自动响应触摸屏轮询);
'  PROTOCOL缺省=3(控制器为从站);站号由主程序 ADDRESS=1 设置,与触摸屏"站号:1"一致。
'
'寄存器映射(4x保持寄存器,编号从0开始):
'  输入区(触摸屏->控制器) REG 0~2 :
'    REG0(4x0) : HMI数据1, int16 -> 存入 hmi_val(0), 标志 hmi_flag(0), 计数 hmi_cnt(0)
'    REG1(4x1) : HMI数据2, float32(占4x1/4x2) -> 存入 hmi_val(1), 标志 hmi_flag(1), 计数 hmi_cnt(1)
'                其它任务用法: if hmi_flag(0)=1 then ...读取 hmi_val(0)... hmi_flag(0)=0 endif
'  回写区(控制器->触摸屏,把收到的原值写回屏上显示):
'    REG4(4x4)     : 收到的 4x0(int16) 回写值
'    REG5(4x5/4x6) : 收到的 4x1(float32) 回写值
'  读区(控制器->触摸屏) :
'    REG3  : 状态字 bit0=有轴运动中 bit1=有轴报警 bit2=已使能 bit3=总线正常
'    REG9  : 总线轴数(本项目单轴,恒为1)
'    REG10(4x10/4x11) : 轴0当前位置 MPOS(0), float32 mm
'    REG12(4x12/4x13) : 轴0指令位置 DPOS(0), float32 mm
'  写区(触摸屏->控制器) REG 60~119 :
'    REG60 : 命令字 bit0=使能 bit1=去使能 bit2=急停 bit3=重扫总线
'    REG61 : 目标轴号(本项目单轴,固定0)
'    REG62 : 目标位置(32位浮点mm,占62/63)
'    REG64 : 速度(mm/s)
'    REG65 : 模式 0=绝对MOVEABS 1=相对MOVE
'    REG66 : 运动触发 0->1上升沿执行一次(需先置0再置1)
'    REG67 : 点动使能 1=点动
'    REG68 : 点动方向 1=正 -1=负
'    REG69 : 点动速度(mm/s)
'注:32位数据字序按触摸屏"32位整数/浮点数:1234"设置;若数值异常,把该项改成反向字序即可。
'注:速度寄存器4x64(定位速度)/4x69(点动速度)为int16,屏端按1位小数输入(存值=显示值×10),程序中已除以10还原;
'   ★屏端这两个元件的"小数位数"必须保持1,若改成0会导致速度被再除以10(慢10倍);
'   ★屏端数值>3276.7时int16会回绕成负数,程序已做上限保护(MV_SPD_MAX=3276.7)。
GLOBAL SUB modbus_task()
LOCAL moving, alarm, cmdw, trig, ax, mbmvspd, mbmvmode, rd, rf, scst, scpar, sccmd
while 1
	'---- HMI输入接收: 4x0=int16, 4x1=float32 -> 存入 hmi_val(),置新数据标志 ----
	rd = MODBUS_REG(0)			'4x0(40001) 16位整数
	if rd <> hmi_val(0) then
		hmi_val(0) = rd			'4x0 接收值存入变量
		hmi_flag(0) = 1			'置新数据标志(供其它任务读取)
		hmi_cnt(0) = hmi_cnt(0) + 1	'接收计数
		if DBG = 1 then
			?"HMI数据:4x0(int)=",rd
		endif
	endif
	rf = MODBUS_IEEE(1)			'4x1(40002) 32位浮点,占4x1/4x2
	if rf <> hmi_val(1) then
		hmi_val(1) = rf			'4x1 接收值存入变量
		hmi_flag(1) = 1
		hmi_cnt(1) = hmi_cnt(1) + 1
		if DBG = 1 then
			?"HMI数据:4x1(float)=",rf
		endif
	endif

	'---- 数据回写触摸屏: 收到的4x0 -> 4x4 ; 收到的4x1 -> 4x5(占4x5/4x6) ----
	MODBUS_REG(4) = hmi_val(0)		'4x0(int16) 原值回写到 4x4
	MODBUS_IEEE(5) = hmi_val(1)		'4x1(float32) 原值回写到 4x5

	'---- 读区:控制器状态 -> 触摸屏(4x3状态字/4x9轴数/4x10轴0MPOS/4x12轴0DPOS) ----
	moving = 0
	alarm = 0
	if IDLE(0) = 0 then
		moving = 1
	endif
	if (AXISSTATUS(0) and MV_ERRMASK) <> 0 then
		alarm = 1
	endif
	mb_state = 0
	if moving = 1 then
		mb_state = mb_state + 1
	endif
	if alarm = 1 then
		mb_state = mb_state + 2
	endif
	if wdog = 1 then
		mb_state = mb_state + 4
	endif
	if bus_ok = 1 then
		mb_state = mb_state + 8			'bit3=总线正常
	endif
	MODBUS_REG(3) = mb_state			'状态字(4x3):bit0运动中 bit1报警 bit2已使能 bit3总线正常
	MODBUS_REG(9) = num			'总线轴数(4x9)
	MODBUS_IEEE(10) = MPOS(0)		'4x10/4x11 轴0当前位置 MPOS(0) mm
	MODBUS_IEEE(12) = DPOS(0)		'4x12/4x13 轴0指令位置 DPOS(0) mm

	'---- 读区:称重传感器 -> 触摸屏(4x120状态字/4x121错误计数/4x122重量/4x124原始值/4x126内码) ----
	scst = 0
	if scale_ok = 1 then
		scst = scst + 1					'bit0 称重通讯正常
	endif
	if scale_valid = 1 then
		scst = scst + 2					'bit1 重量数据有效
	endif
	if (scale_ok = 0) and (scale_err > 0) then
		scst = scst + 4					'bit2 通讯出错/超时
	endif
	if scale_conn = 1 then
		scst = scst + 8					'bit3 称重通道已连接
	endif
	if scale_result = 1 then
		scst = scst + 16				'bit4 称重命令执行中
	endif
	if scale_result = 2 then
		scst = scst + 32				'bit5 称重命令成功
	endif
	if scale_result = 3 then
		scst = scst + 64				'bit6 称重命令失败
	endif
	MODBUS_REG(120) = scst				'4x120 称重状态字
	MODBUS_REG(121) = scale_err			'4x121 错误计数
	MODBUS_IEEE(122) = scale_w			'4x122/123 重量(float32,显示单位)
	MODBUS_IEEE(124) = scale_raw		'4x124/125 原始重量(模块内部计数)
	MODBUS_IEEE(126) = scale_ad			'4x126/127 内码(AD码)
	MODBUS_IEEE(128) = scale_zero		'4x128/129 零点内码(零点校准瞬间记录)
	MODBUS_REG(135) = scale_result		'4x135 命令结果 0空闲/1执行中/2成功/3失败

	'---- 读区:机器人状态 -> 触摸屏(4x140状态字/4x141错误计数/4x142报文计数) ----
	scst = 0
	if robot_conn = 1 then
		scst = scst + 1					'bit0 机器人通道已连接
	endif
	if robot_ok = 1 then
		scst = scst + 2					'bit1 机器人通讯正常
	endif
	if robot_alarm = 1 then
		scst = scst + 4					'bit2 机器人报警
	endif
	if robot_txlen > 0 then
		scst = scst + 8					'bit3 有命令待发给机器人
	endif
	if robot_moncnt > 0 then
		scst = scst + 16				'bit4 监控数据已建立(至少完整轮询过一轮)
	endif
	MODBUS_REG(140) = scst				'4x140 机器人状态字
	MODBUS_REG(141) = robot_err			'4x141 机器人错误计数
	MODBUS_REG(142) = robot_msgcnt		'4x142 机器人累计报文数(诊断)
	MODBUS_REG(143) = robot_st(0)		'4x143 电机上电状态 motor_on_state    (1=已上电)
	MODBUS_REG(144) = robot_st(1)		'4x144 程序运行状态 robot_running_state(1=运行中)
	MODBUS_REG(145) = robot_st(2)		'4x145 急停状态 estop_state          (1=急停)
	MODBUS_REG(146) = robot_st(3)		'4x146 工作模式 operating_mode       (按机器人协议定义)
	MODBUS_REG(147) = robot_st(5)		'4x147 故障状态 fault_state          (0=正常)
	MODBUS_REG(148) = robot_st(6)		'4x148 碰撞检测 collision_state      (1=碰撞)
	MODBUS_REG(149) = robot_st(7)		'4x149 运行任务状态 task_state

	'---- 写区:称重参数与命令(4x130站号/4x131轮询ms/4x132比例系数/4x134命令字) ----
	scpar = MODBUS_REG(130)
	if (scpar >= 1) and (scpar <= 247) and (scpar <> scale_stasave) then
		scale_station = scpar			'Modbus站号(1~247)
		scale_stasave = scpar
		?"HMI称重:模块站号改为",scpar
	endif
	scpar = MODBUS_REG(131)
	if (scpar < 20) or (scpar > 5000) then
		scpar = SCALE_POLL_MS
	endif
	scale_poll = scpar				'轮询周期(20~5000ms)
	scpar = MODBUS_IEEE(132)			'4x132/133 重量比例系数(float32)
	if (scpar > 0.000001) and (scpar <> scale_divsave) then
		scale_div = scpar			'显示重量 = 原始重量 / 比例系数
		scale_divsave = scpar
		?"HMI称重:重量比例系数改为",scpar
	endif
	sccmd = MODBUS_REG(134)			'4x134 称重命令字
	if (sccmd > 0) and (scale_cmdold = 0) then		'上升沿触发,执行后触摸屏需置0
		scale_do_cmd(sccmd)
	endif
	scale_cmdold = sccmd

	'---- 写区:触摸屏命令 -> 控制器 ----
	cmdw = MODBUS_REG(60)
	if ((cmdw and 1) <> 0) and ((mb_cmdold and 1) = 0) then		'bit0 使能
		base(0)
		AXIS_ENABLE = 1
		wdog = 1
		?"HMI命令:轴0使能"
	endif
	if ((cmdw and 2) <> 0) and ((mb_cmdold and 2) = 0) then		'bit1 去使能
		wdog = 0
		base(0)
		AXIS_ENABLE = 0
		?"HMI命令:轴0去使能"
	endif
	if ((cmdw and 4) <> 0) and ((mb_cmdold and 4) = 0) then		'bit2 急停
		RAPIDSTOP(2)
		?"HMI命令:急停"
	endif
	if ((cmdw and 8) <> 0) and ((mb_cmdold and 8) = 0) then		'bit3 重扫总线
		rescan_flag = 1
		?"HMI命令:重扫总线"
	endif
	mb_cmdold = cmdw

	'---- 运动触发(上升沿,需先置0再置1) ----
	trig = MODBUS_REG(66)
	if (trig <> 0) and (mb_trigold = 0) then
		if bus_ok = 1 then
			ax = MODBUS_REG(61)
			mbmvspd = MODBUS_REG(64) / 10	'4x64屏端按1位小数写入(int16=显示值×10),此处除以10还原为mm/s
			if (mbmvspd > MV_SPD_MAX) or (mbmvspd < 0) then	'超上限或int16回绕(负)->按上限处理
				mbmvspd = MV_SPD_MAX
			endif
			mbmvmode = MODBUS_REG(65)
			mb_tgt = MODBUS_IEEE(62)
			if ax = 0 then
				if mbmvspd >= 1 then
					SPEED(ax) = mbmvspd
				endif
				if mbmvmode = 1 then
					MOVE(mb_tgt) AXIS(ax)
					?"HMI命令:相对运动 轴",ax," 位置",mb_tgt," 速度",SPEED(ax)
				else
					MOVEABS(mb_tgt) AXIS(ax)
					?"HMI命令:绝对运动 轴",ax," 位置",mb_tgt," 速度",SPEED(ax)
				endif
			else
				?"HMI命令:轴号非0(单轴),忽略运动",ax
			endif
		else
			?"HMI命令:总线未就绪,忽略运动"
		endif
	endif
	mb_trigold = trig

	'---- 点动(上升沿启动/下降沿停止) ----
	mb_jog = MODBUS_REG(67)
	if (mb_jog = 1) and (mb_jogold = 0) then
		ax = MODBUS_REG(61)
		mb_dir = MODBUS_REG(68)
		mb_spd = MODBUS_REG(69) / 10	'4x69屏端按1位小数写入(int16=显示值×10),此处除以10还原为mm/s
		if (mb_spd > MV_SPD_MAX) or (mb_spd < 0) then	'超上限或int16回绕(负)->按上限处理
			mb_spd = MV_SPD_MAX
		endif
		if (bus_ok = 1) and (ax = 0) then
			if mb_spd < 1 then
				mb_spd = MV_SPEED
			endif
			SPEED(ax) = mb_spd
			if mb_dir < 0 then
				VMOVE(-1) AXIS(ax)
			else
				VMOVE(1) AXIS(ax)
			endif
			?"HMI命令:点动 轴",ax," 方向",mb_dir," 速度",SPEED(ax)
		endif
	elseif (mb_jog = 0) and (mb_jogold = 1) then
		ax = MODBUS_REG(61)
		if ax = 0 then
			base(ax)
			CANCEL(2)
			?"HMI命令:停止点动"," 速度",SPEED(ax)
		endif
	endif
	mb_jogold = mb_jog

	delay(5)						'轮询间隔≈5ms(对应触摸屏"间隔:5")
wend
END SUB

'=================== 称重采集任务 ===================
'任务3：控制器=TCP客户端(主站) -> TAS-LAN-869串口服务器(192.168.1.80:10123, TCP Server模式,
'        ★原样透传,未开启"Modbus TCP/RTU转换") -> RS485数字称重模块(站号1,9600,8,N,1)
'
'★实测结论(2026-09-22, 用PC调试助手以TCP客户端连192.168.1.80:10123验证通过):
'  调试助手16进制发送 01 03 00 00 00 02 C4 0B
'  收到              01 03 04 FD 93 FF FF 3A 02
'  → 网口上跑的就是"485总线上的Modbus-RTU原始字节", 带CRC16、无MBAP头。
'    ★所以必须按 Modbus-RTU 组帧(站号+功能码+数据+CRC16), 不能按 Modbus-TCP(无CRC)组帧;
'     之前按MBAP组帧时多出7字节垃圾又缺CRC, 模块不应答 → 表现为"应答超时"。
'
'★为什么手工组帧而不用 MODBUSM_* 主站指令:
'  手册的 MODBUSM_DES2(id,port,"ip") 是ModbusTCP主站, 按标准502端口通讯, 没有"目标端口"
'  参数; 而本网关(TAS-LAN-869)监听在10123且是透传; 所以用 OPEN #11,"TCP_CLIENT",10123,"192.168.1.80"
'  直接建链, 再手工收发 Modbus RTU 报文, 端口任意、时序可控。
'
'★报文与字节序(称重模块说明书 4.寄存器详解 + 5.通讯示例):
'  Modbus-RTU 帧: 站号(1) + 功能码(1) + 数据(n) + CRC16(2,低字节在前)
'  读4个寄存器(start=0x0000,num=4)一次取回 实时重量(0,1) + 内码(2,3):
'   请求 [0]站号 [1]03 [2..3]起始地址 [4..5]寄存器数 [6][7]CRC16   → 01 03 00 00 00 04 <CRC>
'   应答 [0]站号 [1]03 [2]字节数N [3..2+N]数据 [末2]CRC16           → 总长 5+N
'   数据 [3][4]=重量低字 [5][6]=重量高字 [7][8]=内码低字 [9][10]=内码高字
'   ★低字寄存器地址小(地址0=低字), 组合 32位值 = 高字*65536 + 低字, 最高位为1表示负数
'   ★必须按32位读: 只读低字在负数/大数时会显示异常
'   异常应答 [0]站号 [1]功能码+128 [2]异常码 [3][4]CRC16, 整帧长5
'   写单寄存器(06): 请求 [0]站号 [1]06 [2..3]地址 [4..5]值 [6][7]CRC16; 应答=原帧回显(8字节)
'  ★CRC16 用控制器自带 CRC16(数组,起点,字节数), 默认初值$FFFF/多项式$A001 即Modbus标准值
'  ★★【踩坑记录·2026-09-22 实测定案】内置CRC16()的"返回值字节序"与标准整数相反:
'     它把结果按"发送顺序"打包 → 高8位=帧内先发的低字节, 低8位=帧内后发的高字节。
'     实测: 数据 01 03 00 00 00 04 → CRC16()=0x4409 (标准整数值应为0x0944)
'     手册例 FE 48 06 00 6D 00 00 00 → 文档写"结果$1A0D" (标准值是$0D1A), 结论一致
'     所以组帧时必须 SCALE_TX(6)=crc\256, SCALE_TX(7)=crc AND $FF; 写反则模块CRC校验不过,
'     按Modbus规范从站"静默不回应" → 现象就是"应答超时/缓冲内0字节"(与帧内容无关)
'
'★触摸屏寄存器映射(4x保持寄存器, 120起):
'  4x120 : 称重状态字 bit0=通讯正常 bit1=数据有效 bit2=通讯出错/超时 bit3=通道已连接
'                          bit4=命令执行中 bit5=命令成功 bit6=命令失败
'  4x121 : 错误计数
'  4x122/123 : 重量 float32(显示单位, 已按比例换算)
'  4x124/125 : 原始重量 float32(模块内部计数,无小数点)
'  4x126/127 : 内码 float32(AD码,诊断用)
'  4x128/129 : 零点内码 float32(执行零点校准时记录)
'  4x130 : 模块站号(可写,默认1;写入后自动生效)
'  4x131 : 轮询周期ms(可写,默认200;<20或>5000按200处理)
'  4x132/133 : 重量比例系数 float32(可写,默认100.0, 即模块10000=显示100.00)
'  4x134 : 称重命令字(写1=去皮 2=取消去皮 3=零点校准 4=清错误计数; 上升沿有效,执行后请置0)
'  4x135 : 命令结果 0=空闲 1=执行中 2=成功 3=失败
'  注:32位数据字序按触摸屏"32位整数/浮点数:1234"设置
'===================

'---- 按32位有符号解析SCALE_RX中idx起的4字节(低字在前,字内高位在前) ----
'★不用 hi*65536+lo 直接算: hi>=32768时相乘会溢出; 改为 (hi-65536)*65536+lo 保证负数正确
GLOBAL SUB scale_par32(idx)
LOCAL lo, hi
lo = SCALE_RX(idx)*256 + SCALE_RX(idx+1)
hi = SCALE_RX(idx+2)*256 + SCALE_RX(idx+3)
if hi >= 32768 then
	scale_tmp = (hi - 65536)*65536 + lo
else
	scale_tmp = hi*65536 + lo
endif
END SUB

'---- 调试: 以16进制打印发送帧(8字节,便于与调试助手/抓包对比) ----
GLOBAL SUB scale_dump_tx()
?"TX",SCALE_TXLEN,"字节:",HEX(SCALE_TX(0)),HEX(SCALE_TX(1)),HEX(SCALE_TX(2)),HEX(SCALE_TX(3))
?"  ",HEX(SCALE_TX(4)),HEX(SCALE_TX(5)),HEX(SCALE_TX(6)),HEX(SCALE_TX(7))
END SUB

'---- 接收缓冲前移1字节(帧头非法/CRC错时重新同步用) ----
'★TCP Server模式下网关会把485上所有数据推给客户端, 可能混入其它站数据, 必须能重同步
GLOBAL SUB scale_rx_shift()
LOCAL i
for i = 0 to scale_rxlen - 2
	SCALE_RX(i) = SCALE_RX(i+1)
next
scale_rxlen = scale_rxlen - 1
END SUB

'---- 调试: 以16进制打印接收帧(前16字节) ----
GLOBAL SUB scale_dump_rx(cnt)
?"RX",cnt,"字节:",HEX(SCALE_RX(0)),HEX(SCALE_RX(1)),HEX(SCALE_RX(2)),HEX(SCALE_RX(3))
if cnt > 4 then
	?"  ",HEX(SCALE_RX(4)),HEX(SCALE_RX(5)),HEX(SCALE_RX(6)),HEX(SCALE_RX(7)),HEX(SCALE_RX(8))
endif
if cnt > 9 then
	?"  ",HEX(SCALE_RX(9)),HEX(SCALE_RX(10)),HEX(SCALE_RX(11)),HEX(SCALE_RX(12)),HEX(SCALE_RX(13))
endif
if cnt > 14 then
	?"  ",HEX(SCALE_RX(14)),HEX(SCALE_RX(15))
endif
END SUB

'---- 称重模块写命令入队(地址/值的字节由调用方以常量给出, 避开整数除法取高低字节) ----
GLOBAL SUB scale_push(ah, al, vh, vl)
if scale_qn >= 15 then
	?"称重:命令队列满,丢弃本次命令"
else
	SCALE_Q(scale_qn*4) = ah
	SCALE_Q(scale_qn*4+1) = al
	SCALE_Q(scale_qn*4+2) = vh
	SCALE_Q(scale_qn*4+3) = vl
	scale_qn = scale_qn + 1
endif
END SUB

'---- 称重功能命令: 1=去皮 2=取消去皮 3=零点校准(自动先关写保护) 4=清错误计数 ----
'★去皮/零点校准都受"写保护寄存器0x0017"限制: 必须先写 0x0017=1 关闭写保护
GLOBAL SUB scale_do_cmd(c)
if c = 1 then
	scale_push(0,23,0,1)			'0x0017=1 关闭写保护(说明书去皮示例也先关写保护,兼容各固件)
	scale_push(0,21,0,1)			'0x0015=1 去皮
	scale_result = 1
	?"称重命令:去皮"
elseif c = 2 then
	scale_push(0,23,0,1)			'0x0017=1 关闭写保护
	scale_push(0,21,0,2)			'0x0015=2 取消去皮
	scale_result = 1
	?"称重命令:取消去皮"
elseif c = 3 then
	scale_zero = scale_ad			'记录当前内码作为校准前零点(诊断用)
	scale_push(0,23,0,1)			'0x0017=1 关闭写保护
	scale_push(0,22,0,1)			'0x0016=1 零点校准
	scale_result = 1
	?"称重命令:零点校准"
elseif c = 4 then
	scale_err = 0
	scale_result = 2
	?"称重命令:清除错误计数"
endif
END SUB

'---- 称重采集主循环 ----
GLOBAL SUB scale_task()
LOCAL n, i, total, fc, k, crc, crc2, bad
scale_lastpoll = TICKS
if SCALE_DBG = 1 then
	?"称重:通道",SCALE_CH,"初始状态",PORT_STATUS(SCALE_CH),"(1=已连接)"
	?*PORT							'打印所有通讯口,确认通道号是否可用
	'★CRC字节序自检: 用调试助手已实测合法的整帧 01 03 00 00 00 04 44 09 反查内置CRC16的返回值约定
	SCALE_TX(0) = 1
	SCALE_TX(1) = 3
	SCALE_TX(2) = 0
	SCALE_TX(3) = 0
	SCALE_TX(4) = 0
	SCALE_TX(5) = 4
	SCALE_TX(6) = $44
	SCALE_TX(7) = $09
	crc = CRC16(SCALE_TX, 0, 8)
	?"CRC自检:合法整帧校验值=",HEX(crc),"(0=整帧合法; ★余数为0对字节序是盲的(swap(0)=0), 判据只看下一行)"
	crc = CRC16(SCALE_TX, 0, 6)
	?"CRC自检:6字节数据CRC=",HEX(crc)," 先发字节=",HEX(crc \ 256)," 后发字节=",HEX(crc AND $FF)," (期望先发=44 后发=9)"
endif
while 1
	'---- 1.连接管理: ★必须先建立TCP连接、确认已连接(PORT_STATUS=1), 才允许收发 ----
	'★手册 PORT_STATUS(port): 0=没有连接, 1=有连接使用
	'★原代码发送时用 "(scale_conn = 1) or (scale_valid = 0)" 无条件盲发, 违反"先连接再通讯",已删除
	'★【应答超时头号原因】TAS-LAN-869的TCP Server默认"单路模式"(AT+TCPALONE=1):
	'   设备发送的数据"仅发送到最新连上的客户端或者最活跃的客户端"。
	'   PC调试助手作为Client一直连着时, 称重模块的应答会被发给助手, 控制器连上了也永远超时!
	'   处理: 调试完必须断开助手连接(或给网关上电重启), 或设 AT+TCPALONE=0 改为多路模式。
	scale_conn = PORT_STATUS(SCALE_CH)
	if scale_opened = 0 then
		OPEN #SCALE_CH, "TCP_CLIENT", SCALE_PORT, "192.168.1.80"
		scale_opened = 1
		scale_reconn = TICKS
		scale_try = 0
		?"称重:发起TCP连接 -> 192.168.1.80:",SCALE_PORT," (本机TCP_CLIENT,通道",SCALE_CH,")"
	endif

	if scale_conn <> scale_connold then
		scale_connold = scale_conn
		if scale_conn = 1 then
			?"称重:★链路已建立(PORT_STATUS=1),开始Modbus-RTU轮询"
		else
			?"称重:链路未连接(PORT_STATUS=0),暂停收发"
		endif
	endif

	if scale_conn <> 1 then
		'---- 未连接: 清空半包与等待标志, 绝不发送, 只按间隔重试OPEN ----
		scale_rxlen = 0
		scale_send = 0
		scale_ok = 0
		if scale_reconn - TICKS > SCALE_RECONN_MS then
			scale_reconn = TICKS
			scale_try = scale_try + 1
			?"称重:连接未建立,第",scale_try,"次重试;请查: 网关是否TCP Server/10123, 是否已被上位机调试助手占用, 控制器能否访问192.168.1.80"
			OPEN #SCALE_CH, "TCP_CLIENT", SCALE_PORT, "192.168.1.80"
		endif
	else
		'★以下"超时处理/接收/解析/发送"全部只在已连接时执行(缩进保持原样)

	'---- 3.应答超时处理(TICKS是倒数计数器: 先取的值减后取的值=已过去的时间) ----
	if scale_send = 1 then
		if scale_t0 - TICKS > SCALE_TIMEOUT then
			scale_send = 0
			scale_ok = 0
			scale_err = scale_err + 1
			scale_tocnt = scale_tocnt + 1
			if scale_result = 1 then
				scale_result = 3
			endif
			'★限频打印: 每SCALE_LOG_MS才输出一条"完整快照", 避免1秒一条刷屏看不清
			if scale_logt - TICKS > SCALE_LOG_MS then
				scale_logt = TICKS
				?"称重:应答超时(本窗口",scale_tocnt,"次/累计",scale_err,"次) 连接",scale_conn," 缓冲内",scale_rxlen,"字节未成帧"
				scale_tocnt = 0
				if SCALE_DBG = 1 then
					scale_dump_tx()			'dump上次发出的请求帧,便于与调试助手对照
				endif
			endif
			scale_rxlen = 0
		endif
	endif

	'---- 4.接收(语法4非阻塞,返回字节数;从缓冲尾部追加,天然支持粘包/分包) ----
	if scale_rxlen < SCALE_RXMAX then
		n = GET #SCALE_CH, SCALE_RX(scale_rxlen), SCALE_RXMAX - scale_rxlen
		if n > 0 then
			scale_rxlen = scale_rxlen + n
			if (SCALE_DBG = 1) and (scale_rxlogt - TICKS > SCALE_LOG_MS) then
				scale_rxlogt = TICKS
				?"称重:收到",n,"字节(缓冲共",scale_rxlen,"字节)"
				scale_dump_rx(scale_rxlen)	'直接看原始字节, 确认链路到底有没有数据回来
			endif
		endif
	endif

	'---- 5.解析: 网关是串口透传,网口字节==485上的Modbus-RTU帧 ----
	'Modbus-RTU帧格式: [0]站号 [1]功能码 [数据...] [末2]CRC16(低字节在前)
	'读应答  = 站号+03+字节数N+数据N+CRC2  => 总长 5+N
	'异常应答= 站号+功能码|0x80+异常码+CRC2 => 总长 5
	'写应答  = 站号+06+地址2+值2+CRC2      => 总长 8(原帧回显)
	k = 1
	while (k = 1) and (scale_rxlen >= 3)
		'---- 帧头检查: 站号必须匹配, 功能码只能是03/06/异常应答 ----
		bad = 0
		if SCALE_RX(0) <> scale_station then
			bad = 1
		elseif (SCALE_RX(1) <> 3) and (SCALE_RX(1) <> 6) and (SCALE_RX(1) < 128) then
			bad = 1
		endif
		if bad = 1 then
			scale_err = scale_err + 1
			scale_badcnt = scale_badcnt + 1
			'★这条原来是"每丢弃1字节打1条", 乱码进来瞬间刷屏; 改为按窗口汇总
			if (SCALE_DBG = 1) and (scale_logt - TICKS > SCALE_LOG_MS) then
				scale_logt = TICKS
				?"称重:帧头非法,本窗口丢弃",scale_badcnt,"字节(例:站号",HEX(SCALE_RX(0)),"功能码",HEX(SCALE_RX(1)),")丢弃首字节重同步"
				scale_badcnt = 0
			endif
			scale_rx_shift()
		else
			total = 0
			if SCALE_RX(1) = 3 then
				if scale_rxlen >= 3 then
					total = 5 + SCALE_RX(2)		'字节数字段有效后才知总长
				endif
			elseif SCALE_RX(1) = 6 then
				total = 8
			else
				total = 5					'异常应答
			endif
			if total = 0 then
				k = 0						'只收到2字节,等字节数字段到齐
			elseif total > SCALE_RXMAX then
				scale_err = scale_err + 1
				?"称重:报文长度非法,整包丢弃"
				scale_rxlen = 0
				k = 0
			elseif scale_rxlen < total then
				k = 0						'半包,等下一批字节到达
			else
				'---- CRC16 校验(多项式$A001, 帧内低字节在前) ----
				'★内置CRC16返回值按发送顺序打包: 高8位=帧内先发的低字节, 低8位=帧内后发的高字节
				'  实测: CRC16("01 03 00 00 00 04")=0x4409 (标准整数值是0x0944);
				'        手册例 TABLE(0,$FE,$48,$06,$00,$6D,$00,$00) 算出$1A0D (标准值是$0D1A)
				'  所以帧内"倒数第2字节(低字节)"要放到返回值的高8位去比
				crc = CRC16(SCALE_RX, 0, total-2)
				crc2 = SCALE_RX(total-2)*256 + SCALE_RX(total-1)
				if crc <> crc2 then
					scale_err = scale_err + 1
					if (SCALE_DBG = 1) and (scale_logt - TICKS > SCALE_LOG_MS) then
						scale_logt = TICKS
						?"称重:CRC错(算出",HEX(crc),"帧内",HEX(crc2),")丢弃首字节重同步"
					endif
					scale_rx_shift()
				else
					if (SCALE_DBG = 1) and (scale_logt - TICKS > SCALE_LOG_MS) then
						scale_logt = TICKS
						scale_dump_rx(total)
					endif
					fc = SCALE_RX(1)
					if fc >= 128 then			'异常应答:功能码最高位置1
						scale_ok = 0
						scale_err = scale_err + 1
						?"称重:模块异常应答,功能码",fc,"异常码",SCALE_RX(2)
						if scale_result = 1 then
							scale_result = 3
						endif
					elseif fc = 3 then
						if SCALE_RX(2) >= 4 then
							scale_par32(3)			'实时重量(32位有符号,低字在前)
							scale_raw = scale_tmp
							if SCALE_RX(2) >= 8 then
								scale_ad = SCALE_RX(9)*256*65536.0 + SCALE_RX(10)*65536.0 + SCALE_RX(7)*256 + SCALE_RX(8)
											'内码(AD码): 32位无符号低字在前,用浮点累加避免整数溢出
							endif
							if scale_div < 0.000001 then
								scale_div = 100.0	'比例系数非法时兜底
							endif
							scale_w = scale_raw / scale_div
							scale_ok = 1
							if scale_valid = 0 then
								scale_valid = 1
								?"称重:数据接入成功,重量",scale_w,"原始值",scale_raw
							endif
						else
							scale_err = scale_err + 1
							?"称重:读应答数据长度不足"
						endif
					elseif fc = 6 then
						if scale_result = 1 then
							scale_result = 2		'写命令成功(模块原帧回显)
						endif
						scale_ok = 1
					endif
					scale_send = 0
					for i = 0 to scale_rxlen - total - 1	'移出已处理帧,剩余字节前移
						SCALE_RX(i) = SCALE_RX(i+total)
					next
					scale_rxlen = scale_rxlen - total
				endif
			endif
		endif
	wend

	'---- 6.优先下发称重写命令(去皮/零点校准等) ----
	if (scale_qn > 0) and (scale_send = 0) then
		SCALE_TX(0) = scale_station		'站号
		SCALE_TX(1) = 6					'功能码06 写单个寄存器
		SCALE_TX(2) = SCALE_Q(0)		'寄存器地址高字节
		SCALE_TX(3) = SCALE_Q(1)		'寄存器地址低字节
		SCALE_TX(4) = SCALE_Q(2)		'值高字节
		SCALE_TX(5) = SCALE_Q(3)		'值低字节
		crc = CRC16(SCALE_TX, 0, 6)		'Modbus CRC16(初值$FFFF/多项式$A001)
		'★【已实测定案】内置CRC16返回值按"发送顺序"打包: 高8位=帧内先发的低字节, 低8位=帧内后发的高字节
		SCALE_TX(6) = crc \ 256			'★先发低字节  → 必须是 crc 的高8位
		SCALE_TX(7) = crc AND $FF		'★后发高字节  → 必须是 crc 的低8位
		for i = 0 to scale_qn*4 - 5
			SCALE_Q(i) = SCALE_Q(i+4)	'出队
		next
		scale_qn = scale_qn - 1
		if SCALE_DBG = 1 then
			scale_dump_tx()				'打印即将发送的8字节(16进制)
		endif
		PUTCHAR #SCALE_CH, SCALE_TX(0, SCALE_TXLEN)	'★数组+字节数=二进制发送,一帧一次发完
		scale_send = 1
		scale_t0 = TICKS
		scale_lastpoll = TICKS
		?"称重:下发写命令 寄存器",SCALE_TX(2)*256+SCALE_TX(3)," 值",SCALE_TX(4)*256+SCALE_TX(5)
	endif

	'---- 7.周期发送读请求(实时重量+内码;串行:一帧应答完再发下一帧) ----
	'★已确认连接(在上面的else分支内)才发, 不再有"盲发"分支
	if (scale_send = 0) and (scale_qn = 0) then
		if scale_lastpoll - TICKS > scale_poll then
			SCALE_TX(0) = scale_station		'站号
			SCALE_TX(1) = 3					'功能码03 读保持寄存器
			SCALE_TX(2) = 0					'起始地址0x0000(实时重量低字)
			SCALE_TX(3) = 0
			SCALE_TX(4) = 0					'读4个寄存器:重量2+内码2
			SCALE_TX(5) = 4
			crc = CRC16(SCALE_TX, 0, 6)		'Modbus CRC16(初值$FFFF/多项式$A001)
			'★先发低字节(\256)/后发高字节(AND $FF): 内置CRC16返回值已按发送顺序打包, 反了模块就不应答
			SCALE_TX(6) = crc \ 256
			SCALE_TX(7) = crc AND $FF
			if (SCALE_DBG = 1) and (scale_logt - TICKS > SCALE_LOG_MS) then
				scale_logt = TICKS
				scale_dump_tx()
			endif
			PUTCHAR #SCALE_CH, SCALE_TX(0, SCALE_TXLEN)
			scale_send = 1
			scale_t0 = TICKS
			scale_lastpoll = TICKS
		endif
	endif
	endif						'★连接管理: if scale_conn <> 1 then ... else 的收尾

	delay(2)
wend
END SUB

'=================== 机器人Socket通讯任务 ===================
'任务4：控制器=TCP客户端, 机器人=TCP服务端(192.168.1.221:4320)
'职责: ①上电延迟等待机器人启动完毕 ②间隔重连 ③周期查询并缓存机器人状态 ④把上层命令透传给机器人
'连接纪律(与称重任务一致): 必须先确认 PORT_STATUS=1 才收发; 未连接时只按间隔重试OPEN, 绝不盲发
'★协议适配区(按机器人实际协议改这四处即可):
'  ① ROBOT_KEYTAB     监控指令表("motor_on_state/..."按序轮询, 发送时自动追加LF)
'  ② ROBOT_CTLTAB     控制指令表(与PC短命令RON/ROFF...一一对应)
'  ③ ROBOT_ALARM_KEY  报警关键字(默认"fault"; 在应答报文中查找)
'  ④ robot_num()      数值提取(兼容"带字段名"与"一问一答纯值"两种应答)
'★消息后缀=LF("\n"); ★协议测试期把 ROBOT_DBG 置1(打印收发), 定案后改回0
GLOBAL SUB robot_task()
LOCAL n, ch, k, i, l
robot_boott = TICKS				'★记录任务启动时刻:随后等 ROBOT_BOOT_WAIT ms,让机器人先开机完毕
robot_lastpoll = TICKS
robot_reconn = TICKS
robot_lastrx = TICKS
?*PORT							'★诊断: 打印端口表,确认哪些通道是ECUSTOM(自定义网口)、哪些已被占用
if ROBOT_CH > PORT_MAX then
	'★越界保护: 通道号超过本控制器上限时直接访问 PORT_STATUS 会让任务报错停止(2024),
	'  这里改成打印提示并空转,保证其它任务不受影响
	?"机器人:★ROBOT_CH=",ROBOT_CH," 超过本控制器上限;实测本控制器只有 2 个自定义网口(ECUSTOM=10/11),"
	?"机器人:  10 已给 PC 服务端(4321),11 已给称重网关(10123),无空闲通道 → 机器人链路无法建立"
	?"机器人:  备选方案见 README 第八节(称重改接 RS485 / 上位机中转 / 串口转网口模块)"
	while 1
		delay(1000)
	wend
endif
if ROBOT_DBG = 1 then
	?"机器人:通道",ROBOT_CH,"初始状态",PORT_STATUS(ROBOT_CH),"(1=已连接)"
endif
while 1
	'---- 0.★上电延迟:等待机器人启动完毕,期间不发起连接 ----
	'   TICKS是倒数计数器: 先取的TICKS(robot_boott)减后取的TICKS = 已过去的时间(ms)
	if robot_boott - TICKS < ROBOT_BOOT_WAIT then
		if ROBOT_DBG = 1 then
			?"机器人:等待机器人启动完毕,剩余约",(ROBOT_BOOT_WAIT - (robot_boott - TICKS))/1000,"s"
		endif
		delay(500)
	else
		'---- 1.连接管理 ----
		robot_conn = PORT_STATUS(ROBOT_CH)
		if robot_opened = 0 then
			OPEN #ROBOT_CH, "TCP_CLIENT", ROBOT_PORT, "192.168.1.221"
			robot_opened = 1
			robot_reconn = TICKS
			robot_try = 0
			?"机器人:发起TCP连接 -> 192.168.1.221:",ROBOT_PORT," (本机TCP_CLIENT,通道",ROBOT_CH,")"
		endif

		if robot_conn <> robot_connold then
			robot_connold = robot_conn
			if robot_conn = 1 then
				?"机器人:★链路已建立(PORT_STATUS=1),开始状态监控"
				robot_lastpoll = TICKS		'连上后先空一个周期再发第一帧
				robot_lastrx = TICKS
			else
				?"机器人:链路未连接(PORT_STATUS=0),暂停收发"
				robot_rxlen = 0
				robot_msgn = 0
				robot_send = 0
				robot_ok = 0
			endif
		endif

		if robot_conn <> 1 then
			'---- 未连接:清半包与等待标志,只按间隔重试OPEN ----
			robot_rxlen = 0
			robot_msgn = 0
			robot_send = 0
			robot_ok = 0
			if robot_reconn - TICKS > ROBOT_RECONN_MS then
				robot_reconn = TICKS
				robot_try = robot_try + 1
				?"机器人:连接未建立,第",robot_try,"次重试;请查: 机器人是否上电/4320端口是否开放/控制器能否访问192.168.1.221"
				OPEN #ROBOT_CH, "TCP_CLIENT", ROBOT_PORT, "192.168.1.221"
			endif
		else
			'---- 2.应答超时处理(先取的TICKS减后取的=已过去的时间) ----
			if robot_send = 1 then
				if robot_t0 - TICKS > ROBOT_TIMEOUT then
					robot_send = 0
					robot_ok = 0
					robot_err = robot_err + 1
					if robot_logt - TICKS > ROBOT_LOG_MS then
						robot_logt = TICKS
						?"机器人:应答超时(累计",robot_err,"次) 缓冲内",robot_rxlen,"字节未成帧"
					endif
					robot_rxlen = 0
					robot_msgn = 0
				endif
			endif

			'---- 3.接收(语法4非阻塞,返回本次读取字节数) ----
			if robot_rxlen < ROBOT_RXMAX then
				n = GET #ROBOT_CH, ROBOT_RX(robot_rxlen), ROBOT_RXMAX - robot_rxlen
				if n > 0 then
					robot_rxlen = robot_rxlen + n
					robot_lastrx = TICKS
					if ROBOT_DBG = 1 then
						robot_dump_rx(robot_rxlen - n, n)	'★打印本次收到的原始字节(HEX),协议测试用
					endif
				endif
			endif

			'---- 4.拆包:10(LF)/13(CR)/59(;)视为一条报文结束,逐条交 robot_parse ----
			k = 0
			while k < robot_rxlen
				ch = ROBOT_RX(k)
				if (ch = 10) or (ch = 13) or (ch = 59) then
					if robot_msgn > 0 then
						ROBOT_MSG(robot_msgn) = 0	'字符串结尾
						robot_parse()
						robot_msgn = 0
					endif
				else
					if robot_msgn < ROBOT_MSGLEN-1 then
						ROBOT_MSG(robot_msgn) = ch
						robot_msgn = robot_msgn + 1
					endif
				endif
				k = k + 1
			wend
			robot_rxlen = 0
			if robot_msgn >= ROBOT_MSGLEN-1 then	'无结束符且已塞满:丢弃防溢出
				robot_msgn = 0
			endif

			'---- 5.发送:优先透传上层命令,否则按周期轮询下一条监控指令 ----
			if robot_txlen > 0 then
				robot_espermon = 0			'本轮是控制命令,应答不参与监控解析
				ROBOT_TXBUF(robot_txlen) = 10	'★消息后缀=LF("\n")(实测确定, 原CRLF已改)
				ROBOT_TXBUF(robot_txlen+1) = 0
				if ROBOT_DBG = 1 then
					?"机器人:★TX(控制)",robot_txlen,"字节:",ROBOT_TXBUF
				endif
				PRINT #ROBOT_CH, ROBOT_TXBUF
				robot_txlen = 0
				robot_t0 = TICKS
				robot_send = 1
			elseif robot_send = 0 then
				if robot_lastpoll - TICKS > ROBOT_POLL_MS then
					robot_lastpoll = TICKS
					'★取当前轮询的监控指令(robot_curk 由 robot_parse 逐条前进) + LF
					robot_espermon = 1
					robot_getkey(robot_curk)
					l = STRLEN(ROBOT_CMDBUF)
					if l > ROBOT_TXMAX-2 then
						l = ROBOT_TXMAX-2
					endif
					for i = 0 to l-1
						ROBOT_TXBUF(i) = ROBOT_CMDBUF(i)
					next
					ROBOT_TXBUF(l) = 10			'★消息后缀=LF("\n")
					ROBOT_TXBUF(l+1) = 0
					if ROBOT_DBG = 1 then
						?"机器人:★TX(监控",robot_curk,"):",ROBOT_CMDBUF
					endif
					PRINT #ROBOT_CH, ROBOT_TXBUF
					robot_t0 = TICKS
					robot_send = 1
				endif
			endif
		endif
	endif
	delay(2)
wend
END SUB

'---- 调试:以16进制打印机器人本次收到的原始字节(协议测试期对数据用, 最多32字节) ----
GLOBAL SUB robot_dump_rx(st, cnt)
LOCAL di, dn, dh
dn = cnt
if dn > 32 then
	dn = 32
endif
dh = ""
for di = st to st+dn-1
	dh = dh + HEX(ROBOT_RX(di)) + " "
next
?"机器人:★RX",dn,"字节 HEX:",dh
END SUB

'---- 从监控指令表 ROBOT_KEYTAB(/分隔)取第 k 段(第0段起) -> ROBOT_CMDBUF ----
GLOBAL SUB robot_getkey(k)
LOCAL rp, rs, rc, rl, rn
ROBOT_CMDBUF(0) = 0
rl = STRLEN(ROBOT_KEYTAB)
rp = 0
rs = 0
while (rp < rl) and (rs <= k)
	rc = ROBOT_KEYTAB(rp)
	if rc = 47 then				'47 = '/' 分隔符
		rs = rs + 1
	else
		if rs = k then
			rn = STRLEN(ROBOT_CMDBUF)
			if rn < ROBOT_CMDMAX-1 then
				ROBOT_CMDBUF(rn) = rc
				ROBOT_CMDBUF(rn+1) = 0
			endif
		endif
	endif
	rp = rp + 1
wend
END SUB

'---- 从 ROBOT_MSG 第 pos 字符起解析第一个数值(支持正负号与小数点,不依赖VAL) ----
'出参: robot_numval=数值, robot_numok=1 表示解析成功(0.0 也算成功)
GLOBAL SUB robot_num(pos)
LOCAL ri, rl, rc, rv, rf, rneg, rndig, rdot
rl = STRLEN(ROBOT_MSG)
robot_numval = 0
robot_numok = 0
ri = pos
'①跳过 key 之后的分隔符: " : , = 空格 TAB [ ( 等
rdot = 0
while (ri < rl) and (rdot = 0)
	rc = ROBOT_MSG(ri)
	if (rc = 34) or (rc = 58) or (rc = 44) or (rc = 61) or (rc = 32) or (rc = 9) or (rc = 91) or (rc = 40) then
		ri = ri + 1
	else
		rdot = 1
	endif
wend
if ri < rl then
	'②符号位
	rneg = 0
	rc = ROBOT_MSG(ri)
	if rc = 45 then				'45='-'(负号)
		rneg = 1
		ri = ri + 1
	elseif rc = 43 then			'43='+'(正号)
		ri = ri + 1
	endif
	'③整数/小数部分
	rv = 0
	rf = 0.1
	rndig = 0
	rdot = 0
	while ri < rl
		rc = ROBOT_MSG(ri)
		if (rc >= 48) and (rc <= 57) then
			if rdot = 0 then
				rv = rv * 10 + (rc - 48)
			else
				rv = rv + (rc - 48) * rf
				rf = rf * 0.1
			endif
			rndig = rndig + 1
			ri = ri + 1
		elseif (rc = 46) and (rdot = 0) then		'46='.'(小数点)
			rdot = 1
			ri = ri + 1
		else
			ri = rl + 1			'数值结束
		endif
	wend
	if rndig > 0 then
		if rneg = 1 then
			rv = -rv
		endif
		robot_numval = rv
		robot_numok = 1
	endif
endif
END SUB

'---- 机器人应答解析:数值状态存入 robot_st(robot_curk),更新报警标志并推进轮询 ----
'输入: ROBOT_MSG(0结尾的一条完整报文) + robot_curk(本轮查询的监控量序号)
'★若机器人是"一问一答+纯值"应答, 走 else 分支按整条报文解析; 若应答带字段名(JSON/CSV)则按key定位
GLOBAL SUB robot_parse()
LOCAL ri, rl, rp, rlen, rk
robot_msgcnt = robot_msgcnt + 1
robot_send = 0					'收到应答,清除等待标志
robot_ok = 1
robot_lastrx = TICKS
rk = robot_curk					'记录本轮监控序号(下面②会推进, 打印要用原值)

'①定位并解析本轮监控量的数值(仅监控查询的应答参与;透传命令的应答只记录报文)
if robot_espermon = 1 then
	robot_getkey(robot_curk)
	rp = STRFIND(ROBOT_MSG, ROBOT_CMDBUF)
	rlen = STRLEN(ROBOT_CMDBUF)
	if (rp >= 0) and (rlen > 0) then
		robot_num(rp + rlen)			'报文含该key: 取key之后的数值
	else
		robot_num(0)				'报文为纯值应答: 整条解析
	endif
	if robot_numok = 1 then
		robot_st(robot_curk) = robot_numval
		robot_stv(robot_curk) = 1
	else
		if STRFIND(ROBOT_MSG, "true") >= 0 then
			robot_st(robot_curk) = 1
			robot_stv(robot_curk) = 1
		elseif STRFIND(ROBOT_MSG, "false") >= 0 then
			robot_st(robot_curk) = 0
			robot_stv(robot_curk) = 1
		endif
	endif

	'②轮询推进:下一条监控指令(一轮走完后计数+1)
	robot_curk = robot_curk + 1
	if robot_curk >= ROBOT_MON_N then
		robot_curk = 0
		robot_moncnt = robot_moncnt + 1
	endif
endif

'③报警汇总: 故障(5)/急停(2)/碰撞(6)任一为真即报警, 报文命中关键字也算
robot_alarm = 0
if (robot_stv(5) = 1) and (robot_st(5) > 0) then
	robot_alarm = 1
endif
if (robot_stv(2) = 1) and (robot_st(2) > 0) then
	robot_alarm = 1
endif
if (robot_stv(6) = 1) and (robot_st(6) > 0) then
	robot_alarm = 1
endif
if STRFIND(ROBOT_MSG, ROBOT_ALARM_KEY) >= 0 then
	robot_alarm = 1
endif

'④报文变化时打印一条(避免周期刷屏)
'④打印: 测试期(ROBOT_DBG=1)每条都打(带监控序号), 正式运行仅在报文变化时打一条(避免刷屏)
if (STRCOMP(ROBOT_MSG, robot_lastmsg) <> 0) or (ROBOT_DBG = 1) then
	rl = STRLEN(ROBOT_MSG)
	if rl > ROBOT_MSGLEN-1 then
		rl = ROBOT_MSGLEN-1
	endif
	for ri = 0 to rl
		robot_lastmsg(ri) = ROBOT_MSG(ri)
	next
	if ROBOT_DBG = 1 then
		?"机器人:★报文[监控条",rk,"]=",ROBOT_MSG,"  (解析值",robot_numval," 有效",robot_numok,")"
	else
		?"机器人:",ROBOT_MSG
	endif
	if robot_alarm = 1 then
		?"机器人:★报警"
	endif
endif
END SUB

'---- 从控制指令表 ROBOT_CTLTAB(/分隔)取第 k 段(第0段起) -> ROBOT_CMDBUF ----
GLOBAL SUB robot_getctl(k)
LOCAL rp, rs, rc, rl, rn
ROBOT_CMDBUF(0) = 0
rl = STRLEN(ROBOT_CTLTAB)
rp = 0
rs = 0
while (rp < rl) and (rs <= k)
	rc = ROBOT_CTLTAB(rp)
	if rc = 47 then				'47 = '/' 分隔符
		rs = rs + 1
	else
		if rs = k then
			rn = STRLEN(ROBOT_CMDBUF)
			if rn < ROBOT_CMDMAX-1 then
				ROBOT_CMDBUF(rn) = rc
				ROBOT_CMDBUF(rn+1) = 0
			endif
		endif
	endif
	rp = rp + 1
wend
END SUB

'---- 把 ROBOT_CMDBUF 中的控制命令放入发送缓冲(robot_task 自动补LF发出) ----
'未连接时拒绝入队: robot_pushok=0, 调用方应答 ERR:ROBOT
GLOBAL SUB robot_pushcmd()
LOCAL rp, rl
robot_pushok = 0
if robot_conn <> 1 then
	?"机器人:控制命令被拒(链路未连接):",ROBOT_CMDBUF
else
	rl = STRLEN(ROBOT_CMDBUF)
	if rl > ROBOT_TXMAX-2 then
		rl = ROBOT_TXMAX-2			'预留LF与结尾0
	endif
	if rl > 0 then
		for rp = 0 to rl-1
			ROBOT_TXBUF(rp) = ROBOT_CMDBUF(rp)
		next
		robot_txlen = rl
		robot_pushok = 1
	endif
endif
END SUB
