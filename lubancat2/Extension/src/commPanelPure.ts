// commPanelPure.ts —— 「工具 → 通讯状态」面板的纯逻辑（D10，v0.8.7；不 import vscode）
//
// 数据：`conn` 订阅事件（~2Hz）——控制器 `PortManager` 进程级快照（kind/监听端口/对端 IP/连接态）
//   + `PORT_INFO` 脚本标签（用途/主从）+ EtherCAT 主站健康与**每个从站明细**（身份/AL/在线/轴）。
//
// 本模块只做：事件 → 行视图（文案取自 strings.ts，NFR-8）+ 骨架 HTML。
// 纪律：没数据 → 明示「等待连接数据」，不补 0 冒充（NFR-6）；缺 d10 → 面板置灰由扩展侧处理。

import { KxStatusStore } from './store';
import { S } from './strings';

/** 对外连接行（视图） */
export interface CommRowView {
    id: string;
    kindText: string;     // 连接方式（含用途标签）
    roleText: string;     // 服务端 / 客户端
    masterText: string;   // 主站 / 从站 / —
    peerText: string;     // 对方 IP
    portText: string;     // 端口
    stateText: string;    // 已连接 / 监听中 / 连接中
    stateOk: boolean;
}

/** EtherCAT 从站行（视图） */
export interface CommSlaveView {
    id: string;
    indexText: string;
    identityText: string;
    nameText: string;
    axisText: string;
    alText: string;
    onlineText: string;
    onlineOk: boolean;
}

export interface CommPanelSpec {
    connected: boolean;
    loaded: boolean;
    rows: CommRowView[];
    busText: string;
    busOk: boolean;
    slaves: CommSlaveView[];
}

/** EtherCAT AL 状态码 → 文本（1=INIT 2=PREOP 3=BOOT 4=SAFEOP 8=OP） */
export function alStateText(n: number): string {
    switch (n) {
        case 0:
            return '—';
        case 1:
            return 'INIT';
        case 2:
            return 'PREOP';
        case 3:
            return 'BOOT';
        case 4:
            return 'SAFEOP';
        case 8:
            return 'OP';
        default:
            return `AL ${n}`;
    }
}

/** 十六进制（身份显示用；0 显示为 0x0） */
function hex(n: number): string {
    return '0x' + (Number.isFinite(n) ? n.toString(16) : '0');
}

/** store 快照 + 连接态 → 渲染数据（纯函数，可单测） */
export function buildCommPanelSpec(
    store: KxStatusStore,
    extras: { connected: boolean },
): CommPanelSpec {
    const rows: CommRowView[] = store.conns.map((c) => {
        const isServer = c.kind === 'TCP_SERVER';
        const tag = c.tag.length > 0 ? `（${c.tag}）` : '';
        return {
            id: `p${c.port}-${c.peer}:${c.peerPort}`,   // 多客户端：同端口多行，需唯一 id（2026-09-28）
            kindText: (isServer ? S.commPanel.kindServer : S.commPanel.kindClient) + tag,
            roleText: isServer ? S.commPanel.roleServer : S.commPanel.roleClient,
            masterText: c.role.length > 0 ? c.role : S.commPanel.masterNone,
            peerText: c.peer.length > 0 ? c.peer : S.commPanel.noneYet,
            portText: String(isServer ? (c.listen > 0 ? c.listen : S.commPanel.noneYet) : (c.peerPort > 0 ? c.peerPort : S.commPanel.noneYet)),
            stateText: c.conn
                ? S.commPanel.stateConnected
                : isServer
                  ? S.commPanel.stateListening
                  : S.commPanel.stateConnecting,
            stateOk: c.conn,
        };
    });

    const h = store.busHealth;
    const busText = h
        ? S.commPanel.busSummary(h.linkUp ? 'up' : 'down', `AL${h.slaveAl}`, h.slavesResponding)
        : '';
    const busOk = h ? h.linkUp && h.slaveOp : false;

    const slaves: CommSlaveView[] = store.busSlaves.map((s) => ({
        id: `s${s.index}`,
        indexText: String(s.index),
        identityText: `${hex(s.vid)}:${hex(s.pid)} r${s.rev}`,
        nameText: s.name.length > 0 ? s.name : S.commPanel.noneYet,
        axisText: s.axis >= 0 ? `轴${s.axis}` : S.commPanel.noneYet,
        alText: alStateText(s.alState),
        onlineText: s.online ? S.commPanel.stateConnected : S.commPanel.stateDisconnected,
        onlineOk: s.online,
    }));

    return {
        connected: extras.connected,
        loaded: store.connsLoaded,
        rows,
        busText,
        busOk,
        slaves,
    };
}

