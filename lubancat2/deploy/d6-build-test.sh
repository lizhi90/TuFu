#!/bin/bash
# d6-build-test.sh —— 在鲁班猫2 上编译并运行 DebugServer 单测（含 D6 文件管理用例）
# 在板上执行（源码已由 deploy/18-push-src.sh 推到 /opt/kine-x/app）。
set -u
cd /opt/kine-x/app || exit 1
sep() { echo; echo "===== $* ====="; }

sep "编译 debug_server_test"
g++ -std=c++17 -O2 -Wall -Wextra -pthread -Isrc -Ithird_party/lua \
  tools/debug_server_test.cpp src/script/debug_server.cpp src/script/engine_rule.cpp \
  src/script/lua_engine.cpp src/script/script.cpp src/script/motion_host.cpp \
  src/script/nvram_store.cpp \
  src/script/port_manager.cpp src/common/config.cpp \
  -o /tmp/debug_server_test -lpthread -lrt \
  $( [ -f liblua_zm.a ] && echo liblua_zm.a -lm || echo -Lthird_party/lua -llua -lm )
rc=$?
if [ $rc -ne 0 ] || [ ! -x /tmp/debug_server_test ]; then
  echo "✘ debug_server_test 编译失败 (rc=$rc)"
  exit 1
fi
echo "  ✔ 生成 /tmp/debug_server_test"

sep "运行单测（含 D6 文件管理用例）"
/tmp/debug_server_test | tee /tmp/kx_d6_test_result.log | tail -5
rc=$?
sep "结果"
if grep -q "FAIL" /tmp/kx_d6_test_result.log; then
  echo "✘ 存在失败用例（grep FAIL 见上）"
  exit 1
fi
echo "✔ debug_server_test 全部通过 (rc=$rc)"
