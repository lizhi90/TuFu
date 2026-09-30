#!/bin/bash
# 07-probe4.sh —— 第四轮：网络管理方式 / 网口规划 / 实时组 / 可精简服务
sep() { echo; echo "===== $* ====="; }

sep "网络管理方式"
for s in NetworkManager systemd-networkd networking ifupdown2 dhcpcd; do
  printf '  %-18s %s\n' "$s" "$(systemctl is-active $s 2>/dev/null) / $(systemctl is-enabled $s 2>/dev/null)"
done
echo "-- netplan? --"; ls /etc/netplan/ 2>/dev/null || echo "(无 netplan)"
echo "-- /etc/network/interfaces --"; cat /etc/network/interfaces 2>/dev/null
echo "-- /etc/network/interfaces.d --"; ls -l /etc/network/interfaces.d/ 2>/dev/null
echo "-- NM 连接 --"; nmcli -t con show 2>/dev/null | head -10 || echo "(无 nmcli)"

sep "网口现状"
ip -br addr
for i in eth0 eth1; do
  echo "-- $i --"
  echo "   operstate=$(cat /sys/class/net/$i/operstate 2>/dev/null) carrier=$(cat /sys/class/net/$i/carrier 2>/dev/null) speed=$(cat /sys/class/net/$i/speed 2>/dev/null)"
  echo "   driver=$(basename $(readlink -f /sys/class/net/$i/device/driver) 2>/dev/null)"
  echo "   irq: $(grep -iE "[[:space:]]$i([[:space:]-]|$)" /proc/interrupts | sed -E 's/^\s*([0-9]+):.*/\1/' | tr '\n' ' ')"
done

sep "中断亲和（调优后）"
for n in 50 51 52 53; do
  echo "  irq $n = $(cat /proc/irq/$n/smp_affinity_list 2>/dev/null)  [$(grep -E "^ *$n:" /proc/interrupts | head -1 | cut -c1-60)]"
done

sep "realtime 组 / 用户"
getent group realtime || echo "(无 realtime 组)"
id cat 2>/dev/null
grep -rn 'realtime' /etc/group 2>/dev/null | head

sep "实时相关内核参数现状"
echo "governor        = $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)"
echo "cpuidle disable = $(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/disable 2>/dev/null | tr '\n' ' ')"
echo "rt_runtime_us   = $(cat /proc/sys/kernel/sched_rt_runtime_us)"
echo "timer_migration = $(cat /proc/sys/kernel/timer_migration)"
echo "cmdline         = $(cat /proc/cmdline)"

sep "可精简的服务（实时系统建议关闭）"
for s in bluetooth ModemManager avahi-daemon cups triggerhappy apt-daily.timer apt-daily-upgrade.timer man-db.timer systemd-timesyncd chrony; do
  st=$(systemctl is-enabled "$s" 2>/dev/null)
  [ "$st" = "enabled" ] && printf '  [enabled] %-26s active=%s\n' "$s" "$(systemctl is-active $s 2>/dev/null)"
done

sep "已装/可用工具"
for t in cyclictest hackbench ethtool git stty socat nc; do
  command -v "$t" >/dev/null 2>&1 && echo "  OK  $t" || echo "  缺  $t"
done

sep "PROBE4 DONE"
