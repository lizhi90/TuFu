#!/bin/bash
# 38-m2-move.sh [dist_mm] [speed] [profile] [dc] [secs] —— M2 第②步：PP 点位走位 + 回原位
#   流程：使能 -> MOVE_ABS(+dist) -> 到位(bit12+bit10) -> MOVE_ABS(回起始位) -> 去使能
#   默认 5mm @ 5mm/s（慢速小位移，安全）。确认轴能安全运动后再逐步加大。
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
DIST="${1:-5}"
SPD="${2:-5}"
PROF="${3:-2}"      # 必须 >=2：档位1 未映射 0x6061，Axis 的"等模式回显"门控会让 PP 超时
DC="${4:-0x0300}"
SECS="${5:-30}"

sep "清理占用主站的残留进程"
sudo pkill -f 'ecat_axis' 2>/dev/null; sudo pkill -f 'ecat_probe' 2>/dev/null
sudo pkill -f 'kine-x' 2>/dev/null; sleep 1
ps -eo pid,cmd | grep -E 'ecat_|kine-x' | grep -v grep || echo "  (无残留)"

sep "eth0 up"
sudo ip link set eth0 up
sleep 1
ip -br link show eth0

sep "ethercat 服务"
sudo systemctl start ethercat 2>/dev/null || true
sleep 1
sudo "$EC" master 2>&1 | head -5

sep "PP 走位：+${DIST}mm @ ${SPD}mm/s，走完回起始位，然后去使能"
sudo timeout $((SECS + 20)) "$APP/ecat_axis" --enable --move "$DIST" --speed "$SPD" \
  --profile "$PROF" --dc "$DC" --secs "$SECS" \
  > /tmp/kx_m2_move.txt 2>&1
echo "rc=$?"
echo "---- 关键事件 ----"
grep -aE 'CiA402 ->|下发|已使能|定位完成|回起始位|已回|✘|\[sdo\]' /tmp/kx_m2_move.txt
echo "---- 过程采样（每 1s，看 607a 与 mp 的变化）----"
grep -aE '^\[m2\] t=' /tmp/kx_m2_move.txt | awk 'NR%10==1' | tail -30
echo "---- 结论 ----"
sed -n '/M2 结论/,$p' /tmp/kx_m2_move.txt

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -5

sep "38-m2-move DONE"
