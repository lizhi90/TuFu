# -*- coding: utf-8 -*-
"""导出手册中 Modbus 主站(MODBUSM_*)章节: p426-427, p446-457"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
OUT = r"d:\VsCode\Kine-X\pdf_out\modbusm_plumber.txt"
WANT = list(range(425, 429)) + list(range(440, 458))

pdf = pdfplumber.open(PDF)
with open(OUT, "w", encoding="utf-8") as f:
    for p in WANT:
        try:
            t = pdf.pages[p - 1].extract_text() or ""
        except Exception as e:
            t = "<<err %s>>" % e
        f.write("===== PDF page %d =====\n" % p)
        f.write(t + "\n\n")
print("dumped:", OUT)
