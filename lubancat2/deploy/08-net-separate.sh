#!/bin/bash
# 08-net-separate.sh [apply|status|revert]
# 网口分离：eth0 = EtherCAT 专用（Linux 不配 IP，交给 IgH/ecMaster）；
#           eth1 = 业务/管理口（出厂默认固定静态 192.168.1.11，SSH 与上位机走这里）。
# 说明：业务口一律使用「静态固定 IP」，不用 DHCP——避免每台控制器地址漂移、每次都要去路由器上查。
# 做法：
#   1) 让 NetworkManager 不再管理 EtherCAT 网口（unmanaged-devices）
#   2) 关闭 EtherCAT 网口的 offload（gso/tso/gro），降低收发抖动（尽力而为）
#   3) 业务网口写入静态固定 IP（出厂默认 192.168.1.11，已存在则保持不动）
set -u
sep() { echo; echo "===== $* ====="; }

ECAT_IF="${ECAT_IF:-eth0}"
HOUSE_IF="${HOUSE_IF:-eth1}"
HOUSE_IP="${HOUSE_IP:-192.168.1.11}"     # 出厂默认固定 IP（固定，非 DHCP）
HOUSE_PREFIX="${HOUSE_PREFIX:-24}"
MODE="${1:-apply}"

NM_CONF=/etc/NetworkManager/conf.d/99-kine-x-ecat.conf

case "$MODE" in
  apply)
    sep "1) NetworkManager 设为不管理 $ECAT_IF"
    sudo mkdir -p "$(dirname "$NM_CONF")"
    sudo tee "$NM_CONF" >/dev/null <<EOF
# Kine-X: EtherCAT 专用网口由 IgH/ecMaster 独占，NetworkManager 不要碰它
[keyfile]
unmanaged-devices=interface-name:${ECAT_IF}
EOF
    echo "  已写入 $NM_CONF"
    if systemctl is-active NetworkManager >/dev/null 2>&1; then
      sudo systemctl reload NetworkManager 2>/dev/null || sudo systemctl restart NetworkManager
      sleep 2
    fi
    echo "  nmcli 中 $ECAT_IF 归属：$(nmcli -t dev status 2>/dev/null | grep -E "^${ECAT_IF}:" || echo '(未列出)')"

    sep "2) 关闭 EtherCAT 网口 offload（若网口已 up）"
    if ip link show "$ECAT_IF" >/dev/null 2>&1; then
      for k in gso tso gro lro rx tx sg; do
        sudo ethtool -K "$ECAT_IF" $k off 2>/dev/null || true
      done
      echo "  当前 offload："
      sudo ethtool -k "$ECAT_IF" 2>/dev/null | grep -E '^(generic-receive|generic-segmentation|tcp-segmentation|large-receive|rx-checksumming|tx-checksumming)' | sed 's/^/    /'
    else
      echo "  $ECAT_IF 不存在，跳过"
    fi

    sep "3) 业务网口 $HOUSE_IF 静态固定 IP（出厂默认 ${HOUSE_IP}，不用 DHCP）"
    if ip -4 addr show "$HOUSE_IF" 2>/dev/null | grep -q "$HOUSE_IP"; then
      echo "  已存在 $HOUSE_IP，保持不动"
    elif command -v nmcli >/dev/null 2>&1; then
      # 按「设备」找连接（含未激活的），保证出厂/掉线时也能落到静态固定 IP
      con=$(nmcli -t -f NAME,DEVICE con show 2>/dev/null | awk -F: -v d="$HOUSE_IF" '$2==d{print $1; exit}')
      [ -z "$con" ] && con=$(nmcli -t -f NAME,DEVICE con show --active 2>/dev/null | awk -F: -v d="$HOUSE_IF" '$2==d{print $1; exit}')
      if [ -n "$con" ]; then
        sudo nmcli con mod "$con" \
          ipv4.addresses "${HOUSE_IP}/${HOUSE_PREFIX}" \
          ipv4.method manual \
          ipv4.gateway "" \
          ipv6.method ignore
        sudo nmcli con up "$con" 2>/dev/null || sudo nmcli dev reapply "$HOUSE_IF" 2>/dev/null || true
        echo "  已把连接 '$con' 设为静态固定 $HOUSE_IP/$HOUSE_PREFIX（重启后保持）"
      else
        echo "  未找到 $HOUSE_IF 的 NetworkManager 连接，未改动"
      fi
    else
      echo "  非 NM 管理（无 nmcli），未改动（避免打断 SSH）"
    fi

    sep "结果"
    ip -br addr show "$ECAT_IF"; ip -br addr show "$HOUSE_IF"
    echo "提示：$ECAT_IF 保持无 IP/DOWN 是正常的——运行时由 EtherCAT 主站接管。"
    ;;

  status)
    sep "网口地址"; ip -br addr
    sep "NM 设备状态"; nmcli -t dev status 2>/dev/null || echo "(无 nmcli)"
    sep "NM unmanaged 配置"; cat "$NM_CONF" 2>/dev/null || echo "(未配置)"
    sep "EtherCAT 网口 offload"; sudo ethtool -k "$ECAT_IF" 2>/dev/null | head -12 || echo "(不可读)"
    sep "中断亲和"; grep -iE "$ECAT_IF|$HOUSE_IF" /proc/interrupts
    ;;

  revert)
    sudo rm -f "$NM_CONF"
    systemctl is-active NetworkManager >/dev/null 2>&1 && sudo systemctl reload NetworkManager
    echo "已移除 NM unmanaged 配置（offload/IP 如需还原请手工处理）"
    ;;

  *) echo "用法: $0 [apply|status|revert]"; exit 2 ;;
esac

sep "08-net-separate DONE ($MODE)"
