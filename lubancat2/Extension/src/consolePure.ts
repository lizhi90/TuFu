// consolePure.ts —— 「Kine-X 控制台」Webview 的纯逻辑
//
// v0.4.2 结构调整（修「连接后菜单点不动」的根因）：
//   连接后 axis/bus/mb 事件最高 20Hz 推送，此前菜单栏与状态表格同在一个视图里，
//   每帧全量重建 DOM 会把正在悬停/点击的菜单元素一并换掉——点击落空。
//   现在控制台**只保留菜单栏 + 状态行**（均为低频数据：连接态/引擎/脚本状态），
//   轴状态 / 总线 / 寄存器表格整体迁至右侧「轴状态」面板（axisPanelPure.ts /
//   axisPanel.ts，菜单「工具 → 轴状态」打开，与「工具 → 曲线」同形态）。
//   控制台只在连接态/引擎/脚本状态变化时重绘，菜单不再被高频刷新冲掉。
//
// 本文件不 import vscode：
//   * `buildConsoleSpec(store, extras)`：store 快照 + 连接态 → 渲染数据（可 Node 单测）；
//   * `consoleHtml(nonce)`：骨架 HTML（数据由扩展经 postMessage 推送，前端 JS 渲染）。
//
// 纪律（沿用既有约定）：
//   * 点击菜单只 postMessage，真正的命令执行走 `vscode.commands.executeCommand`。

import { S } from './strings';
import { KxStatusStore } from './store';
import { engineDisplay } from './engineRule';
import { toolbarMenus, ToolbarMenuGroup, ToolbarSpec } from './toolbarPure';

/** 控制台渲染数据（postMessage 的 payload 主体；v0.4.9 起文件列表迁至原生 TreeView） */
export interface ConsoleSpec {
    connected: boolean;
    target: string;
    engine: string;
    axisCount: number;
    scriptStatus: string;
    /** 本项目配置的脚本语言（kine-x.engine），菜单勾选显示用 */
    configEngine: string;
    /** 脚本引擎不可用（调试口未独占 / 缺 d2）时的提示，空串 = 可用 */
    scriptDisabledHint: string;
    menus: ToolbarMenuGroup[];
}

/** store 快照 + 连接态 → 渲染数据（纯函数，可单测） */
export function buildConsoleSpec(
    store: KxStatusStore,
    extras: {
        connected: boolean;
        target: string;
        scriptStatus: string;
        caps: ReadonlySet<string>;
        configEngine: string;
    },
): ConsoleSpec {
    const menuSpec: ToolbarSpec = {
        connected: extras.connected,
        scriptStatus: extras.scriptStatus,
        caps: extras.caps,
        target: extras.target,
        configEngine: extras.configEngine,
    };

    return {
        connected: extras.connected,
        target: extras.target,
        engine: store.engine ? engineDisplay(store.engine) : '',
        axisCount: store.axisCount,
        scriptStatus: extras.scriptStatus,
        configEngine: extras.configEngine,
        scriptDisabledHint:
            extras.connected && !extras.caps.has('d2') ? S.toolbar.disabledNeedEngine : '',
        menus: toolbarMenus(menuSpec),
    };
}

/**
 * 控制台骨架 HTML。数据由扩展端 `postMessage({type:'render', spec})` 推送，前端 JS 渲染。
 *
 * 样式全部取自 `--vscode-*` 主题变量；菜单栏是 sticky 顶行。
 * 「展开中丢帧」守卫（openEl）保留为兜底：虽然本视图已无高频数据，
 * 但连接态/脚本状态变化仍可能落在菜单展开期间，丢一帧无感知。
 */
