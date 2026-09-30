// modbusPanelPure.ts —— 「Modbus 寄存器」右侧面板的纯逻辑（v0.4.3 自轴状态面板拆出）
//
// 用户拍板：Modbus 寄存器与轴状态**分开**——「工具 → 轴状态」（轴/总线）与
// 「工具 → Modbus」（4x 寄存器）两个独立面板，各自打开、各自驱动订阅（mb 主题）。
//
// 本文件不 import vscode：
//   * `buildModbusPanelSpec(store, extras)`：store 快照 → 渲染数据（可 Node 单测）；
//   * `modbusPanelHtml(nonce)`：骨架 HTML（数据由扩展经 postMessage 推送，前端 JS 渲染）。
//
// v0.8.6：「已用寄存器」监视——控制器 `mb` 事件带 `used` 位图（脚本访问过即标记），
//   已用单元格高亮；工具栏「仅显示已用」切换（过滤逻辑在本模块，前端只渲染）。
//
// 纪律不变：字段缺失即未知 → 显示「等待寄存器数据」，不补 0 冒充正常（NFR-6）；
// 寄存器分组 details 展开状态跨重绘保留。

import { KxStatusStore, MB_REG_COUNT } from './store';

/** 单个寄存器单元格（addr = 4x 地址；used = 控制器脚本访问过） */
export interface ModbusRegCell {
    addr: number;
    value: number;
    used: boolean;
}

/** 面板渲染数据（postMessage 的 payload 主体） */
export interface ModbusPanelSpec {
    connected: boolean;
    /** 订阅不可用（缺 D3）时的明示原因，空串 = 正常 */
    subDegradeReason: string;
    /** 「仅显示已用」当前模式（由面板按钮切换，扩展端持有状态） */
    usedOnly: boolean;
    regs: {
        loaded: boolean;
        /** 已用寄存器数量（0..256） */
        usedCount: number;
        /** 结构指纹：仅「仅显示已用」模式或其集合变化时变化 → 前端据此重建 DOM（展开态尽量保留） */
        shapeKey: string;
        groups: Array<{ start: number; cells: ModbusRegCell[] }>;
    };
}

/** 每组寄存器个数（16 组 × 16 个 = 256，FR-6.3） */
const REG_GROUP = 16;

/** store 快照 + 连接态 → 渲染数据（纯函数，可单测） */
export function buildModbusPanelSpec(
    store: KxStatusStore,
    extras: { connected: boolean; usedOnly?: boolean },
): ModbusPanelSpec {
    const usedOnly = extras.usedOnly === true;

    const usedAddrs: number[] = [];
    for (let i = 0; i < MB_REG_COUNT; i++) {
        if (store.regsUsed[i]) {
            usedAddrs.push(i);
        }
    }

    const groups: Array<{ start: number; cells: ModbusRegCell[] }> = [];
    if (store.regsLoaded) {
        for (let g = 0; g * REG_GROUP < MB_REG_COUNT; g++) {
            const start = g * REG_GROUP;
            const cells: ModbusRegCell[] = [];
            for (let i = 0; i < REG_GROUP; i++) {
                const addr = start + i;
                const used = store.regsUsed[addr] === true;
                if (usedOnly && !used) {
                    continue;
                }
                cells.push({ addr, value: store.regs[addr], used });
            }
            if (usedOnly && cells.length === 0) {
                continue;   // 仅已用模式：空组不展示
            }
            groups.push({ start, cells });
        }
    }

    return {
        connected: extras.connected,
        subDegradeReason: store.subUnavailable ? store.subDegradeReason : '',
        usedOnly,
        regs: {
            loaded: store.regsLoaded,
            usedCount: usedAddrs.length,
            // 全量模式结构恒定（"a"），仅已用模式随已用集合变化
            shapeKey: usedOnly ? 'u:' + usedAddrs.join(',') : 'a',
            groups,
        },
    };
}

/**
 * 「Modbus 寄存器」面板骨架 HTML。数据由扩展端 `postMessage({type:'render', spec})` 推送。
 *
 * 寄存器分 16 组折叠（details）：首帧默认展开第 0 组，用户展开过的组跨重绘保留；
 * 结构指纹（shapeKey）变化时才重建 DOM；「已用」单元格高亮（左侧色条），
 * 「仅显示已用」按钮经 `toggleUsedOnly` 交扩展端重建 spec（过滤逻辑在 buildModbusPanelSpec）。
 */
