#!/bin/bash
# d6-restart-verify.sh —— 重启 kine-x 服务（加载 D6 新固件）并做实机验证
# 在板上执行。验证项：新进程 pid / 5000 监听 / caps 含 d6 / file.list 应答 / 下载落盘。
set -u
echo "== 重启 kine-x（加载 D6 新固件）=="
sudo systemctl restart kine-x || { echo "✘ restart 失败"; exit 1; }
sleep 4
echo "状态: $(systemctl is-active kine-x)  MainPID: $(systemctl show kine-x -p MainPID --value)"
ss -ltn | grep -q ':5000 ' && echo "✓ 5000 已监听" || { echo "✘ 5000 未监听"; exit 1; }

echo "== 实机 D6 验证（本机回环直连 5000）=="
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
print("✓ caps 含 d6")

# 下载带 name：编译成功即落盘
comp = rpc(s, 2, "script.compile", {"src": 'PRINT "d6"\nEND', "engine": "basic", "name": "d6_selftest.bas"})
print("compile saved:", comp["r"].get("saved"))
assert comp["r"].get("saved") is True, "✘ 下载未落盘"
print("✓ 下载带 name → saved=true")

fl = rpc(s, 3, "file.list")
names = [f["name"] for f in fl["r"]["files"]]
print("file.list:", names)
assert "d6_selftest.bas" in names, "✘ file.list 缺文件"
print("✓ file.list 含 d6_selftest.bas")

fg = rpc(s, 4, "file.get", {"name": "d6_selftest.bas"})
assert fg["r"]["src"] == 'PRINT "d6"\nEND'
print("✓ file.get 内容一致")

fd = rpc(s, 5, "file.del", {"name": "d6_selftest.bas"})
assert fd["r"].get("deleted") is True
print("✓ file.del 成功（清理自测文件）")
s.close()
print("\n实机 D6 端到端验证全部通过")
PY
rc=$?
exit $rc
