#!/bin/bash
# 14-probe-igh.sh —— 评估板上能否编译 IgH EtherCAT 主站
# 检查：内核头文件/构建工具/网络/磁盘/apt 中的 headers
set -u
sep() { echo; echo "===== $* ====="; }

KREL="$(uname -r)"
sep "基本环境"
echo "kernel : $KREL"
echo "arch   : $(uname -m)"
echo "nproc  : $(nproc)"
echo "disk   : $(df -h / | tail -1)"
echo "mem    : $(free -h | awk '/Mem:/{print $2" total / "$7" avail"}')"

sep "内核头文件 / 模块构建"
if [ -d "/lib/modules/$KREL/build" ]; then
  echo "  /lib/modules/$KREL/build -> $(readlink -f /lib/modules/$KREL/build)"
else
  echo "  ✘ 无 /lib/modules/$KREL/build"
fi
ls -d /usr/src/linux-headers-* 2>/dev/null || echo "  /usr/src 内容: $(ls /usr/src 2>/dev/null | tr '\n' ' ')"
echo "  modules 目录: $(ls -d /lib/modules/$KREL 2>/dev/null || echo '(无)')"
[ -f /lib/modules/$KREL/build/Makefile ] && echo "  ✔ 可用 Makefile" || echo "  ✘ 缺 Makefile"

sep "构建工具"
for c in gcc make autoconf automake libtool m4 pkg-config git wget curl xz patch; do
  printf '  %-12s %s\n' "$c" "$(command -v $c || echo '(缺失)')"
done
echo "  gcc: $(gcc --version 2>/dev/null | head -1)"

sep "apt 中的内核头文件"
apt-cache search --names-only 'linux-headers' 2>/dev/null | head -15 || echo "  (apt-cache 不可用)"
echo "  --- 与当前内核相关:"
apt-cache search --names-only 'linux-headers' 2>/dev/null | grep -iE 'rk35|rockchip|current|6\.1' | head -10 || echo "  (无匹配)"

sep "网络连通性"
for u in https://gitlab.com https://github.com https://mirrors.ustc.edu.cn; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 "$u" 2>/dev/null || echo FAIL)
  echo "  $u -> $code"
done
echo "  DNS: $(getent hosts gitlab.com | head -1 || echo '(解析失败)')"

sep "已装 EtherCAT 相关?"
ls /usr/local/include/ecrt.h /usr/include/ecrt.h 2>/dev/null || echo "  (无 ecrt.h)"
command -v ethercat >/dev/null 2>&1 && echo "  ethercat CLI: $(command -v ethercat)" || echo "  (无 ethercat CLI)"
lsmod 2>/dev/null | grep -E '^ec_' || echo "  (无 ec_* 模块)"

sep "sudo 免密"
sudo -n true 2>/dev/null && echo "  ✔ sudo 免密可用" || echo "  ✘ sudo 需要密码"

sep "14-probe-igh DONE"
