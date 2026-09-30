#!/bin/bash
# 34-m1-accept.sh [秒数] —— M1 验收：环境固化 + SV630 进 OP + 过程数据非零
# 用法：./deploy/run_bg.sh deploy/34-m1-accept.sh /tmp/kx_m1.log 8
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
EC=/usr/local/bin/ethercat
SECS="${1:-8}"
PASS=0
FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

sep "A. 环境固化"
systemctl is-enabled kx-ecat-if.service  >/dev/null 2>&1 && ok "kx-ecat-if.service 已 enable"   || bad "kx-ecat-if.service 未 enable"
systemctl is-enabled ethercat.service    >/dev/null 2>&1 && ok "ethercat.service 已 enable"    || bad "ethercat.service 未 enable"
lsmod | grep -q ec_master                                && ok "ec_master 模块已加载"          || bad "ec_master 未加载"
ls /dev/EtherCAT0 >/dev/null 2>&1                        && ok "/dev/EtherCAT0 存在"            || bad "/dev/EtherCAT0 缺失"
[ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = "1" ] && ok "eth0 carrier=1"                 || bad "eth0 无载波"

sep "B. 配置固化"
CONF=/opt/kine-x/config/app.conf
if [ -f "$CONF" ]; then
  ok "$CONF 存在"
  for k in ECAT_VENDOR ECAT_PRODUCT ECAT_EXPLICIT_PDO ECAT_EXPLICIT_PROFILE; do
    grep -qE "^${k}[[:space:]]*=" "$CONF" && ok "  $k 已配置" || bad "  $k 缺失"
  done
else
  bad "$CONF 不存在"
fi

sep "C. 从站识别"
SLAVES=$(sudo "$EC" slaves 2>&1)
echo "$SLAVES" | sed 's/^/  /'
echo "$SLAVES" | grep -q SV630 && ok "识别到 SV630" || bad "未识别到 SV630"

sep "D. 进 OP + 过程数据（档位1 + DC 0x0300，${SECS}s）"
sudo ip link set eth0 up 2>/dev/null
sudo timeout $((SECS+15)) "$APP/ecat_probe" "$SECS" --profile 1 --dc 0x0300 > /tmp/kx_m1_probe.txt 2>&1 &
PP=$!
sleep 1
OPSEEN=0
for i in $(seq 1 "$SECS"); do
  st=$(sudo "$EC" slaves 2>&1 | awk 'NR==1{print $3}')
  echo "  t=${i}s  AL=$st"
  [ "$st" = "OP" ] && OPSEEN=1
  sleep 1
done
wait $PP 2>/dev/null
[ "$OPSEEN" = "1" ] && ok "从站进入 OP" || bad "从站未进入 OP"

sed -n '/已激活/,$p' /tmp/kx_m1_probe.txt | grep -E 'PDO 偏移|sw=|结束：' | tail -4 | sed 's/^/  /'
LAST=$(grep -oE 'sw=0x[0-9a-f]{4}' /tmp/kx_m1_probe.txt | tail -1)
[ -n "$LAST" ] && [ "$LAST" != "sw=0x0000" ] && ok "过程数据有效（$LAST）" || bad "过程数据为 0 或未读到"
grep -q '抖动=0\|滞后周期=0' /tmp/kx_m1_probe.txt && ok "1ms 周期零丢拍" || echo "  [INFO] 有少量丢拍（非 RT 内核，见 抖动=）"

sep "E. 结论"
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" = "0" ] && echo "  ✅ M1 验收通过" || echo "  ⚠️ 存在失败项，见上"
sep "34-m1-accept DONE"
