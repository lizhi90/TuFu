# -*- coding: utf-8 -*-
"""导出手册 网络通讯指令 章节: p426(4.2网络), p430-440"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
OUT = r"d:\VsCode\Kine-X\pdf_out\net_plumber.txt"
WANT = list(range(429, 440))

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
