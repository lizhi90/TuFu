// curvePure.ts —— 实时曲线的**纯逻辑**部分（docs/planA/15 T-21 / FR-6.6）
//
// 拆两个东西出来，都不依赖 vscode，可在 Node 下直接单测：
//   * `CurveRing`：定长环形缓冲，按「多条序列共享时间轴」组织采样；
//   * `curveHtml()`：Webview 文档（自包含 HTML + Canvas 绘制脚本，不加载任何外部资源）。
//
// 纪律（承 §4.4 / FR-6.5）：
//   * **曲线是订阅驱动的**，不轮询；缺样点即断线（不插值伪造）；
//   * 高频采样（>订阅 hz）需 `13` §9 另开通道，本版按订阅速率绘制（D-06「首版不做高频」）。

import { S } from './strings';

/** 一条曲线（对应轴状态里的一个字段） */
export interface CurveSeries {
    /** 数据键，与事件字段同名（如 `mpos` / `dpos`） */
    key: string;
    label: string;
    color: string;
}

/**
 * 定长环形缓冲：时间轴 + 每条序列一列。
 * 未在本帧出现的序列补 `null`（**断线**，不是 0）。
 */
export class CurveRing {
    private readonly times: number[] = [];
    private readonly cols = new Map<string, Array<number | null>>();

    constructor(readonly cap: number) {
        if (!Number.isInteger(cap) || cap <= 0) {
            throw new Error(S.curvePure.capError(String(cap)));
        }
    }

    get size(): number {
        return this.times.length;
    }

    /** 追加一帧：`values` 中缺失或非有限数的键记为 `null` */
    push(t: number, values: Record<string, number | undefined>): void {
        this.times.push(t);
        for (const [k, v] of Object.entries(values)) {
            if (typeof v === 'number' && Number.isFinite(v)) {
                this.col(k).push(v);
            }
        }
        // 未在本帧出现（或值非法）的序列补齐 `null` → 曲线断开，不插值、不补 0
        for (const arr of this.cols.values()) {
            while (arr.length < this.times.length) {
                arr.push(null);
            }
        }
        this.trim();
    }

    /** 取某条序列的快照（与 `times` 等长；未知键返回 `undefined`） */
    series(key: string): Array<number | null> | undefined {
        return this.cols.get(key);
    }

    timesSnapshot(): number[] {
        return [...this.times];
    }

    clear(): void {
        this.times.length = 0;
        for (const arr of this.cols.values()) {
            arr.length = 0;
        }
    }

    private col(key: string): Array<number | null> {
        let arr = this.cols.get(key);
        if (!arr) {
            arr = [];
            // 新列补齐历史（前面没这个字段 → 断线）
            for (let i = 0; i < this.times.length - 1; i++) {
                arr.push(null);
            }
            this.cols.set(key, arr);
        }
        return arr;
    }

    private trim(): void {
        while (this.times.length > this.cap) {
            this.times.shift();
            for (const arr of this.cols.values()) {
                arr.shift();
            }
        }
    }
}

export interface CurveHtmlOptions {
    title: string;
    series: CurveSeries[];
    /** 保留的样点数（滚动窗口宽度） */
    maxPoints: number;
}

/**
 * 生成 Webview 文档。自包含（`enableScripts: true`，无外部资源），
 * 收 `{type:'config'}` / `{type:'data'}` 两类消息，回 `{type:'ready'}` / `{type:'axis'}`。
 */
