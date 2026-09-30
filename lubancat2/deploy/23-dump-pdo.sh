#!/bin/bash
# 23-dump-pdo.sh —— 导出 SV630 的实际 PDO 映射与对象字典（用于修正代码映射）
set -u
sep() { echo; echo "===== $* ====="; }
ETH=/usr/local/bin/ethercat

sep "从站概览"
sudo $ETH slaves 2>&1

sep "cstruct（实际 PDO 结构，关键）"
sudo $ETH cstruct 2>&1 | head -120

sep "pdos（当前 PDO 配置）"
sudo $ETH pdos 2>&1 | head -80

sep "sdos（对象字典，只挑关键对象）"
sudo $ETH sdos 2>&1 | grep -iE '6040|6041|6060|607A|6081|6083|6084|6091|60FF|6071|6061|6062|6064|6098|60C2|60FD|0x1C' | head -60

sep "23-dump-pdo DONE"
