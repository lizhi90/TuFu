# -*- coding: utf-8 -*-
"""检索手册中 目标端口/客户端 相关: PORT_TARGET, TCP_CLIENT, 目标端口, MODBUSM_DES2 端口设置"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
KW = ["PORT_TARGET", "TCP_CLIENT", "目标端口", "对方端口", "目的端口", "OPEN #", "MODBUS 主端"]

pdf = pdfplumber.open(PDF)
for i, pg in enumerate(pdf.pages):
    try:
        t = pg.extract_text() or ""
    except Exception:
        continue
    up = t.upper()
    found = [k for k in KW if k.upper() in up]
    if found:
        print("P%-4d %s | %s" % (i + 1, ",".join(found), " ".join(t.split())[:130]))
