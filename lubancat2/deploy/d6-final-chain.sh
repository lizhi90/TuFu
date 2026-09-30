#!/bin/bash
# d6-final-chain.sh —— 等待 finalize-deploy 结束 → 重启服务 → 实机 D6 验证（板上自治链）
# 用法（PC 侧）：deploy/run_bg.sh deploy/d6-final-chain.sh /tmp/kx_d6_final.log
set -u
echo "== 等待 finalize-deploy 结束 =="
for i in $(seq 1 240); do          # 240 × 10s = 40 分钟上限
  if grep -q "finalize-deploy 结束" /tmp/kx_finalize.log 2>/dev/null; then
    echo "finalize 已结束（第 ${i}0 秒级轮询）"
    break
  fi
  sleep 10
done
if ! grep -q "finalize-deploy 结束" /tmp/kx_finalize.log 2>/dev/null; then
  echo "✘ 超时：finalize 未结束"; exit 1
fi

OLD_PID=$(systemctl show kine-x -p MainPID --value)
echo "重启前 MainPID: $OLD_PID"
sudo systemctl restart kine-x || { echo "✘ restart 失败"; exit 1; }
sleep 4
NEW_PID=$(systemctl show kine-x -p MainPID --value)
echo "重启后 MainPID: $NEW_PID"
[ "$OLD_PID" != "$NEW_PID" ] && echo "✓ 进程已更换（加载新固件）" || echo "✘ 进程未变（重启未生效）"
systemctl is-active kine-x

echo "== 实机 D6 验证 =="
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
caps = info["r"]["caps"]
print("caps:", caps)
assert "d6" in caps, "✘ caps 不含 d6"

comp = rpc(s, 2, "script.compile", {"src": 'PRINT "d6"\nEND', "engine": "basic", "name": "d6_selftest.bas"})
assert comp["r"].get("saved") is True, f"✘ 下载未落盘: {comp['r']}"
print("✓ 下载带 name → saved=true")

fl = rpc(s, 3, "file.list")
assert any(f["name"] == "d6_selftest.bas" for f in fl["r"]["files"]), "✘ file.list 缺文件"
print("✓ file.list 含 d6_selftest.bas")

fg = rpc(s, 4, "file.get", {"name": "d6_selftest.bas"})
assert fg["r"]["src"] == 'PRINT "d6"\nEND'
print("✓ file.get 内容一致")

fd = rpc(s, 5, "file.del", {"name": "d6_selftest.bas"})
assert fd["r"].get("deleted") is True
print("✓ file.del 成功")
s.close()
print("\n=== 实机 D6 端到端验证全部通过 ===")
PY
echo "== d6-final-chain DONE =="
