// mbdevPanelPure.ts —— 「工具 → Modbus 主站」面板纯逻辑（D13，v0.12.0；planA/21；不 import vscode）
//
// 数据：控制器 config/modbus_master.json（mbdev.get） + 在线状态（mbdev.status）。
// 纯函数：解析 / 校验 / 构建 / HTML。★内联脚本一律用 [0-9] 字符类与 \\n 双转义，
// 避免 TS 模板吞转义（v0.11.2 教训）；smoke 有「内联脚本可解析」守卫。

import { S } from './strings';

export interface MbPoint {
    name: string;
    dir: 'read' | 'write';
    fc: number;
    addr: number;
    count: number;
    type: string;
    mapKind: 'reg' | 'var';
    mapAddr?: string;
    on_change?: boolean;
}
export interface MbDevice {
    name: string;
    kind: 'tcp' | 'rtu-tcp';
    host: string;
    port: number;
    unit: number;
    timeout_ms: number;
    retries: number;
    poll_ms: number;
    points: MbPoint[];
}
export interface MbDevFile {
    version?: number;
    devices?: MbDevice[];
}
export interface MbDevStatus {
    name: string;
    online: boolean;
    err: number;
    timeouts: number;
    ok: number;
    last_err?: string;
    points?: number;
}

const TYPE_SPAN: Record<string, number> = { u16: 1, i16: 1, u32: 2, f32: 2, f32hi: 2 };
const READ_FC = [1, 2, 3, 4];
const WRITE_FC = [5, 6, 15, 16];
const BIT_FC = [1, 2, 5, 15];

export function addrNum(a: string | undefined): number | null {
    const m = /^4x([0-9]+)$/.exec((a ?? '').trim());
    return m ? Number(m[1]) : null;
}

export function parseMbDev(text: string): { ok: boolean; error?: string; file?: MbDevFile } {
    if (!text || !text.trim()) {
        return { ok: true, file: { version: 1, devices: [] } };        // 空 = 无组态
    }
    let obj: unknown;
    try {
        obj = JSON.parse(text);
    } catch (e) {
        return { ok: false, error: S.mbdevPanel.errJson(String(e)) };
    }
    const f = obj as MbDevFile;
    if (!f || typeof f !== 'object') {
        return { ok: false, error: S.mbdevPanel.errShape };
    }
    const devs = Array.isArray(f.devices)
        ? f.devices
        : f.devices === undefined
          ? []
          : null;
    if (devs === null) {
        return { ok: false, error: S.mbdevPanel.errShape };
    }
    const devices: MbDevice[] = [];
    for (const d of devs as MbDevice[]) {
        if (!d || typeof d !== 'object' || typeof d.name !== 'string') {
            return { ok: false, error: S.mbdevPanel.errShape };
        }
        devices.push({
            name: d.name,
            kind: d.kind === 'rtu-tcp' ? 'rtu-tcp' : 'tcp',
            host: d.host ?? '',
            port: Number(d.port ?? 502),
            unit: Number(d.unit ?? 1),
            timeout_ms: Number(d.timeout_ms ?? 300),
            retries: Number(d.retries ?? 1),
            poll_ms: Number(d.poll_ms ?? 200),
            points: Array.isArray(d.points)
                ? d.points.map((p) => ({
                      name: p.name ?? '',
                      dir: p.dir === 'write' ? 'write' : 'read',
                      fc: Number(p.fc ?? 3),
                      addr: Number(p.addr ?? 0),
                      count: Number(p.count ?? 1),
                      type: p.type ?? 'u16',
                      mapKind: p.mapKind === 'reg' || (p as { map?: { kind?: string } }).map?.kind === 'reg'
                          ? 'reg'
                          : 'var',
                      mapAddr:
                          p.mapAddr ??
                          (p as { map?: { addr?: string } }).map?.addr ??
                          undefined,
                      on_change: p.on_change !== false,
                  }))
                : [],
        });
    }
    return { ok: true, file: { version: f.version ?? 1, devices } };
}

