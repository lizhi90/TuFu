#!/bin/bash
# 06-cmdline.sh [show|apply|revert] [额外内核参数...]
# 作用：向鲁班猫2「真正的启动入口」/boot/uEnv/uEnv.txt 注入实时内核参数。
#
# 背景：extlinux.conf 在本机不生效；boot.scr 会 `env import -t` 读取 uEnv.txt，
#       并把其中的 cmdline= 变量追加到 bootargs。
# 默认注入：threadirqs skew_tick=1 isolcpus=3 rcu_nocbs=3
#   说明：内核为 PREEMPT_VOLUNTARY（非 RT）→ 没有 nohz_full（CONFIG_NO_HZ_FULL 未开），
#         故不加 nohz_full，避免误解。IRQ_FORCED_THREADING=y 支持 threadirqs。
# 注意：修改后需重启生效；脚本自动备份，可用 revert 还原。
set -u
sep() { echo; echo "===== $* ====="; }

ENVFILE=/boot/uEnv/uEnv.txt          # 实为符号链接
DEFAULT_EXTRA="threadirqs skew_tick=1 isolcpus=3 rcu_nocbs=3"

MODE="${1:-show}"
shift || true
EXTRA="${*:-$DEFAULT_EXTRA}"

REAL=$(readlink -f "$ENVFILE")
sep "启动环境文件"
echo "uEnv.txt -> $REAL"
ls -l "$REAL"
CMD_LINE=$(grep -n '^cmdline=' "$REAL" | head -1 | cut -d: -f1)
echo "cmdline= 位于第 ${CMD_LINE:-?} 行"

show_cmdline() {
  sep "当前 cmdline 变量"
  grep -n '^cmdline=' "$REAL" || echo "(未找到 cmdline= 行)"
}

case "$MODE" in
  show)
    show_cmdline
    sep "当前运行内核参数"; cat /proc/cmdline
    ;;
  apply)
    BAK="${REAL}.bak-$(date +%Y%m%d-%H%M%S)"
    sudo cp -a "$REAL" "$BAK" && echo "已备份 -> $BAK"
    if [ -z "$CMD_LINE" ]; then
      echo "未找到 cmdline= 行，追加新行"
      sudo tee -a "$REAL" >/dev/null <<EOF
cmdline="$EXTRA"
EOF
    else
      # 幂等：把 EXTRA 中每个参数补进 cmdline 双引号内（已存在则跳过）
      cur=$(sed -n "${CMD_LINE}p" "$REAL")
      inner=$(printf '%s' "$cur" | sed -E 's/^cmdline="(.*)"[[:space:]]*$/\1/')
      new="$inner"
      for a in $EXTRA; do
        case " $new " in *" $a "*) ;; *) new="$new $a" ;; esac
      done
      new=$(printf '%s' "$new" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
      sudo sed -i "${CMD_LINE}s|.*|cmdline=\"$new\"|" "$REAL"
      echo "已更新 cmdline 行："
    fi
    show_cmdline
    sep "对照：即将生效的内核参数（重启后）"
    echo "$(cat /proc/cmdline) $EXTRA"
    sep "回退方法"
    echo "  sudo cp -a '$BAK' '$REAL' && sudo reboot"
    echo "  或： deploy/06-cmdline.sh revert"
    ;;
  revert)
    LAST=$(ls -1t "${REAL}".bak-* 2>/dev/null | head -1)
    if [ -z "$LAST" ]; then echo "没找到备份，无法回退"; exit 1; fi
    sudo cp -a "$LAST" "$REAL" && echo "已从 $LAST 还原 $REAL"
    show_cmdline
    echo "重启后生效：sudo reboot"
    ;;
  *)
    echo "用法: $0 [show|apply|revert] [额外内核参数...]"; exit 2 ;;
esac

sep "06-cmdline DONE ($MODE)"
