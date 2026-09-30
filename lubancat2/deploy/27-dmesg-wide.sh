#!/bin/bash
# 27-dmesg-wide.sh —— 宽窗口抓 EtherCAT 内核日志（找 SDO abort / PDO 配置失败等被 tail 截掉的线索）
set -u
sep() { echo; echo "===== $* ====="; }

sep "dmesg 全量（ethercat 相关，最多 200 行）"
sudo dmesg | grep -iE 'ethercat|ec_|FMMU|SyncManager|state change|AL status' | tail -200

sep "关键字过滤"
sudo dmesg | grep -inE 'abort|does not exist|invalid|Failed|refused|Inhibit|PDO' | tail -60

sep "27-dmesg-wide DONE"
