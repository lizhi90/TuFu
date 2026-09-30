#!/bin/bash
# 13-pin-irq.sh [ifname] [cpulist] [up|noup]
# 把某个网口的中断绑定到指定 CPU（默认 eth0 -> cpu3）。
# 用途：EtherCAT 专用网口在 Linux 侧是「未管理、默认 down」的，网口 down 时内核里
#       看不到对应 IRQ。本脚本可先把网口 up，等 IRQ 出现后再绑定，保证运行时亲和正确。
# 典型调用（kine-x.service 的 ExecStartPre）：
#   bash /opt/kine-x/deploy/13-pin-irq.sh eth0 3 up
set -u
IF="${1:-eth0}"
CPUS="${2:-3}"
DOUP="${3:-up}"
WAIT_SEC="${WAIT_SEC:-8}"

if ! ip link show "$IF" >/dev/null 2>&1; then
  echo "[pin-irq] 网口 $IF 不存在"; exit 1
fi

if [ "$DOUP" = "up" ]; then
  sudo ip link set "$IF" up 2>/dev/null || true
fi

irqs() { grep -iE "^ *[0-9]+:.*[[:space:]]$IF([[:space:]-]|$)" /proc/interrupts 2>/dev/null | sed -E 's/^ *([0-9]+):.*/\1/'; }

t=0
while [ "$t" -lt "$WAIT_SEC" ]; do
  [ -n "$(irqs)" ] && break
  sleep 1; t=$((t + 1))
done

list=$(irqs)
if [ -z "$list" ]; then
  echo "[pin-irq] 未在 /proc/interrupts 中找到 $IF 的中断（网口可能无载波/驱动未注册）"
  exit 0
fi

for n in $list; do
  if sudo sh -c "echo '$CPUS' > /proc/irq/$n/smp_affinity_list" 2>/dev/null; then
    echo "[pin-irq] $IF irq $n -> cpu$CPUS (当前=$(cat /proc/irq/$n/smp_affinity_list))"
  else
    echo "[pin-irq] $IF irq $n 绑定失败"
  fi
done
