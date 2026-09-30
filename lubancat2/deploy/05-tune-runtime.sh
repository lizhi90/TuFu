#!/bin/bash
# 05-tune-runtime.sh [apply|status|revert]
# 运行时实时调优（不重启即可生效；全部记录原值，支持回退）。
#   - CPU governor -> performance
#   - 关闭 cpuidle 深睡眠（只保留最浅状态，降低唤醒抖动）
#   - 中断亲和：EtherCAT 网口 IRQ 绑到 RT_CPU，其余外设 IRQ 移出 RT_CPU
#   - kernel.sched_rt_runtime_us=-1 / timer_migration=0 / nmi_watchdog=0 等
#   - 放开 rtprio / memlock 限制（新增限值文件，可删除回退）
# 环境变量：ECAT_IF(默认 eth0)  HOUSE_IF(默认 eth1)  RT_CPU(默认 3)
set -u
sep() { echo; echo "===== $* ====="; }

MODE="${1:-apply}"
# 网口规划：eth0 = EtherCAT 专用口（IgH/ecMaster 接管，Linux 不配 IP）
#           eth1 = 业务/管理口（出厂固定静态 192.168.1.11，SSH 在此）
ECAT_IF="${ECAT_IF:-eth0}"
HOUSE_IF="${HOUSE_IF:-eth1}"
RT_CPU="${RT_CPU:-3}"
HOUSE_CPUS="${HOUSE_CPUS:-0-2}"
STATE=/var/tmp/kx_rt_state
LIMITS=/etc/security/limits.d/99-kine-x-rt.conf

nproc_all=$(nproc)

# ---------- 记录/回退 ----------
save_state() {
  mkdir -p "$STATE"
  # 只保存一次「原始」状态，重复 apply 不覆盖，保证 revert 能回到调优前
  if [ -f "$STATE/.saved" ]; then
    echo "已存在原状态快照 $STATE（不覆盖）"
    return 0
  fi
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    cat "$p/scaling_governor" > "$STATE/$(basename $p).gov" 2>/dev/null
  done
  grep -H . /proc/sys/kernel/sched_rt_runtime_us /proc/sys/kernel/timer_migration \
          /proc/sys/kernel/nmi_watchdog /proc/sys/vm/swappiness > "$STATE/sysctl.bak" 2>/dev/null
  for f in /proc/irq/*/smp_affinity_list; do
    n=$(basename "$(dirname "$f")")
    cat "$f" > "$STATE/irq-$n.aff" 2>/dev/null
  done
  for c in /sys/devices/system/cpu/cpu*/cpuidle/state*; do
    [ -e "$c/disable" ] || continue
    printf '%s %s\n' "$c/disable" "$(cat $c/disable 2>/dev/null)" >> "$STATE/cpuidle.bak"
  done
  echo "已保存原状态到 $STATE"
  touch "$STATE/.saved"
}

do_revert() {
  sep "回退 governor"
  for f in "$STATE"/*.gov; do
    [ -e "$f" ] || continue
    pol=$(basename "$f" .gov)
    sudo sh -c "echo '$(cat $f)' > /sys/devices/system/cpu/cpufreq/$pol/scaling_governor" 2>/dev/null && echo "  $pol -> $(cat $f)"
  done
  sep "回退 sysctl"
  if [ -r "$STATE/sysctl.bak" ]; then
    while IFS=: read -r k v; do
      sudo sysctl -w "${k#/proc/sys/}=$v" >/dev/null 2>&1 && echo "  $k=$v"
    done < "$STATE/sysctl.bak"
  fi
  sep "回退 cpuidle"
  if [ -r "$STATE/cpuidle.bak" ]; then
    while read -r f v; do sudo sh -c "echo $v > $f" 2>/dev/null; done < "$STATE/cpuidle.bak"
    echo "  已恢复"
  fi
  sep "回退 IRQ 亲和"
  for f in "$STATE"/irq-*.aff; do
    [ -e "$f" ] || continue
    n=$(basename "$f" .aff); n=${n#irq-}
    sudo sh -c "echo '$(cat $f)' > /proc/irq/$n/smp_affinity_list" 2>/dev/null && echo "  irq $n -> $(cat $f)"
  done
  sep "回退 rtprio/memlock"
  sudo rm -f "$LIMITS" && echo "  已删除 $LIMITS"
  echo "（新登录会话生效）"
}

# ---------- 应用 ----------
irqs_of_if() {  # $1=网口名 -> 输出该网口的 IRQ 号
  grep -iE "^\s*[0-9]+:.*[[:space:]]$1([[:space:]-]|$)" /proc/interrupts 2>/dev/null \
    | sed -E 's/^\s*([0-9]+):.*/\1/'
}
pin_if() {  # $1=网口名 $2=cpulist
  local ifn="$1" cpul="$2" n any=0
  for n in $(irqs_of_if "$ifn"); do
    if sudo sh -c "echo '$cpul' > /proc/irq/$n/smp_affinity_list" 2>/dev/null; then
      echo "  irq $n ($ifn) -> cpu$cpul"; any=1
    fi
  done
  [ "$any" = 0 ] && echo "  ($ifn 未发现可设置的 IRQ)"
}

