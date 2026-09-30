#!/bin/bash
# 21-post-reboot-check.sh —— 重启后 + 接好 SV630 的链路/主站体检
# 顺序：板子状态 → eth0 物理链路 → 内核模块/主站设备 → master/slaves → dmesg
set -u
sep() { echo; echo "===== $* ====="; }

sep "板子状态"
echo "  uptime : $(uptime -p 2>/dev/null || uptime)"
echo "  kernel : $(uname -r)"
echo "  cmdline: $(cat /proc/cmdline)"

sep "网口"
for i in eth0 eth1; do
  ip -br addr show "$i" 2>/dev/null || echo "  $i 不存在"
done
echo "  --- eth0 物理链路 ---"
sudo ethtool eth0 2>/dev/null | grep -E 'Speed|Duplex|Link detected' || echo "  (ethtool 不可用)"
echo "  --- 网口统计（错误计数）---"
ip -s link show eth0 2>/dev/null | tail -6

sep "调优是否持久化生效"
echo "  governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
echo "  rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null)"
systemctl is-active kine-x-tune.service 2>/dev/null || echo "  kine-x-tune.service: (未安装)"

sep "内核模块"
lsmod | grep -E '^ec_' || echo "  ✘ 无 ec_* 模块加载"
echo "  --- /dev/EtherCAT* ---"
ls -l /dev/EtherCAT* 2>&1

sep "主站状态"
sudo /usr/local/bin/ethercat master 2>&1 | head -20

sep "从站列表"
sudo /usr/local/bin/ethercat slaves 2>&1
echo "  --- slaves -v（前 40 行）---"
sudo /usr/local/bin/ethercat slaves -v 2>&1 | head -40

sep "dmesg（ethercat 相关）"
sudo dmesg | grep -iE 'ethercat|ec_master|ec_generic' | tail -25

sep "21-post-reboot-check DONE"
