#!/bin/bash
# 17-config-master.sh [mac|ifname] [start|nostart]
# 作用：在鲁班猫2 上配置 IgH EtherCAT 主站并启动（generic 驱动绑定 eth0）。
#   1) 定位 ethercatctl 实际读取的配置文件（--prefix=/usr/local 时可能是 /usr/local/etc/ethercat.conf）
#   2) 写入 MASTER0_DEVICE=<eth0 永久 MAC>、DEVICE_MODULES="generic"
#   3) 顺带在 /etc/ethercat.conf 放同一份，方便管理与查阅
#   4) systemctl enable --now ethercat，并跑 ethercat master / slaves
# 前提：deploy/16-igh-build.sh 已成功（已装 ec_master.ko / ec_generic.ko / ethercat 工具）
set -u
sep() { echo; echo "===== $* ====="; }

ECAT_IF="${ECAT_IF:-eth0}"
ARG_DEV="${1:-}"          # 可传 MAC（aa:bb:...）或网口名（eth0）；默认取 ECAT_IF 的永久 MAC
DO_START="${2:-start}"
KREL="$(uname -r)"

# ---------- 解析要写入 MASTER0_DEVICE 的值 ----------
if [ -n "$ARG_DEV" ]; then
  DEVVAL="$ARG_DEV"
else
  # addr_assign_type=0 表示永久 MAC（非随机）
  DEVVAL="$(cat /sys/class/net/$ECAT_IF/address 2>/dev/null || true)"
  [ -n "$DEVVAL" ] || { echo "✘ 取不到 $ECAT_IF 的 MAC"; exit 1; }
fi
# 已存在的主站设备值（用于保留旧配置里的其它项）

sep "环境"
echo "  kernel        : $KREL"
echo "  EtherCAT 网口 : $ECAT_IF"
echo "  永久 MAC      : $(cat /sys/class/net/$ECAT_IF/address 2>/dev/null) (assign_type=$(cat /sys/class/net/$ECAT_IF/addr_assign_type 2>/dev/null))"
echo "  MASTER0_DEVICE= $DEVVAL"

# ---------- 定位 ethercatctl / 单元 / 配置文件 ----------
sep "定位 ethercatctl 与配置路径"
CTL=""
for p in /usr/local/sbin/ethercatctl /usr/sbin/ethercatctl /sbin/ethercatctl; do
  [ -x "$p" ] && { CTL="$p"; break; }
done
echo "  ethercatctl   : ${CTL:-（未找到）}"

CONF=""
if [ -n "$CTL" ]; then
  # ethercatctl 里通常写死/展开成配置路径，抓出来
  CONF="$(grep -oE '/[A-Za-z0-9_./-]*ethercat\.conf' "$CTL" 2>/dev/null | head -1)"
fi
[ -n "$CONF" ] || { [ -f /etc/ethercat.conf ] && CONF=/etc/ethercat.conf; }
[ -n "$CONF" ] || { [ -f /usr/local/etc/ethercat.conf ] && CONF=/usr/local/etc/ethercat.conf; }
echo "  配置文件      : ${CONF:-（未找到）}"

sep "内核模块位置"
ls -l "/lib/modules/$KREL/ethercat/" 2>/dev/null || ls -l "/lib/modules/$KREL/updates/" 2>/dev/null || echo "  (未找到 ec_* 模块目录)"
ls -l /usr/local/bin/ethercat /usr/bin/ethercat 2>/dev/null

# ---------- 生成配置内容 ----------
read -r -d '' CONF_BODY <<EOF
# Kine-X 生成（deploy/17-config-master.sh）—— IgH EtherCAT Master generic 驱动
# 单主站：eth0 作为 EtherCAT 专用口，generic 驱动接管（不使用内核原生网卡驱动）
MASTER0_DEVICE="$DEVVAL"
DEVICE_MODULES="generic"
EOF

if [ -n "$CONF" ]; then
  sep "写入 $CONF"
  if [ -f "$CONF" ] && [ ! -f "$CONF.kine-x.bak" ]; then
    sudo cp -a "$CONF" "$CONF.kine-x.bak" && echo "  已备份 -> $CONF.kine-x.bak"
  fi
  echo "$CONF_BODY" | sudo tee "$CONF" >/dev/null && echo "  ✔ 写入完成"
  if [ "$CONF" != "/etc/ethercat.conf" ]; then
    echo "$CONF_BODY" | sudo tee /etc/ethercat.conf >/dev/null && echo "  ✔ 同份写入 /etc/ethercat.conf"
  fi
  echo "  --- $CONF ---"; cat "$CONF" | sed 's/^/    /'
else
  echo "✘ 找不到 ethercat.conf，安装可能未完成"
fi

# ---------- 启动服务 ----------
sep "systemd 服务"
if [ -f /lib/systemd/system/ethercat.service ]; then
  ls -l /lib/systemd/system/ethercat.service
  if [ "$DO_START" = "start" ]; then
    sudo systemctl daemon-reload 2>/dev/null
    # 关键：ec_master 必须带 main_devices=<MAC> 参数加载才能创建 /dev/EtherCAT0。
    # 若模块已被无参预加载（例如编译脚本里的试加载），modprobe 会因「已加载」而忽略参数，
    # 主站就不会被创建。所以先彻底 stop + rmmod，再由 ethercatctl 正规启动。
    sudo systemctl stop ethercat 2>/dev/null || true
    sudo rmmod ec_generic 2>/dev/null || true
    sudo rmmod ec_master 2>/dev/null || true
    if lsmod | grep -qE '^ec_'; then
      echo "  ⚠ 仍有残留模块："; lsmod | grep -E '^ec_'
    fi
    sudo systemctl enable ethercat 2>&1 | sed 's/^/  /'
    sudo systemctl start ethercat 2>&1 | sed 's/^/  /' || true
    sleep 3
    echo "  --- status ---"
    systemctl --no-pager -l status ethercat 2>&1 | head -20 | sed 's/^/    /'
    echo "  --- /dev/EtherCAT* ---"
    ls -l /dev/EtherCAT* 2>&1 | sed 's/^/    /'
  else
    echo "  (按参数要求不启动)"
  fi
else
  echo "  ✘ 无 /lib/systemd/system/ethercat.service"
  echo "  提示：可用 sudo ethercatctl start 手动启动"
fi

# ---------- 结果 ----------
sep "模块与主站"
lsmod | grep -E '^ec_' || echo "  (无 ec_* 模块加载)"
echo "  --- ethercat master ---"
sudo /usr/local/bin/ethercat master 2>&1 | head -30 || sudo ethercat master 2>&1 | head -30
echo "  --- ethercat slaves ---"
sudo /usr/local/bin/ethercat slaves 2>&1 | head -20 || sudo ethercat slaves 2>&1 | head -20

sep "dmesg（ethercat 相关）"
sudo dmesg | grep -iE 'ethercat|ec_master|ec_generic' | tail -20 || echo "  (无)"

sep "17-config-master DONE"