do_apply() {
  save_state
  sep "CPU governor -> performance"
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    sudo sh -c "echo performance > $p/scaling_governor" 2>/dev/null \
      && echo "  $(basename $p) -> $(cat $p/scaling_governor)"
  done

  sep "关闭 cpuidle 深睡眠（保留 state0）"
  for d in /sys/devices/system/cpu/cpu*/cpuidle; do
    [ -d "$d" ] || continue
    for s in "$d"/state*; do
      [ -e "$s/disable" ] || continue
      idx=$(basename "$s"); idx=${idx#state}
      if [ "$idx" -ge 1 ]; then
        sudo sh -c "echo 1 > $s/disable" 2>/dev/null
      fi
    done
  done
  echo "  cpu0 state disable = $(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/disable 2>/dev/null | tr '\n' ' ')"

  sep "中断亲和"
  echo "  将 $ECAT_IF 的 IRQ 绑到 RT_CPU=$RT_CPU，其余外设 IRQ 移到 $HOUSE_CPUS"
  ECAT_IRQS=" $(irqs_of_if "$ECAT_IF" | tr '\n' ' ')"
  pin_if "$ECAT_IF" "$RT_CPU"
  pin_if "$HOUSE_IF" "$HOUSE_CPUS"
  # 其余可在 /proc/interrupts 中列出、且非 IPI/定时器的 IRQ，尽量移出 RT_CPU
  # 注意：排除 EtherCAT 网口的 IRQ（它应保留在 RT_CPU 上）
  for n in $(awk -F: '/^[[:space:]]*[0-9]+:/{print $1}' /proc/interrupts | tr -d ' '); do
    case "$ECAT_IRQS" in *" $n "*) continue ;; esac
    cur=$(cat /proc/irq/$n/smp_affinity_list 2>/dev/null) || continue
    case "$cur" in *"$RT_CPU"*|"$RT_CPU")
      sudo sh -c "echo '$HOUSE_CPUS' > /proc/irq/$n/smp_affinity_list" 2>/dev/null \
        && echo "  irq $n : $cur -> $HOUSE_CPUS" ;;
    esac
  done

  sep "内核 sysctl"
  sudo sysctl -w kernel.sched_rt_runtime_us=-1 >/dev/null && echo "  sched_rt_runtime_us=-1 (RT 不再被限流)"
  sudo sysctl -w kernel.timer_migration=0 >/dev/null && echo "  timer_migration=0"
  sudo sysctl -w kernel.nmi_watchdog=0 >/dev/null 2>&1 && echo "  nmi_watchdog=0"
  sudo sysctl -w vm.swappiness=10 >/dev/null && echo "  vm.swappiness=10"

  sep "rtprio / memlock 限制"
  sudo tee "$LIMITS" >/dev/null <<'EOF'
# Kine-X 实时运行所需（由 deploy/05-tune-runtime.sh 生成）
*            -    rtprio     99
*            -    memlock    unlimited
@realtime    -    rtprio     99
@realtime    -    memlock    unlimited
EOF
  echo "  已写入 $LIMITS"
  # 让 cat 用户能开 RT 线程（跑运动控制服务）
  if ! getent group realtime >/dev/null 2>&1; then
    sudo groupadd -r realtime && echo "  新建组 realtime"
  fi
  # 注意：systemd 服务里 $USER 可能未定义（set -u 会报错），改用 id -un
  CUR_USER="$(id -un 2>/dev/null || echo cat)"
  id -nG "$CUR_USER" 2>/dev/null | grep -qw realtime || {
    sudo usermod -aG realtime "$CUR_USER" && echo "  已将 $CUR_USER 加入 realtime 组（下次登录生效）"
  }
  echo "  当前会话 ulimit -r = $(ulimit -r)（重新登录后为 99）"

  sep "确认 irqbalance"
  if systemctl is-active irqbalance >/dev/null 2>&1; then
    sudo systemctl disable --now irqbalance && echo "  已停用 irqbalance（改用手工亲和）"
  else
    echo "  irqbalance 未运行，无需处理"
  fi
}

case "$MODE" in
  apply)  do_apply ;;
  revert) do_revert ;;
  status)
    sep "governor"; for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "  $(basename $p)=$(cat $p/scaling_governor)"; done
    sep "cmdline"; cat /proc/cmdline
    sep "rt_runtime"; cat /proc/sys/kernel/sched_rt_runtime_us
    sep "interrupts"; grep -iE "eth|gmac|dwmac" /proc/interrupts
    ;;
  *) echo "用法: $0 [apply|status|revert]"; exit 2 ;;
esac

sep "05-tune-runtime DONE ($MODE)"
