#!/bin/bash
# 03-apt-setup.sh —— 修复 apt 列表并安装实时调试/调优工具
# 目标包：rt-tests(cyclictest/hackbench) ethtool cpufrequtils
# 幂等：已装则跳过；失败不致命，尽量继续。
sep() { echo; echo "===== $* ====="; }

sep "APT 源"
grep -vhE '^\s*#|^\s*$' /etc/apt/sources.list 2>/dev/null | head
ls /etc/apt/sources.list.d/ 2>/dev/null

sep "apt-get update（限时 120s）"
sudo timeout 120 apt-get update -o Acquire::Retries=2 -o Acquire::http::Timeout=10 2>&1 | tail -15
echo "update rc=${PIPESTATUS[0]}"

sep "安装工具"
export DEBIAN_FRONTEND=noninteractive
sudo timeout 180 apt-get install -y --no-install-recommends \
  rt-tests ethtool cpufrequtils 2>&1 | tail -20
echo "install rc=${PIPESTATUS[0]}"

sep "验证"
for t in cyclictest hackbench ethtool cpupower; do
  if command -v "$t" >/dev/null 2>&1; then echo "  OK  $t -> $(command -v $t)"; else echo "  缺失 $t"; fi
done

sep "03-apt-setup DONE"
