#!/bin/bash
# d6-final-chain2.sh —— 等第二次 finalize（mkdir_p 版固件）拷贝完成后重启并验证（板上自治）
# 判据：finalize-deploy 进程退出 且 /opt/kine-x/bin/kine-x mtime 比本脚本启动时新。
set -u
BIN=/opt/kine-x/bin/kine-x
START_MTIME=$(stat -c %Y "$BIN" 2>/dev/null || echo 0)
echo "启动基线 bin mtime: $START_MTIME"

for i in $(seq 1 240); do
  RUNNING=$(pgrep -f finalize-deploy.sh | head -1)
  CUR_MTIME=$(stat -c %Y "$BIN" 2>/dev/null || echo 0)
  if [ -z "$RUNNING" ] && [ "$CUR_MTIME" -gt "$START_MTIME" ]; then
    echo "finalize 已退出且固件已更新（第 ${i} 次轮询）"
    break
  fi
  sleep 10
done
CUR_MTIME=$(stat -c %Y "$BIN" 2>/dev/null || echo 0)
if [ "$CUR_MTIME" -le "$START_MTIME" ]; then
  echo "✘ 超时：固件未更新（finalize 未正常完成？）"; exit 1
fi

OLD_PID=$(systemctl show kine-x -p MainPID --value)
sudo systemctl restart kine-x || { echo "✘ restart 失败"; exit 1; }
sleep 4
NEW_PID=$(systemctl show kine-x -p MainPID --value)
echo "MainPID: $OLD_PID -> $NEW_PID"
[ "$OLD_PID" != "$NEW_PID" ] && echo "✓ 已加载 mkdir_p 修复版固件" || { echo "✘ 进程未变"; exit 1; }

python3 - <<'PY'
import json, socket, time

def rpc(sock, rid, m, p=None):
    req = {"id": rid, "m": m}
    if p is not None:
        req["p"] = p
    sock.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + 5
    while time.time() < deadline:
        if b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.strip():
                return json.loads(line)
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    raise TimeoutError(m)

s = socket.create_connection(("127.0.0.1", 5000), timeout=5)
info = rpc(s, 1, "sys.info")
assert "d6" in info["r"]["caps"], f"✘ caps: {info['r']['caps']}"
print("✓ caps 含 d6:", info["r"]["caps"])
comp = rpc(s, 2, "script.compile", {"src": 'PRINT "v"\nEND', "engine": "basic", "name": "d6_selftest2.bas"})
assert comp["r"].get("saved") is True
print("✓ 下载落盘 saved=true")
fl = rpc(s, 3, "file.list")
assert any(f["name"] == "d6_selftest2.bas" for f in fl["r"]["files"])
print("✓ file.list 正常")
rpc(s, 5, "file.del", {"name": "d6_selftest2.bas"})
print("✓ 清理完成")
s.close()
print("=== mkdir_p 版固件实机验证通过 ===")
PY
echo "== d6-final-chain2 DONE =="
