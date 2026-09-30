#!/bin/bash
# 20-run-probe.sh [秒数] [--dc N] —— 在鲁班猫2 上运行 ecat_probe 自检
# 先打印 master/slaves 现状，再跑 ecat_probe，最后汇总 dmesg。
set -u
APP="${APP_DEST:-/opt/kine-x/app}"
SECS="${1:-5}"
shift || true
EXTRA="$*"
sep() { echo; echo "===== $* ====="; }

sep "主站状态"
sudo /usr/local/bin/ethercat master 2>&1 | head -12
sep "从站列表"
sudo /usr/local/bin/ethercat slaves 2>&1
echo "(以上为空表示总线上没有从站)"

sep "运行 ecat_probe $SECS $EXTRA"
sudo timeout 60 "$APP/ecat_probe" "$SECS" $EXTRA 2>&1 | tail -50
rc=${PIPESTATUS[0]}
echo "  ecat_probe rc=$rc"

sep "dmesg（ethercat）"
sudo dmesg | grep -iE 'ethercat|ec_master|ec_generic' | tail -12

sep "20-run-probe DONE"
