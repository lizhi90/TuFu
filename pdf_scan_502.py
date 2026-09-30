# -*- coding: utf-8 -*-
"""检索手册中 MODBUS_TCP 主站相关: 502端口/MODBUSM_DES2 例程"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
pdf = pdfplumber.open(PDF)
for i, pg in enumerate(pdf.pages):
    try:
        t = pg.extract_text() or ""
    except Exception:
        continue
    up = t.upper()
    if ("MODBUSM_DES2" in up) or ("MODBUS_TCP主端" in t) or ("MODBUS TCP主端" in t) or ("502" in t and "MODBUS" in up):
        print("P%-4d | %s" % (i + 1, " ".join(t.split())[:200]))
