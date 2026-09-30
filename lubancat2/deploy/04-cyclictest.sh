#!/bin/bash
# 04-cyclictest.sh [秒数] [标签] [是否加压 load|idle]
# 采集实时抖动：cyclictest 4 线程(SCHED_FIFO/prio90, 1ms 周期)，可选 hackbench 加压。
DUR="${1:-60}"
TAG="${2:-baseline}"
LOAD="${3:-idle}"
sep() { echo; echo "===== $* ====="; }

sep "环境快照 ($TAG)"
echo "kernel   : $(uname -r)"
echo "cmdline  : $(cat /proc/cmdline)"
echo "clocksrc : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null)"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  echo "$(basename $p): governor=$(cat $p/scaling_governor 2>/dev/null) cur=$(cat $p/scaling_cur_freq 2>/dev/null) freq=$(cat $p/scaling_min_freq 2>/dev/null)~$(cat $p/scaling_max_freq 2>/dev/null)"
done
echo "irqbalance: $(systemctl is-active irqbalance 2>/dev/null)"
echo "loadavg  : $(cat /proc/loadavg)"

if [ "$LOAD" = "load" ]; then
  sep "启动 hackbench 加压"
  sudo hackbench -l 10000 -g 4 >/dev/null 2>&1 &
  HB=$!
  echo "hackbench pid=$HB"
fi

sep "cyclictest (-D $DUR -t4 -i1000 -p90)"
OUT="/tmp/cyclictest_${TAG}.txt"
sudo cyclictest -q -m -S -p90 -i1000 -D "$DUR" -t4 -h 200 2>&1 | tee "$OUT"

if [ -n "${HB:-}" ]; then
  kill "$HB" 2>/dev/null
fi

sep "结果摘要 ($TAG)"
grep -E '^T:|^#' "$OUT" 2>/dev/null | tail -20

sep "04-cyclictest DONE ($TAG)"
