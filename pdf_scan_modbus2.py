# -*- coding: utf-8 -*-
"""枚举手册中所有含 MODBUS 的指令/标识符, 并定位 主站/客户端 相关页"""
import sys
import re
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"

pdf = pdfplumber.open(PDF)
pages = {}
tokens = {}
for i, pg in enumerate(pdf.pages):
    try:
        t = pg.extract_text() or ""
    except Exception:
        continue
    pages[i + 1] = t
    for m in re.findall(r"[A-Za-z_]*[Mm][Oo][Dd][Bb][Uu][Ss][A-Za-z_]*", t):
        tokens.setdefault(m.upper(), set()).add(i + 1)

print("=== MODBUS 相关标识符 ===")
for k in sorted(tokens):
    ps = sorted(tokens[k])
    print("%-24s pages=%d  %s" % (k, len(ps), ps[:8]))

print()
print("=== 含'主站'或 TCP_CLIENT 的页 ===")
for p, t in pages.items():
    if ("主站" in t) or ("TCP_CLIENT" in t.upper()) or ("MBAP" in t.upper()) or ("功能码" in t):
        print("P%-4d %s" % (p, " ".join(t.split())[:150]))
