#!/bin/bash
# 24-probe-variants.sh —— 用不同显式 PDO 档位跑 ecat_probe，定位 SV630 进 OP 的正确映射
# 档位：1=SV630实测默认(3Rx+2Tx) 2=+0x6061模式回显 3=扩展(60ff/6062/6061)
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app

sep "确保 eth0(EtherCAT 口) 已 up"
sudo ip link set eth0 up
sleep 1
ip -br link show eth0
echo "  carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null) speed=$(cat /sys/class/net/eth0/speed 2>/dev/null)Mb/s"

sep "变体1: --profile 1 --dc 0 （SV630 默认 3Rx+2Tx，关DC）"
sudo timeout 70 "$APP/ecat_probe" 8 --profile 1 --dc 0 2>&1 | tail -30

sep "变体2: --profile 2 --dc 0 （默认+0x6061模式回显，关DC）"
sudo timeout 70 "$APP/ecat_probe" 8 --profile 2 --dc 0 2>&1 | tail -30

sep "变体3: --profile 1 --dc 0x0300 （默认映射 + DC SYNC0）"
sudo timeout 70 "$APP/ecat_probe" 8 --profile 1 --dc 0x0300 2>&1 | tail -30

sep "从站状态"
sudo /usr/local/bin/ethercat slaves 2>&1

sep "dmesg（ethercat）"
sudo dmesg | grep -iE 'ethercat' | tail -25

sep "24-probe-variants DONE"
