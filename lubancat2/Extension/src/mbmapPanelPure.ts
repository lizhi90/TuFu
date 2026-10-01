// mbmapPanelPure.ts —— 「工具 → Modbus 配置」面板纯逻辑（D11，v0.9.0；不 import vscode）
//
// 数据来源（优先级）：
//   1) 控制器在线且声明 d11 → `mbmap.get`（权威：脚本目录 .mbmap，内容由 tools/gen-regmap.mjs 生成）
//   2) 控制器不可用 / 不声明 d11 / 读取失败 → 插件内置 data/regmap.json（★本地副本，界面明确标注）
//
// 纯函数：解析 / 分组 / 文案映射 / HTML 组装；冒烟测试直接覆盖（test/connection.smoke.cjs）。
// 「已存在的寄存器 = 已配置」（2026-09-30 定案）：本页只读展示；用户自定义编辑留待未来项目。

import { S } from './strings';

/** 单条寄存器（data/regmap.json 结构，与 .mbmap 同一格式） */
export interface RegmapEntry {
    addr: string;
    name?: string;
    type?: string;
    access?: string; // r / w / rw / ''
    persist?: boolean;
    desc?: string;
    zone?: string;
    doc?: string;
    line?: number;
    /** true = 用户寄存器（4x300~999，可编辑且真正生效）；固定区条目不设该字段 */
    user?: boolean;
    /** 默认值（D12 固件组态） */
    default?: number;
}

export interface RegmapFile {
    version?: number;
    station?: number;
    generatedFrom?: string[];
    /** D11 `.mbmap` 键 */
    entries?: RegmapEntry[];
    /** D12 `config/modbus.json` 键（与 entries 等价，解析时归一化到 entries） */
    registers?: RegmapEntry[];
}

/** 配置协议：d12=固件组态（全表可编辑）；d11=过渡（用户寄存器可编辑） */
export type MbProto = 'd12' | 'd11';

/** D12 组态地址范围（4x 全空间）；D11 用户区为 300~999 */
export const D12_BASE = 0;
export const D12_N = 65536;

/** 用户寄存器区（与控制器 kx_base.USER_BASE/USER_N 一致） */
export const USER_BASE = 300;
export const USER_N = 700;

const TYPE_SPAN: Record<string, number> = { u16: 1, i16: 1, u32: 2, f32: 2, f32hi: 2 };

export function addrNum(a: string | undefined): number | null {
    const m = /^4x(\d+)$/.exec((a ?? '').trim());
    return m ? Number(m[1]) : null;
}

/** 校验单条寄存器（返回中文错误或 null）；others 用于重叠检查（不含自身）；
 *  range 默认 D11 用户区 300~999；D12 传 [0, 65535] */
export function validateUserEntry(
    e: RegmapEntry,
    others: RegmapEntry[],
    range: [number, number] = [USER_BASE, USER_BASE + USER_N - 1],
): string | null {
    const a = addrNum(e.addr);
    if (a === null) {
        return S.mbmapPanel.errAddr;
    }
    if (a < range[0] || a > range[1]) {
        return S.mbmapPanel.errRange(range[0], range[1]);
    }
    const span = TYPE_SPAN[e.type ?? 'u16'] ?? 1;
    if (a + span - 1 > range[1]) {
        return S.mbmapPanel.errRange(range[0], range[1]);
    }
    if (e.access !== 'r' && e.access !== 'rw') {
        return S.mbmapPanel.errAccess;
    }
    for (const o of others) {
        const oa = addrNum(o.addr);
        if (oa === null) {
            continue;
        }
        const os = TYPE_SPAN[o.type ?? 'u16'] ?? 1;
        if (a <= oa + os - 1 && oa <= a + span - 1) {
            return S.mbmapPanel.errOverlap(o.addr);
        }
    }
    return null;
}

/** D12（固件组态）条目校验：地址全域/类型字数/权限 r·w·rw/掉电保持 ≤1023/重叠（← 固件 modbus_config 规则） */
export function validateD12Entries(list: RegmapEntry[]): string | null {
    const seen: RegmapEntry[] = [];
    for (const e of list) {
        const a = addrNum(e.addr);
        if (a === null) return S.mbmapPanel.errAddr;
        if (a < 0 || a > 65535) return S.mbmapPanel.errRange(0, 65535);
        const span = TYPE_SPAN[e.type ?? 'u16'] ?? 1;
        if (a + span - 1 > 65535) return S.mbmapPanel.errRange(0, 65535);
        if (e.access !== 'r' && e.access !== 'w' && e.access !== 'rw') return S.mbmapPanel.errAccessD12;
        if (e.persist === true && a > 1023) return S.mbmapPanel.errPersistRange;
        for (const o of seen) {
            const oa = addrNum(o.addr);
            if (oa === null) continue;
            const os = TYPE_SPAN[o.type ?? 'u16'] ?? 1;
            if (a <= oa + os - 1 && oa <= a + span - 1) return S.mbmapPanel.errOverlap(o.addr);
        }
        seen.push(e);
    }
    return null;
}

