#!/bin/bash
# push-igh.sh —— 把本机已下载的 IgH 源码包上传到鲁班猫2 并解压
# 用法：deploy/push-igh.sh [源码包路径]
#   默认 IGH_TARBALL=/tmp/et.tgz   IGH_DEST=/opt/kine-x/ethercat
# 说明：本机只负责「取源码 + 传输」；编译在板上做（板上才有匹配的内核构建树）。
set -u
BOARD="${BOARD:-cat@192.168.1.11}"
PASS="${PASS:-temppwd}"
TARBALL="${1:-${IGH_TARBALL:-/tmp/et.tgz}}"
DEST="${IGH_DEST:-/opt/kine-x/ethercat}"
TMP="/tmp/igh-src.tar.gz"

[ -f "$TARBALL" ] || { echo "找不到源码包: $TARBALL" >&2; exit 1; }

ASKPASS="$(mktemp /tmp/kx_askpass.XXXXXX)"
trap 'rm -f "$ASKPASS"' EXIT
printf '#!/bin/bash\necho "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=10)

echo "== 上传 $(basename "$TARBALL") ($(du -h "$TARBALL" | cut -f1)) =="
scp "${SSH_OPTS[@]}" "$TARBALL" "$BOARD:$TMP" || exit 1

echo "== 远端解压到 $DEST =="
ssh "${SSH_OPTS[@]}" "$BOARD" "
set -e
sudo rm -rf '$DEST'
sudo mkdir -p '$DEST'
sudo chown -R \$(id -un):\$(id -gn) '$DEST'
tar xzf '$TMP' -C '$DEST' --strip-components=1
rm -f '$TMP'
echo '  \$DEST 顶层:'; ls '$DEST' | head -12
echo '  版本(NEWS 首行):'; head -3 '$DEST/NEWS.md' | tail -1
" || exit 1

echo "完成：下一步  ./deploy/run_bg.sh deploy/16-igh-build.sh /tmp/igh_build.log"
