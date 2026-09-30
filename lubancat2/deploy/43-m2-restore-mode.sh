#!/bin/bash
# 43-m2-restore-mode.sh —— 把驱动器模式复位回上电默认 CSP(6060=8)，并留档被测试改过的斜坡参数
#
# 背景：`40/42-m2-jog.sh` 会把 6060 置 3(PV)，测试结束不会自动复位；
#       实读（41）显示运行后 `6060=6061=3`。为避免与「上电默认 8=CSP」的既有结论混淆，
#       本脚本用 SDO 写回 6060=8 并回读 6061 确认。
#       同时留档 `6081/6083/6084/607F/6065`（这些是测试残留或出厂值，本脚本不修改）。
#
# 依赖：`ecat_sdo` 需支持 `--set 0xIDX:SUB:BITS=VALUE`（本脚本会强制重编以取最新源码）。
# 用法：deploy/ssh_run.sh deploy/43-m2-restore-mode.sh
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat

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

sep "强制重编 ecat_sdo（取最新 --set 支持）"
cd "$APP" || { echo "✘ 源码目录不存在: $APP"; exit 1; }
g++ -std=c++17 -O2 -Wall -Wextra -I/usr/local/include -Isrc \
    tools/ecat_sdo.cpp -o ecat_sdo \
    -L/usr/local/lib -lethercat -lpthread -lrt
echo "  rc=$?  产物: $(ls -l ecat_sdo | awk '{print $5" bytes  "$6" "$7" "$8}')"

sep "① 复位前：模式与斜坡参数留档"
sudo "$APP/ecat_sdo" \
  --get 0x6060:00:8 --get 0x6061:00:8 \
  --get 0x6081:00:32 --get 0x6083:00:32 --get 0x6084:00:32 \
  --get 0x609A:00:32 --get 0x607F:00:32 --get 0x6065:00:32 \
  2>&1 | head -14

sep "② 写回 6060=8（CSP，上电默认）并自动回读"
sudo "$APP/ecat_sdo" --set 0x6060:00:8=8 2>&1 | head -6

sep "③ 等 500ms 让 6061 模式回显更新"
sleep 1
sudo "$APP/ecat_sdo" --get 0x6061:00:8 --get 0x6041:00:16 2>&1 | head -6

cat <<'TXT'

  判读：③ 的 6061 应为 8（=CSP）。若仍为 3，说明驱动不接受该写入（查 6502h 支持模式表）。
  说明：6081/6083/6084 是 38/40 测试写入的残留（写入掉电保持，H0E.01=3），
        正式投产前请确认程序每次 PP 定位都会重设，或按机械值手工修正。
TXT

sep "从站状态"
sudo "$EC" slaves 2>&1 | head -3

sep "43-m2-restore-mode DONE"
