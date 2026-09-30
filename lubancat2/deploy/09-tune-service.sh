#!/bin/bash
# 09-tune-service.sh [install|uninstall|status]
# 把运行时实时调优做成开机自启的 oneshot 服务（重启后自动恢复 governor/中断亲和/限流等）。
# 依赖：调优脚本已随工程部署到 $DEPLOY_DIR/05-tune-runtime.sh
set -u
sep() { echo; echo "===== $* ====="; }

DEPLOY_DIR="${DEPLOY_DIR:-/opt/kine-x/deploy}"
UNIT=/etc/systemd/system/kine-x-tune.service
ECAT_IF="${ECAT_IF:-eth0}"
HOUSE_IF="${HOUSE_IF:-eth1}"
RT_CPU="${RT_CPU:-3}"
HOUSE_CPUS="${HOUSE_CPUS:-0-2}"
MODE="${1:-install}"

case "$MODE" in
  install)
    if [ ! -f "$DEPLOY_DIR/05-tune-runtime.sh" ]; then
      echo "警告：未找到 $DEPLOY_DIR/05-tune-runtime.sh（请先运行 deploy/push.sh）"
    fi
    sep "写入 $UNIT"
    sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=Kine-X realtime runtime tuning (governor/IRQ affinity/limits)
After=network-online.target NetworkManager.service
Wants=network-online.target
ConditionPathExists=${DEPLOY_DIR}/05-tune-runtime.sh

[Service]
Type=oneshot
RemainAfterExit=yes
Environment=ECAT_IF=${ECAT_IF}
Environment=HOUSE_IF=${HOUSE_IF}
Environment=RT_CPU=${RT_CPU}
Environment=HOUSE_CPUS=${HOUSE_CPUS}
ExecStart=/bin/bash ${DEPLOY_DIR}/05-tune-runtime.sh apply

[Install]
WantedBy=multi-user.target
EOF
    sudo systemctl daemon-reload
    sudo systemctl enable --now kine-x-tune.service
    sep "状态"
    systemctl --no-pager --full status kine-x-tune.service 2>&1 | head -20
    ;;

  uninstall)
    sudo systemctl disable --now kine-x-tune.service 2>/dev/null
    sudo rm -f "$UNIT"
    sudo systemctl daemon-reload
    echo "已卸载 kine-x-tune.service"
    ;;

  status)
    systemctl --no-pager --full status kine-x-tune.service 2>&1 | head -20
    cat "$UNIT" 2>/dev/null
    ;;

  *) echo "用法: $0 [install|uninstall|status]"; exit 2 ;;
esac

sep "09-tune-service DONE ($MODE)"
