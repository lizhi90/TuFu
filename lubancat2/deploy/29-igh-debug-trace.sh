#!/bin/bash
# 29-igh-debug-trace.sh [档位] [dc] —— 打开 IgH 内核调试日志，抓取从站配置 FSM 全过程
# 目的：看清主站在 PDO 配置/SM 配置/DC 配置阶段到底写了什么，从站在哪一步返回 0x001E。
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
PROF="${1:-1}"
DC="${2:-0x0300}"
EC=/usr/local/bin/ethercat

sep "打开 IgH 调试日志 (level 1)"
sudo "$EC" debug 1
sudo dmesg -C >/dev/null 2>&1 || true   # 清空 dmesg 便于只看本次

sep "跑 ecat_probe（档位 $PROF, dc $DC，6 秒）"
sudo timeout 40 "$APP/ecat_probe" 6 --profile "$PROF" --dc "$DC" 2>&1 | head -20

sep "关闭 IgH 调试日志"
sudo "$EC" debug 0

sep "本次配置 FSM 日志（关键行）"
sudo dmesg | grep -iE 'ethercat' | head -160

sep "从站当前 SDO 状态（PDO/SM/同步错误）"
sudo "$APP/ecat_sdo" --slave 0 \
  --get 0x1c12:0:8  --get 0x1c12:1:16 \
  --get 0x1c13:0:8  --get 0x1c13:1:16 \
  --get 0x1600:0:8  --get 0x1600:1:32 --get 0x1600:2:32 --get 0x1600:3:32 \
  --get 0x1a00:0:8  --get 0x1a00:1:32 --get 0x1a00:2:32 \
  --get 0x1c32:1:16 --get 0x1c32:2:32 --get 0x1c32:4:16 --get 0x1c32:20:1 \
  --get 0x1c33:1:16 --get 0x1c33:2:32 --get 0x1c33:4:16 --get 0x1c33:20:1 \
  --get 0x603f:0:16 --get 0x6060:0:8 --get 0x6061:0:8 2>&1

sep "从站 AL 状态"
sudo "$EC" slaves 2>&1

sep "29-igh-debug-trace DONE"
