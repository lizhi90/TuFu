#!/bin/bash
# 40-m2-jog.sh [spd_mm_s] [jog_secs] [mode] [profile] [dc] [secs]
#   M2 第④⑤步：PV(6060=3) 点动 与 急停(6040.bit8 halt)
#   mode = jog   点动 spd 秒后 60FF=0 减速停（默认）
#   mode = estop 点动加速到速后下发 halt，验证能在极短距离内停住
#   注意：PV 需要 6061 模式回显确认，故 profile 必须 >=2（默认 2）。
#         60FF 未映射 PDO，由 ecat_axis 内部走 SDO 下发。
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
SPD="${1:-5}"
JSEC="${2:-2}"
MODE="${3:-jog}"       # jog | estop
PROF="${4:-2}"
DC="${5:-0x0300}"
SECS="${6:-30}"

EXTRA=""
case "$MODE" in
  estop) EXTRA="--estop" ;;
  jog|"") ;;
  *) echo "✘ 未知 mode: $MODE（应为 jog 或 estop）"; exit 2 ;;
esac

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

sep "PV 点动：${SPD}mm/s，持续 ${JSEC}s，mode=${MODE}"
sudo timeout $((SECS + 20)) "$APP/ecat_axis" --jog "$SPD" --jog-secs "$JSEC" $EXTRA \
  --profile "$PROF" --dc "$DC" --secs "$SECS" \
  > /tmp/kx_m2_jog.txt 2>&1
echo "rc=$?"
echo "---- 关键事件 ----"
grep -aE 'CiA402 ->|下发|已使能|已进 PV|检出运动|点动到时|点动已停稳|急停|✘|\[sdo\]' /tmp/kx_m2_jog.txt
echo "---- 过程采样（每 100 拍，看 6061 与 60ff 与 mp）----"
grep -aE '^\[m2\] t=' /tmp/kx_m2_jog.txt | awk 'NR%5==1' | tail -30
echo "---- 结论 ----"
sed -n '/M2 结论/,$p' /tmp/kx_m2_jog.txt

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -5

sep "40-m2-jog DONE"
