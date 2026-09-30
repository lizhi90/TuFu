#!/usr/bin/env node
// gen-commands.mjs —— 从控制器源码生成命令数据（单一事实来源）
//
// 数据源：lubancat2/src/script/command_table.h
//   * kNames[]   —— 命令名 + 分节注释（分节注释即补全分组标签）
//   * kDocs[]    —— 每条命令的签名与一句话说明（补全/Hover/签名帮助共用）
//
// 产出：
//   1) Extension/data/commands.json —— names / groupList / docs（供补全、Hover、签名帮助）
//   2) Extension/syntaxes/*.tmLanguage.json —— 就地更新命令名高亮（support.function.kx-*）
//
// 纪律（见 docs/planA/15 §8 T-04、NFR-7）：命令名不得硬编码在插件里；
// 改 command_table.h 后必须重跑本脚本（smoke 有一致性校验兜底）。
//
// 用法：node tools/gen-commands.mjs

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const extRoot = resolve(here, '..');
const headerPath = resolve(extRoot, '..', 'src', 'script', 'command_table.h');
const outPath = resolve(extRoot, 'data', 'commands.json');

let text;
try {
  text = readFileSync(headerPath, 'utf8');
} catch (e) {
  console.error(`[gen-commands] 读不到 command_table.h: ${headerPath}`);
  console.error(String(e.message || e));
  process.exit(1);
}

// ---------------------------------------------------------------------------
// 1) 解析 kNames[]（带分节注释 → 分组）
// ---------------------------------------------------------------------------
const namesBlock = text.match(/kNames\[\]\s*=\s*\{([\s\S]*?)\};/);
if (!namesBlock) {
  console.error('[gen-commands] 未在 command_table.h 中找到 kNames[] 数组');
  process.exit(1);
}

const names = [];
const seen = new Set();
const groupList = [];
for (const rawLine of namesBlock[1].split('\n')) {
  const line = rawLine.trim();
  const comment = line.match(/^\/\/\s*(.+)$/);
  if (comment && !line.includes('"')) {
    groupList.push({ key: comment[1], label: comment[1], names: [] });
    continue;
  }
  for (const m of rawLine.matchAll(/"([^"]+)"/g)) {
    const name = m[1];
    if (seen.has(name)) {
      continue;
    }
    seen.add(name);
    names.push(name);
    if (groupList.length > 0) {
      groupList[groupList.length - 1].names.push(name);
    }
  }
}

// ---------------------------------------------------------------------------
// 2) 解析 kDocs[]（签名 + 说明）
// ---------------------------------------------------------------------------
const docsBlock = text.match(/kDocs\[\]\s*=\s*\{([\s\S]*?)\};/);
if (!docsBlock) {
  console.error('[gen-commands] 未在 command_table.h 中找到 kDocs[] 数组');
  process.exit(1);
}
const docs = {};
let docCount = 0;
for (const line of docsBlock[1].split('\n')) {
  const m = line.match(/^\s*\{\s*"([A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}/);
  if (!m) {
    continue;
  }
  docs[m[1]] = { sig: m[2], brief: m[3] };
  docCount += 1;
}
for (const n of names) {
  if (!docs[n]) {
    console.warn(`[gen-commands] 警告：命令 ${n} 缺少 kDocs 条目（补全/Hover 将没有签名与说明）`);
  }
}

const payload = {
  $comment: '自动生成，请勿手改。来源：src/script/command_table.h（node tools/gen-commands.mjs）',
  source: 'src/script/command_table.h',
  count: names.length,
  names,
  groupList,
  docs,
};

mkdirSync(dirname(outPath), { recursive: true });
writeFileSync(outPath, JSON.stringify(payload, null, 2) + '\n', 'utf8');

// ---------------------------------------------------------------------------
// 3) 就地更新语法高亮的命令名表（syntaxes/*.tmLanguage.json）
//    只替换 support.function.kx-* 规则的 match，保持文件其余部分与缩进不变。
// ---------------------------------------------------------------------------
const alternation = names.slice().sort((a, b) => b.length - a.length || a.localeCompare(b)).join('|');
const grammarFiles = ['syntaxes/kx-basic.tmLanguage.json', 'syntaxes/kx-lua.tmLanguage.json'];
const updated = [];
for (const rel of grammarFiles) {
  const p = resolve(extRoot, rel);
  let g;
  try {
    g = readFileSync(p, 'utf8');
  } catch {
    console.warn(`[gen-commands] 跳过（不存在）：${rel}`);
    continue;
  }
  const re = /("name":\s*"support\.function\.kx-(?:basic|lua)",\s*\n\s*"match":\s*")([^"]*)(")/;
  if (!re.test(g)) {
    console.warn(`[gen-commands] ${rel} 未找到 support.function 匹配（跳过）`);
    continue;
  }
  const next = g.replace(re, (_all, head, _old, tail) => `${head}\\\\b(${alternation})\\\\b${tail}`);
  if (next !== g) {
    writeFileSync(p, next, 'utf8');
    updated.push(rel);
  }
}

console.log(
  `[gen-commands] 已生成 ${names.length} 条命令（文档 ${docCount} 条，分组 ${groupList.length} 个）` +
  ` -> ${outPath}${updated.length ? `；语法高亮已更新: ${updated.join(', ')}` : ''}`,
);
