#!/usr/bin/env node
// gen-regmap.mjs —— 从寄存器文档生成「Modbus 寄存器表」数据（单一事实来源）
//
// 数据源（文档即事实来源，见 AGENTS 文档纪律）：
//   * docs/planA/18-触摸屏寄存器总表.md（HMI/轴/称重/泵 区，zones 由 ## 标题分段）
//   * docs/planA/19-机器人Modbus从站(503)接口.md（机器人 A/B 区与 Kine-X 自定义行）
//
// 产出（同一内容两份）：
//   1) Extension/data/regmap.json —— 插件「Modbus 从站」页离线副本（无 d12 控制器时兜底）
//   （历史：deploy/mbmap.json 随 D11 退役移除，2026-10-01；板端权威=config/modbus.json 经 D12）
//
// 解析规则：跟踪 markdown 标题为 zone；表格行首个单元格以 4x 开头者收集；
//   其余单元格按特征分类：读/写/读写 → access；含类型关键字 → type；`标识符` → name；
//   最长剩余 → desc；任一处含「掉电保持/掉电保存/泵内/模块内」→ persist。
//
// 用法：node tools/gen-regmap.mjs

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const extRoot = resolve(here, '..');
const repoRoot = resolve(extRoot, '..', '..');
const docs = [
  { tag: '18', file: resolve(repoRoot, 'lubancat2/docs/planA/18-触摸屏寄存器总表.md') },
  { tag: '19', file: resolve(repoRoot, 'lubancat2/docs/planA/19-机器人Modbus从站(503)接口.md') },
];
const outExt = resolve(extRoot, 'data', 'regmap.json');

const isSep = (cells) => cells.every((c) => /^:?-{2,}:?$/.test(c.replace(/\s/g, '')));
const addrRe = /^4x\d/;
// ★现场覆写（2026-10-01）：屏「一键启动」按钮直写 B 区 4x1117（ctrl_motoron_pptomain_start）。
//   固件接管 502 前 Lua 从站不拦主站写（行为一直如此）；固件只读拦截会使其报 0x02。
//   该条改为 rw（机器人读不受影响；桥接脚本写不受权限限制），其余 B 区维持只读拦截。
const accessOverride = { '4x1117': 'rw' };
const accessOf = (c) => {
  const t = c.replace(/\*/g, '').trim();
  if (t === '读') return 'r';
  if (t === '写') return 'w';
  if (t === '读写') return 'rw';
  return '';
};
const typeRe = /(int16|uint16|int32|uint32|float32|布尔|位|枚举|字符串|字节)/;
const nameRe = /^`([A-Za-z_][\w.]*)`$/;
const persistRe = /(掉电保持|掉电保存|泵内|模块内|NVRAM|\.nvram)/;

let count = 0;
const entries = [];

for (const { tag, file } of docs) {
  let text;
  try {
    text = readFileSync(file, 'utf8');
  } catch {
    console.error(`[gen-regmap] 读不到文档: ${file}`);
    process.exit(1);
  }
  const lines = text.split(/\r?\n/);
  let zone = '';
  lines.forEach((line, idx) => {
    const h = line.match(/^#{2,3}\s+(.*)$/);
    if (h) {
      zone = h[1].trim();
      return;
    }
    if (!line.trim().startsWith('|')) return;
    const cells = line
      .split('|')
      .slice(1, -1)
      .map((c) => c.trim());
    if (cells.length < 3 || isSep(cells)) return;
    const addr = cells[0].replace(/\*/g, '').trim();
    if (!addrRe.test(addr)) return;

    let access = '';
    let type = '';
    let name = '';
    const rest = [];
    for (const c of cells.slice(1)) {
      const a = accessOf(c);
      const m = c.match(nameRe);
      if (!access && a) {
        access = a;
        continue;
      }
      if (!name && m) {
        name = m[1];
        continue;
      }
      if (!type && typeRe.test(c) && c.length < 48) {
        type = c;
        continue;
      }
      rest.push(c);
    }
    // 机器人区表格无独立「读/写」列 → 由分区标题（A. 机器人写 / B. 机器人读）推断
    if (!access) {
      if (/写/.test(zone)) access = 'w';
      else if (/读/.test(zone)) access = 'r';
      else if (/空闲|free/i.test(zone)) access = '';
    }
    if (accessOverride[addr]) access = accessOverride[addr];
    const desc = rest.sort((a, b) => b.length - a.length)[0] || '';
    const persist = persistRe.test(cells.join(' '));
    const start = Number((addr.match(/^4x(\d+)/) || [])[1] ?? 0);
    entries.push({
      addr,
      start,
      doc: tag,
      zone,
      name,
      type: type || '—',
      access,
      persist,
      desc,
      line: idx + 1,
    });
    count++;
  });
}

const payload = {
  version: 1,
  generatedFrom: docs.map((d) => `planA/${d.tag}`),
  entries: entries.map(({ start, ...e }) => e),
};
const text = JSON.stringify(payload, null, 2) + '\n';
writeFileSync(outExt, text, 'utf8');

// ---- 同时生成固件「Modbus 从站」组态（D12 config/modbus.json；planA/20）----
//   规则：仅收 单址 4xN 与 斜杠 4xN/M（按类型推字数）；~ 区间与「空闲/—」行跳过；
//   类型按关键字映射 u16/u32/f32（f32 低字先）；access 空 → 跳过；按起始地址去重（首见优先）；
//   name：文档标识符（[A-Za-z_]\w*）优先，否则 r<addr>；persist 超出 .nvram 容量（≥1024）则丢弃该标记。
const mbTypeOf = (t) => {
  if (!t) return null;
  if (/float32/.test(t)) return 'f32';
  if (/int16/.test(t)) return 'i16';
  if (/uint16/.test(t)) return 'u16';
  return null;
};
const seen = new Set();
const regs = [];
let skipped = 0, dup = 0;
for (const e of entries) {
  if (e.addr.includes('~')) { skipped++; continue; }
  const m = /^4x(\d+)(?:\/(\d+))?$/.exec(e.addr);
  if (!m) { skipped++; continue; }
  const addr = Number(m[1]);
  if (seen.has(addr)) { dup++; continue; }
  const type = mbTypeOf(e.type);
  const access = e.access;                       // r / w / rw / ''
  if (!type || !access) { skipped++; continue; }
  const span = type === 'u32' || type === 'f32' ? 2 : 1;
  const name = /^[A-Za-z_][A-Za-z0-9_]*$/.test(e.name || '') ? e.name : 'r' + addr;
  const persist = e.persist === true && addr + span - 1 < 1024;
  seen.add(addr);
  regs.push({
    name,
    addr: '4x' + addr,
    type,
    access,
    persist,
    desc: (e.desc || '').slice(0, 120) || undefined,
  });
}
const mbcfg = { version: 1, station: 1, registers: regs };
writeFileSync(resolve(repoRoot, 'lubancat2/deploy/modbus.json'), JSON.stringify(mbcfg, null, 2) + '\n', 'utf8');

const zones = new Set(entries.map((e) => e.zone));
console.log(
  `[gen-regmap] 已生成 ${count} 条寄存器（${zones.size} 个分区）` +
    ` -> ${outExt}`,
);
console.log(
  `[gen-regmap] 从站组态：${regs.length} 条（跳过区间/空行 ${skipped}、重复地址 ${dup}）` +
    ` -> lubancat2/deploy/modbus.json`,
);
