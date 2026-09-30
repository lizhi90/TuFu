#!/bin/bash
# 41-m2-sdo-stop.sh —— M2 第④步：实机 SDO 实读「停机语义 / 报警码 / H0E 通信参数」
#
# 目的：把《SV630N_手册_EtherCAT学习笔记.md》§9（停机语义）、§14（603F/203F）、§15（H0E 组）
#       的**手册默认值**与**实机出厂值**逐条对照，闭合 §17.4 的 "实机 SDO 实读" pending 项。
#   * 只读，不写任何对象、不使能、不走位（绝对安全）。
#   * 依赖板端 /opt/kine-x/app/ecat_sdo（支持 --get 0xIDX:SUB:BITS）。
#
# 用法：deploy/ssh_run.sh deploy/41-m2-sdo-stop.sh
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

sep "① 停机方式三兄弟（手册默认：605A=2 / 605D=1 / 605C=0）"
echo "  -- 605Ah: 6040.bit2=0 快速停机方式"
echo "  -- 605Dh: 6040.bit8=1 halt 停机方式   <-- 本工程急停真正依赖的对象"
echo "  -- 605Ch: 6040.bit7 下降沿(伺服 OFF) 停机方式"
sudo "$APP/ecat_sdo" \
  --get 0x605A:00:16 --get 0x605D:00:16 --get 0x605C:00:16 \
  2>&1 | head -8

sep "② 停机减速度与限幅（手册默认：6085=2147483647 / 607F=4294967295）"
sudo "$APP/ecat_sdo" \
  --get 0x6085:00:32 --get 0x6084:00:32 --get 0x609A:00:32 \
  --get 0x607F:00:32 --get 0x6081:00:32 --get 0x6083:00:32 \
  2>&1 | head -10

sep "③ 位置偏差保护与 PV 零速判定（手册默认：6065=219895608 / 606F=10）"
sudo "$APP/ecat_sdo" \
  --get 0x6065:00:32 --get 0x6066:00:16 \
  --get 0x606F:00:16 --get 0x6070:00:16 \
  2>&1 | head -8

sep "④ 报警码（603Fh 会重复 / 203Fh 唯一）"
sudo "$APP/ecat_sdo" \
  --get 0x603F:00:16 --get 0x203F:00:32 \
  2>&1 | head -6

sep "⑤ 齿轮比与当前模式（对照 §17.1 已实测结论）"
sudo "$APP/ecat_sdo" \
  --get 0x6091:01:32 --get 0x6091:02:32 \
  --get 0x6060:00:8 --get 0x6061:00:8 --get 0x6041:00:16 \
  2>&1 | head -10

sep "⑥ H0E 通信参数（厂商对象 200Eh，映射规律 Hxx.yy ↔ 200x-yyh）"
echo "  -- H0E.01=200E:02 EEPROM 保存策略（默认3）"
echo "  -- H0E.07=200E:08 对象字典单位（默认0 指令单位）"
echo "  -- H0E.22=200E:17 允许丢包次数（默认8）"
echo "  -- H0E.29=200E:1E 端口链接丢失计数（只读，低16=IN 高16=OUT）"
echo "  -- H0E.31=200E:20 同步模式（默认2）"
echo "  -- H0E.32=200E:21 同步误差阈值 ns（默认4000）"
echo "  -- H0E.34=200E:23 CSP 位置指令异常允许时间 ms（默认20）"
echo "  -- H0E.35=200E:24 AL 故障码（只读）"
echo "  -- H0E.38=200E:27 DC 时钟同步调节（默认0）"
sudo "$APP/ecat_sdo" \
  --get 0x200E:02:16 --get 0x200E:08:16 \
  --get 0x200E:17:16 --get 0x200E:1E:32 \
  --get 0x200E:20:16 --get 0x200E:21:16 \
  --get 0x200E:23:16 --get 0x200E:24:16 --get 0x200E:27:16 \
  2>&1 | head -14

sep "⑦ 设备信息与从站状态"
sudo "$APP/ecat_sdo" \
  --get 0x1000:00:32 --get 0x1018:01:32 --get 0x1018:02:32 \
  --get 0x1018:03:32 --get 0x1018:04:32 \
  2>&1 | head -8
sudo "$EC" slaves 2>&1 | head -3

sep "41-m2-sdo-stop DONE"
