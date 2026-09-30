#!/bin/bash
# 25-probe-full.sh —— 抓 ecat_probe 完整输出（含初始化/偏移/SDO 配置过程）
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app

sep "完整输出: --explicit --dc 0"
sudo timeout 40 "$APP/ecat_probe" 4 --explicit --dc 0 > /tmp/p1.txt 2>&1
echo "  rc=$?"
cat /tmp/p1.txt

sep "完整输出: 默认映射 --dc 0"
sudo timeout 40 "$APP/ecat_probe" 4 --dc 0 > /tmp/p2.txt 2>&1
echo "  rc=$?"
cat /tmp/p2.txt

sep "从站状态"
sudo /usr/local/bin/ethercat slaves 2>&1

sep "25-probe-full DONE"
