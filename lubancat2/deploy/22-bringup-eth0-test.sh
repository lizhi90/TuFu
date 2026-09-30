#!/bin/bash
# 22-bringup-eth0-test.sh [秒数] —— 拉起 eth0(EtherCAT 口) 并验证 SV630 从站
# 背景：08-net-separate.sh 把 eth0 设为无 IP / NM unmanaged，但重启后网口未被 up，
#       导致主站 generic 驱动虽已绑定，PHY 链路不激活 → Link: DOWN / Slaves: 0。
set -u
SECS="${1:-10}"
sep() { echo; echo "===== $* ====="; }

sep "拉起 eth0"
sudo ip link set eth0 up
sleep 2
ip -br link show eth0
echo "  carrier  : $(cat /sys/class/net/eth0/carrier 2>/dev/null)  (1=有线连接)"
echo "  operstate: $(cat /sys/class/net/eth0/operstate 2>/dev/null)"
echo "  speed    : $(cat /sys/class/net/eth0/speed 2>/dev/null) Mb/s"

sep "主站状态"
sudo /usr/local/bin/ethercat master 2>&1 | head -12

sep "从站列表"
sudo /usr/local/bin/ethercat slaves 2>&1
echo "  --- slaves -v ---"
sudo /usr/local/bin/ethercat slaves -v 2>&1 | head -60

sep "ecat_probe（先关 DC，验证基础链路）"
sudo timeout 80 /opt/kine-x/app/ecat_probe "$SECS" --dc 0 2>&1 | tail -45

sep "dmesg（ethercat/link）"
sudo dmesg | grep -iE 'ethercat|ec_master|ec_generic|eth0|Link is' | tail -20

sep "22-bringup-eth0-test DONE"
