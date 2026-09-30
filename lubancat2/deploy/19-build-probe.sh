#!/bin/bash
# 19-build-probe.sh —— 在鲁班猫2 上直接用 g++ 编译 ecat_probe 自检工具
# 背景：板上没装 cmake，且本项目 ecat_probe 依赖极少（ethercat_master.cpp + rt_util.h + ecrt.h），
#       直接用 g++ 编译即可，避免为了一个自检工具去装 cmake。
# 前提：deploy/16-igh-build.sh 已安装 IgH（/usr/local/include/ecrt.h、/usr/local/lib/libethercat.so）
set -u
sep() { echo; echo "===== $* ====="; }
APP="${APP_DEST:-/opt/kine-x/app}"
CXXFLAGS_COMMON=(-std=c++17 -O2 -Wall -pthread)

cd "$APP" 2>/dev/null || { echo "✘ 源码目录不存在: $APP（先跑 deploy/18-push-src.sh）"; exit 1; }

sep "环境"
echo "  g++    : $(g++ --version | head -1)"
echo "  源码   : $APP"
echo "  ecrt.h : $(ls /usr/local/include/ecrt.h 2>/dev/null || echo '✘ 缺失')"
echo "  lib    : $(ls /usr/local/lib/libethercat.so 2>/dev/null || echo '✘ 缺失')"

sep "编译 ecat_probe"
g++ "${CXXFLAGS_COMMON[@]}" \
  -I/usr/local/include -Isrc \
  tools/ecat_probe.cpp src/motion/ethercat_master.cpp \
  -o ecat_probe \
  -L/usr/local/lib -lethercat -lpthread -lrt
rc=$?
if [ $rc -ne 0 ] || [ ! -x ./ecat_probe ]; then
  echo "✘ 编译失败 (rc=$rc)"
  exit 1
fi
echo "  ✔ 生成 $(ls -l ecat_probe | awk '{print $5" bytes  "$9}')"

sep "编译 ecat_sdo（SDO 诊断工具）"
g++ "${CXXFLAGS_COMMON[@]}" \
  -I/usr/local/include -Isrc \
  tools/ecat_sdo.cpp \
  -o ecat_sdo \
  -L/usr/local/lib -lethercat -lpthread -lrt
rc=$?
if [ $rc -ne 0 ] || [ ! -x ./ecat_sdo ]; then
  echo "✘ ecat_sdo 编译失败 (rc=$rc)"
  exit 1
fi
echo "  ✔ 生成 $(ls -l ecat_sdo | awk '{print $5" bytes  "$9}')"

sep "链接依赖核对"
ldd ./ecat_probe | grep -E 'ethercat|not found' || echo "  (未见 libethercat，请检查 -L/-l)"

sep "冒烟：--help"
./ecat_probe --help || true

sep "19-build-probe DONE"
echo "上板验证（需已 systemctl start ethercat）："
echo "  sudo $APP/ecat_probe 10            # 默认映射 + DC，跑 10s"
echo "  sudo $APP/ecat_probe 10 --dc 0     # 先关 DC 验证基础链路"