/** 下发前宿主侧权威校验（D12 文本）：解析失败/首错返回文本，null=通过（不依赖 webview 内联脚本） */
export function validateD12Text(text: string): string | null {
    let obj: { registers?: RegmapEntry[] } | undefined;
    try {
        obj = JSON.parse(text) as { registers?: RegmapEntry[] };
    } catch (err) {
        return S.mbmapPanel.errJson(String(err));
    }
    if (!obj || !Array.isArray(obj.registers)) return S.mbmapPanel.errShape;
    return validateD12Entries(obj.registers);
}

/** 取用户条目（user === true） */
export function userEntries(f: RegmapFile | undefined): RegmapEntry[] {
    return (f?.entries ?? []).filter((e) => e.user === true);
}

/** 取固定条目（描述性展示，不可编辑） */
export function fixedEntries(f: RegmapFile | undefined): RegmapEntry[] {
    return (f?.entries ?? []).filter((e) => e.user !== true);
}

/** 合并（固定 + 用户）为要下发的 .mbmap 文本；不改动固定条目 */
export function buildRegmapText(f: RegmapFile | undefined, users: RegmapEntry[]): string {
    const payload = {
        version: f?.version ?? 1,
        generatedFrom: f?.generatedFrom ?? ['planA/18', 'planA/19'],
        entries: [...fixedEntries(f), ...users],
    };
    return JSON.stringify(payload, null, 2) + '\n';
}

/** D12：构建固件组态文本（全部条目即 registers；station 保留） */
export function buildConfigText(f: RegmapFile | undefined, rows: RegmapEntry[], station: number): string {
    const payload: RegmapFile & { registers?: RegmapEntry[] } = {
        version: f?.version ?? 1,
        station: station,
        registers: rows,
    };
    return JSON.stringify(payload, null, 2) + '\n';
}

/** 解析寄存器表 JSON 文本（宽松：结构不对时明确报错，不抛异常） */
export function parseRegmap(text: string): { ok: boolean; error?: string; file?: RegmapFile } {
    if (!text || !text.trim()) {
        return { ok: false, error: S.mbmapPanel.errEmpty };
    }
    let obj: unknown;
    try {
        obj = JSON.parse(text);
    } catch (e) {
        return { ok: false, error: S.mbmapPanel.errJson(String(e)) };
    }
    const f = obj as RegmapFile;
    if (!f || typeof f !== 'object') {
        return { ok: false, error: S.mbmapPanel.errShape };
    }
    // v0.11.1：兼容 D12 `registers`（固件组态）与 D11 `entries`（.mbmap），归一化到 entries
    const raw = Array.isArray(f.entries) ? f.entries : Array.isArray(f.registers) ? f.registers : null;
    if (!raw) {
        return { ok: false, error: S.mbmapPanel.errShape };
    }
    const entries = raw.filter((e) => e && typeof e === 'object' && typeof e.addr === 'string');
    if (entries.length !== raw.length) {
        return { ok: false, error: S.mbmapPanel.errShape };
    }
    return { ok: true, file: { ...f, entries } };
}

export interface MbmapRow {
    addr: string;
    name: string;
    type: string;
    accessText: string;
    persistText: string;
    desc: string;
}
export interface MbmapGroup {
    zone: string;
    rows: MbmapRow[];
}
export type MbmapSource = 'controller' | 'local' | 'none';
export interface MbmapSpec {
    source: MbmapSource;
    sourceText: string;
    sourceDetail?: string;
    error?: string;
    groups: MbmapGroup[];
    total: number;
    /** 原文件（用于编辑合并；仅 controller/local 来源时存在） */
    file?: RegmapFile;
    /** 是否可编辑（控制器在线 且解析成功） */
    editable: boolean;
    /** 不可编辑原因（提示文案） */
    readonlyReason?: string;
    /** 配置协议（d12=固件组态全表可编辑；d11=过渡） */
    proto: MbProto;
    /** 从站站号（D12） */
    station: number;
}

