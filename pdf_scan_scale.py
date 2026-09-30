# -*- coding: utf-8 -*-
"""提取《单路485说明书(1).pdf》全文 + 表格, 输出到 pdf_out/scale_plumber.txt"""
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\单路485说明书(1).pdf"
OUT = r"d:\VsCode\Kine-X\pdf_out\scale_plumber.txt"

pdf = pdfplumber.open(PDF)
print("pages:", len(pdf.pages))

with open(OUT, "w", encoding="utf-8") as f:
    for i, pg in enumerate(pdf.pages):
        try:
            t = pg.extract_text() or ""
        except Exception as e:
            t = "<<extract_text error: %s>>" % e
        f.write("===== PDF page %d =====\n" % (i + 1))
        f.write(t + "\n")
        try:
            tables = pg.extract_tables()
        except Exception as e:
            tables = []
            f.write("<<table error: %s>>\n" % e)
        for j, tb in enumerate(tables):
            f.write("----- page %d table %d -----\n" % (i + 1, j + 1))
            for row in tb:
                f.write(" | ".join("" if c is None else str(c).replace("\n", " ") for c in row) + "\n")
        f.write("\n")

print("dumped:", OUT)
