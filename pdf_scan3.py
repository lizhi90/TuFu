# -*- coding: utf-8 -*-
import sys
import pdfplumber

sys.stdout.reconfigure(encoding='utf-8')
PDF = r"d:\VsCode\Kine-X\ZBasic编程手册V3.3.0.pdf"
OUT = r"d:\VsCode\Kine-X\pdf_out\axisstatus_plumber.txt"

pdf = pdfplumber.open(PDF)
texts = {}
axis_pages = []
ticks_pages = []
idle_pages = []
rapid_pages = []

for i, pg in enumerate(pdf.pages):
    try:
        t = pg.extract_text() or ""
    except Exception:
        continue
    texts[i + 1] = t
    if "AXISSTATUS" in t and ("跟随误差" in t or "位0" in t or "bit0" in t or "限位" in t):
        axis_pages.append(i + 1)
    if "TICKS" in t and len(ticks_pages) < 6:
        ticks_pages.append(i + 1)
    if "IDLE" in t and ("空闲" in t or "停止" in t) and len(idle_pages) < 6:
        idle_pages.append(i + 1)
    if "RAPIDSTOP" in t and len(rapid_pages) < 6:
        rapid_pages.append(i + 1)

print("AXISSTATUS table pages:", axis_pages[:10])
print("TICKS pages:", ticks_pages)
print("IDLE pages:", idle_pages)
print("RAPIDSTOP pages:", rapid_pages)

sel = sorted(set(axis_pages[:8] + ticks_pages[:2] + idle_pages[:2] + rapid_pages[:2]))
with open(OUT, "w", encoding="utf-8") as f:
    for p in sel:
        f.write("===== PDF page %d =====\n" % p)
        f.write(texts[p] + "\n")
print("dumped:", sel)
