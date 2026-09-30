#!/bin/bash
# 35-reboot-verify.sh —— 【本机运行】重启鲁班猫2 并自动验证重启后：
#   eth0 自动 up / ec_master / SV630 识别 / 再进 OP（复用 21 与 34 两个板端脚本）
# 用法：./deploy/bg.sh /tmp/kx_rb.log ./deploy/35-reboot-verify.sh
set -u
cd "$(dirname "$0")/.." || exit 1
BOARD="${BOARD:-cat@192.168.1.11}"
PASS="${PASS:-temppwd}"
WAIT_MAX="${WAIT_MAX:-300}"

ASKPASS="$(mktemp /tmp/kx_askpass.XXXXXX)"
trap 'rm -f "$ASKPASS"' EXIT
printf '#!/bin/bash\necho "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
ssh_do() { ssh "${SSH_OPTS[@]}" "$BOARD" bash -s; }

sep() { echo; echo "===== $* ====="; }

# ---------- 1) 重启前 ----------
sep "1) 重启前状态"
ssh_do <<'EOS' || true
echo "uptime  : $(uptime -p 2>/dev/null)"
echo "boot_id : $(cat /proc/sys/kernel/random/boot_id)"
echo "eth0    : $(ip -br link show eth0 2>&1)"
echo "carrier : $(cat /sys/class/net/eth0/carrier 2>/dev/null)"
echo "kx-ecat-if : $(systemctl is-active kx-ecat-if.service 2>&1)"
echo "slaves  : $(sudo /usr/local/bin/ethercat slaves 2>&1 | tr '\n' '|')"
EOS

# ---------- 2) 触发重启 ----------
sep "2) 触发重启"
ssh_do <<'EOS' || true
sync
setsid nohup bash -c 'sleep 2; sudo systemctl reboot' >/dev/null 2>&1 &
echo "  已在板上排定重启（2s 后执行）"
EOS

# ---------- 3) 等待回来 ----------
sep "3) 等待板子回来（最长 ${WAIT_MAX}s）"
sleep 10
t=0
BOOTED=0
while [ "$t" -lt "$WAIT_MAX" ]; do
  t=$((t + 10))
  UP=$(ssh_do 2>/dev/null <<'EOS'
awk '{printf "%d", $1}' /proc/uptime
EOS
)
  UP=$(printf '%s' "${UP:-}" | tr -dc '0-9')
  if [ -n "$UP" ] && [ "$UP" -lt 300 ]; then
    echo "  第 ${t}s：已恢复，uptime=${UP}s ✔"
    BOOTED=1
    break
  fi
  echo "  第 ${t}s：尚未就绪（uptime=${UP:-无响应}）"
  sleep 10
done
if [ "$BOOTED" != "1" ]; then
  echo "  ✘ 超时未恢复，请手动检查板子/网线/eth1 配置"
  exit 1
fi

# 再等各服务稳定（ethercat.service 会加载模块、ecat 状态机起飞）
sleep 8

# ---------- 4) 重启后自检 ----------
sep "4) 重启后自检（21-post-reboot-check）"
ssh_do < deploy/21-post-reboot-check.sh 2>&1

# ---------- 5) M1 验收 ----------
sep "5) M1 验收（34-m1-accept）"
ssh_do < deploy/34-m1-accept.sh 2>&1

sep "35-reboot-verify DONE"
