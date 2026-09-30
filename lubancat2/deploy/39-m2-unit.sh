#!/bin/bash
# 39-m2-unit.sh —— M2 第③步：单位与标定核对（INC_PER_MM）
#
# 目的：搞清「C++ 侧 607a = mm × INC_PER_MM」这条换算链，与驱动器侧用户单位的关系。
#   * 只读 SDO + 从站状态，**不使能、不走位**（绝对安全）。
#   * 走位差分版（需要判 INC_PER_MM 是否与机械一致）请用 deploy/38-m2-move.sh。
#
# 背景（旧系统依据）：
#   EtherCAT_SocketServer.bas:93  GLOBAL CONST PULSE_EQUIV = 14043.41  '脉冲当量(脉冲/mm)
#   EtherCAT_SocketServer.bas:368 units(0)=PULSE_EQUIV
#   EtherCAT_SocketServer.bas:651 MOVEABS(mvpos) ... '勿乘 PULSE_EQUIV,控制器内部按 UNITS 换算
#   → 正运动把 MOVEABS(mm) 在控制器内部换算成「脉冲」再下发；对 EtherCAT 轴，该「脉冲」
#     就是 CiA402 位置对象(0x607A)的单位。故 C++ 侧 inc_per_mm 必须等于同一 PULSE_EQUIV，
#     才能与旧系统下发完全一致的 0x607A。
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat

sep "清理占用主站的残留进程"
sudo pkill -f 'ecat_axis' 2>/dev/null; sudo pkill -f 'ecat_probe' 2>/dev/null
sudo pkill -f 'kine-x' 2>/dev/null; sleep 1
ps -eo pid,cmd | grep -E 'ecat_|kine-x' | grep -v grep || echo "  (无残留)"

sep "eth0 up"
sudo ip link set eth0 up; sleep 1
ip -br link show eth0

sep "ethercat 服务"
sudo systemctl start ethercat 2>/dev/null || true; sleep 1
sudo "$EC" master 2>&1 | head -3

sep "① 驱动器侧换算对象（判断用户单位）"
sudo "$APP/ecat_sdo" \
  --get 0x6091:01:32 --get 0x6091:02:32 \
  --get 0x6092:01:32 --get 0x6092:02:32 \
  --get 0x608F:01:32 --get 0x608F:02:32 \
  2>&1 | head -12

sep "② 位置/速度因子与插补周期（多数 SV630 不支持，abort 属正常）"
sudo "$APP/ecat_sdo" \
  --get 0x6093:01:32 --get 0x6094:01:32 --get 0x60C2:01:8 \
  2>&1 | head -8

sep "③ 当前模式/状态/位置（上电默认模式应为 8=CSP）"
sudo "$APP/ecat_sdo" \
  --get 0x6060:00:8 --get 0x6061:00:8 --get 0x6041:00:16 \
  --get 0x6064:00:32 --get 0x607A:00:32 \
  2>&1 | head -12

sep "④ 结论与判定方法"
cat <<'TXT'
  1) 软件侧一致性（已由 38-m2-move.sh 实机证明）：
     命令 +5.000mm → 0x607A 增量 = 5.000 × 14043.41 = 70217，0x6064 增量同量级，
     实测位移 = 5.0123 - 0.0124 = 4.9999mm，相对误差 ≈ 0.002%。
     → 说明「mm → 607A → 6064 → mm」这条链自洽，无累积换算误差。

  2) 与旧系统一致（本脚本核对的重点）：
     C++ 侧 inc_per_mm = 14043.41 = 旧 PULSE_EQUIV，故下发的 0x607A 与正运动**逐位相同**；
     驱动器参数(config/app.conf 未改 SV630 任何对象)不变 ⇒ 物理运动完全一致。
     ⚠ EtherCAT.txt（早期调试脚本）里曾写 units=16770，正式版才定 14043.41
        → 该值是**人工标定**后的最终值，勿随意改。

  3) 物理绝对标定（本远程会话无法闭合，属现场工装项）：
     * 方法：命令走 100.000mm，用卷尺/百分表量实际位移 m(mm)，
       INC_PER_MM_new = INC_PER_MM × 100 / m，写入 config/app.conf。
     * 若 m 与 100 相差 > 0.5%，说明 SV630 电子齿轮/机械比与 14043.41 不匹配，
       需先核对驱动器 [H05-07]电子齿轮比 与丝杠导程，再决定改控制器还是改驱动器。
     * 判据：只要旧系统与新系统用同一 INC_PER_MM 且驱动器参数不变，二者位移必定相同，
       对外「零改动」即成立；绝对精度由机械决定，与本次控制器迁移无关。

  4) 已知差异：无（已定案）
     旧程序启动时的 datum(0) 已在 ZBasic 手册中查实为"清除控制器所有轴错误"（模式 0），
     **不改变坐标**；置零须写 DATUM(3) AXIS(n)。故旧系统上电 MPOS 同样从编码器绝对零点起
     （C++ 侧实测使能瞬间 0.0124mm / 174 inc），屏显 MPOS 绝对值一致。
     依据：pdf_out/example_tcp_plumber.txt:227/399/436/538
TXT

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -3

sep "39-m2-unit DONE"