/** 骨架 HTML（数据经 postMessage({type:'render', spec}) 推送；两张表：TCP 连接 + EtherCAT 明细） */
export function commPanelHtml(nonce: string): string {
    const csp = `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';`;
    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="${csp}" />
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 12px 14px 20px;
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    color: var(--vscode-foreground);
    background: transparent;
  }
  .statusline { font-size: 12px; color: var(--vscode-descriptionForeground); padding: 0 0 8px; }
  h3 {
    margin: 14px 0 6px; font-size: 11px; font-weight: 600;
    text-transform: uppercase; letter-spacing: 0.04em;
    color: var(--vscode-descriptionForeground);
  }
  table { border-collapse: collapse; width: 100%; }
  th, td {
    text-align: left; font-size: 11px; padding: 3px 8px 3px 0;
    border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.2));
    font-variant-numeric: tabular-nums;
  }
  th { color: var(--vscode-descriptionForeground); font-weight: 600; }
  td.ok { color: var(--vscode-charts-green, #89d185); }
  td.bad { color: var(--vscode-descriptionForeground); }
  .empty { color: var(--vscode-descriptionForeground); font-size: 11px; padding: 4px 0; }
  .dim { color: var(--vscode-descriptionForeground); }
  .busline { font-size: 11px; padding: 2px 0 6px; }
</style>
</head>
<body>
  <div class="statusline" id="statusline"></div>
  <h3>${S.commPanel.headConns}</h3>
  <div id="connBox"></div>

  <h3>${S.commPanel.headBus}</h3>
  <div class="busline" id="busline"></div>
  <div id="slaveBox"></div>
  <div class="dim" id="upd"></div>

  <script nonce="${nonce}">
    // 增量渲染：行集合（id 列表）不变时只更新单元格文本，避免整表重建闪烁。
    const vscode = acquireVsCodeApi();
    const $ = (id) => document.getElementById(id);
    const statusEl = $('statusline');
    const connBox = $('connBox');
    const buslineEl = $('busline');
    const slaveBox = $('slaveBox');
    const updEl = $('upd');

    const updPrefix = ${JSON.stringify('最后更新 ')};
    let lastSpec = null;
    let connCells = [];   // [{id, cells: [td,…]}]
    let slaveCells = [];
    const setText = (el, t) => { if (el.textContent !== t) { el.textContent = t; } };
    const pad = (x, n) => String(x).padStart(n, '0');

    function table(headers) {
      const t = document.createElement('table');
      const tr = document.createElement('tr');
      for (const h of headers) {
        const th = document.createElement('th');
        th.textContent = h;
        tr.appendChild(th);
      }
      t.appendChild(tr);
      return t;
    }

    function ensureConns(rows) {
      const key = rows.map((r) => r.id).join(',');
      if (connBox.dataset.key === key) { return; }
      connBox.dataset.key = key;
      connBox.innerHTML = '';
      connCells = [];
      if (rows.length === 0) {
        const e = document.createElement('div');
        e.className = 'empty';
        e.textContent = '${S.commPanel.waitData}';
        connBox.appendChild(e);
        return;
      }
      const t = table(['${S.commPanel.colKind}', '${S.commPanel.colRole}', '${S.commPanel.colMaster}',
                       '${S.commPanel.colPeer}', '${S.commPanel.colPort}', '${S.commPanel.colState}']);
      for (const r of rows) {
        const tr = document.createElement('tr');
        const cells = [];
        for (let i = 0; i < 6; i++) {
          const td = document.createElement('td');
          tr.appendChild(td);
          cells.push(td);
        }
        t.appendChild(tr);
        connCells.push({ id: r.id, cells });
      }
      connBox.appendChild(t);
    }

    function ensureSlaves(rows) {
      const key = rows.map((r) => r.id).join(',');
      if (slaveBox.dataset.key === key) { return; }
      slaveBox.dataset.key = key;
      slaveBox.innerHTML = '';
      slaveCells = [];
      if (rows.length === 0) {
        const e = document.createElement('div');
        e.className = 'empty';
        e.textContent = '${S.commPanel.waitData}';
        slaveBox.appendChild(e);
        return;
      }
      const t = table(['${S.commPanel.colSlave}', '${S.commPanel.colIdentity}', '${S.commPanel.colName}',
                       '${S.commPanel.colAxis}', '${S.commPanel.colAl}', '${S.commPanel.colOnline}']);
      for (const r of rows) {
        const tr = document.createElement('tr');
        const cells = [];
        for (let i = 0; i < 6; i++) {
          const td = document.createElement('td');
          tr.appendChild(td);
          cells.push(td);
        }
        t.appendChild(tr);
        slaveCells.push({ id: r.id, cells });
      }
      slaveBox.appendChild(t);
    }

    function applySpec(spec) {
      setText(statusEl, spec.connected ? spec.busText : '${S.commPanel.offline}');
      setText(buslineEl, spec.busText);
      buslineEl.className = 'busline ' + (spec.busOk ? 'ok' : 'dim');

      ensureConns(spec.rows);
      for (const r of spec.rows) {
        const slot = connCells.find((c) => c.id === r.id);
        if (!slot) { continue; }
        setText(slot.cells[0], r.kindText);
        setText(slot.cells[1], r.roleText);
        setText(slot.cells[2], r.masterText);
        setText(slot.cells[3], r.peerText);
        setText(slot.cells[4], r.portText);
        setText(slot.cells[5], r.stateText);
        slot.cells[5].className = r.stateOk ? 'ok' : 'bad';
      }

      ensureSlaves(spec.slaves);
      for (const r of spec.slaves) {
        const slot = slaveCells.find((c) => c.id === r.id);
        if (!slot) { continue; }
        setText(slot.cells[0], r.indexText);
        setText(slot.cells[1], r.identityText);
        setText(slot.cells[2], r.nameText);
        setText(slot.cells[3], r.axisText);
        setText(slot.cells[4], r.alText);
        setText(slot.cells[5], r.onlineText);
        slot.cells[5].className = r.onlineOk ? 'ok' : 'bad';
      }

      const d = new Date();
      setText(updEl, updPrefix + pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ':' +
        pad(d.getSeconds(), 2));
    }

    window.addEventListener('message', (ev) => {
      const m = ev.data;
      if (!m || m.type !== 'render') { return; }
      lastSpec = m.spec;
      applySpec(lastSpec);
    });
    vscode.postMessage({ type: 'ready' });
  </script>
</body>
</html>`;
}
