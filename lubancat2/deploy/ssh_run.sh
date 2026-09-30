#!/bin/bash
# ssh_run.sh <本地脚本|-> [传给远端脚本的参数...]
# 作用：免交互地把本地脚本通过 stdin 送到鲁班猫2 执行；带重试（sshd 可能限流）。
#   远端以 `bash -s -- <参数...>` 方式启动，脚本内可用 $1/$2 接收参数。
# 前提：本机无 sshpass，用 SSH_ASKPASS + SSH_ASKPASS_REQUIRE=force 提供密码。
# 用法：deploy/ssh_run.sh deploy/04-cyclictest.sh 60 tuned-idle idle
set -u
BOARD="${BOARD:-cat@192.168.1.11}"
PASS="${PASS:-temppwd}"
RETRIES="${RETRIES:-5}"

SCRIPT="$1"; shift || true
# "-" 表示直接从标准输入读取脚本内容（用于临时命令）
if [ "$SCRIPT" != "-" ] && [ ! -f "$SCRIPT" ]; then
  echo "用法: $0 <本地脚本路径|-> [传给远端脚本的参数...]" >&2
  exit 2
fi

ASKPASS="$(mktemp /tmp/kx_askpass.XXXXXX)"
trap 'rm -f "$ASKPASS"' EXIT
printf '#!/bin/bash\necho "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"

export SSH_ASKPASS="$ASKPASS"
export SSH_ASKPASS_REQUIRE=force
export DISPLAY=:0

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=8 -o ServerAliveInterval=10)

run() {
  if [ "$SCRIPT" = "-" ]; then
    ssh "${SSH_OPTS[@]}" "$BOARD" 'bash -s'
  elif [ "$#" -gt 0 ]; then
    ssh "${SSH_OPTS[@]}" "$BOARD" bash -s -- "$@" < "$SCRIPT"
  else
    ssh "${SSH_OPTS[@]}" "$BOARD" 'bash -s' < "$SCRIPT"
  fi
}

i=1
while :; do
  run "$@" && exit 0
  rc=$?
  if [ "$i" -ge "$RETRIES" ]; then
    echo "[ssh_run] 已达最大重试次数($RETRIES)，最后 rc=$rc" >&2
    exit "$rc"
  fi
  echo "[ssh_run] 第 $i 次失败(rc=$rc)，${i}s 后重试…" >&2
  sleep "$i"
  i=$((i + 1))
done
