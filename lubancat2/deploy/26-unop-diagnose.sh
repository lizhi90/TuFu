#!/bin/bash
# 26-unop-diagnose.sh [从站位置=0] —— 从站进不了 OP（AL 0x001E Invalid input configuration）时的深度诊断
# 前提：应用已释放主站（ecat_probe 已退出），此时可用 ethercat upload 读 SDO。
set -u
sep() { echo; echo "===== $* ====="; }
S="${1:-0}"
EC=/usr/local/bin/ethercat

up() {  # up <十六进制索引> <子索引> —— 读 SDO，失败也打印原因
  local idx="$1" sub="$2"
  printf '  0x%s:%02x = ' "$idx" "$sub"
  sudo "$EC" upload -p "$S" "0x$idx" "0x$sub" 2>&1 | tr -d '\r' | tr '\n' ' '
  echo
}

sep "从站概览（AL 状态）"
sudo "$EC" slaves -v 2>&1 | head -40

sep "当前已分配的 PDO（0x1C12 / 0x1C13）"
up 1c12 00; up 1c12 01; up 1c12 02
up 1c13 00; up 1c13 01; up 1c13 02

sep "RxPDO 0x1600 映射内容"
for i in 00 01 02 03 04 05; do up 1600 "$i"; done

sep "TxPDO 0x1A00 映射内容"
for i in 00 01 02 03 04 05; do up 1a00 "$i"; done

sep "SM 同步参数 0x1C32(SM2) / 0x1C33(SM3)"
for i in 01 02 04 05 06 0d 20; do up 1c32 "$i"; done
echo "  ---"
for i in 01 02 04 05 06 0d 20; do up 1c33 "$i"; done

sep "从站基本信息 / 模式"
up 1000 00      # device type
up 6060 00      # modes of operation (当前设定)
up 6061 00      # modes of operation display
up 603f 00      # error code
up 10f1 01      # vendor id (子索引1)

sep "SII 原始 PDO 定义（ethercat cstruct）"
sudo "$EC" cstruct -p "$S" 2>&1 | sed -n '1,120p'

sep "ethercat pdos -p $S"
sudo "$EC" pdos -p "$S" 2>&1 | head -30

sep "dmesg（ethercat）"
sudo dmesg | grep -iE 'ethercat' | tail -30

sep "26-unop-diagnose DONE"
