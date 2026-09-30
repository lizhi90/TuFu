#!/bin/bash
# 33-persist-if-config.sh —— 固化两件重启后必需的设置
#   1) eth0(EtherCAT 口) 开机自动 up
#      背景：08-net-separate.sh 把 eth0 改成无 IP / NM unmanaged，重启后网口保持 down，
#            generic 驱动虽有绑定但无载波 -> ethercat master 显示 Link: DOWN / Slaves: 0。
#   2) 把 app.conf 安装到主程序服务读取的 /opt/kine-x/config/app.conf
#      背景：kine-x.service 的 ExecStart 传的就是这个路径；缺 ECAT_VENDOR/ECAT_PRODUCT 会
#            导致从站 attach 失败 -> SM 长度退回 SII 默认 -> AL 0x001E -> 进不了 OP。
#
# 用法：./deploy/run_bg.sh deploy/33-persist-if-config.sh /tmp/kx_33.log [ifname]
set -u
sep() { echo; echo "===== $* ====="; }

APP_SRC="${APP_SRC:-/opt/kine-x/app}"      # 源码/配置同步目录（18-push-src.sh 的目标）
SVC_CONF_DIR="${SVC_CONF_DIR:-/opt/kine-x/config}"
APP_IF="${1:-eth0}"
RT_CPU="${RT_CPU:-3}"
UNIT=/etc/systemd/system/kx-ecat-if.service
EC=/usr/local/bin/ethercat

sep "0) 现状"
echo "-- /opt/kine-x --"; ls -la /opt/kine-x 2>&1
echo "-- 服务配置目录 --"; ls -la "$SVC_CONF_DIR" 2>&1
echo "-- eth0 --"; ip -br link show "$APP_IF" 2>&1
echo "-- ethercat 单元/脚本 --"
systemctl list-unit-files 2>/dev/null | grep -i ethercat || echo "  (无 ethercat.service)"
ls -l /etc/init.d/ethercat 2>/dev/null || echo "  (无 /etc/init.d/ethercat)"
echo "-- ec_master 模块状态 --"; lsmod | grep -E 'ec_master|ec_generic' || echo "  (未加载)"

sep "1) 安装开机拉起 $APP_IF 的 systemd 单元"
sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=Bring up EtherCAT NIC ($APP_IF) before EtherCAT master
DefaultDependencies=no
After=sys-subsystem-net-devices-${APP_IF}.device
Wants=sys-subsystem-net-devices-${APP_IF}.device
# 若 IgH 提供了 ethercat.service，则保证在其之前（单元不存在时该排序项自动忽略）
Before=ethercat.service

[Service]
Type=oneshot
RemainAfterExit=yes
# 拉起网口（无 IP，专用于 EtherCAT）；等待 PHY 载波
ExecStart=/bin/bash -c '/sbin/ip link set ${APP_IF} up; sleep 2; cat /sys/class/net/${APP_IF}/carrier'
# 网口 up 后内核才注册 IRQ，此时再把中断绑到 RT CPU（无脚本/无 IRQ 都不算失败）
ExecStartPost=-/bin/bash /opt/kine-x/deploy/13-pin-irq.sh ${APP_IF} ${RT_CPU} noup

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now kx-ecat-if.service
systemctl --no-pager --full status kx-ecat-if.service 2>&1 | head -14
ip -br link show "$APP_IF"

sep "2) 安装 app.conf 到 $SVC_CONF_DIR"
if [ -f "$APP_SRC/config/app.conf" ]; then
  sudo mkdir -p "$SVC_CONF_DIR"
  sudo cp -f "$APP_SRC/config/app.conf" "$SVC_CONF_DIR/app.conf"
  echo "  已安装 $SVC_CONF_DIR/app.conf"
else
  echo "  ✘ 未找到 $APP_SRC/config/app.conf（先在主机跑 ./deploy/bg.sh /tmp/x.log ./deploy/18-push-src.sh）"
fi
echo "-- 关键项 --"
grep -nE 'ECAT_(VENDOR|PRODUCT|EXPLICIT_PDO|EXPLICIT_PROFILE|DC_ASSIGN|CYCLE_NS|SLAVE_POS)' \
     "$SVC_CONF_DIR/app.conf" 2>&1

sep "3) 复测：拉起主站链路"
sudo "$EC" master 2>&1 | head -10
sudo "$EC" slaves 2>&1

sep "33-persist-if-config DONE"
echo "重启后自检：systemctl status kx-ecat-if.service; ip -br link show $APP_IF; sudo $EC slaves"