export function validateDevice(d: MbDevice, others: MbDevice[]): string | null {
    if (!d.name || d.name.trim().length === 0) return S.mbdevPanel.errDevName;
    if (others.some((o) => o !== d && o.name === d.name)) return S.mbdevPanel.errDevName;
    if (!d.host || d.host.trim().length === 0) return S.mbdevPanel.errDevHost;
    if (!Number.isFinite(d.port) || d.port < 1 || d.port > 65535) return S.mbdevPanel.errDevPort;
    if (!Number.isFinite(d.unit) || d.unit < 1 || d.unit > 247) return S.mbdevPanel.errDevUnit;
    if (!Number.isFinite(d.timeout_ms) || d.timeout_ms < 50 || d.timeout_ms > 5000)
        return S.mbdevPanel.errDevTimeout;
    if (!Number.isFinite(d.retries) || d.retries < 0 || d.retries > 5) return S.mbdevPanel.errDevRetries;
    if (!Number.isFinite(d.poll_ms) || d.poll_ms < 20 || d.poll_ms > 60000) return S.mbdevPanel.errDevPoll;
    if (d.points.length === 0) return S.mbdevPanel.errDevNoPoints;
    return null;
}

export function validatePoint(p: MbPoint, dev: MbDevice): string | null {
    if (!p.name || p.name.trim().length === 0) return S.mbdevPanel.errPointName;
    if (dev.points.some((o) => o !== p && o.name === p.name)) return S.mbdevPanel.errPointName;
    const okFc = p.dir === 'write' ? WRITE_FC.includes(p.fc) : READ_FC.includes(p.fc);
    if (!okFc) return S.mbdevPanel.errPointFc;
    const bits = BIT_FC.includes(p.fc);
    if (bits && p.type !== 'u16' && p.type !== 'i16') return S.mbdevPanel.errPointBitType;
    if (p.fc === 6 && !bits && (TYPE_SPAN[p.type] ?? 1) === 2) return S.mbdevPanel.errPointFc6Span;
    const span = bits ? 1 : TYPE_SPAN[p.type] ?? 1;
    if (!Number.isFinite(p.addr) || p.addr < 0 || p.addr > 65535 || p.addr + p.count - 1 > 65535) {
        return S.mbdevPanel.errPointAddr;
    }
    if (p.count !== span) return S.mbdevPanel.errPointCount;
    if (p.mapKind === 'reg' && addrNum(p.mapAddr) === null) return S.mbdevPanel.errPointMap;
    return null;
}

/** 写点 4x 映射全局唯一（客户端预检；固件端权威校验） */
export function validateAll(devices: MbDevice[]): string | null {
    for (const d of devices) {
        const de = validateDevice(d, devices);
        if (de) return de;
        for (const p of d.points) {
            const pe = validatePoint(p, d);
            if (pe) return pe;
        }
    }
    const used: string[] = [];
    for (const d of devices) {
        for (const p of d.points) {
            if (p.dir !== 'write' || p.mapKind !== 'reg') continue;
            const a = addrNum(p.mapAddr);
            if (a === null) continue;
            for (let w = a; w < a + p.count; ++w) {
                const key = '4x' + w;
                if (used.includes(key)) return S.mbdevPanel.errWriteDup(key);
                used.push(key);
            }
        }
    }
    return null;
}

/** 构建下发文本（键与固件一致：link/points/map.kind 等） */
export function buildMbDevText(file: MbDevFile | undefined, devices: MbDevice[]): string {
    const out = {
        version: file?.version ?? 1,
        devices: devices.map((d) => ({
            name: d.name,
            link: { kind: d.kind, host: d.host, port: d.port },
            unit: d.unit,
            timeout_ms: d.timeout_ms,
            retries: d.retries,
            poll_ms: d.poll_ms,
            points: d.points.map((p) => {
                const o: Record<string, unknown> = {
                    name: p.name,
                    dir: p.dir,
                    fc: p.fc,
                    addr: p.addr,
                    count: p.count,
                    type: p.type,
                    map: p.mapKind === 'reg' ? { kind: 'reg', addr: p.mapAddr } : { kind: 'var' },
                };
                if (p.dir === 'write') o.on_change = p.on_change !== false;
                return o;
            }),
        })),
    };
    return JSON.stringify(out, null, 2) + '\n';
}

