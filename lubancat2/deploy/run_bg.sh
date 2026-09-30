#!/bin/bash
# run_bg.sh <本地脚本> [日志文件] [传给远端脚本的参数...]
# 在后台把脚本送到鲁班猫2 执行，立即返回；进度写到日志文件（默认 /tmp/kx_bg.log）。
set -u
cd "$(dirname "$0")/.." || exit 1
SCRIPT="${1:?用法: run_bg.sh <本地脚本> [日志文件] [参数...]}"
shift
LOG="${1:-/tmp/kx_bg.log}"
[ "$#" -gt 0 ] && shift
PIDFILE="/tmp/kx_bg.pid"
rm -f "$LOG"
setsid nohup ./deploy/ssh_run.sh "$SCRIPT" "$@" > "$LOG" 2>&1 < /dev/null &
echo $! > "$PIDFILE"
echo "started pid=$(cat "$PIDFILE") log=$LOG"