export function curveHtml(opts: CurveHtmlOptions): string {
    const config = JSON.stringify({
        title: opts.title,
        series: opts.series,
        maxPoints: opts.maxPoints,
    });
    // 说明：内嵌脚本刻意**不用模板字符串**，避免与外层 TS 模板冲突。
    // Webview 内文案随页面一起注入（保持「文案集中」，同时让内嵌脚本保持纯 JS）
    const labels = {
        axisPrefix: S.curvePure.axisPrefix,
        samplePrefix: S.curvePure.samplePrefix,
        waiting: S.curvePure.waiting,
        exportCsv: S.curvePure.exportCsv,
        clear: S.curvePure.clear,
    };
    const script = `
const CONFIG = ${config};
const L = ${JSON.stringify(labels)};
const state = { series: CONFIG.series, maxPoints: CONFIG.maxPoints, points: [], axes: [0], axis: 0, hidden: {} };
// 默认只显示两条位置曲线；推算通道（跟随误差/速度）量级差异大，点击图例再显示
state.hidden = { followerr: true, speed: true };
const canvas = document.getElementById('chart');
const ctx = canvas.getContext('2d');
const legend = document.getElementById('legend');
const axisSel = document.getElementById('axis');
const statusEl = document.getElementById('status');

function resize() {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || 600;
  const h = canvas.clientHeight || 300;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  draw();
}

function renderLegend() {
  legend.innerHTML = '';
  for (const s of state.series) {
    const span = document.createElement('span');
    span.className = 'item' + (state.hidden[s.key] ? ' off' : '');
    span.title = '点击显示/隐藏（导出 CSV 不受影响）';
    span.innerHTML = '<i style="background:' + s.color + '"></i>' + s.label;
    span.addEventListener('click', function () {
      state.hidden[s.key] = !state.hidden[s.key];
      renderLegend();
      draw();
    });
    legend.appendChild(span);
  }
}

function renderAxes() {
  axisSel.innerHTML = '';
  for (const a of state.axes) {
    const o = document.createElement('option');
    o.value = String(a);
    o.textContent = L.axisPrefix + a;
    if (a === state.axis) o.selected = true;
    axisSel.appendChild(o);
  }
}

function draw() {
  const w = canvas.clientWidth || 600;
  const h = canvas.clientHeight || 300;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#1e1e1e';
  ctx.fillRect(0, 0, w, h);

  const pts = state.points;
  if (pts.length === 0) {
    ctx.fillStyle = '#888';
    ctx.font = '13px sans-serif';
    ctx.fillText(L.waiting, 12, 22);
    return;
  }

  let lo = Infinity, hi = -Infinity, t0 = Infinity, t1 = -Infinity;
  for (const p of pts) {
    if (p.t < t0) t0 = p.t;
    if (p.t > t1) t1 = p.t;
    for (const v of p.v) {
      if (v !== null && v !== undefined) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
  }
  if (!isFinite(lo) || !isFinite(hi)) { lo = 0; hi = 1; }
  if (hi - lo < 1e-9) { hi = lo + 1; }
  const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
  const spanT = (t1 - t0) > 0 ? (t1 - t0) : 1;

  const X = (t) => 44 + ((t - t0) / spanT) * (w - 56);
  const Y = (v) => 14 + (1 - (v - lo) / (hi - lo)) * (h - 34);

  // 网格 + Y 轴刻度
  ctx.strokeStyle = '#333';
  ctx.fillStyle = '#777';
  ctx.font = '11px monospace';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = 14 + (i / 4) * (h - 34);
    ctx.beginPath(); ctx.moveTo(44, y); ctx.lineTo(w - 12, y); ctx.stroke();
    const v = hi - (i / 4) * (hi - lo);
    ctx.fillText(v.toFixed(2), 4, y + 4);
  }

  for (let si = 0; si < state.series.length; si++) {
    const s = state.series[si];
    if (state.hidden[s.key]) { continue; }
    ctx.strokeStyle = s.color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    let pen = false;
    for (const p of pts) {
      const v = p.v[si];
      if (v === null || v === undefined) { pen = false; continue; }
      const x = X(p.t), y = Y(v);
      if (pen) ctx.lineTo(x, y); else { ctx.moveTo(x, y); pen = true; }
    }
    ctx.stroke();
  }
  statusEl.textContent = L.samplePrefix + pts.length + ' / ' + state.maxPoints;
}

function pushData(t, values) {
  const v = state.series.map(function (s) {
    const x = values[s.key];
    return (typeof x === 'number' && isFinite(x)) ? x : null;
  });
  state.points.push({ t: t, v: v });
  while (state.points.length > state.maxPoints) state.points.shift();
  draw();
}

window.addEventListener('message', function (e) {
  const m = e.data;
  if (m.type === 'config') {
    state.series = m.series;
    state.maxPoints = m.maxPoints;
    state.axes = m.axes;
    state.axis = m.axis;
    state.points = [];
    renderLegend(); renderAxes(); draw();
  } else if (m.type === 'snapshot') {
    state.points = m.points || [];
    draw();
  } else if (m.type === 'data') {
    pushData(m.t, m.values);
  } else if (m.type === 'reset') {
    state.points = [];
    draw();
  }
});

axisSel.addEventListener('change', function () {
  const a = Number(axisSel.value);
  state.axis = a;
  state.points = [];
  draw();
  vscode.postMessage({ type: 'axis', axis: a });
});

window.addEventListener('resize', resize);
document.getElementById('export').textContent = L.exportCsv;
document.getElementById('clear').textContent = L.clear;
document.getElementById('export').addEventListener('click', function () {
  vscode.postMessage({ type: 'export' });
});
document.getElementById('clear').addEventListener('click', function () {
  vscode.postMessage({ type: 'clear' });
});
renderLegend();
resize();
vscode.postMessage({ type: 'ready' });
`;

    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';" />
<title>${escapeHtml(opts.title)}</title>
<style>
  body { margin: 0; background: #1e1e1e; color: #ccc; font-family: system-ui, sans-serif; }
  header { display: flex; align-items: center; gap: 12px; padding: 8px 12px; border-bottom: 1px solid #333; }
  header h1 { font-size: 13px; font-weight: 600; margin: 0; }
  header select { background: #2d2d2d; color: #ccc; border: 1px solid #444; border-radius: 3px; padding: 2px 4px; }
  header button {
    background: #2d2d2d; color: #ccc; border: 1px solid #444; border-radius: 3px;
    padding: 2px 8px; font-size: 12px; cursor: pointer;
  }
  header button:hover { background: #3a3a3a; }
  #legend { display: flex; gap: 14px; flex-wrap: wrap; font-size: 12px; }
  #legend .item { display: inline-flex; align-items: center; gap: 5px; cursor: pointer; }
  #legend .item.off { opacity: 0.45; }
  #legend i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; }
  #status { margin-left: auto; font-size: 11px; color: #777; }
  main { padding: 8px; }
  canvas { width: 100%; height: calc(100vh - 92px); display: block; }
</style>
</head>
<body>
<header>
  <h1>${escapeHtml(opts.title)}</h1>
  <label><select id="axis"></select></label>
  <span id="legend"></span>
  <button id="export"></button>
  <button id="clear"></button>
  <span id="status"></span>
</header>
<main><canvas id="chart"></canvas></main>
<script>
const vscode = acquireVsCodeApi();
${script}
</script>
</body>
</html>`;
}

/**
 * 曲线数据 → CSV 文本（v0.7.0，可导出；纯函数便于单测）。
 * 列：time_iso, t_ms, <series.key...>；缺失值写空单元格（不补 0）。
 */
export function curveToCsv(
    times: number[],
    series: CurveSeries[],
    cols: Array<Array<number | null | undefined> | undefined>,
): string {
    const head = ['time_iso', 't_ms'].concat(series.map((s) => s.key));
    const lines: string[] = [head.join(',')];
    for (let i = 0; i < times.length; i++) {
        const t = times[i] ?? 0;
        const row: string[] = [new Date(t).toISOString(), String(Math.round(t))];
        for (const col of cols) {
            const v = col ? col[i] : undefined;
            row.push(typeof v === 'number' && Number.isFinite(v) ? String(v) : '');
        }
        lines.push(row.join(','));
    }
    return lines.join('\n') + '\n';
}

function escapeHtml(s: string): string {
    return s.replace(/[&<>"']/g, (c) => {
        switch (c) {
            case '&':
                return '&amp;';
            case '<':
                return '&lt;';
            case '>':
                return '&gt;';
            case '"':
                return '&quot;';
            default:
                return '&#39;';
        }
    });
}
