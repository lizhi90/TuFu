#!/bin/bash
# 00-probe.sh —— 鲁班猫2 实时/环境探测（只读，无需 sudo）
# 用法（本机）: ssh cat@192.168.1.11 'bash -s' < deploy/00-probe.sh
sep() { echo; echo "===== $* ====="; }

sep "OS / release"
head -4 /etc/os-release 2>/dev/null
uname -a
cat /proc/version

sep "内核抢占模型（最关键）"
if [ -e /sys/kernel/realtime ]; then
  echo "/sys/kernel/realtime = $(cat /sys/kernel/realtime)  -> PREEMPT_RT 内核"
else
  echo "/sys/kernel/realtime 不存在 -> 非 RT 内核"
fi
echo "-- /sys/kernel/debug/sched/preempt（PREEMPT_DYNAMIC 支持的运行时可切换模式）--"
cat /sys/kernel/debug/sched/preempt 2>/dev/null || echo "(读不到：需 root，或未启用 CONFIG_PREEMPT_DYNAMIC)"
echo "-- 内核配置（PREEMPT/HZ/NO_HZ/隔离相关）--"
for f in "/boot/config-$(uname -r)" /proc/config.gz; do
  if [ -r "$f" ]; then
    echo "found: $f"
    (zcat "$f" 2>/dev/null || cat "$f") | grep -E '^CONFIG_(PREEMPT|HZ=|HZ_|NO_HZ|HIGH_RES_TIMERS|RCU_NOCB|CPU_ISOLATION|IRQ_TIME_ACCOUNTING|SCHED_AUTOGROUP|SCHED_DEBUG)' | sort
    break
  fi
done
dpkg -l 2>/dev/null | grep -iE 'linux-image|linux-headers' | awk '{print $2, $3}'

sep "CPU / 调频"
nproc
grep -m1 -E 'model name|Hardware|CPU part' /proc/cpuinfo
ls /sys/devices/system/cpu/cpufreq/ 2>/dev/null
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  echo "-- $p"
  echo "   driver   : $(cat $p/scaling_driver 2>/dev/null)"
  echo "   governor : $(cat $p/scaling_governor 2>/dev/null)"
  echo "   avail_gov: $(cat $p/scaling_available_governors 2>/dev/null)"
  echo "   cur_freq : $(cat $p/scaling_cur_freq 2>/dev/null)"
done

sep "cmdline / isolcpus"
cat /proc/cmdline

sep "启动配置（extlinux / Rockchip 常见）"
ls -l /boot/extlinux/ 2>/dev/null
cat /boot/extlinux/extlinux.conf 2>/dev/null

sep "内存 / THP / swap"
free -m
echo "THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"
swapon --show 2>/dev/null || echo "(无 swap)"

sep "网卡"
ip -br addr
for i in $(ls /sys/class/net 2>/dev/null | grep -v '^lo$'); do
  drv=$(basename "$(readlink -f /sys/class/net/$i/device/driver 2>/dev/null)" 2>/dev/null)
  spd=$(cat /sys/class/net/$i/speed 2>/dev/null)
  st=$(cat /sys/class/net/$i/operstate 2>/dev/null)
  printf '  %-6s driver=%-10s speed=%-8s operstate=%s\n' "$i" "$drv" "$spd" "$st"
done
echo "-- PCI 设备（找 i210 之类独立网卡）--"
lspci 2>/dev/null | head -20 || echo "(无 lspci / 无 PCI)"

sep "irqbalance / 网卡中断"
systemctl is-active irqbalance 2>/dev/null || echo "irqbalance: 未安装或未运行"
grep -iE 'eth|gmac|stmmac|dwmac|rtl|igb|pcie|mmc' /proc/interrupts | head -20

sep "cpuidle（深睡眠会恶化抖动）"
for c in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
  [ -e "$c" ] || continue
  printf '  %s disable=%s\n' "$(cat $c/name 2>/dev/null)" "$(cat $c/disable 2>/dev/null)"
done

sep "实时/资源限制"
systemctl --version | head -1
ls /etc/security/limits.d/ 2>/dev/null
grep -rn 'rtprio\|memlock' /etc/security/limits.conf /etc/security/limits.d/ 2>/dev/null || echo "(limits 未设置 rtprio/memlock)"
echo "ulimit -r (rtprio) = $(ulimit -r)"
echo "ulimit -l (memlock KB) = $(ulimit -l)"
cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
cat /proc/sys/kernel/timer_migration 2>/dev/null
for t in cyclictest hackbench; do command -v $t >/dev/null 2>&1 && echo "$t: $(command -v $t)" || echo "$t: 未安装"; done

sep "EtherCAT"
ls /usr/local/include/ecrt.h /usr/include/ecrt.h 2>/dev/null || echo "(未装 IgH ecrt.h)"
command -v ethercat >/dev/null 2>&1 && echo "ethercat: $(command -v ethercat)" || echo "(无 ethercat CLI)"
ls /usr/local/lib/libethercat* 2>/dev/null || echo "(无 libethercat)"
ls /lib/modules/"$(uname -r)"/ 2>/dev/null | head -5

sep "sudo"
sudo -n true 2>/dev/null && echo "sudo 免密可用" || echo "sudo 需要密码"

sep "PROBE DONE"
