#!/bin/bash
# 12-verify.sh —— 重启后复核实时化配置是否生效
sep() { echo; echo "===== $* ====="; }

sep "内核与启动参数"
uname -a
echo "cmdline: $(cat /proc/cmdline)"
for k in threadirqs skew_tick isolcpus rcu_nocbs; do
  if grep -qw "$k" /proc/cmdline; then echo "  ✔ $k 已启用"; else echo "  ✘ $k 未生效"; fi
done
echo "注意：isolcpus 隔离核 = $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '(未设置)')"
echo "      nohz_full   = $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '(未设置/内核未开 NO_HZ_FULL)')"

sep "调频 / 睡眠"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "  $(basename $p): governor=$(cat $p/scaling_governor) cur=$(cat $p/scaling_cur_freq)"
done
echo "  cpuidle disable = $(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/disable 2>/dev/null | tr '\n' ' ')"

sep "实时调度"
echo "  sched_rt_runtime_us = $(cat /proc/sys/kernel/sched_rt_runtime_us)"
echo "  timer_migration     = $(cat /proc/sys/kernel/timer_migration)"
echo "  ulimit -r / -l      = $(ulimit -r) / $(ulimit -l)"
echo "  realtime 组         = $(getent group realtime)"

sep "中断亲和"
grep -iE 'eth0|eth1|gmac|dwmac' /proc/interrupts
for n in $(grep -iE 'eth0|eth1' /proc/interrupts | sed -E 's/^\s*([0-9]+):.*/\1/'); do
  echo "  irq $n -> $(cat /proc/irq/$n/smp_affinity_list 2>/dev/null)"
done

sep "网口"
ip -br addr
for i in eth0 eth1; do
  echo "  $i operstate=$(cat /sys/class/net/$i/operstate 2>/dev/null) speed=$(cat /sys/class/net/$i/speed 2>/dev/null)"
done
echo "  NM 是否管理 eth0: $(nmcli -t dev status 2>/dev/null | grep -E '^eth0:' || echo '(未列出→已 unmanaged)')"

sep "systemd 服务"
for u in kine-x-tune.service kine-x.service; do
  printf '  %-22s enabled=%s active=%s\n' "$u" "$(systemctl is-enabled $u 2>/dev/null)" "$(systemctl is-active $u 2>/dev/null)"
done

sep "EtherCAT 环境"
ls /usr/local/include/ecrt.h /usr/include/ecrt.h 2>/dev/null || echo "  (未安装 IgH ecrt.h)"
command -v ethercat >/dev/null 2>&1 && echo "  ethercat CLI: $(command -v ethercat)" || echo "  (无 ethercat CLI)"

sep "12-verify DONE"
