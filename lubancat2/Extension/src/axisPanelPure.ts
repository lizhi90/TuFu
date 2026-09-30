// axisPanelPure.ts —— 「轴状态」右侧面板的纯逻辑（v0.4.2 自控制台迁出；v0.4.3 寄存器拆出）
//
// 背景（修「连接后菜单点不动」的根因）：连接后 axis/bus 事件最高 20Hz 推送，
// 此前状态表格与菜单栏同视图，每帧全量重建 DOM 会冲掉正在悬停/点击的菜单。
// 本面板在**编辑器区**独立承载 轴状态 / 总线 表格
// （菜单「工具 → 轴状态」打开，与「工具 → 曲线」同形态），控制台菜单栏因此稳定。
// v0.4.3：Modbus 寄存器拆至独立面板（modbusPanelPure.ts，「工具 → Modbus」）。
//
// 本文件不 import vscode：
//   * `buildAxisPanelSpec(store, extras)`：store 快照 → 渲染数据（可 Node 单测）；
//   * `axisPanelHtml(nonce)`：骨架 HTML（数据由扩展经 postMessage 推送，前端 JS 渲染）。
//
// 纪律不变：字段缺失即未知 → 显示「—」，不补 0 冒充正常（NFR-6 降级不伪装）；
// 订阅不可用（缺 D3）→ 明示原因，不静默空表。

import { KxStatusStore } from './store';
import { alStateName, decodeAxisStatus } from './statusBits';

/** 轴表格行（字段缺失 → undefined → 前端显示「—」） */
export interface AxisRow {
    index: number;
    busOk?: number;
    enabled?: number;
    idle?: number;
    alarm?: number;
    mpos?: number;
    dpos?: number;
    statusName: string;
    unknownBits: number[];
    errCode?: number;
    /** 当前生效的脉冲当量（inc/mm；0/undefined = 未设置，由脚本 UNITS 设置，v0.8.1） */
    incPerMm?: number;
}

/** 总线表格行 */
export interface BusRow {
    index: number;
    axisCount?: number;
    status?: number;
    alName: string;
}

/** 面板渲染数据（postMessage 的 payload 主体） */
export interface AxisPanelSpec {
    connected: boolean;
    /** 订阅不可用（缺 D3）时的明示原因，空串 = 正常 */
    subDegradeReason: string;
    axes: AxisRow[];
    bus: {
        nodeCount: number;
        rows: BusRow[];
    } | null;
}

/** store 快照 + 连接态 → 渲染数据（纯函数，可单测） */
export function buildAxisPanelSpec(
    store: KxStatusStore,
    extras: { connected: boolean },
): AxisPanelSpec {
    const ids = new Set<number>();
    for (let i = 0; i < store.axisCount; i++) {
        ids.add(i);
    }
    for (const k of store.axes.keys()) {
        ids.add(k);
    }

    const axes: AxisRow[] = [...ids]
        .filter((i) => i >= 0)
        .sort((a, b) => a - b)
        .map((i) => {
            const s = store.axes.get(i);
            if (!s) {
                return { index: i, statusName: '', unknownBits: [] };
            }
            const decoded = s.axis_status === undefined ? undefined : decodeAxisStatus(s.axis_status);
            return {
                index: i,
                busOk: s.bus_ok,
                enabled: s.enabled,
                idle: s.idle,
                alarm: s.alarm,
                mpos: s.mpos,
                dpos: s.dpos,
                statusName: decoded ? alStateName(s.axis_status!) : '',
                unknownBits: decoded ? decoded.unknownBits : [],
                errCode: s.err_code,
                incPerMm: s.inc_per_mm,
            };
        });

    const busRows: BusRow[] = (store.bus?.nodes ?? []).map((n) => ({
        index: n.index,
        axisCount: n.axis_count,
        status: n.status,
        alName: n.status === undefined ? '' : alStateName(n.status),
    }));

    return {
        connected: extras.connected,
        subDegradeReason: store.subUnavailable ? store.subDegradeReason : '',
        axes,
        bus: store.bus ? { nodeCount: store.bus.node_count, rows: busRows } : null,
    };
}

