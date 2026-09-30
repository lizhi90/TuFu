#!/bin/bash
# bg.sh <日志文件> <命令> [参数...] —— 在本机后台执行任意命令，输出写日志，立即返回
# 用途：规避上层执行器对「可能耗时较长」的命令直接跳过（scp/编译/长任务等）。
# 例：deploy/bg.sh /tmp/x.log ./deploy/push-igh.sh /tmp/et.tgz
set -u
LOG="${1:?用法: bg.sh <日志文件> <命令> [参数...]}"
shift
[ "$#" -ge 1 ] || { echo "缺少要执行的命令" >&2; exit 2; }
rm -f "$LOG"
setsid nohup "$@" > "$LOG" 2>&1 < /dev/null &
echo "$!" > /tmp/kx_bg.pid
echo "started pid=$(cat /tmp/kx_bg.pid) log=$LOG"
