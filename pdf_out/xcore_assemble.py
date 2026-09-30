# -*- coding: utf-8 -*-
"""合并《xCore 控制系统使用手册 V2.2 学习笔记》（可重复运行）。
front（文档头） + 章节目录 + 各章片段（按 1..17 顺序，ch09_11_14 拆分以插入 ch10）。
对数字标题做规范化：hashes = 1 + 段数（如 15.4.7.5 -> #####）。"""
import os, re

ROOT = '/home/ubuntu20/Kine-X'
OUT  = os.path.join(ROOT, 'xCore控制系统使用手册V2.2_学习笔记.md')

def read(p): return open(os.path.join(ROOT, p), encoding='utf-8').read()

HEAD = re.compile(r'^(#{1,6})\s+(\d+(?:\.\d+)*)(.*)$')
def canon_heading(line: str) -> str:
    m = HEAD.match(line)
    if not m: return line
    segs = m.group(2).split('.')
    depth = min(1 + len(segs), 6)
    return '#' * depth + ' ' + m.group(2) + m.group(3)

def canon_body(text: str) -> str:
    return '\n'.join(canon_heading(l) for l in text.splitlines())

def strip_top_h1(text: str) -> str:
    lines = text.splitlines(); i = 0
    while i < len(lines):
        s = lines[i].strip()
        if s == '' or s.startswith('# ') or s.startswith('> '):
            i += 1; continue
        break
    return '\n'.join(lines[i:]).rstrip() + '\n'

def fix(text: str, pairs) -> str:
    for a, b in pairs:
        if a not in text:
            print('  [warn] pattern not found:', a[:50])
        text = text.replace(a, b)
    return text

front = read('pdf_out/xcore_md_front.md').rstrip() + '\n'

toc = '''## 章节目录

- 1 手册概述 / 2 安全 / 3 名词术语
- 4 机器人基础知识 / 5 机器人系统构成及连接
- 6 HMI 简介 / 7 控制系统基础操作
- 8 编程（RL 工程 / 编辑器 / 任务 / 变量 / 点位 / 路径 / IO / 坐标系 / 工具 / 工件 / 视觉）
- 9 设置
- 10 通信 ★（系统IO / 外部通信与交互指令 / 总线设备 / 寄存器与寄存器远程控制 / IO设备 / 末端工具 / RCI / xPanel / 电爪吸盘 / 串口 / 编码器 / OPC-UA）
- 11 安全 / 12 工艺包 / 13 日志 / 14 选项
- 15 RL 指令（变量类型 / 函数 / 运动 / Trigger / 力控 / 拖动回放 / IO / 通信 / 网络 / 逻辑 / 起始点 / 数学 / 位 / 字符串 / 运算符 / 时钟 / 高级 / 功能 / 寄存器 / 末端工具）
- 16 附录 / 17 故障排查（错误码 1XXXX~6XXXX）
'''

ch01_07 = strip_top_h1(read('pdf_out/xcore_md_frag_ch01_07.md'))
ch08    = strip_top_h1(read('pdf_out/xcore_md_frag_ch08.md'))
ch09_11 = read('pdf_out/xcore_md_frag_ch09_11_14.md')
i11 = ch09_11.index('\n## 11 ')
ch09 = strip_top_h1(ch09_11[:i11])
ch11_14 = strip_top_h1(ch09_11[i11:])
ch10    = strip_top_h1(read('pdf_out/xcore_md_frag_ch10.md'))
ch15a   = strip_top_h1(read('pdf_out/xcore_md_frag_ch15a.md'))
ch15b   = strip_top_h1(read('pdf_out/xcore_md_frag_ch15b.md'))
ch16_17 = strip_top_h1(read('pdf_out/xcore_md_frag_ch16_17.md'))
ch16_17 = fix(ch16_17, [('### 提取不清与原文疑点汇总', '### 附：16~17 章提取不清与原文疑点汇总')])

ch15a = fix(ch15a, [('### 附：提取不清处汇总', '### 附：15.1~15.4.7 提取不清处汇总')])
ch15b = fix(ch15b, [
    ('### 15.4.7 通信指令\n\n> 本文件从 15.4.7.5 的示例开始；15.4.7 的级标题与 15.4.7.1~15.4.7.5 的正文不在本文件内。\n',
     '> **（承接前文 15.4.7）** 以下自 15.4.7.5 的例 3 起。\n'),
    ('SendByte（仅本文件所含例 3、例 4，p.281）', 'SendByte（续：例 3、例 4，p.281）'),
    ('> 语法、参数与例 1、例 2 不在本文件；以下为原文示例片段', '> 语法、参数与例 1、例 2 见前文；以下为原文示例片段'),
    ('## 整理说明（疑点/提取不清清单）', '### 附：15.4.7.5~15.4.19 整理说明（疑点/提取不清清单）'),
])

footer = '''
---

## 整理说明（本次整理的产生方式与校核建议）

- 全文由 PDF 文本层提取（`pdf_out/xcore_full.txt`，381 页，含 `===== PDF page N =====` 页码标记）；目录大纲另存 `pdf_out/xcore_outline.txt`（694 条）。
- 分章切片：`pdf_out/xcore_s01_07.txt`、`xcore_s08.txt`、`xcore_s09.txt`、`xcore_s10_comm.md`、`xcore_s11_14.txt`、`xcore_s15a.txt`、`xcore_s15b.txt`、`xcore_s16_17.txt`；
  分章整理片段：`pdf_out/xcore_md_frag_*.md`（本文件由 `pdf_out/xcore_assemble.py` 合并生成，可重复运行）。
- 页面/截图图片：`pdf_out/xcore_img/`（p.156/157 整页渲染 `xcore_p156.png`、`xcore_p157.png`；外部通信配置界面截图原图 `xcore_p156_printf_config_raw.png`，896×783）。
- 整理原则：忠实原文（数字、命令、默认值、错误码不臆造）；提取不清处标注 `（提取不清：…）`，原文疑似笔误但照录处标注 `（原文如此）`；
  原文以截图/界面图呈现的操作步骤无法从文本层还原，相关处以要点或“（原文为插图）”说明。
- 建议现场使用前抽查：第 5/9 章网络默认值与 IP、第 10.3 交互指令与 10.5.6 远程控制命令码、第 15 章常用指令语法、第 17 章与本站相关的错误码（1XXXX socket/通信、6XXXX RL 启动、5XXXX 运动）。
'''

parts = [front, toc, ch01_07, ch08, ch09, ch10, ch11_14, ch15a, ch15b, ch16_17, footer]
body = ''
for p in parts:
    if not p.strip(): continue
    body += '\n---\n\n' + canon_body(p).rstrip() + '\n'
out = body.lstrip('\n')
if out.startswith('---'):
    out = out.split('\n', 1)[1].lstrip('\n') if '\n' in out else out
open(OUT, 'w', encoding='utf-8').write(out)
print('assembled:', OUT, os.path.getsize(OUT), 'bytes')
