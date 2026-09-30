# -*- coding: utf-8 -*-
"""提取 TAS-LAN-86X 使用说明书 + 产品规格书 全文"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
BASE = r"d:\VsCode\Kine-X\TAS-LAN-869&869F_产品资料"
JOBS = [
    (BASE + r"\01_产品说明书\TAS-LAN-86X_使用说明书_V1.0.6.pdf", r"d:\VsCode\Kine-X\pdf_out\tas_manual.txt"),
    (BASE + r"\03_产品规格书\TAS-LAN-869&869F 产品规格书  V1.0.6.pdf", r"d:\VsCode\Kine-X\pdf_out\tas_spec.txt"),
]

for src, out in JOBS:
    pdf = pdfplumber.open(src)
    with open(out, "w", encoding="utf-8") as f:
        for i, pg in enumerate(pdf.pages):
            try:
                t = pg.extract_text() or ""
            except Exception as e:
                t = "<<err %s>>" % e
            f.write("===== PDF page %d =====\n" % (i + 1))
            f.write(t + "\n\n")
    print("pages=%d -> %s" % (len(pdf.pages), out))
