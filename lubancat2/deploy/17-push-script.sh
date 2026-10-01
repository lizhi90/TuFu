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

# ★前置检查（v0.12.2）：本脚本 MB_WIRE=false，依赖固件内建 502（含 D12 且已放行 config/modbus.enable）。
#   若板端固件缺失而脚本先上，502 将无人服务（触摸屏/机器人失联）——默认中止，FORCE=1 可跳过。
if [ "${FORCE:-0}" != "1" ]; then
  PRE="$(printf 'test -f /userdata/kine-x/config/modbus.enable && grep -aq "mbreg.get" /opt/kine-x/bin/kine-x && echo KX_PREFLIGHT_OK\n' | bash "$HERE/ssh_run.sh" - 2>/dev/null | tr -d "\r")"
  if ! printf '%s' "$PRE" | grep -q KX_PREFLIGHT_OK; then
    echo "✘ 前置检查未通过：板端缺少 D12 固件或 config/modbus.enable"
    echo "  本脚本不再监听 502（MB_WIRE=false），先部署含 D12 的 kine-x 并创建 enable 文件，或 FORCE=1 强制推送"
    exit 1
  fi
  echo "✓ 前置检查通过（固件 D12 + modbus.enable）"
fi

# 先清理板端退役残留（kx_regmap.lua/.mbmap），否则会进入下面 md5 glob 造成"必然不一致"
printf 'rm -f %s/kx_regmap.lua %s/.mbmap\n' "$DEST" "$DEST" | bash "$HERE/ssh_run.sh" - >/dev/null 2>&1 || true

{
  printf 'set -e\n'
  printf "cat > %s/demo.lua <<'KXEOF'\n" "$DEST"; cat "$ROOT/EtherCAT_SocketServer.lua"; printf 'KXEOF\n'
  for f in "$ROOT"/kx_*.lua; do
      [ -f "$f" ] || continue
      [ "$(basename "$f")" = "kx_regmap.lua" ] && continue    # ★P3b 已退役（不部署；D12 取代用户寄存器层）
      printf "cat > %s/%s <<'KXEOF'\n" "$DEST" "$(basename "$f")"; cat "$f"; printf 'KXEOF\n'
  done
  printf 'md5sum %s/demo.lua %s/kx_*.lua' "$DEST" "$DEST"
  printf '\n'
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
