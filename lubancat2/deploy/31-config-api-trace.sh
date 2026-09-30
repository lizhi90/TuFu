#!/bin/bash
# 31-config-api-trace.sh [profile] [dc] —— 抓 EC_CONFIG_DBG，确认应用配置是否真的写进了 slave->config
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
PROF="${1:-1}"
DC="${2:-0x0300}"

sep "eth0 up"
sudo ip link set eth0 up
sleep 1

sep "debug 1 + 清 dmesg"
sudo "$EC" debug 1
sudo dmesg -C >/dev/null 2>&1 || true

sep "跑 probe（profile=$PROF dc=$DC, 3s）"
sudo timeout 25 "$APP/ecat_probe" 3 --profile "$PROF" --dc "$DC" >/dev/null 2>&1

sep "debug 0"
sudo "$EC" debug 0
sleep 0.3

sep "配置 API 调用（EC_CONFIG_DBG）"
sudo dmesg | grep -aiE 'ecrt_slave_config|ec_slave_config_load_default|not assigned|not found|invalid' | head -50

sep "SM 页写入"
sudo dmesg | grep -aiE 'SM[0-3]: Addr' | head -20

sep "ERROR/WARNING"
sudo dmesg | grep -aiE 'EtherCAT (ERROR|WARNING)' | head -30

sep "DONE"
