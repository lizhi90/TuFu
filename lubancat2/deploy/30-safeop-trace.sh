#!/bin/bash
# 30-safeop-trace.sh [profile] [dc] —— 抓取激活时主站写入的 SM2/SM3 长度与 SAFEOP 结果
# 目的：确认 0x001E「Invalid input configuration」究竟是 SM3 长度不符，还是别的输入配置
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
PROF="${1:-1}"
DC="${2:-0x0300}"

sep "eth0 up"
sudo ip link set eth0 up
sleep 1
ip -br link show eth0

sep "探测前从站状态"
sudo "$EC" slaves 2>&1

sep "开调试级别1 并清空 dmesg"
sudo "$EC" debug 1
sudo dmesg -C >/dev/null 2>&1 || true

sep "激活: profile=$PROF dc=$DC"
sudo timeout 30 "$APP/ecat_probe" 5 --profile "$PROF" --dc "$DC" 2>&1 | head -25

sep "关调试"
sudo "$EC" debug 0
sleep 0.3

sep "SM 写入行（主站实际下发的 SM 配置）"
sudo dmesg | grep -aiE 'SM[0-3]: Addr'

sep "FMMU / 输入配置 / SAFEOP 相关"
sudo dmesg | grep -aiE 'FMMU|SAFEOP|\bOP\b|AL status|Invalid|Synchron|Failed' | head -40

sep "配置 FSM 全量（去时间戳，末尾120行）"
sudo dmesg | sed -n 's/^\[[^]]*\] //p' | grep -aiE 'EtherCAT' | tail -120

sep "从站状态"
sudo "$EC" slaves 2>&1
sudo "$EC" master 2>&1 | head -20

sep "30-safeop-trace DONE"
