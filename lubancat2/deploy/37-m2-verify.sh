#!/bin/bash
# 37-m2-verify.sh [profile] [dc] [secs] —— M2 第①步：**只使能、不走位**，验证 CiA402 使能序列
#   期望证据链：0x1650(Switch on disabled) -> 0x0631/0x0633(Ready/Switched on) -> 0x0637(Operation enabled)
#   结束时自动去使能（回 0x06）并卸力。走位请用 deploy/38-m2-move.sh。
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
PROF="${1:-1}"
DC="${2:-0x0300}"
SECS="${3:-20}"

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

sep "① 只使能（ctrl_word 走 0x80/0x06/0x07/0x0F，不走位）"
sudo timeout $((SECS + 10)) "$APP/ecat_axis" --enable --profile "$PROF" --dc "$DC" --secs "$SECS" \
  > /tmp/kx_m2_enable.txt 2>&1
echo "rc=$?"
echo "---- CiA402 状态迁移链 ----"
grep -aE 'CiA402 ->|下发 ENABLE|已使能|去使能' /tmp/kx_m2_enable.txt
echo "---- 过程数据（末 10 行）----"
grep -aE '^\[m2\] t=' /tmp/kx_m2_enable.txt | tail -10
echo "---- 结论 ----"
sed -n '/M2 结论/,$p' /tmp/kx_m2_enable.txt

sep "② SDO 复核（模式 / 轮廓参数对象是否可达）"
sudo "$APP/ecat_sdo" --quiet \
  --get 0x6060:00:8 --get 0x6061:00:8 --get 0x6041:00:16 --get 0x603F:00:16 \
  --get 0x6081:00:32 --get 0x6083:00:32 --get 0x6084:00:32 \
  2>&1 | head -20

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -5

sep "37-m2-verify DONE"
echo "若使能序列 PASS，继续：deploy/38-m2-move.sh [dist_mm] [speed]"
