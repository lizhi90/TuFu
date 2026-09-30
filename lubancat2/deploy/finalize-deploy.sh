#!/bin/bash
# finalize-deploy.sh —— 部署收尾流水线（在鲁班猫2 上后台执行）
#   前提：18-push-src.sh 已推源码、36-build-m2.sh 已在板上启动（或已完成）编译。
#   流程：等 kine-x 产物出现（最多 20 分钟）→ 拷贝到 /opt/kine-x/bin/ →
#         拷贝 app.conf 到 /opt/kine-x/config/ → 10-install-service.sh install（自启）
#         → 输出验证结果（服务状态 + 5000 监听）。
#   用法（PC 侧触发）：deploy/ssh_run.sh deploy/finalize-deploy.sh
#   结果日志：板上 /tmp/kx_finalize.log
set -u
sep() { echo; echo "===== $* ====="; }
APP=/opt/kine-x/app
BIN=/opt/kine-x/bin/kine-x
CONF=/opt/kine-x/config/app.conf
TIMEOUT_ITER=120   # 120 × 10s = 20 分钟

echo "finalize-deploy 启动 $(date '+%F %T')"

# 0) 删除旧产物：防止上一次部署遗留的 kine-x 让等待条件立即满足（拷到旧的固件）
#    （实机踩坑：旧产物存在 → finalize 秒过 → 部署的还是旧固件）
rm -f "$APP/kine-x"

# 1) 等 kine-x 产物（编译由 36-build-m2.sh 负责）
n=0
while [ $n -lt $TIMEOUT_ITER ]; do
  if [ -x "$APP/kine-x" ]; then
    # 产物出现后确认 g++ 已退出（链接收尾），mtime 3 秒不变视为稳定
    sleep 3
    if [ -x "$APP/kine-x" ]; then
      echo "产物就绪: $APP/kine-x"
      break
    fi
  fi
  n=$((n + 1))
  sleep 10
done
if [ ! -x "$APP/kine-x" ]; then
  echo "✘ 超时：$APP/kine-x 未生成（编译失败？看 /tmp/kx_remote_build.log）"
  exit 1
fi
tail -20 /tmp/kx_remote_build.log 2>/dev/null | grep -E "编译 kine-x|ALL PASS|✘" | tail -6 || true

# 2) 放置二进制与配置（10-install-service 要求 APP_BIN/APP_CONF 路径就位）
sep "放置二进制与配置"
sudo mkdir -p /opt/kine-x/bin /opt/kine-x/config
sudo cp -f "$APP/kine-x" "$BIN" && echo "已拷贝 $BIN"
if [ -f "$APP/config/app.conf" ]; then
  sudo cp -f "$APP/config/app.conf" "$CONF" && echo "已拷贝 $CONF"
else
  echo "✘ 缺 $APP/config/app.conf"
  exit 1
fi
sudo chmod 755 "$BIN"

# 3) 安装 systemd 服务（enable --now，开机自启）
sep "安装并启动服务"
bash /opt/kine-x/deploy/10-install-service.sh install

# 4) 验证
sep "验证"
sleep 2
echo "服务状态: $(systemctl is-active kine-x)"
if ss -tln 2>/dev/null | grep -q ':5000'; then
  echo "✓ 调试通道 5000 已监听"
  ss -tlnp 2>/dev/null | grep ':5000'
else
  echo "✘ 5000 未监听（journalctl -u kine-x -n 50 排查）"
fi
echo "finalize-deploy 结束 $(date '+%F %T')"
