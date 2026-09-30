#!/bin/bash
# 15-igh-deps.sh —— 安装 IgH EtherCAT Master 编译依赖（板上执行）
set -u
export DEBIAN_FRONTEND=noninteractive
sep() { echo; echo "===== $* ====="; }

sep "apt update"
sudo apt-get update 2>&1 | tail -5

sep "安装依赖"
sudo apt-get install -y --no-install-recommends \
  build-essential autoconf automake libtool pkg-config m4 \
  git patch bison flex bc rsync kmod 2>&1 | tail -15

sep "版本核对"
for c in gcc make autoconf automake libtool pkg-config m4 bison flex bc; do
  printf '  %-12s %s\n' "$c" "$(command -v $c || echo '(缺失)')"
done
echo "  gcc: $(gcc --version | head -1)"

sep "内核构建树"
echo "  kernel : $(uname -r)"
echo "  headers: $(readlink -f /lib/modules/$(uname -r)/build)"
ls -l "/lib/modules/$(uname -r)/build/Module.symvers" 2>/dev/null || echo "  ✘ 无 Module.symvers"

sep "15-igh-deps DONE"
