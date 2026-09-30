#!/bin/bash
# 01-probe2.sh —— 第二轮深挖（需要 sudo，本机 sudo 免密）
# 目的：确认抢占模型细节、PCIe 扩展网卡可能性、RT 内核来源、时钟源、看门狗
sep() { echo; echo "===== $* ====="; }

sep "内核 config 的 PREEMPT 全貌"
sudo grep -E '^CONFIG_(PREEMPT|HZ|NO_HZ|SCHED_AUTOGROUP|RT_GROUP_SCHED|TRANSPARENT_HUGEPAGE|DEBUG_PREEMPT|MIGRATION)' "/boot/config-$(uname -r)" | sort
echo "-- PREEMPT_DYNAMIC 是否开启（1=可运行时切换抢占模式）--"
echo "count = $(sudo grep -c '^CONFIG_PREEMPT_DYNAMIC=y' "/boot/config-$(uname -r)")"
echo "-- /sys/kernel/debug/sched/preempt --"
sudo cat /sys/kernel/debug/sched/preempt 2>/dev/null || echo "(不存在：未启用 PREEMPT_DYNAMIC)"

sep "THP 透明大页"
ls -l /sys/kernel/mm/transparent_hugepage/ 2>/dev/null || echo "(内核未编译 THP)"

sep "/boot 内容"
ls -l /boot/

sep "PCIe 控制器与已枚举设备（决定能否加 i210 网卡）"
ls /sys/bus/pci/devices/ 2>/dev/null || echo "(pci 总线不存在)"
echo "-- device-tree 顶层 pcie 节点 --"
ls -d /proc/device-tree/*pcie* 2>/dev/null || echo "(DT 顶层无 pcie 节点)"
echo "-- lspci --"
command -v lspci >/dev/null 2>&1 && sudo lspci -nn 2>/dev/null || echo "(无 lspci)"

sep "apt 源 / 可用的内核与 RT 包"
grep -vhE '^\s*#|^\s*$' /etc/apt/sources.list 2>/dev/null
ls /etc/apt/sources.list.d/ 2>/dev/null
echo "-- 当前内核包 --"
apt-cache policy linux-image-6.1.99-rk356x 2>/dev/null | head -6
echo "-- 搜 RT / rockchip 内核 --"
apt-cache search 'linux-image' 2>/dev/null | grep -iE 'rt|realtime|rockchip|rk356' | head -10
echo "-- rt-tests --"
apt-cache policy rt-tests 2>/dev/null | head -4
echo "-- ethercat 相关包 --"
apt-cache search 'ethercat' 2>/dev/null | head -5
echo "-- 外网可达? --"
(timeout 6 ping -c1 -W3 deb.debian.org >/dev/null 2>&1 && echo "外网可达") || echo "外网不可达（apt 装包可能受限）"

sep "内核源码 / 头文件（为将来编 PREEMPT/RT 内核做准备）"
ls /usr/src/ 2>/dev/null || echo "(无 /usr/src)"
ls "/usr/src/linux-headers-$(uname -r)/" 2>/dev/null | head -5

sep "设备树 model/compatible"
cat /proc/device-tree/model 2>/dev/null; echo
tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null; echo

sep "时钟源（抖动敏感）"
echo "current  : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
echo "available: $(cat /sys/devices/system/clocksource/clocksource0/available_clocksource)"

sep "看门狗 / 电源管理"
ls /sys/class/watchdog/ 2>/dev/null
for w in /sys/class/watchdog/watchdog*/timeout; do [ -e "$w" ] && echo "$w = $(cat $w)"; done
systemctl is-active systemd-timesyncd 2>/dev/null
systemctl is-enabled ntp chrony 2>/dev/null | head

sep "当前 boot cmdline vs extlinux"
echo "proc cmdline: $(cat /proc/cmdline)"
echo "--- extlinux.conf ---"
sudo cat /boot/extlinux/extlinux.conf

sep "PROBE2 DONE"