/** r/w/rw → 中文文案（与 planA/18 读写列一致） */
export function accessText(a: string | undefined): string {
    switch (a) {
        case 'r':
            return S.mbmapPanel.accessR;
        case 'w':
            return S.mbmapPanel.accessW;
        case 'rw':
            return S.mbmapPanel.accessRW;
        default:
            return S.mbmapPanel.accessNone;
    }
}

/** 组装面板渲染数据：解析 + 按 zone 分组（保持源顺序） */
export function buildMbmapSpec(
    source: MbmapSource,
    text: string,
    sourceDetail?: string,
    proto: MbProto = 'd11',
): MbmapSpec {
    const srcText =
        source === 'controller'
            ? S.mbmapPanel.sourceController
            : source === 'local'
              ? S.mbmapPanel.sourceLocal
              : S.mbmapPanel.sourceNone;
    if (source === 'none') {
        return { source, sourceText: srcText, sourceDetail, groups: [], total: 0, editable: false, proto, station: 1 };
    }
    const pr = parseRegmap(text);
    if (!pr.ok || !pr.file) {
        return {
            source,
            sourceText: srcText,
            sourceDetail,
            error: pr.error,
            groups: [],
            total: 0,
            editable: false,
            proto,
            station: 1,
        };
    }
    const groups: MbmapGroup[] = [];
    const byZone = new Map<string, MbmapGroup>();
    for (const e of pr.file.entries ?? []) {
        const zone = e.zone && e.zone.length > 0 ? e.zone : S.mbmapPanel.zoneDefault;
        let g = byZone.get(zone);
        if (!g) {
            g = { zone, rows: [] };
            byZone.set(zone, g);
            groups.push(g);
        }
        g.rows.push({
            addr: e.addr,
            name: e.name ?? '',
            type: e.type ?? '',
            accessText: accessText(e.access),
            persistText: e.persist ? S.mbmapPanel.persistMark : '',
            desc: e.desc ?? '',
        });
    }
    const total = groups.reduce((n, g) => n + g.rows.length, 0);
    return {
        source,
        sourceText: srcText,
        sourceDetail,
        groups,
        total,
        file: pr.file,
        editable: source === 'controller',
        readonlyReason: source === 'controller' ? undefined : S.mbmapPanel.errNoD11,
        proto,
        station: pr.file.station ?? 1,
    };
}

const esc = (s: string): string =>
    s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