export function consoleHtml(nonce: string): string {
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
    padding: 0;
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    color: var(--vscode-foreground);
    background: transparent;
  }
  /* ---- 菜单栏（sticky 顶行）---- */
  .menubar {
    position: sticky;
    top: 0;
    z-index: 10;
    display: flex;
    align-items: center;
    height: 26px;
    background: var(--vscode-menubar-background, var(--vscode-editor-background));
    border-bottom: 1px solid var(--vscode-menu-separatorBackground, rgba(128,128,128,0.3));
    user-select: none;
  }
  .menu {
    position: relative;
    display: inline-flex;
    align-items: center;
    height: 100%;
    padding: 0 9px;
    font-size: 12px;
    cursor: pointer;
    white-space: nowrap;
  }
  .menu:hover, .menu.open {
    background: var(--vscode-menubar-selectionBackground, rgba(128,128,128,0.25));
  }
  .menu .caret { margin-left: 4px; font-size: 8px; opacity: 0.7; }
  .dropdown {
    position: absolute;
    top: 26px;
    left: 0;
    min-width: 172px;
    padding: 3px 0;
    background: var(--vscode-menu-background, var(--vscode-editor-background));
    color: var(--vscode-menu-foreground, var(--vscode-foreground));
    border: 1px solid var(--vscode-menu-border, var(--vscode-menu-separatorBackground, rgba(128,128,128,0.4)));
    box-shadow: 0 3px 10px rgba(0,0,0,0.35);
  }
  .mi {
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 4px 12px;
    font-size: 12px;
    line-height: 18px;
    cursor: pointer;
    white-space: nowrap;
  }
  .mi svg { width: 14px; height: 14px; fill: currentColor; opacity: 0.9; }
  .mi:hover:not(.disabled) {
    background: var(--vscode-menu-selectionBackground);
    color: var(--vscode-menu-selectionForeground);
  }
  .mi.disabled { opacity: 0.4; cursor: default; }
  /* 子分组：标题行 + 平铺子项（**内联展开**，不用浮层）。
     浮层在 webview 视图里会被高度/宽度裁剪或 flip 推出可视区，故弃用。 */
  .mi.head {
    font-size: 10px;
    color: var(--vscode-descriptionForeground);
    padding: 5px 12px 2px;
    cursor: default;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .mi.child { padding-left: 26px; }
  .child-mark { display: inline-block; width: 16px; color: var(--vscode-testing-iconPassed, #73c991); }
  .conn {
    margin-left: auto;
    padding: 0 9px;
    font-size: 11px;
    color: var(--vscode-descriptionForeground);
    white-space: nowrap;
  }
  /* ---- 状态行与提示 ---- */
  .content { padding: 6px 8px 14px; }
  .statusline { font-size: 11px; color: var(--vscode-descriptionForeground); padding: 2px 0; }
  .hint {
    margin: 8px 0 0;
    padding: 5px 8px;
    border-left: 2px solid var(--vscode-charts-blue, #4fc9ff);
    color: var(--vscode-descriptionForeground);
    font-size: 11px;
    line-height: 1.5;
  }
  .hint b { font-weight: 600; color: var(--vscode-foreground); }
</style>
</head>
<body>
  <div class="menubar" id="bar" role="menubar"></div>
  <div class="content" id="content"></div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const bar = document.getElementById('bar');
    const content = document.getElementById('content');
    let openEl = null;
    let closeTimer = 0;

    function closeMenu() {
      if (closeTimer) { clearTimeout(closeTimer); closeTimer = 0; }
      if (openEl) {
        openEl.classList.remove('open');
        const d = openEl.querySelector('.dropdown');
        if (d) { d.remove(); }
        openEl = null;
      }
    }

    /** 延迟关闭：给「移向二级子菜单」留出缓冲，避免手一抖就没了 */
    function scheduleClose() {
      if (closeTimer) { clearTimeout(closeTimer); }
      closeTimer = setTimeout(() => { closeTimer = 0; closeMenu(); }, 600);
    }

    function buildMenus(menus) {
      bar.innerHTML = '';
      for (const g of menus) {
        const m = document.createElement('div');
        m.className = 'menu';
        m.innerHTML = '<span>' + g.label + '</span><span class="caret">\\u25BE</span>';
        m.addEventListener('click', (ev) => {
          ev.stopPropagation();
          if (openEl === m) { closeMenu(); return; }
          closeMenu();
          const dd = document.createElement('div');
          dd.className = 'dropdown';
          // 下拉自身 hover 时取消待关闭（鼠标在菜单内移动不应触发关闭）
          dd.addEventListener('mouseenter', () => { if (closeTimer) { clearTimeout(closeTimer); closeTimer = 0; } });
          dd.addEventListener('mouseleave', scheduleClose);
          for (const it of g.items) {
            const mi = document.createElement('div');
            if (it.kind === 'submenu') {
              // 子分组：**内联平铺**——标题行 + 子项直接排在下方。
              const head = document.createElement('div');
              head.className = 'mi head';
              head.textContent = it.label;
              dd.appendChild(head);
              for (const c of it.children) {
                const ci = document.createElement('div');
                ci.className = 'mi child' + (c.enabled ? '' : ' disabled');
                ci.title = c.tooltip;
                const mark = c.text.indexOf('✓') === 0
                  ? '<span class="child-mark">✓</span>'
                  : '<span class="child-mark"></span>';
                ci.innerHTML = mark + '<span>' + c.text.replace('✓ ', '') + '</span>';
                ci.addEventListener('click', (e2) => {
                  e2.stopPropagation();
                  if (!c.enabled) { return; }
                  closeMenu();
                  vscode.postMessage({ type: 'cmd', command: c.command });
                });
                dd.appendChild(ci);
              }
              continue;
            }
            mi.className = 'mi' + (it.enabled ? '' : ' disabled');
            mi.title = it.tooltip;
            mi.innerHTML = it.svg + '<span>' + it.text + '</span>';
            mi.addEventListener('click', (e2) => {
              e2.stopPropagation();
              if (!it.enabled) { return; }
              closeMenu();
              vscode.postMessage({ type: 'cmd', command: it.command });
            });
            dd.appendChild(mi);
          }
          m.appendChild(dd);
          m.classList.add('open');
          openEl = m;
        });
        m.addEventListener('mouseenter', () => {
          if (closeTimer) { clearTimeout(closeTimer); closeTimer = 0; }
          if (openEl && openEl !== m) { m.click(); }
        });
        bar.appendChild(m);
      }
      const c = document.createElement('div');
      c.className = 'conn';
      bar.appendChild(c);
      return c;
    }

    function buildContent(spec) {
      content.innerHTML = '';
      const s = document.createElement('div');
      s.className = 'statusline';
      if (!spec.connected) {
        s.textContent = '未连接控制器';
      } else {
        const run = spec.scriptStatus || '空闲';
        s.textContent = spec.target + ' · ' + (spec.engine || '?') +
          ' · 轴 ' + spec.axisCount + ' · ' + run;
      }
      content.appendChild(s);

    }

    function render(spec) {
      // 兜底守卫：菜单展开期间丢弃渲染帧（本视图已无高频数据，仅防状态变化恰逢展开）
      if (openEl) { return; }
      buildMenus(spec.menus);
      bar.querySelector('.conn').textContent = spec.connected
        ? spec.target
        : '未连接';
      buildContent(spec);
    }

    // 关闭策略：
    //   * 点击**菜单栏/下拉之外的空白**才立即关；
    //   * 鼠标移出菜单栏区域 → 延迟 600ms 关（给移向二级子菜单留缓冲）；
    //   * 移入下拉/子菜单 → 立即取消待关闭。
    document.addEventListener('click', (ev) => {
      if (ev.target && ev.target.closest && ev.target.closest('.menubar')) { return; }
      closeMenu();
    });
    bar.addEventListener('mouseleave', scheduleClose);
    bar.addEventListener('mouseenter', () => { if (closeTimer) { clearTimeout(closeTimer); closeTimer = 0; } });
    window.addEventListener('blur', closeMenu);
    window.addEventListener('message', (ev) => {
      const m = ev.data;
      if (m && m.type === 'render') { render(m.spec); }
    });
    vscode.postMessage({ type: 'ready' });
  </script>
</body>
</html>`;
}
