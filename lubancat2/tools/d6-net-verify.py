#!/usr/bin/env python3
# d6-net-verify.py —— 从 PC 侧走真实网络路径验证 D6（与 VSCodium 插件同路径）
import json, socket, sys, time

HOST, PORT = "192.168.1.11", 5000

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
                resp = json.loads(line)
                if resp.get("ok") is False:
                    raise RuntimeError(json.dumps(resp.get("err", {})))
                return resp
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    raise TimeoutError(m)

ok = fail = 0
def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print(f"  ✓ {name}")
    else:
        fail += 1
        print(f"  ✗ {name} {extra}")

s = socket.create_connection((HOST, PORT), timeout=5)
info = rpc(s, 1, "sys.info")
check("sys.info caps 含 d6", "d6" in info["r"]["caps"], str(info["r"].get("caps")))

comp = rpc(s, 2, "script.compile",
           {"src": 'PRINT "net"\nEND', "engine": "basic", "name": "net_verify.bas"})
check("下载带 name → saved=true", comp["r"].get("saved") is True, json.dumps(comp.get("r", comp)))

fl = rpc(s, 3, "file.list")
files = fl["r"]["files"]
check("file.list 含 net_verify.bas", any(f["name"] == "net_verify.bas" for f in files), json.dumps(files))

fg = rpc(s, 4, "file.get", {"name": "net_verify.bas"})
check("file.get 内容一致", fg["r"]["src"] == 'PRINT "net"\nEND')

# 正向纪律验证：lua 源码在 basic 引擎下编译必被拒（ENGINE_MISMATCH），且不落盘
try:
    rpc(s, 5, "script.compile", {"src": "print('x')", "engine": "lua", "name": "net_lua.lua"})
    check("lua 源码 + basic 引擎 → 拒绝", False, "未报错")
except RuntimeError as e:
    check("lua 源码 + basic 引擎 → ENGINE_MISMATCH", "ENGINE_MISMATCH" in str(e), str(e))
fl2 = rpc(s, 6, "file.list")
check("编译失败不落盘", not any(f["name"] == "net_lua.lua" for f in fl2["r"]["files"]))

for nm in ("net_verify.bas", "net_lua.lua"):
    try:
        rpc(s, 7 if nm.endswith(".bas") else 8, "file.del", {"name": nm})
    except RuntimeError as e:
        check(f"删除不存在文件 {nm} → NOT_FOUND", "NOT_FOUND" in str(e), str(e))
fl3 = rpc(s, 9, "file.list")
check("file.del 清理完成", not any(f["name"].startswith("net_") for f in fl3["r"]["files"]))

s.close()
print(f"\n[net-verify] 通过 {ok} / 失败 {fail}")
sys.exit(0 if fail == 0 else 1)
