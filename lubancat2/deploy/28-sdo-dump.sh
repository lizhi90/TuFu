#!/bin/bash
# 28-sdo-dump.sh —— 用 ecat_sdo 直读 SV630 的 PDO 分配/映射/SM 参数，定位 AL 0x001E
# 前提：应用已释放主站（ecat_probe / kine-x 未运行）
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app

sep "确认没有应用占用主站"
pgrep -a ecat_probe || echo "  (无 ecat_probe)"
pgrep -a kine-x    || echo "  (无 kine-x)"

sep "SDO 诊断清单"
sudo "$APP/ecat_sdo" --slave 0 2>&1

sep "从站 AL 状态"
sudo /usr/local/bin/ethercat slaves 2>&1

sep "dmesg（本次 SDO 访问）"
sudo dmesg | grep -iE 'ethercat' | tail -20

sep "28-sdo-dump DONE"
