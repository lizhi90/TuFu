#!/bin/bash
# push.sh —— 把 deploy/ 下的脚本同步到鲁班猫2 的 $DEPLOY_DIR（默认 /opt/kine-x/deploy）
# 之后即可在板上直接运行（也可被 systemd 服务引用）。
set -u
cd "$(dirname "$0")" || exit 1
BOARD="${BOARD:-cat@192.168.1.11}"
PASS="${PASS:-temppwd}"
DEPLOY_DIR="${DEPLOY_DIR:-/opt/kine-x/deploy}"

ASKPASS="$(mktemp /tmp/kx_askpass.XXXXXX)"
trap 'rm -f "$ASKPASS"' EXIT
printf '#!/bin/bash\necho "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=10)

echo "== 创建远端目录 $DEPLOY_DIR =="
ssh "${SSH_OPTS[@]}" "$BOARD" "sudo mkdir -p '$DEPLOY_DIR' && sudo chown \$USER '$DEPLOY_DIR' && echo ok" || exit 1

echo "== 上传脚本 =="
scp "${SSH_OPTS[@]}" ./*.sh "$BOARD:$DEPLOY_DIR/" || exit 1

echo "== 远端整理 =="
ssh "${SSH_OPTS[@]}" "$BOARD" "chmod +x '$DEPLOY_DIR'/*.sh && ls -l '$DEPLOY_DIR'"
echo "完成：可运行 bash $DEPLOY_DIR/05-tune-runtime.sh status"