/** 面板 HTML（CSP + nonce；搜索为纯客户端过滤 +「刷新」按钮 postMessage） */
export function mbmapPanelHtml(spec: MbmapSpec, nonce: string): string {
    const rows: string[] = [];
    for (const g of spec.groups) {
        rows.push(`<h4 class="zone">${esc(g.zone)} <span class="cnt">${g.rows.length}</span></h4>`);
        rows.push(
            '<table><thead><tr>' +
                `<th>${esc(S.mbmapPanel.colAddr)}</th><th>${esc(S.mbmapPanel.colName)}</th>` +
                `<th>${esc(S.mbmapPanel.colType)}</th><th>${esc(S.mbmapPanel.colAccess)}</th>` +
                `<th>${esc(S.mbmapPanel.colKeep)}</th><th>${esc(S.mbmapPanel.colDesc)}</th>` +
                '</tr></thead><tbody>',
        );
        for (const r of g.rows) {
            const key = esc(`${r.addr} ${r.name} ${r.type} ${r.accessText} ${r.desc}`.toLowerCase());
            rows.push(
                `<tr data-k="${key}">` +
                    `<td class="addr">${esc(r.addr)}</td>` +
                    `<td class="name">${esc(r.name)}</td>` +
                    `<td class="type">${esc(r.type)}</td>` +
                    `<td class="acc">${esc(r.accessText)}</td>` +
                    `<td class="keep">${esc(r.persistText)}</td>` +
                    `<td class="desc">${esc(r.desc)}</td>` +
                    '</tr>',
            );
        }
        rows.push('</tbody></table>');
    }
    const isD12 = spec.proto === 'd12';
    const body = spec.error
        ? `<div class="err">${esc(spec.error)}</div>`
        : isD12
          ? ''                                    // D12：编辑器即全表（不再重复渲染只读区）
          : spec.total === 0
            ? `<div class="empty">${esc(S.mbmapPanel.empty)}</div>`
            : rows.join('\n');

    return `<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}';">
<style nonce="${nonce}">
  body { font-family: var(--vscode-font-family); font-size: var(--vscode-font-size); color: var(--vscode-foreground); padding: 8px 12px; }
  header { position: sticky; top: 0; background: var(--vscode-sideBar-background, var(--vscode-editor-background)); padding-bottom: 6px; z-index: 2; }
  h3 { margin: 0 0 4px 0; }
  .src { font-size: 0.9em; opacity: 0.85; }
  .src.bad { color: var(--vscode-editorWarning-foreground, #cca700); }
  .bar { display: flex; gap: 8px; align-items: center; margin-top: 6px; }
  .bar input { flex: 1; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, transparent); padding: 3px 6px; }
  .bar button { background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: none; padding: 3px 10px; cursor: pointer; }
  h4.zone { margin: 12px 0 4px; }
  h4.zone .cnt { font-weight: normal; opacity: 0.6; font-size: 0.9em; }
  table { border-collapse: collapse; width: 100%; }
  th, td { text-align: left; padding: 2px 6px; border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.25)); vertical-align: top; }
  th { position: sticky; top: 58px; background: var(--vscode-sideBar-background, var(--vscode-editor-background)); }
  td.addr { white-space: nowrap; font-family: var(--vscode-editor-font-family, monospace); }
  td.name { white-space: nowrap; font-family: var(--vscode-editor-font-family, monospace); }
  td.acc, td.keep { white-space: nowrap; }
  .err { color: var(--vscode-errorForeground, #f14c4c); margin-top: 8px; }
  .empty { opacity: 0.7; margin-top: 8px; }
  .note { margin-top: 10px; font-size: 0.9em; opacity: 0.7; }
  .uedit input.cell, .uedit select { background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, rgba(128,128,128,0.35)); padding: 1px 4px; width: 96%; font-family: var(--vscode-editor-font-family, monospace); }
  .uedit input.ck { width: auto; }
  .uedit td { padding: 2px 4px; }
  .uedit .del { cursor: pointer; background: none; border: none; color: var(--vscode-errorForeground, #f14c4c); }
  .uedit .ro { opacity: 0.75; }
</style></head>
<body>
<header>
  <h3>${esc(S.mbmapPanel.title)}</h3>
  <div class="src${spec.source === 'controller' ? '' : ' bad'}">${esc(spec.sourceText)}${spec.sourceDetail ? ' · ' + esc(spec.sourceDetail) : ''}${isD12 ? ' · ' + esc(S.mbmapPanel.stationText(spec.station)) : ''}</div>
  <div class="bar">
    <input id="q" type="text" placeholder="${esc(S.mbmapPanel.searchPlaceholder)}">
    <button id="rf">${esc(S.mbmapPanel.refreshBtn)}</button>
  </div>
  <div class="src">${esc(S.mbmapPanel.totalText(spec.total))}</div>
</header>
<main>
${body}
</main>
<h4 class="zone" style="margin-top:16px">${esc(isD12 ? S.mbmapPanel.d12Title : S.mbmapPanel.userTitle)} <span class="cnt" id="ucnt"></span></h4>
<div class="src">${esc(isD12 ? S.mbmapPanel.d12Hint : S.mbmapPanel.userHint)}</div>
<div class="bar uedit">
  <button id="uadd">${esc(S.mbmapPanel.addBtn)}</button>
  <button id="usave">${esc(S.mbmapPanel.saveBtn)}</button>
  <button id="ureload">${esc(S.mbmapPanel.reloadBtn)}</button>
</div>
<div class="err" id="uerr"></div>
<div class="uedit">
<table>
  <thead><tr>
    <th style="width:80px">${esc(S.mbmapPanel.colAddr)}</th>
    <th>${esc(S.mbmapPanel.colName)}</th>
    <th style="width:70px">${esc(S.mbmapPanel.colType)}</th>
    <th style="width:90px">${esc(S.mbmapPanel.colAccess)}</th>
    <th style="width:52px">${esc(S.mbmapPanel.colPersist)}</th>
    ${isD12 ? `<th style="width:72px">${esc(S.mbmapPanel.colDefault)}</th>` : ''}
    <th>${esc(S.mbmapPanel.colDesc)}</th>
    <th style="width:44px">${esc(S.mbmapPanel.colUserCol)}</th>
  </tr></thead>
  <tbody id="utbody"></tbody>
</table>
</div>
<div class="note">${esc(S.mbmapPanel.note)}</div>
<script nonce="${nonce}">const KX = ${JSON.stringify({
        editable: spec.editable,
        proto: spec.proto,
        station: spec.station,
        hasDefault: isD12,
        range: isD12 ? [D12_BASE, D12_BASE + D12_N - 1] : [USER_BASE, USER_BASE + USER_N - 1],
        fixed: isD12 ? [] : fixedEntries(spec.file),
        user: isD12 ? (spec.file?.entries ?? []) : userEntries(spec.file),
        version: spec.file?.version ?? 1,
        generatedFrom: spec.file?.generatedFrom ?? ['planA/18', 'planA/19'],
        labels: {
            del: S.mbmapPanel.delBtn,
            r: S.mbmapPanel.accessR,
            rw: S.mbmapPanel.accessRW,
            addrPh: S.mbmapPanel.colAddrPh,
            defPh: S.mbmapPanel.defPh,
            namePh: S.mbmapPanel.colNamePh,
            descPh: S.mbmapPanel.colDescPh,
            userEmpty: S.mbmapPanel.userEmpty,
            errAddr: S.mbmapPanel.errAddr,
            errRange: S.mbmapPanel.errRange(USER_BASE, USER_BASE + USER_N - 1),
            errAccess: S.mbmapPanel.errAccess,
            errOverlapPrefix: S.mbmapPanel.errOverlap('').trim(),
            errNoD11: S.mbmapPanel.errNoD11,
            saveFailPrefix: S.mbmapPanel.saveFail('').trim(),
        },
    }).replace(/</g, '\\u003c')};</script>
<script nonce="${nonce}">
  const vscode = acquireVsCodeApi();
  const q = document.getElementById('q');
  const rows = Array.from(document.querySelectorAll('tr[data-k]'));
  q.addEventListener('input', () => {
    const t = q.value.trim().toLowerCase();
    for (const r of rows) { r.style.display = (!t || r.getAttribute('data-k').includes(t)) ? '' : 'none'; }
  });
  document.getElementById('rf').addEventListener('click', () => vscode.postMessage({ type: 'refresh' }));
  document.getElementById('ureload').addEventListener('click', () => vscode.postMessage({ type: 'refresh' }));

  // ---- 用户寄存器编辑（4x300~999；保存 = 合并后 mbmap.set，控制器热加载） ----
  const TYPES = ['u16', 'i16', 'u32', 'f32', 'f32hi'];
  const SPAN = { u16: 1, i16: 1, u32: 2, f32: 2, f32hi: 2 };
  const U0 = KX.range[0], UN = KX.range[1] - KX.range[0] + 1;
  const L = KX.labels;
  let users = (KX.user || []).slice();
  const num = (a) => { const m = /^4x(\\d+)$/.exec((a || '').trim()); return m ? Number(m[1]) : null; };
  const isD12 = KX.proto === 'd12';
  function validate(list) {
    const errs = []; const seen = [];
    for (const e of list) {
      const a = num(e.addr);
      if (a === null) { errs.push(L.errAddr); continue; }
      const sp = SPAN[e.type || 'u16'] || 1;
      if (isD12 ? (a + sp - 1 > 65535) : (a < U0 || a + sp - 1 > U0 + UN - 1)) { errs.push(L.errRange); continue; }
      const accOk = isD12 ? (e.access === 'r' || e.access === 'w' || e.access === 'rw')
                          : (e.access === 'r' || e.access === 'rw');
      if (!accOk) { errs.push(L.errAccess); continue; }
      if (isD12 && e.persist === true && a > 1023) { errs.push(L.errPersistRange); continue; }
      for (const o of seen) {
        const oa = num(o.addr); if (oa === null) continue;
        const os = SPAN[o.type || 'u16'] || 1;
        if (a <= oa + os - 1 && oa <= a + sp - 1) { errs.push(L.errOverlapPrefix + o.addr); break; }
      }
      seen.push(e);
    }
    return errs;
  }
  function render() {
    const tb = document.getElementById('utbody');
    tb.innerHTML = '';
    if (users.length === 0) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 7; td.textContent = L.userEmpty; td.style.opacity = '0.7';
      tr.appendChild(td); tb.appendChild(tr);
    }
    users.forEach((e, i) => {
      const tr = document.createElement('tr');
      const mkInput = (k, ph, w) => {
        const inp = document.createElement('input');
        inp.className = 'cell'; inp.dataset.i = String(i); inp.dataset.k = k;
        inp.value = e[k] || ''; if (ph) inp.placeholder = ph;
        if (!KX.editable) { inp.disabled = true; }
        return inp;
      };
      const mkSelect = (k, opts) => {
        const sel = document.createElement('select');
        sel.dataset.i = String(i); sel.dataset.k = k;
        for (const [v, label] of opts) {
          const o = document.createElement('option'); o.value = v; o.textContent = label;
          if ((e[k] || opts[0][0]) === v) o.selected = true;
          sel.appendChild(o);
        }
        if (!KX.editable) { sel.disabled = true; }
        return sel;
      };
      const cells = [
        mkInput('addr', L.addrPh),
        mkInput('name', L.namePh),
        mkSelect('type', TYPES.map((t) => [t, t])),
        mkSelect('access', isD12 ? [['rw', L.rw], ['r', L.r], ['w', L.accessW]] : [['rw', L.rw], ['r', L.r]]),
      ];
      for (const c of cells) { const td = document.createElement('td'); td.appendChild(c); tr.appendChild(td); }
      const tdck = document.createElement('td');
      const ck = document.createElement('input'); ck.type = 'checkbox'; ck.className = 'ck';
      ck.dataset.i = String(i); ck.dataset.k = 'persist'; ck.checked = e.persist === true;
      if (!KX.editable) { ck.disabled = true; }
      tdck.appendChild(ck); tr.appendChild(tdck);
      if (KX.hasDefault) {
        const tddv = document.createElement('td');
        const dv = mkInput('default', L.defPh);
        dv.value = (e.default === undefined || e.default === null) ? '' : String(e.default);
        tddv.appendChild(dv); tr.appendChild(tddv);
      }
      const tdd = document.createElement('td'); tdd.appendChild(mkInput('desc', L.descPh)); tr.appendChild(tdd);
      const tdo = document.createElement('td');
      if (KX.editable) {
        const del = document.createElement('button'); del.className = 'del'; del.textContent = '✕'; del.title = L.del;
        del.addEventListener('click', () => { users.splice(i, 1); render(); });
        tdo.appendChild(del);
      }
      tr.appendChild(tdo);
      tb.appendChild(tr);
    });
    document.getElementById('ucnt').textContent = users.length > 0 ? String(users.length) : '';
  }
  document.getElementById('utbody').addEventListener('input', (ev) => {
    const t = ev.target; const i = Number(t.dataset.i);
    if (!Number.isFinite(i)) return;
    if (t.dataset.k === 'persist') users[i].persist = t.checked;
    else users[i][t.dataset.k] = t.value;
  });
  document.getElementById('utbody').addEventListener('change', (ev) => {
    const t = ev.target; const i = Number(t.dataset.i);
    if (!Number.isFinite(i)) return;
    if (t.dataset.k === 'type' || t.dataset.k === 'access') users[i][t.dataset.k] = t.value;
    if (t.dataset.k === 'persist') users[i].persist = t.checked;
  });
  document.getElementById('uadd').addEventListener('click', () => {
    if (!KX.editable) { document.getElementById('uerr').textContent = L.errNoD11; return; }
    users.push({ addr: '', name: '', type: 'u16', access: 'rw', persist: false, desc: '', user: true });
    render();
  });
  document.getElementById('usave').addEventListener('click', () => {
    const ue = document.getElementById('uerr');
    if (!KX.editable) { ue.textContent = L.errNoD11; return; }
    const errs = validate(users);
    if (errs.length > 0) { ue.textContent = errs[0]; return; }
    ue.textContent = '';
    let text;
    if (KX.proto === 'd12') {
      const rows = users.map((e) => {
        const o = { addr: e.addr, name: e.name || undefined, type: e.type, access: e.access,
                    persist: e.persist === true ? true : undefined, desc: e.desc || undefined };
        if (KX.hasDefault) {
          const d = parseFloat(e.default);
          if (isFinite(d)) { o.default = d; }
        }
        return o;
      });
      text = JSON.stringify({ version: KX.version, station: KX.station, registers: rows }, null, 2) + '\\n';
    } else {
      const entries = KX.fixed.concat(users.map((e) => ({
        addr: e.addr, name: e.name || undefined, type: e.type, access: e.access,
        persist: e.persist === true ? true : undefined, desc: e.desc || undefined, user: true,
      })));
      text = JSON.stringify({ version: KX.version, generatedFrom: KX.generatedFrom, entries }, null, 2) + '\\n';
    }
    vscode.postMessage({ type: 'save', text });
  });
  render();
</script>
</body></html>`;
}