export function modbusPanelHtml(nonce: string): string {
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
  details { margin: 2px 0; }
  summary {
    cursor: pointer;
    font-size: 11px;
    color: var(--vscode-descriptionForeground);
    padding: 2px 0;
  }
  .regs { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1px; }
  .reg {
    padding: 2px 5px;
    border: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.2));
    font-size: 11px;
    font-variant-numeric: tabular-nums;
  }
  .reg.used {
    border-left: 3px solid var(--vscode-charts-blue, #3794ff);
  }
  .reg b { font-weight: 400; color: var(--vscode-descriptionForeground); margin-right: 5px; }
  .empty { color: var(--vscode-descriptionForeground); font-size: 11px; padding: 4px 0; }
  .toolbar { display: flex; align-items: center; gap: 8px; padding: 0 0 6px; }
  .toolbar .spacer { flex: 1; }
  .toolbar button {
    font-size: 11px; padding: 1px 8px; border: none; border-radius: 2px; cursor: pointer;
    color: var(--vscode-button-foreground);
    background: var(--vscode-button-background);
  }
  .toolbar button:hover { background: var(--vscode-button-hoverBackground); }
  .dim { color: var(--vscode-descriptionForeground); }
  .hint { font-size: 11px; color: var(--vscode-descriptionForeground); padding: 4px 0; display: none; }
</style>
</head>
<body>
  <div class="toolbar">
    <span id="statusline" class="statusline"></span>
    <span class="spacer"></span>
    <span id="upd" class="dim"></span>
    <button id="usedonly" title="只显示控制器脚本访问过的寄存器">仅显示已用</button>
    <button id="pause" title="暂停/继续界面刷新（数据仍在后台更新）">暂停刷新</button>
  </div>
  <div id="degraded" class="degraded" style="display:none"></div>

  <h3>Modbus 寄存器（4x）</h3>
  <div id="groups"></div>
  <div id="noused" class="hint">尚未检测到已用寄存器（控制器脚本运行并访问寄存器后出现）。</div>

  <script nonce="${nonce}">
    // v0.6.4：与轴面板同款刷新策略——**增量渲染**：分组与值单元格只在结构变化时重建，
    // 之后每帧仅更新变化的数值（展开状态天然保留，不再"重建后恢复"）；
    // 另提供「暂停刷新」与最后更新时间。
    // v0.8.6：结构指纹（shapeKey）——「仅显示已用」或已用集合变化才重建；
    //         已用单元格随帧切换 class="used"（高亮），无已用数据时明示提示。
    const vscode = acquireVsCodeApi();
    const $ = (id) => document.getElementById(id);
    const statusEl = $('statusline');
    const degradedEl = $('degraded');
    const updEl = $('upd');
    const pauseBtn = $('pause');
    const usedBtn = $('usedonly');
    const groupsBox = $('groups');
    const nousedEl = $('noused');

    let paused = false;
    let lastSpec = null;
    let cells = [];        // 扁平化的值单元格（组顺序 × 组内顺序 = 地址顺序）
    let cellUsed = [];     // 与 cells 平行：各单元格当前是否带 used class
    let shapeKey = null;

    const hex4 = (n) => '0x' + n.toString(16).toUpperCase().padStart(4, '0');
    const setText = (el, txt) => { if (el.textContent !== txt) { el.textContent = txt; } };
    const pad = (x, n) => String(x).padStart(n, '0');

    function ensureGroups(spec) {
      const groups = spec.regs.groups;
      const key = spec.regs.shapeKey + '|' + (spec.regs.loaded ? '1' : '0');
      if (shapeKey === key) { return; }
      shapeKey = key;
      groupsBox.innerHTML = '';
      cells = [];
      cellUsed = [];
      if (groups.length === 0) {
        const e = document.createElement('div');
        e.className = 'empty';
        e.textContent = '等待寄存器数据';
        groupsBox.appendChild(e);
        return;
      }
      for (const g of groups) {
        const det = document.createElement('details');
        if (g.start === 0) { det.open = true; }          // 首帧默认展开第 0 组；之后由用户控制
        const usedInGroup = g.cells.filter((c) => c.used).length;
        const sum = document.createElement('summary');
        sum.textContent = hex4(g.start) + ' – ' + hex4(g.start + 15) +
          (usedInGroup > 0 ? '（已用 ' + usedInGroup + '）' : '');
        det.appendChild(sum);
        const grid = document.createElement('div');
        grid.className = 'regs';
        for (const c of g.cells) {
          const cell = document.createElement('div');
          cell.className = c.used ? 'reg used' : 'reg';
          const b = document.createElement('b');
          b.textContent = hex4(c.addr);
          const v = document.createElement('span');
          cell.appendChild(b);
          cell.appendChild(v);
          grid.appendChild(cell);
          cells.push(v);
          cellUsed.push(c.used);
        }
        det.appendChild(grid);
        groupsBox.appendChild(det);
      }
    }

    function applySpec(spec) {
      const usedTxt = '已用 ' + spec.regs.usedCount + '/' + 256;
      setText(statusEl, spec.connected
        ? '数据由事件推送自动刷新 · ' + usedTxt
        : '未连接控制器 · ' + usedTxt);
      setText(usedBtn, spec.usedOnly ? '显示全部' : '仅显示已用');
      usedBtn.title = spec.usedOnly
        ? '切回显示全部 256 个寄存器'
        : '只显示控制器脚本访问过的寄存器';
      if (spec.subDegradeReason) {
        setText(degradedEl, '订阅不可用：' + spec.subDegradeReason);
        degradedEl.style.display = '';
      } else {
        degradedEl.style.display = 'none';
      }
      nousedEl.style.display =
        spec.regs.loaded && !spec.usedOnly && spec.regs.usedCount === 0 ? '' : 'none';

      ensureGroups(spec);
      const groups = spec.regs.groups;
      let k = 0;
      for (const g of groups) {
        for (const c of g.cells) {
          const el = cells[k];
          if (el) {
            setText(el, String(c.value));
            if (cellUsed[k] !== c.used) {
              cellUsed[k] = c.used;
              el.parentElement.className = c.used ? 'reg used' : 'reg';
            }
          }
          k++;
        }
      }

      const d = new Date();
      setText(updEl, '最后更新 ' + pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ':' +
        pad(d.getSeconds(), 2) + '.' + pad(d.getMilliseconds(), 3));
    }

    pauseBtn.addEventListener('click', () => {
      paused = !paused;
      pauseBtn.textContent = paused ? '继续刷新' : '暂停刷新';
      if (!paused && lastSpec) {
        applySpec(lastSpec);
      } else if (paused) {
        setText(updEl, '已暂停（界面冻结在当前值，数据仍在后台更新）');
      }
    });

    usedBtn.addEventListener('click', () => {
      vscode.postMessage({ type: 'toggleUsedOnly' });
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
