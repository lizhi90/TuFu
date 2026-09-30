#!/bin/bash
# 17-push-script.sh —— 推送控制器脚本（主文件 + kx_*.lua 模块）到板端脚本目录，逐文件 md5 校验
# 用法: lubancat2/deploy/17-push-script.sh            # 只推送+校验（不重启）
#       RESTART=1 lubancat2/deploy/17-push-script.sh  # 推送+校验后重启 kine-x.service（会中断现场任务，执行前须确认）
# 说明: 主文件 EtherCAT_SocketServer.lua → 板端 demo.lua（.boot 主文件）；kx_*.lua 原样推送；
#       模块由主文件 include 编译前展开（见 docs/planA/10 §7.5）。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEST=/userdata/kine-x/scripts
TMP=/tmp/kx_push_script.md5

{
  printf 'set -e\n'
  printf "cat > %s/demo.lua <<'KXEOF'\n" "$DEST"; cat "$ROOT/EtherCAT_SocketServer.lua"; printf 'KXEOF\n'
  for f in "$ROOT"/kx_*.lua; do
      [ -f "$f" ] || continue
      printf "cat > %s/%s <<'KXEOF'\n" "$DEST" "$(basename "$f")"; cat "$f"; printf 'KXEOF\n'
  done
  printf 'md5sum %s/demo.lua %s/kx_*.lua\n' "$DEST" "$DEST"
} | bash "$HERE/ssh_run.sh" - > "$TMP" || exit 1

ok=1
while read -r hash name; do
  base="$(basename "$name")"
  if [ "$base" = "demo.lua" ]; then
    local_hash="$(md5sum "$ROOT/EtherCAT_SocketServer.lua" | awk '{print $1}')"
  else
    local_hash="$(md5sum "$ROOT/$base" | awk '{print $1}')"
  fi
  if [ "$hash" != "$local_hash" ]; then
    echo "✗ md5 不一致: $base  板端=$hash 本地=$local_hash"; ok=0
  else
    echo "✓ $base  $hash"
  fi
done < "$TMP"
[ "$ok" = 1 ] || { echo "推送校验失败"; exit 1; }
echo "推送完成（板端 $DEST；主文件=demo.lua）"
if [ "${RESTART:-0}" = "1" ]; then
  echo "重启 kine-x.service ..."
  printf 'sudo -n systemctl restart kine-x.service && sleep 3 && systemctl is-active kine-x.service\n' | bash "$HERE/ssh_run.sh" -
fi
