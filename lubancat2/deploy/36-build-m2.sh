#!/bin/bash
# 36-build-m2.sh —— 在鲁班猫2 上直接用 g++ 编译 M2 产物（板上无 cmake）
#   产物：ecat_axis（M2 验证工具）、kine-x（主程序，含 BASIC + Lua 双脚本引擎）
#   顺带编译并运行纯逻辑自测（cia402_test / axis_test / script_test / lua_engine_test /
#   engine_rule_test），确认运动内核与脚本引擎无回归、脚本语言互斥规则成立。
# 前提：16-igh-build.sh 已装 IgH（/usr/local/include/ecrt.h、/usr/local/lib/libethercat.so）
set -u
sep() { echo; echo "===== $* ====="; }
APP="${APP_DEST:-/opt/kine-x/app}"
CXXFLAGS_COMMON=(-std=c++17 -O2 -Wall -Wextra -pthread)
INC=(-I/usr/local/include -Isrc -Ithird_party/lua)
LIB=(-L/usr/local/lib -lethercat -lpthread -lrt)

cd "$APP" 2>/dev/null || { echo "✘ 源码目录不存在: $APP（先跑 deploy/18-push-src.sh）"; exit 1; }

sep "环境"
echo "  g++    : $(g++ --version | head -1)"
echo "  源码   : $APP"
echo "  ecrt.h : $(ls /usr/local/include/ecrt.h 2>/dev/null || echo '✘ 缺失')"
echo "  lib    : $(ls /usr/local/lib/libethercat.so 2>/dev/null || echo '✘ 缺失')"

build_one() {  # build_one <输出名> <源文件...>
  local out="$1"; shift
  sep "编译 $out"
  g++ "${CXXFLAGS_COMMON[@]}" "${INC[@]}" "$@" -o "$out" "${LIB[@]}"
  local rc=$?
  if [ $rc -ne 0 ] || [ ! -x "./$out" ]; then
    echo "✘ $out 编译失败 (rc=$rc)"
    return 1
  fi
  echo "  ✔ 生成 $(ls -l "$out" | awk '{print $5" bytes  "$9}')"
  return 0
}

# ---- vendor Lua 5.4 静态库（板上无 cmake，用 gcc -c 汇编成 .a）----
sep "编译 vendor Lua (third_party/lua)"
LUA_LIB="$APP/liblua_zm.a"
if compgen -G "third_party/lua/*.c" > /dev/null; then
  rm -rf /tmp/kx_lua_obj && mkdir -p /tmp/kx_lua_obj
  for f in third_party/lua/*.c; do
    gcc -O2 -w -Ithird_party/lua -c "$f" -o "/tmp/kx_lua_obj/$(basename "${f%.c}").o" \
      || { echo "✘ Lua 编译失败: $f"; exit 1; }
  done
  ar rcs "$LUA_LIB" /tmp/kx_lua_obj/*.o
  LIB+=("$LUA_LIB" -lm)
  echo "  ✔ $LUA_LIB"
else
  echo "  ✘ 缺少 third_party/lua/*.c（先跑 deploy/18-push-src.sh）"; exit 1
fi

# ---- 纯逻辑自测（不需要 ecrt，先跑确认运动内核/脚本引擎没回归）----
sep "逻辑自测（假驱动器 / 假宿主，无硬件）"
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/cia402_test.cpp src/motion/cia402.cpp -o /tmp/cia402_test \
  && /tmp/cia402_test; rc_c=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/axis_test.cpp src/motion/cia402.cpp src/motion/axis.cpp -o /tmp/axis_test \
  && /tmp/axis_test; rc_a=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/script_test.cpp src/script/script.cpp -o /tmp/script_test \
  && /tmp/script_test; rc_s=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc -Ithird_party/lua tools/lua_engine_test.cpp src/script/lua_engine.cpp \
  "$LUA_LIB" -lm -o /tmp/lua_engine_test \
  && /tmp/lua_engine_test; rc_l=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc -Ithird_party/lua tools/engine_rule_test.cpp \
  src/script/engine_rule.cpp src/script/script.cpp src/script/lua_engine.cpp \
  "$LUA_LIB" -lm -o /tmp/engine_rule_test \
  && /tmp/engine_rule_test; rc_r=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/boot_config_test.cpp \
  src/script/boot_config.cpp -o /tmp/boot_config_test \
  && /tmp/boot_config_test; rc_b=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/source_include_test.cpp \
  src/script/source_include.cpp src/script/boot_config.cpp -o /tmp/source_include_test \
  && /tmp/source_include_test; rc_i=$?
g++ -std=c++17 -O2 -Wall -pthread -Isrc tools/nvram_test.cpp \
  src/script/nvram_store.cpp -o /tmp/nvram_test \
  && /tmp/nvram_test; rc_n=$?
echo "  cia402_test rc=$rc_c   axis_test rc=$rc_a   script_test rc=$rc_s   lua_engine_test rc=$rc_l   engine_rule_test rc=$rc_r   boot_config_test rc=$rc_b   source_include_test rc=$rc_i   nvram_test rc=$rc_n"

# ---- M2 验证工具 ----
# v0.3.2 修复：ethercat_master 引用 ProfileRegistry，需一并链接 drive_profile + profiles/*
build_one ecat_axis tools/ecat_axis.cpp \
  src/motion/ethercat_master.cpp src/motion/drive_profile.cpp \
  src/motion/profiles/sv630.cpp src/motion/profiles/generic.cpp \
  src/motion/cia402.cpp src/motion/axis.cpp || exit 1

# ---- 主程序 kine-x ----
# 清单与 CMakeLists.txt 的 add_executable(kine-x ...) 保持一致（含 DebugServer）
build_one kine-x src/main.cpp src/common/config.cpp \
  src/motion/ethercat_master.cpp src/motion/drive_profile.cpp \
  src/motion/profiles/sv630.cpp src/motion/profiles/generic.cpp \
  src/motion/cia402.cpp src/motion/axis.cpp \
  src/script/script.cpp src/script/motion_host.cpp src/script/port_manager.cpp \
  src/script/engine_rule.cpp src/script/lua_engine.cpp \
  src/script/debug_server.cpp src/script/boot_config.cpp src/script/port_config.cpp \
  src/script/nvram_store.cpp \
  src/modbus/modbus_config.cpp src/modbus/modbus_server.cpp \
  src/modbus/modbus_master_config.cpp src/modbus/modbus_master.cpp \
  src/script/source_include.cpp || exit 1

# ---- 自检工具 ----
# ecat_probe 引用 ProfileRegistry（同 ecat_axis，v0.3.2 起）：需一并链接 drive_profile + profiles/*
[ -x ./ecat_probe ] || build_one ecat_probe tools/ecat_probe.cpp \
  src/motion/ethercat_master.cpp src/motion/drive_profile.cpp \
  src/motion/profiles/sv630.cpp src/motion/profiles/generic.cpp || true
build_one ecat_sdo tools/ecat_sdo.cpp    # 诊断工具会随需求演进（如新增 --set），始终随源码重编

sep "产物清单"
ls -l ecat_axis kine-x ecat_probe ecat_sdo 2>/dev/null | awk '{print "  "$5" bytes  "$9}'

sep "冒烟：--help"
./ecat_axis --help | head -20

sep "36-build-m2 DONE"
echo "下一步：deploy/37-m2-verify.sh [dist_mm] [speed] [profile] [dc]"
