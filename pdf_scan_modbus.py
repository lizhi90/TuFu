# -*- coding: utf-8 -*-
"""在《ZBasic编程手册V3.3.0.pdf》中检索 MODBUS 相关页, 输出页码+首几行, 便于定位主站/从站用法"""
import sys
import re
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
KW = ["MODBUS_OPEN", "MODBUS_READ", "MODBUS_WRITE", "MODBUS_SEND", "MODBUS_RECV",
      "MODBUS_SETUP", "MODBUS_MASTER", "RTU", "MODBUS_TCP", "MODBUS_CLIENT",
      "MODBUS_BYTES", "MODBUS_REG", "MODBUS_IEEE", "PROTOCOL"]

pdf = pdfplumber.open(PDF)
hits = {}
for i, pg in enumerate(pdf.pages):
    try:
        t = pg.extract_text() or ""
    except Exception:
        continue
    up = t.upper()
    found = [k for k in KW if k in up]
    if "MODBUS" in up:
        hits[i + 1] = (found, t)

print("total pages:", len(pdf.pages), " modbus pages:", len(hits))
for p, (found, t) in hits.items():
    head = " ".join(t.split())[:120]
    print("P%-4d %s | %s" % (p, ",".join(found), head))
