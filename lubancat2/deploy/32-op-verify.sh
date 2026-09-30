#!/bin/bash
# 32-op-verify.sh [profile] [dc] [秒数] —— 边跑 probe 边轮询从站 AL，确认是否真进 OP、过程数据是否非零
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
PROF="${1:-1}"
DC="${2:-0x0300}"
SECS="${3:-12}"

sep "eth0 up"
sudo ip link set eth0 up
sleep 1
ip -br link show eth0

sep "启动 probe 后台 (profile=$PROF dc=$DC ${SECS}s)"
sudo timeout $((SECS+15)) "$APP/ecat_probe" "$SECS" --profile "$PROF" --dc "$DC" > /tmp/kx_probe_out.txt 2>&1 &
PP=$!
sleep 1

sep "运行中每秒轮询 AL 状态"
# 注意：轮询期间只用 `ethercat slaves`（走 ioctl 读 AL 状态，无副作用）；
#       不要用 `slaves -v`，它会读 SII 抢走 PDI 访问权，会把 OP 中的从站打回 PREOP（观测干扰）。
for i in $(seq 1 "$SECS"); do
  line=$(sudo "$EC" slaves 2>&1 | tr '\n' '|')
  echo "t=${i}s  slaves=[$line]"
  sleep 1
done

wait $PP 2>/dev/null

sep "probe 完整输出"
cat /tmp/kx_probe_out.txt

sep "从站 AL 详情"
sudo "$EC" slaves -v 2>&1 | grep -aiE 'Slave|State|AL status|Error|Port|Link|Info|DL' | head -40

sep "master 摘要"
sudo "$EC" master 2>&1 | head -12

sep "32-op-verify DONE"
