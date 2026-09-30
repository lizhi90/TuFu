#!/bin/bash
# 42-m2-estop-time.sh [spd1 spd2 ...] —— 急停响应时间实测（闭合 §17.4 pending）
#
# 原理：ecat_axis 在 PV 点动到速后只置 6040.bit8(halt)，并保留 60FF 不变；
#       进入 ESTOP_HOLD 后逐拍差分 6064h 求瞬时速度，记录：
#         * halt 下发拍 → |v| 首次低于阈值（max(0.5, 2%·v0) mm/s）的用时 = 响应时间
#         * 该段位移（≈停止距离）
#         * 平均减速度 = v0 / 响应时间
#
# 前提：本机改动 tools/ecat_axis.cpp 后需先同步并重编：
#   ./deploy/18-push-src.sh
#   ./deploy/ssh_run.sh deploy/36-build-m2.sh
#
# 用法：deploy/ssh_run.sh deploy/42-m2-estop-time.sh            # 默认测 1/5/10 mm/s
#       deploy/ssh_run.sh deploy/42-m2-estop-time.sh 5 20        # 自定义速度表
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
PROF="${PROF:-2}"          # PV 需 6061 回显 → 档位 ≥ 2
DC="${DC:-0x0300}"
SECS="${SECS:-30}"
LOG=/tmp/kx_estop_summary.txt

if [ "$#" -gt 0 ]; then SPD_LIST="$*"; else SPD_LIST="1 5 10"; fi

sep "清理占用主站的残留进程"
sudo pkill -f 'ecat_axis' 2>/dev/null; sudo pkill -f 'ecat_probe' 2>/dev/null
sudo pkill -f 'kine-x' 2>/dev/null; sleep 1
ps -eo pid,cmd | grep -E 'ecat_|kine-x' | grep -v grep || echo "  (无残留)"

sep "eth0 up"
sudo ip link set eth0 up; sleep 1
ip -br link show eth0

sep "ethercat 服务"
sudo systemctl start ethercat 2>/dev/null || true; sleep 1
sudo "$EC" master 2>&1 | head -3

sep "二进制确认（应含测速日志；如为旧版请先 18-push-src + 36-build-m2）"
ls -l "$APP/ecat_axis" | awk '{print "  "$5" bytes  "$6" "$7" "$8"  "$9}'
# 直接对二进制 grep（板上未必装 binutils 的 strings）
grep -ac '急停响应' "$APP/ecat_axis" | sed 's/^/  含"急停响应"字符串: /'

: > "$LOG"
sep "逐速度测急停响应：$SPD_LIST"
for SPD in $SPD_LIST; do
  echo
  echo "---- 速度 ${SPD} mm/s ----"
  sudo timeout $((SECS + 20)) "$APP/ecat_axis" --jog "$SPD" --jog-secs 2 --estop \
    --profile "$PROF" --dc "$DC" --secs "$SECS" \
    > "/tmp/kx_estop_${SPD}.txt" 2>&1
  echo "  rc=$?"
  grep -aE '检出运动|下发急停|急停响应|响应时间|急停：' "/tmp/kx_estop_${SPD}.txt" \
    | sed 's/^/  /'
  grep -aE '急停响应|响应时间' "/tmp/kx_estop_${SPD}.txt" \
    | sed "s/^/[$SPD mm\/s] /" >> "$LOG"
done

sep "汇总（§17.4 待回填）"
cat "$LOG" 2>/dev/null || echo "  (未采集到响应行)"
cat <<'TXT'

  判读要点：
   1) 响应时间应 ≈ 1 个通信/驱动控制周期量级（本工程 1ms 周期，实测为若干 ms 属正常）；
      若 > 20ms，检查 605Dh（halt 方式，默认 1）、6084h/609Ah（减速度）、H0E.34（CSP 异常允许时间）。
   2) 停止距离与 v0 近似成正比；本机 5mm/s 工况 §17.1 已实测 0.0152mm。
   3) 若某速度下"未在 1.5s 内检出速度降至阈值"，说明 6040.bit8 未生效或减速度极小 → 查 605Dh。
TXT

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -3

sep "42-m2-estop-time DONE"
