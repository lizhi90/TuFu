#!/bin/bash
# 10-install-service.sh [install|uninstall|status]
# 安装 kine-x 主程序为 systemd 服务（开机自启、实时权限、崩溃重启）。
# 可通过环境变量覆盖：
#   APP_NAME(kine-x) APP_BIN(/opt/kine-x/bin/kine-x) APP_CONF(/opt/kine-x/config/app.conf)
#   APP_USER(cat) RT_CORES(0-3) WORKDIR(/opt/kine-x)
set -u
sep() { echo; echo "===== $* ====="; }

APP_NAME="${APP_NAME:-kine-x}"
APP_BIN="${APP_BIN:-/opt/kine-x/bin/kine-x}"
APP_CONF="${APP_CONF:-/opt/kine-x/config/app.conf}"
APP_USER="${APP_USER:-cat}"
WORKDIR="${WORKDIR:-/opt/kine-x}"
RT_CORES="${RT_CORES:-0-3}"        # 允许使用的 CPU 集合（motion 线程亲和可在程序内进一步收窄到 3）
ECAT_IF="${ECAT_IF:-eth0}"         # EtherCAT 专用网口
RT_CPU="${RT_CPU:-3}"              # 运动控制 RT 线程/网口中断所在 CPU
UNIT="/etc/systemd/system/${APP_NAME}.service"

MODE="${1:-install}"

case "$MODE" in
  install)
    sep "写入 systemd 单元 $UNIT"
    sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=Kine-X motion controller (EtherCAT + TCP/Modbus services)
Documentation=file:///home/${APP_USER}/kine-x/README.md
After=network-online.target
Wants=network-online.target
# EtherCAT 专用网口就绪后再启动（避免网口未就位）
# After=sys-subsystem-net-devices-eth0.device

[Service]
Type=simple
User=${APP_USER}
Group=${APP_USER}
SupplementaryGroups=realtime
WorkingDirectory=${WORKDIR}

# —— 实时相关 ——
LimitRTPRIO=99
LimitMEMLOCK=infinity
LimitNOFILE=65535
# 脚本端口服务需要监听 502（Modbus-TCP 从站）；非 root 运行须授予绑定低端口能力
AmbientCapabilities=CAP_NET_BIND_SERVICE
Nice=-10
CPUAffinity=${RT_CORES}
IOSchedulingClass=realtime
IOSchedulingPriority=0

# 说明：不在此处设 CPUSchedulingPolicy，避免把脚本/SDO 等普通线程也变成 RT；
#       运动控制线程的 SCHED_FIFO/优先级/亲和由程序内部（pthread_*）设置，默认目标 CPU 3。

# 启动前：把 EtherCAT 网口拉起并把其中断绑到 RT CPU（网口 down 时内核看不到该 IRQ）
ExecStartPre=-/bin/bash /opt/kine-x/deploy/13-pin-irq.sh ${ECAT_IF} ${RT_CPU} up
ExecStart=${APP_BIN} ${APP_CONF}
Restart=on-failure
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=30
StandardOutput=journal
StandardError=journal

# 让内核/驱动告警也进日志，便于现场排查
[Install]
WantedBy=multi-user.target
EOF
    echo "已写入。"
    sep "校验"
    sudo systemd-analyze verify "$UNIT" 2>&1 | head -10 || true
    sep "启用"
    if [ -x "$APP_BIN" ]; then
      sudo systemctl daemon-reload
      sudo systemctl enable --now "${APP_NAME}.service"
      systemctl --no-pager --full status "${APP_NAME}.service" | head -20
    else
      sudo systemctl daemon-reload
      sudo systemctl enable "${APP_NAME}.service"
      echo "注意：$APP_BIN 尚不存在，已 enable 但未 start。"
      echo "      待交叉编译产物部署到该路径后： sudo systemctl start ${APP_NAME}.service"
    fi
    sep "提示：当前会话 ulimit -r=$(ulimit -r)（非 root 用户需 realtime 组 + 重新登录）"
    ;;

  uninstall)
    sudo systemctl disable --now "${APP_NAME}.service" 2>/dev/null
    sudo rm -f "$UNIT"
    sudo systemctl daemon-reload
    echo "已卸载 ${APP_NAME}.service"
    ;;

  status)
    systemctl --no-pager --full status "${APP_NAME}.service" 2>&1 | head -25
    echo "-- 单元文件 --"; cat "$UNIT" 2>/dev/null
    ;;

  *) echo "用法: $0 [install|uninstall|status]"; exit 2 ;;
esac

sep "10-install-service DONE ($MODE)"