/**
 * 「轴状态」面板骨架 HTML。数据由扩展端 `postMessage({type:'render', spec})` 推送。
 *
 * 面板是编辑器区 Webview（宽度足够），表格直接铺开；数据按 80ms 节流推送。
 */
export function axisPanelHtml(nonce: string): string {
    const csp = `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';`;
    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="${csp}" />
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 12px 14px 20px;
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    color: var(--vscode-foreground);
    background: transparent;
  }
  .statusline { font-size: 12px; color: var(--vscode-descriptionForeground); padding: 0 0 8px; }
  .degraded {
    margin: 4px 0 10px;
    padding: 4px 8px;
    border-left: 2px solid var(--vscode-editorWarning-foreground, #cca700);
    color: var(--vscode-editorWarning-foreground, #cca700);
    font-size: 11px;
  }
  h3 {
    margin: 14px 0 4px;
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--vscode-descriptionForeground);
  }
  table { width: 100%; border-collapse: collapse; }
  th, td {
    border: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.25));
    padding: 3px 7px;
    text-align: left;
    font-size: 11px;
    white-space: nowrap;
  }
  th { color: var(--vscode-descriptionForeground); font-weight: 600; background: rgba(128,128,128,0.08); }
  td.num { text-align: right; font-variant-numeric: tabular-nums; }
  .ok   { color: var(--vscode-testing-iconPassed, #73c991); }
  .bad  { color: var(--vscode-testing-iconFailed, #f14c4c); }
  .dim  { color: var(--vscode-descriptionForeground); }
  .empty { color: var(--vscode-descriptionForeground); font-size: 11px; padding: 4px 0; }
  .toolbar { display: flex; align-items: center; gap: 8px; padding: 0 0 6px; }
  .toolbar .spacer { flex: 1; }
  .toolbar button {
    font-size: 11px; padding: 1px 8px; border: none; border-radius: 2px; cursor: pointer;
    color: var(--vscode-button-foreground);
    background: var(--vscode-button-background);
  }
  .toolbar button:hover { background: var(--vscode-button-hoverBackground); }
  .ctl {
    display: flex; flex-wrap: wrap; align-items: center; gap: 6px;
    padding: 6px 8px; margin: 0 0 6px;
    border: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.25));
    border-radius: 3px;
  }
  .ctl button {
    font-size: 11px; padding: 1px 8px; border: none; border-radius: 2px; cursor: pointer;
    color: var(--vscode-button-foreground);
    background: var(--vscode-button-background);
  }
  .ctl button:hover { background: var(--vscode-button-hoverBackground); }
  .ctl button:disabled { opacity: 0.5; cursor: default; }
  .ctl button.danger { background: var(--vscode-inputValidation-errorBorder, #be1100); }
  .ctl input, .ctl select {
    font-size: 11px; padding: 1px 4px;
    color: var(--vscode-input-foreground);
    background: var(--vscode-input-background);
    border: 1px solid var(--vscode-input-border, rgba(128,128,128,0.4));
    border-radius: 2px;
  }
  .ctl input[type=number] { width: 90px; text-align: right; }
</style>
</head>
<body>
  <div class="toolbar">
    <span id="statusline" class="statusline"></span>
    <span class="spacer"></span>
    <span id="upd" class="dim"></span>
    <button id="pause" title="暂停/继续界面刷新（数据仍在后台更新）">暂停刷新</button>
  </div>
  <div id="degraded" class="degraded" style="display:none"></div>

  <!-- v0.7.0 在线调试（对标 ZDevelop/TwinCAT 的手动调试；经 D1 cmd 下发，移动为异步 wait=0） -->
  <div class="ctl">
    <span class="dim">轴</span><select id="axis"></select>
    <button id="en">使能</button>
    <button id="dis">去使能</button>
    <button id="stop" class="danger">停止</button>
    <button id="jogn" title="按住负向点动（松开停止）">◀ 点动</button>
    <button id="jogp" title="按住正向点动（松开停止）">点动 ▶</button>
    <input id="target" type="number" step="0.001" placeholder="目标 mm" />
    <select id="mode"><option value="abs">绝对</option><option value="rel">相对</option></select>
    <input id="speed" type="number" step="1" value="50" title="速度 mm/s" />
    <button id="go">执行</button>
    <span class="dim" id="scaleNow" title="当前生效的脉冲当量（inc/mm，由脚本 UNITS 设置）">当量 —</span>
    <input id="scale" type="number" step="0.01" min="0" placeholder="脉冲当量 inc/mm"
           title="设置脉冲当量 UNITS（按伺服+机械换算；运动中不可改）" />
    <button id="applyScale">应用当量</button>
  </div>

  <h3>轴状态</h3>
  <div id="axes"></div>

  <h3>总线</h3>
  <div id="bus"></div>

  <script nonce="${nonce}">
    // v0.6.2：**增量渲染**——表格只建一次，之后仅更新变化的单元格
    // （旧实现每帧 content.innerHTML='' 全量重建：20Hz 下闪烁、选中被打断、节奏不稳）。
    // 另提供「暂停刷新」：界面冻结在当前值，便于读数/复制；数据仍在后台更新。
    const vscode = acquireVsCodeApi();
    const $ = (id) => document.getElementById(id);
    const statusEl = $('statusline');
    const degradedEl = $('degraded');
    const updEl = $('upd');
    const pauseBtn = $('pause');
    const axesBox = $('axes');
    const busBox = $('bus');
    const axisSel = $('axis');
    const ctlBtns = ['en', 'dis', 'stop', 'jogn', 'jogp', 'go', 'applyScale'].map((id) => $(id));
    let jogging = false;

    let paused = false;
    let lastSpec = null;
    let axisCells = [];
    let busCells = [];

    const setText = (el, txt) => { if (el.textContent !== txt) el.textContent = txt; };
    const setState = (el, v) => {
      const t = v === undefined ? '—' : (v ? '✓' : '✗');
      if (el.textContent !== t) el.textContent = t;
      const cls = v === undefined ? 'dim' : (v ? 'ok' : 'bad');
      if (el.className !== cls) el.className = cls;
    };
    const pos4 = (v) => v === undefined ? '—' : Number(v).toFixed(4) + ' mm';   // 指令/反馈位置：均 4 位小数
    const cell = (cls) => { const td = document.createElement('td'); if (cls) td.className = cls; return td; };
    const pad = (x, n) => String(x).padStart(n, '0');

    function ensureAxisTable(axes) {
      if (axesBox.dataset.shape === String(axes.length)) { return; }
      axesBox.dataset.shape = String(axes.length);
      axesBox.innerHTML = '';
      axisCells = [];
      if (axes.length === 0) {
        const e = document.createElement('div');
        e.className = 'empty';
        e.textContent = '等待轴数据（连接后由事件推送，不轮询）';
        axesBox.appendChild(e);
        return;
      }
      const t = document.createElement('table');
      t.innerHTML = '<thead><tr><th>轴</th><th>总线</th><th>使能</th><th>空闲</th><th>报警</th>' +
        '<th>指令位置</th><th>反馈位置</th><th>AL 状态</th><th>错误码</th></tr></thead>';
      const tb = document.createElement('tbody');
      for (let i = 0; i < axes.length; i++) {
        const tr = document.createElement('tr');
        const c = { label: cell(), bus: cell('dim'), en: cell('dim'), idle: cell('dim'),
                    alarm: cell('dim'), dpos: cell('num'), mpos: cell('num'), st: cell(), err: cell('num') };
        Object.keys(c).forEach((k) => tr.appendChild(c[k]));
        tb.appendChild(tr);
        axisCells.push(c);
      }
      t.appendChild(tb);
      axesBox.appendChild(t);
    }

    function ensureBusTable(rows) {
      if (busBox.dataset.shape === String(rows.length)) { return; }
      busBox.dataset.shape = String(rows.length);
      busBox.innerHTML = '';
      busCells = [];
      if (rows.length === 0) {
        const e = document.createElement('div');
        e.className = 'empty';
        e.textContent = '等待总线数据';
        busBox.appendChild(e);
        return;
      }
      const t = document.createElement('table');
      t.innerHTML = '<thead><tr><th>从站</th><th>轴数</th><th>AL 状态</th></tr></thead>';
      const tb = document.createElement('tbody');
      for (let i = 0; i < rows.length; i++) {
        const tr = document.createElement('tr');
        const c = { node: cell(), cnt: cell('num'), al: cell() };
        Object.keys(c).forEach((k) => tr.appendChild(c[k]));
        tb.appendChild(tr);
        busCells.push(c);
      }
      t.appendChild(tb);
      busBox.appendChild(t);
    }

    function applySpec(spec) {
      setText(statusEl, spec.connected ? '数据由事件推送自动刷新（连接后无需手动刷新）' : '未连接控制器');
      if (spec.subDegradeReason) {
        setText(degradedEl, '订阅不可用：' + spec.subDegradeReason);
        degradedEl.style.display = '';
      } else {
        degradedEl.style.display = 'none';
      }

      // ---- 在线调试控件：轴下拉 + 连接态可用性 ----
      const ids = spec.axes.map((a) => String(a.index)).join(',');
      if (axisSel.dataset.ids !== ids) {
        axisSel.dataset.ids = ids;
        const keep = axisSel.value;
        axisSel.innerHTML = '';
        for (const a of spec.axes) {
          const o = document.createElement('option');
          o.value = String(a.index);
          o.textContent = '轴 ' + a.index;
          axisSel.appendChild(o);
        }
        if (keep !== '' && ids.split(',').indexOf(keep) >= 0) { axisSel.value = keep; }
      }
      for (const b of ctlBtns) { b.disabled = !spec.connected; }
      {   // 当前脉冲当量（v0.8.1：由脚本 UNITS 设置，控制器无默认）
        const cur = spec.axes.find((a) => a.index === Number(axisSel.value))?.incPerMm;
        setText($('scaleNow'), cur && cur > 0 ? '当量 ' + Number(cur).toFixed(2) : '当量 未设置');
        if (cur && cur > 0 && $('scale').value === '') { $('scale').placeholder = String(cur); }
      }

      ensureAxisTable(spec.axes);
      spec.axes.forEach((a, i) => {
        const c = axisCells[i];
        if (!c) { return; }
        setText(c.label, '轴 ' + a.index);
        setState(c.bus, a.busOk);
        setState(c.en, a.enabled);
        setState(c.idle, a.idle === undefined ? undefined : (a.idle ? 1 : 0));
        if (a.alarm === undefined) {
          setText(c.alarm, '—');
          if (c.alarm.className !== 'dim') { c.alarm.className = 'dim'; }
        } else {
          setText(c.alarm, a.alarm ? '报警' : '正常');
          const cls = a.alarm ? 'bad' : 'ok';
          if (c.alarm.className !== cls) { c.alarm.className = cls; }
        }
        setText(c.dpos, pos4(a.dpos));       // 指令位置 = DPOS（4 位小数）
        setText(c.mpos, pos4(a.mpos));       // 反馈位置 = MPOS（4 位小数）
        setText(c.st, a.statusName
          ? a.statusName + (a.unknownBits.length ? '（位' + a.unknownBits.join(',') + '未知）' : '')
          : '—');
        const stCls = a.statusName ? '' : 'dim';
        if (c.st.className !== stCls) { c.st.className = stCls; }
        setText(c.err, a.errCode === undefined ? '—' : String(a.errCode));
      });

      const busRows = spec.bus ? spec.bus.rows : [];
      ensureBusTable(busRows);
      busRows.forEach((n, i) => {
        const c = busCells[i];
        if (!c) { return; }
        setText(c.node, '从站 ' + n.index);
        setText(c.cnt, n.axisCount === undefined ? '—' : String(n.axisCount));
        setText(c.al, n.alName || '—');
      });

      const d = new Date();
      setText(updEl, '最后更新 ' + pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ':' +
        pad(d.getSeconds(), 2) + '.' + pad(d.getMilliseconds(), 3));
    }

    // ---- 在线调试：经扩展转发为 D1 cmd（移动 wait=0 异步；点动按住有效、松开停止）----
    const ctl = (action, extra) => {
      const axis = Number(axisSel.value);
      const speed = Number($('speed').value);
      vscode.postMessage(Object.assign(
        { type: 'axisCtl', action, axis: Number.isFinite(axis) ? axis : 0, speed },
        extra || {},
      ));
    };
    $('en').addEventListener('click', () => ctl('enable'));
    $('dis').addEventListener('click', () => ctl('disable'));
    $('stop').addEventListener('click', () => ctl('stop'));
    $('go').addEventListener('click', () => {
      const t = Number($('target').value);
      if (!Number.isFinite(t)) { setText(updEl, '请先填写目标位置（mm）'); return; }
      ctl('move', { target: t, mode: $('mode').value });
    });
    $('applyScale').addEventListener('click', () => {
      const v = Number($('scale').value);
      if (!Number.isFinite(v) || v <= 0) { setText(updEl, '请填写 > 0 的脉冲当量（inc/mm）'); return; }
      ctl('setScale', { value: v });
    });
    let jogTick = null;
    const startJog = (dir) => {
      if (jogging) { return; }
      jogging = true;
      ctl('jog', { dir });
      // 心跳：扩展侧 1.5s 收不到 jog/jogTick 就自动 CANCEL（面板关闭/浏览器卡死时兜底停机）
      jogTick = setInterval(() => { if (jogging) { ctl('jogTick'); } }, 700);
    };
    const stopJog = () => {
      if (!jogging) { return; }
      jogging = false;
      if (jogTick) { clearInterval(jogTick); jogTick = null; }
      ctl('jogStop');
    };
    $('jogn').addEventListener('mousedown', (e) => { e.preventDefault(); startJog(-1); });
    $('jogp').addEventListener('mousedown', (e) => { e.preventDefault(); startJog(1); });
    ['mouseup', 'mouseleave'].forEach((ev) => {
      $('jogn').addEventListener(ev, stopJog);
      $('jogp').addEventListener(ev, stopJog);
    });
    $('jogn').addEventListener('touchstart', (e) => { e.preventDefault(); startJog(-1); }, { passive: false });
    $('jogp').addEventListener('touchstart', (e) => { e.preventDefault(); startJog(1); }, { passive: false });
    $('jogn').addEventListener('touchend', stopJog);
    $('jogp').addEventListener('touchend', stopJog);
    window.addEventListener('blur', stopJog);

    pauseBtn.addEventListener('click', () => {
      paused = !paused;
      pauseBtn.textContent = paused ? '继续刷新' : '暂停刷新';
      if (!paused && lastSpec) {
        applySpec(lastSpec);
      } else if (paused) {
        setText(updEl, '已暂停（界面冻结在当前值，数据仍在后台更新）');
      }
    });

    window.addEventListener('message', (ev) => {
      const m = ev.data;
      if (!m || m.type !== 'render') { return; }
      lastSpec = m.spec;
      if (!paused) { applySpec(lastSpec); }
    });
    vscode.postMessage({ type: 'ready' });
  </script>
</body>
</html>`;
}
