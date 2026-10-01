#!/bin/bash
# 18-push-src.sh —— 把本机项目源码（src/ tools/ CMakeLists.txt）同步到鲁班猫2
# 用途：板上没有 cmake，用 deploy/19-build-probe.sh 直接 g++ 编译自检工具。
# 目标目录：/opt/kine-x/app
set -u
cd "$(dirname "$0")/.." || exit 1          # 切到项目根
BOARD="${BOARD:-cat@192.168.1.11}"
PASS="${PASS:-temppwd}"
DEST="${APP_DEST:-/opt/kine-x/app}"
TAR="/tmp/kx_app.tgz"

ASKPASS="$(mktemp /tmp/kx_askpass.XXXXXX)"
trap 'rm -f "$ASKPASS"' EXIT
printf '#!/bin/bash\necho "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=10)

echo "== 打包源码 =="
rm -f "$TAR"
# 注意：config/ 一并打包——否则板端 app.conf 缺 ECAT_VENDOR/ECAT_PRODUCT/ECAT_EXPLICIT_PDO，
#       会退回旧默认值导致从站 attach 失败（AL 0x001E，进不了 OP）。
# 注意：third_party/ 一并打包——Lua 脚本引擎依赖 vendored Lua 5.4 源码（见 docs/planA/09）。
#   deploy/modbus.json 一并打包（finalize 作为组态种子安装；见 planA/20 §8）
tar czf "$TAR" src tools config third_party CMakeLists.txt deploy/modbus.json 2>/dev/null || {
  tar czf "$TAR" src tools config third_party CMakeLists.txt 2>/dev/null || { echo "打包失败"; exit 1; }
}
echo "  $TAR ($(du -h "$TAR" | cut -f1))"

echo "== 上传 =="
scp "${SSH_OPTS[@]}" "$TAR" "$BOARD:/tmp/kx_app.tgz" || exit 1

echo "== 远端解压到 $DEST =="
ssh "${SSH_OPTS[@]}" "$BOARD" "
set -e
sudo mkdir -p '$DEST'
sudo chown -R \$(id -un):\$(id -gn) '$DEST'
tar xzf /tmp/kx_app.tgz -C '$DEST'
rm -f /tmp/kx_app.tgz
echo '  顶层:'; ls '$DEST'
echo '  config:'; ls '$DEST/config'
echo '  motion:'; ls '$DEST/src/motion'
echo '  lua   :'; ls '$DEST/third_party/lua' | head -5
" || exit 1

echo "完成：下一步  ./deploy/run_bg.sh deploy/19-build-probe.sh /tmp/probe_build.log"