export interface MbdevSpec {
    source: 'controller' | 'none';
    sourceText: string;
    editable: boolean;
    error?: string;
    devices: MbDevice[];
    status: MbDevStatus[];
}

export function buildMbdevSpec(
    source: 'controller' | 'none',
    text: string,
    statusJson?: string,
): MbdevSpec {
    const srcText = source === 'controller' ? S.mbdevPanel.sourceController : S.mbdevPanel.sourceNone;
    let status: MbDevStatus[] = [];
    if (statusJson && statusJson.trim()) {
        try {
            const st = JSON.parse(statusJson) as { devices?: MbDevStatus[] };
            status = Array.isArray(st.devices) ? st.devices : [];
        } catch {
            status = [];
        }
    }
    if (source === 'none') {
        return { source, sourceText: srcText, editable: false, devices: [], status };
    }
    const pr = parseMbDev(text);
    if (!pr.ok) {
        return { source, sourceText: srcText, editable: false, error: pr.error, devices: [], status };
    }
    return {
        source,
        sourceText: srcText,
        editable: true,
        devices: pr.file?.devices ?? [],
        status,
    };
}

const esc = (s: string): string =>
    s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

/** 面板 HTML（设备表单 + 点位表格；全部编辑走注入 JS；保存 postMessage{type:'save',text}） */
export function mbdevPanelHtml(spec: MbdevSpec, nonce: string): string {
    const payload = {
        editable: spec.editable,
        error: spec.error ?? '',
        devices: spec.devices,
        status: spec.status,
        labels: {
            addDev: S.mbdevPanel.addDev, delDev: S.mbdevPanel.delDev, addPoint: S.mbdevPanel.addPoint,
            saveBtn: S.mbdevPanel.saveBtn, reloadBtn: S.mbdevPanel.reloadBtn, del: S.mbdevPanel.delBtn,
            namePh: S.mbdevPanel.namePh, hostPh: S.mbdevPanel.hostPh, portPh: S.mbdevPanel.portPh,
            unitPh: S.mbdevPanel.unitPh, timeoutPh: S.mbdevPanel.timeoutPh, retriesPh: S.mbdevPanel.retriesPh,
            pollPh: S.mbdevPanel.pollPh, addrPh: S.mbdevPanel.addrPh, mapAddrPh: S.mbdevPanel.mapAddrPh,
            kindTcp: S.mbdevPanel.kindTcp, kindRtu: S.mbdevPanel.kindRtu,
            lblKind: S.mbdevPanel.lblKind, lblName: S.mbdevPanel.lblName, lblHost: S.mbdevPanel.lblHost,
            lblPort: S.mbdevPanel.lblPort, lblUnit: S.mbdevPanel.lblUnit, lblTimeout: S.mbdevPanel.lblTimeout,
            lblRetries: S.mbdevPanel.lblRetries, lblPoll: S.mbdevPanel.lblPoll,
            dirRead: S.mbdevPanel.dirRead, dirWrite: S.mbdevPanel.dirWrite,
            mapReg: S.mbdevPanel.mapReg, mapVar: S.mbdevPanel.mapVar,
            empty: S.mbdevPanel.empty, emptyPoints: S.mbdevPanel.emptyPoints,
            devTitle: S.mbdevPanel.devTitle, pointTitle: S.mbdevPanel.pointTitle,
            statusTitle: S.mbdevPanel.statusTitle, online: S.mbdevPanel.statusOnline,
            offline: S.mbdevPanel.statusOffline,
            errDevName: S.mbdevPanel.errDevName, errDevHost: S.mbdevPanel.errDevHost,
            errDevPort: S.mbdevPanel.errDevPort, errDevUnit: S.mbdevPanel.errDevUnit,
            errPointName: S.mbdevPanel.errPointName, errPointAddr: S.mbdevPanel.errPointAddr,
            errPointFc: S.mbdevPanel.errPointFc, errPointCount: S.mbdevPanel.errPointCount,
            errPointMap: S.mbdevPanel.errPointMap, errWriteDupPrefix: '主站写点的 4x 映射重叠：',
            errNone: S.mbdevPanel.sourceNone,
        },
    };
    const json = JSON.stringify(payload).replace(/</g, '\\u003c');

    return `<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}';">
<style nonce="${nonce}">
  body { font-family: var(--vscode-font-family); font-size: var(--vscode-font-size); color: var(--vscode-foreground); padding: 8px 12px; }
  header { position: sticky; top: 0; background: var(--vscode-sideBar-background, var(--vscode-editor-background)); padding-bottom: 6px; z-index: 2; }
  h3 { margin: 0 0 4px 0; }
  h4 { margin: 12px 0 4px; }
  .src { font-size: 0.9em; opacity: 0.85; }
  .src.bad { color: var(--vscode-editorWarning-foreground, #cca700); }
  .bar { display: flex; gap: 8px; align-items: center; margin: 6px 0; flex-wrap: wrap; }
  .bar button, .del { background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: none; padding: 3px 10px; cursor: pointer; }
  .del { background: none; color: var(--vscode-errorForeground, #f14c4c); }
  input, select { background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, rgba(128,128,128,0.35)); padding: 1px 4px; font-family: var(--vscode-editor-font-family, monospace); }
  table { border-collapse: collapse; width: 100%; }
  th, td { text-align: left; padding: 2px 5px; border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.25)); vertical-align: middle; }
  .devform { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin: 4px 0; }
  .devform label { opacity: 0.8; font-size: 0.9em; }
  .err { color: var(--vscode-errorForeground, #f14c4c); margin-top: 6px; }
  .note { margin-top: 10px; font-size: 0.9em; opacity: 0.75; }
  .st-on { color: var(--vscode-testing-iconPassed, #3fb950); }
  .st-off { color: var(--vscode-editorWarning-foreground, #cca700); }
  .st { font-size: 0.9em; opacity: 0.9; display: flex; gap: 14px; flex-wrap: wrap; }
</style></head>
<body>
<header>
  <h3>${esc(S.mbdevPanel.title)}</h3>
  <div class="src${spec.source === 'controller' ? '' : ' bad'}">${esc(spec.sourceText)}</div>
  <div class="bar">
    <button id="dadd">${esc(S.mbdevPanel.addDev)}</button>
    <button id="ddel">${esc(S.mbdevPanel.delDev)}</button>
    <span class="src">${esc(S.mbdevPanel.colName)}:</span>
    <select id="dsel"></select>
    <button id="save">${esc(S.mbdevPanel.saveBtn)}</button>
    <button id="reload">${esc(S.mbdevPanel.reloadBtn)}</button>
  </div>
  <div class="err" id="err">${esc(spec.error ?? '')}</div>
</header>
<main>
  <h4>${esc(S.mbdevPanel.devTitle)}</h4>
  <div class="devform" id="dform"></div>
  <h4>${esc(S.mbdevPanel.pointTitle)} <button id="padd">${esc(S.mbdevPanel.addPoint)}</button></h4>
  <table>
    <thead><tr>
      <th>${esc(S.mbdevPanel.colName)}</th>
      <th style="width:64px">${esc(S.mbdevPanel.colDir)}</th>
      <th style="width:70px">${esc(S.mbdevPanel.colFc)}</th>
      <th style="width:70px">${esc(S.mbdevPanel.colAddr)}</th>
      <th style="width:52px">${esc(S.mbdevPanel.colCount)}</th>
      <th style="width:78px">${esc(S.mbdevPanel.colType)}</th>
      <th style="width:150px">${esc(S.mbdevPanel.colMap)}</th>
      <th style="width:66px">${esc(S.mbdevPanel.colOnChange)}</th>
      <th style="width:44px">${esc(S.mbdevPanel.colOp)}</th>
    </tr></thead>
    <tbody id="ptbody"></tbody>
  </table>
  <h4>${esc(S.mbdevPanel.statusTitle)}</h4>
  <div class="st" id="stlist"></div>
  <div class="note">${esc(S.mbdevPanel.hint)}</div>
</main>
<script nonce="${nonce}">const KX = ${json};</script>
<script nonce="${nonce}">
  const vscode = acquireVsCodeApi();
  const L = KX.labels;
  const TYPES = ['u16', 'i16', 'u32', 'f32', 'f32hi'];
  const SPAN = { u16: 1, i16: 1, u32: 2, f32: 2, f32hi: 2 };
  const RFC = [1, 2, 3, 4], WFC = [5, 6, 15, 16], BFC = [1, 2, 5, 15];
  let devices = (KX.devices || []).slice();
  let sel = devices.length > 0 ? 0 : -1;

  const el = (id) => document.getElementById(id);
  const num = (a) => { const m = /^4x([0-9]+)$/.exec((a || '').trim()); return m ? Number(m[1]) : null; };

  function validateAllLocal() {
    for (const d of devices) {
      if (!d.name || d.name.trim().length === 0) return L.errDevName;
      if (devices.filter((o) => o.name === d.name).length > 1) return L.errDevName;
      if (!d.host || d.host.trim().length === 0) return L.errDevHost;
      if (!(d.port >= 1 && d.port <= 65535)) return L.errDevPort;
      if (!(d.unit >= 1 && d.unit <= 247)) return L.errDevUnit;
      if (!(d.timeout_ms >= 50 && d.timeout_ms <= 5000)) return L.errDevTimeout;
      if (!(d.retries >= 0 && d.retries <= 5)) return L.errDevRetries;
      if (!(d.poll_ms >= 20 && d.poll_ms <= 60000)) return L.errDevPoll;
      if (d.points.length === 0) return L.errDevNoPoints;
      for (const p of d.points) {
        if (!p.name || p.name.trim().length === 0) return L.errPointName;
        if (d.points.filter((o) => o.name === p.name).length > 1) return L.errPointName;
        const okFc = p.dir === 'write' ? WFC.indexOf(p.fc) >= 0 : RFC.indexOf(p.fc) >= 0;
        if (!okFc) return L.errPointFc;
        const bits = BFC.indexOf(p.fc) >= 0;
        if (bits && p.type !== 'u16' && p.type !== 'i16') return L.errPointBitType;
        if (p.fc === 6 && !bits && (SPAN[p.type] || 1) === 2) return L.errPointFc6Span;
        const span = bits ? 1 : (SPAN[p.type] || 1);
        if (!(p.addr >= 0 && p.addr <= 65535 && p.addr + p.count - 1 <= 65535)) return L.errPointAddr;
        if (p.count !== span) return L.errPointCount;
        if (p.mapKind === 'reg' && num(p.mapAddr) === null) return L.errPointMap;
      }
    }
    const used = [];
    for (const d of devices) {
      for (const p of d.points) {
        if (p.dir !== 'write' || p.mapKind !== 'reg') continue;
        const a = num(p.mapAddr);
        if (a === null) continue;
        for (let w = a; w < a + p.count; w++) {
          const key = '4x' + w;
          if (used.indexOf(key) >= 0) return L.errWriteDupPrefix + key;
          used.push(key);
        }
      }
    }
    return null;
  }

  function mkInput(val, ph, on, w) {
    const i = document.createElement('input');
    i.value = val === undefined || val === null ? '' : String(val);
    if (ph) i.placeholder = ph;
    if (w) i.style.width = w;
    if (on) i.addEventListener('input', () => on(i.value));
    if (!KX.editable) i.disabled = true;
    return i;
  }
  function mkSelect(val, opts, on) {
    const s = document.createElement('select');
    for (const [v, label] of opts) {
      const o = document.createElement('option');
      o.value = v; o.textContent = label;
      if (String(val) === v) o.selected = true;
      s.appendChild(o);
    }
    if (on) s.addEventListener('change', () => on(s.value));
    if (!KX.editable) s.disabled = true;
    return s;
  }
  function mkTd(node) { const td = document.createElement('td'); if (node) td.appendChild(node); return td; }

  function renderDevForm() {
    const box = el('dform');
    box.innerHTML = '';
    if (sel < 0 || !devices[sel]) { box.textContent = L.empty; return; }
    const d = devices[sel];
    const add = (label, node) => {
      const wrap = document.createElement('span');       // 功能文字置顶（v0.12.4：填值后仍可见字段含义）
      wrap.style.display = 'inline-flex';
      wrap.style.flexDirection = 'column';
      wrap.style.marginRight = '8px';
      const lb = document.createElement('span');
      lb.textContent = label;
      lb.style.opacity = '0.7';
      lb.style.fontSize = '11px';
      lb.style.marginBottom = '2px';
      wrap.appendChild(lb);
      wrap.appendChild(node);
      box.appendChild(wrap);
    };
    add(L.lblKind, mkSelect(d.kind, [['tcp', L.kindTcp], ['rtu-tcp', L.kindRtu]], (v) => { d.kind = v; }));
    add(L.lblName, mkInput(d.name, L.namePh, (v) => { d.name = v; renderSel(); }, '140px'));
    add(L.lblHost, mkInput(d.host, L.hostPh, (v) => { d.host = v; }, '140px'));
    add(L.lblPort, mkInput(d.port, L.portPh, (v) => { d.port = Number(v) || 0; }, '64px'));
    add(L.lblUnit, mkInput(d.unit, L.unitPh, (v) => { d.unit = Number(v) || 0; }, '52px'));
    add(L.lblTimeout, mkInput(d.timeout_ms, L.timeoutPh, (v) => { d.timeout_ms = Number(v) || 300; }, '68px'));
    add(L.lblRetries, mkInput(d.retries, L.retriesPh, (v) => { d.retries = Number(v) || 0; }, '52px'));
    add(L.lblPoll, mkInput(d.poll_ms, L.pollPh, (v) => { d.poll_ms = Number(v) || 200; }, '68px'));
  }

  function renderSel() {
    const s = el('dsel');
    s.innerHTML = '';
    devices.forEach((d, i) => {
      const o = document.createElement('option');
      o.value = String(i);
      o.textContent = d.name || ('#' + (i + 1));
      if (i === sel) o.selected = true;
      s.appendChild(o);
    });
    if (!KX.editable) s.disabled = true;
  }

  function renderPoints() {
    const tb = el('ptbody');
    tb.innerHTML = '';
    if (sel < 0 || !devices[sel]) return;
    const d = devices[sel];
    if (d.points.length === 0) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 9; td.textContent = L.emptyPoints; td.style.opacity = '0.7';
      tr.appendChild(td); tb.appendChild(tr);
      return;
    }
    d.points.forEach((p, i) => {
      const tr = document.createElement('tr');
      tr.appendChild(mkTd(mkInput(p.name, L.namePh, (v) => { p.name = v; }, '110px')));
      tr.appendChild(mkTd(mkSelect(p.dir, [['read', L.dirRead], ['write', L.dirWrite]], (v) => {
        p.dir = v === 'write' ? 'write' : 'read';
        renderPoints();
      })));
      tr.appendChild(mkTd(mkInput(p.fc, '', (v) => { p.fc = Number(v) || 0; }, '44px')));
      tr.appendChild(mkTd(mkInput(p.addr, L.addrPh, (v) => { p.addr = Number(v) || 0; }, '58px')));
      tr.appendChild(mkTd(mkInput(p.count, '', (v) => { p.count = Number(v) || 1; }, '36px')));
      tr.appendChild(mkTd(mkSelect(p.type, TYPES.map((t) => [t, t]), (v) => {
        p.type = v; p.count = SPAN[v] || 1; renderPoints();
      })));
      const mapBox = document.createElement('span');
      mapBox.appendChild(mkSelect(p.mapKind, [['reg', L.mapReg], ['var', L.mapVar]], (v) => {
        p.mapKind = v === 'reg' ? 'reg' : 'var';
        renderPoints();
      }));
      if (p.mapKind === 'reg') {
        mapBox.appendChild(mkInput(p.mapAddr, L.mapAddrPh, (v) => { p.mapAddr = v; }, '76px'));
      }
      tr.appendChild(mkTd(mapBox));
      const ck = document.createElement('input');
      ck.type = 'checkbox';
      if (p.dir !== 'write') { ck.disabled = true; }
      else { ck.checked = p.on_change !== false; }
      ck.addEventListener('change', () => { p.on_change = ck.checked; });
      if (!KX.editable) ck.disabled = true;
      tr.appendChild(mkTd(ck));
      const del = document.createElement('button');
      del.className = 'del'; del.textContent = '✕'; del.title = L.del;
      del.addEventListener('click', () => { d.points.splice(i, 1); renderPoints(); });
      if (!KX.editable) del.disabled = true;
      tr.appendChild(mkTd(del));
      tb.appendChild(tr);
    });
  }

  function renderStatus() {
    const box = el('stlist');
    box.innerHTML = '';
    const st = KX.status || [];
    if (st.length === 0) { box.textContent = '—'; return; }
    for (const s of st) {
      const sp = document.createElement('span');
      sp.className = s.online ? 'st-on' : 'st-off';
      sp.textContent = s.name + ': ' + (s.online ? L.online : L.offline) +
        ' (err=' + s.err + ', timeouts=' + s.timeouts + ', ok=' + s.ok + ')';
      box.appendChild(sp);
    }
  }

  el('dsel').addEventListener('change', (e) => { sel = Number(e.target.value); renderDevForm(); renderPoints(); });
  el('dadd').addEventListener('click', () => {
    if (!KX.editable) { el('err').textContent = L.errNone; return; }
    devices.push({ name: '', kind: 'tcp', host: '', port: 502, unit: 1, timeout_ms: 300, retries: 1, poll_ms: 200, points: [] });
    sel = devices.length - 1;
    renderSel(); renderDevForm(); renderPoints();
  });
  el('ddel').addEventListener('click', () => {
    if (!KX.editable || sel < 0) return;
    devices.splice(sel, 1);
    sel = devices.length > 0 ? 0 : -1;
    renderSel(); renderDevForm(); renderPoints();
  });
  el('padd').addEventListener('click', () => {
    if (!KX.editable || sel < 0) { el('err').textContent = L.errNone; return; }
    devices[sel].points.push({ name: '', dir: 'read', fc: 3, addr: 0, count: 1, type: 'u16', mapKind: 'var', on_change: true });
    renderPoints();
  });
  el('reload').addEventListener('click', () => vscode.postMessage({ type: 'refresh' }));
  el('save').addEventListener('click', () => {
    if (!KX.editable) { el('err').textContent = L.errNone; return; }
    const ve = validateAllLocal();
    if (ve) { el('err').textContent = ve; return; }
    el('err').textContent = '';
    const out = {
      version: 1,
      devices: devices.map((d) => ({
        name: d.name,
        link: { kind: d.kind, host: d.host, port: d.port },
        unit: d.unit, timeout_ms: d.timeout_ms, retries: d.retries, poll_ms: d.poll_ms,
        points: d.points.map((p) => {
          const o = {
            name: p.name, dir: p.dir, fc: p.fc, addr: p.addr, count: p.count, type: p.type,
            map: p.mapKind === 'reg' ? { kind: 'reg', addr: p.mapAddr || '' } : { kind: 'var' },
          };
          if (p.dir === 'write') o.on_change = p.on_change !== false;
          return o;
        }),
      })),
    };
    vscode.postMessage({ type: 'save', text: JSON.stringify(out, null, 2) + '\\n' });
  });
  renderSel(); renderDevForm(); renderPoints(); renderStatus();
  if (!KX.editable && KX.error) { el('err').textContent = KX.error; }
</script>
</body></html>`;
}
