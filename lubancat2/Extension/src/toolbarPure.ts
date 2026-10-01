// toolbarPure.ts —— 侧边栏顶部**横向菜单条**的纯逻辑（docs/planA/15）
//
// 为什么需要它：
//   VS Code 的侧边栏里，「容器标题 → 视图标题」之间**没有**扩展可插入自定义横条的贡献点
//   （activitybar 容器标题不可扩展、viewsWelcome 会被渲染成竖排文字按钮）。
//   要得到「轴状态上面一行横着的菜单栏」，唯一可行做法是：
//   在容器内**轴状态视图之上**放一个**固定高度的 Webview 视图**，条内自绘横排按钮。
//
// 本文件**不 import vscode**，只负责「按钮定义 + HTML 字符串」；
// 由 toolbar.ts 负责 WebviewView 适配与消息转发。
//
// 边界（与既有纪律一致）：
//   * 按钮**只发消息**给扩展，真正的命令执行走 `vscode.commands.executeCommand`
//     （与命令面板/菜单同一条路，不另开后门）；
//   * 不可用按钮**显式置灰并标注原因**（降级不伪装，NFR-6）；
//   * 文案集中在 strings.ts，本文件只引用。

import { S } from './strings';

/** 横条按钮定义（纯数据；id 用于回传与单测断言） */
export interface ToolbarButton {
    /** 稳定 id（回传消息用，也是单测锚点） */
    id: string;
    /** 命令 ID（点击后由扩展执行） */
    command: string;
    /** 内联 SVG 图标表的键（见 `ICON_SVG`） */
    icon: string;
    /** 按钮上的**可见短文字**（与图标并排；保证任何环境下都能看懂/点到） */
    text: string;
    /** 悬停提示（完整说明） */
    tooltip: string;
}

/** 横条渲染输入 */
export interface ToolbarSpec {
    /** 是否已连接控制器（决定显示「连接」还是「断开」） */
    connected: boolean;
    /** 当前脚本运行态（`READY` / `PAUSED` / `DONE` …；空串表示未运行） */
    scriptStatus: string;
    /** 控制器能力位（`16` §7.1）；用于禁用需要 d3/d4/d5 的按钮 */
    caps: ReadonlySet<string>;
    /** 当前连接目标（`host:port`），未连接时为空 */
    target: string;
    /** 本项目配置的脚本语言（`kine-x.engine`），供菜单勾选显示 */
    configEngine: string;
}

/** 菜单项（下拉里的每一行） */
export interface ToolbarMenuItem {
    /** 稳定 id（对应 `toolbarEnabled` 的判定键，也是单测锚点） */
    id: string;
    command: string;
    text: string;
    tooltip: string;
    icon: string;
}

/** 渲染用的菜单项（含可用性与图标） */
export interface ToolbarRenderItem extends ToolbarMenuItem {
    kind: 'item';
    enabled: boolean;
    svg: string;
}

/** 二级子菜单（如「脚本语言 ▸ Basic / Lua」） */
export interface ToolbarRenderSubmenu {
    kind: 'submenu';
    id: string;
    label: string;
    enabled: boolean;
    children: ToolbarRenderItem[];
}

export type ToolbarEntry = ToolbarRenderItem | ToolbarRenderSubmenu;

/** 菜单栏的一个顶层菜单（如「控制器」「工具」） */
export interface ToolbarMenuGroup {
    id: string;
    /** 顶层显示名（如「控制器」） */
    label: string;
    items: ToolbarEntry[];
}

/**
 * 生成**菜单栏**（普通软件那种：横排文字 + 点开展开下拉）。
 *
 * 顶层菜单：`控制器`（含原「脚本」组的下载/运行/停止 + 脚本语言 Basic/Lua 子菜单）/ `工具`。
 * 每项可用性由 `toolbarEnabled` 判定（未连接 → 脚本类置灰；缺 d2 → 脚本类置灰）。
 */
export function toolbarMenus(spec: ToolbarSpec): ToolbarMenuGroup[] {
    const mk = (id: string, command: string, text: string, tooltip: string, icon: string): ToolbarRenderItem => ({
        kind: 'item',
        id,
        command,
        text,
        tooltip,
        icon,
        enabled: toolbarEnabled(id, spec),
        svg: ICON_SVG[icon] ?? '',
    });

    // 脚本语言子菜单：当前配置语言打 ✓（选择即切换本项目语言环境）
    const langItem = (lang: 'basic' | 'lua', text: string, tooltip: string): ToolbarRenderItem => ({
        kind: 'item',
        id: `engine.${lang}`,
        command: `kine-x.engine.${lang}`,
        text: spec.configEngine === lang ? `✓ ${text}` : text,
        tooltip,
        icon: '',
        enabled: true,
        svg: '',
    });

    const scriptSub: ToolbarRenderSubmenu = {
        kind: 'submenu',
        id: 'scriptLang',
        label: S.toolbar.menuScriptLang,
        enabled: true,
        children: [
            langItem('basic', S.toolbar.engineBasicText, S.toolbar.engineBasicTip),
            langItem('lua', S.toolbar.engineLuaText, S.toolbar.engineLuaTip),
        ],
    };

    return [
        {
            id: 'controller',
            label: S.toolbar.menuController,
            items: [
                spec.connected
                    ? mk('disconnect', 'kine-x.disconnect', S.toolbar.disconnectText, S.toolbar.disconnectTip, 'debug-disconnect')
                    : mk('connect', 'kine-x.connect', S.toolbar.connectText, S.toolbar.connectTip, 'plug'),
                mk('setIp', 'kine-x.ip.set', S.toolbar.setIpText, S.toolbar.setIpTip, 'globe'),
                mk('sysInfo', 'kine-x.sysInfo', S.toolbar.sysInfoText, S.toolbar.sysInfoTip, 'info'),
                mk('sync', 'kine-x.sync', S.toolbar.syncLocalText, S.toolbar.syncLocalTip, 'sync'),
                mk('restart', 'kine-x.restart', S.toolbar.restartText, S.toolbar.restartTip, 'debug-restart'),
                mk('portMax', 'kine-x.portMax', S.toolbar.portMaxText, S.toolbar.portMaxTip, 'server-process'),
                mk('download', 'kine-x.download', S.toolbar.downloadText, S.toolbar.downloadTip, 'cloud-upload'),
                mk('run', 'kine-x.run', S.toolbar.runText, S.toolbar.runTip, 'debug-start'),
                mk('stop', 'kine-x.stop', S.toolbar.stopText, S.toolbar.stopTip, 'debug-stop'),
                scriptSub,
            ],
        },
        {
            id: 'tools',
            label: S.toolbar.menuTools,
            items: [
                mk('axisPanel', 'kine-x.axis.open', S.toolbar.axisPanelText, S.toolbar.axisPanelTip, 'table'),
                mk('modbus', 'kine-x.modbus.open', S.toolbar.modbusText, S.toolbar.modbusTip, 'registers'),
                mk('cmd', 'kine-x.cmd', S.toolbar.cmdText, S.toolbar.cmdTip, 'terminal'),
                mk('curve', 'kine-x.curve.open', S.toolbar.curveText, S.toolbar.curveTip, 'graph'),
                mk('refresh', 'kine-x.panel.refresh', S.toolbar.refreshText, S.toolbar.refreshTip, 'refresh'),
                mk('comm', 'kine-x.comm.open', S.toolbar.commText, S.toolbar.commTip, 'radio-tower'),
                mk('mbmap', 'kine-x.mbmap.open', S.toolbar.mbmapText, S.toolbar.mbmapTip, 'sliders'),
                mk('mbdev', 'kine-x.mbdev.open', S.toolbar.mbdevText, S.toolbar.mbdevTip, 'mbdev'),
            ],
        },
    ];
}

/**
 * 生成横条按钮列表（顺序固定，便于单测比对）。
 *
 * 分组顺序：连接 | 下载 | 运行 | 停止 | 命令 | 曲线 | 刷新 | 信息
 * 未连接时：仅「连接」「修改 IP」「控制器信息」保持可用，其余置灰。
 */
export function toolbarButtons(spec: ToolbarSpec): ToolbarButton[] {
    return [
        spec.connected
            ? { id: 'disconnect', command: 'kine-x.disconnect', icon: 'debug-disconnect', text: S.toolbar.disconnectText, tooltip: S.toolbar.disconnectTip }
            : { id: 'connect', command: 'kine-x.connect', icon: 'plug', text: S.toolbar.connectText, tooltip: S.toolbar.connectTip },
        {
            id: 'download',
            command: 'kine-x.download',
            icon: 'cloud-upload',
            text: S.toolbar.downloadText,
            tooltip: S.toolbar.downloadTip,
        },
        {
            id: 'downloadAndRun',
            command: 'kine-x.downloadAndRun',
            icon: 'play',
            text: S.toolbar.downloadAndRunText,
            tooltip: S.toolbar.downloadAndRunTip,
        },
        {
            id: 'stop',
            command: 'kine-x.stop',
            icon: 'debug-stop',
            text: S.toolbar.stopText,
            tooltip: S.toolbar.stopTip,
        },
        {
            id: 'curve',
            command: 'kine-x.curve.open',
            icon: 'graph',
            text: S.toolbar.curveText,
            tooltip: S.toolbar.curveTip,
        },
        {
            id: 'refresh',
            command: 'kine-x.panel.refresh',
            icon: 'refresh',
            text: S.toolbar.refreshText,
            tooltip: S.toolbar.refreshTip,
        },
        {
            id: 'sysInfo',
            command: 'kine-x.sysInfo',
            icon: 'info',
            text: S.toolbar.sysInfoText,
            tooltip: S.toolbar.sysInfoTip,
        },
    ];
}

/**
 * 按钮是否可用（降级不伪装）。
 *
 * 规则：
 *   * 未连接：只有「连接 / 修改 IP / 控制器信息」可用，脚本类按钮一律禁用；
 *   * 已连接但引擎被自动脚本占用（caps 无 `d2`）：脚本类按钮禁用；
 *   * `d5` 缺失不影响本横条（断点走 DAP，不走这里）。
 */
export function toolbarEnabled(id: string, spec: ToolbarSpec): boolean {
    // 连接/断开：互斥，按当前状态决定
    if (id === 'connect') return !spec.connected;
    if (id === 'disconnect') return spec.connected;
    // 与连接无关：改 IP、看控制器信息、切脚本语言（写本项目配置）
    if (id === 'setIp' || id === 'sysInfo') return true;
    if (id === 'engine.basic' || id === 'engine.lua') return true;
    // 以下都必须已连接
    if (!spec.connected) return false;
    // 已连接的工具类：轴状态面板 / Modbus 面板 / 设备命令 / 曲线 / 刷新 / 同步
    if (id === 'sync' || id === 'axisPanel' || id === 'modbus' ||
        id === 'cmd' || id === 'curve' || id === 'refresh') return true;
    // 重启控制器（D7 重启能力）：需控制器声明 d7，未声明时置灰（降级不伪装）
    if (id === 'restart') return spec.caps.has('d7');
    // 通讯状态（D10）：需控制器声明 d10，未声明时置灰（降级不伪装）
    if (id === 'comm') return spec.caps.has('d10');
    // Modbus 配置（D12 固件组态 / D11 过渡）：需控制器声明 d12 或 d11，否则置灰（降级不伪装）
    if (id === 'mbmap') return spec.caps.has('d12') || spec.caps.has('d11');
    // Modbus 主站（D13）：需控制器声明 d13，否则置灰（降级不伪装）
    if (id === 'mbdev') return spec.caps.has('d13');
    // 修改端口数量（D9）：需控制器声明 d9，未声明时置灰（降级不伪装）
    if (id === 'portMax') return spec.caps.has('d9');
    // 脚本类（下载/运行/停止）：需调试口可独占脚本引擎（`16` §9.1：caps 含 d2）
    return spec.caps.has('d2');
}

/** 状态徽标文案（横条右侧）：连接目标 + 脚本运行态 */
export function toolbarStatusText(spec: ToolbarSpec): string {
    if (!spec.connected) {
        return S.toolbar.statusOffline;
    }
    const run = spec.scriptStatus.length > 0 ? spec.scriptStatus : S.toolbar.statusIdle;
    return `${spec.target} · ${run}`;
}

/**
 * 生成横条 Webview HTML。
 *
 * 关键点：
 *   * 单行 `flex` 横向排布，`overflow-x:auto` 防窄侧边栏挤压换行；
 *   * 用 VS Code 主题变量（`--vscode-*`）跟随主题，不写死颜色；
 *   * CSP 收紧（`default-src 'none'`），脚本用 nonce 放行；
 *   * 点击只 `postMessage({type:'cmd', command})`，由扩展转 `executeCommand`。
 */
export function toolbarHtml(nonce: string): string {
    const csp = `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';`;
    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="${csp}" />
<style>
  * { box-sizing: border-box; }
  html, body {
    margin: 0;
    padding: 0;
    background: transparent;
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    overflow: visible;      /* 允许下拉浮层溢出（菜单栏必需） */
  }
  /* ---- 菜单栏本体：一条紧贴的横条 ---- */
  .menubar {
    position: relative;
    display: flex;
    align-items: stretch;
    height: 24px;
    background: var(--vscode-menubar-background, transparent);
    border-bottom: 1px solid var(--vscode-menu-separatorBackground, rgba(128,128,128,0.25));
    user-select: none;
  }
  /* ---- 顶层菜单：纯文字 + 小三角，像普通软件一样 ---- */
  .menu {
    position: relative;
    display: inline-flex;
    align-items: center;
    padding: 0 8px;
    color: var(--vscode-menubar-foreground, var(--vscode-foreground));
    font-size: 12px;
    cursor: pointer;
    white-space: nowrap;
  }
  .menu:hover,
  .menu.open {
    background: var(--vscode-menubar-selectionBackground, rgba(128,128,128,0.25));
    color: var(--vscode-menubar-selectionForeground, var(--vscode-foreground));
  }
  .menu .caret {
    margin-left: 4px;
    font-size: 8px;
    opacity: 0.75;
  }
  /* ---- 下拉浮层 ---- */
  .dropdown {
    position: absolute;
    top: 24px;
    left: 0;
    min-width: 168px;
    padding: 3px 0;
    background: var(--vscode-menu-background, var(--vscode-editor-background));
    color: var(--vscode-menu-foreground, var(--vscode-foreground));
    border: 1px solid var(--vscode-menu-border, var(--vscode-menu-separatorBackground, rgba(128,128,128,0.4)));
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.35);
    z-index: 1000;
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
  .mi svg {
    width: 14px;
    height: 14px;
    fill: currentColor;
    flex: 0 0 auto;
    opacity: 0.9;
  }
  .mi:hover:not(.disabled) {
    background: var(--vscode-menu-selectionBackground, var(--vscode-list-activeSelectionBackground));
    color: var(--vscode-menu-selectionForeground, var(--vscode-list-activeSelectionForeground));
  }
  .mi.disabled {
    opacity: 0.4;
    cursor: default;
  }
  /* 二级子菜单：hover 右侧展开（如「脚本语言 ▸ Basic / Lua」） */
  .mi.sub {
    position: relative;
  }
  .mi.sub::after {
    content: '\\25B8';
    margin-left: auto;
    padding-left: 12px;
    font-size: 9px;
    opacity: 0.7;
  }
  .mi.sub .dropdown {
    display: none;
    top: -4px;
    left: calc(100% + 2px);
  }
  .mi.sub:hover .dropdown {
    display: block;
  }
  .mi .hint {
    margin-left: auto;
    padding-left: 12px;
    font-size: 10px;
    opacity: 0.65;
  }
  /* ---- 右侧状态徽标 ---- */
  .status {
    margin-left: auto;
    display: flex;
    align-items: center;
    padding: 0 8px;
    font-size: 11px;
    color: var(--vscode-descriptionForeground);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
</style>
</head>
<body>
  <div class="menubar" id="bar" role="menubar"></div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const bar = document.getElementById('bar');
    let openEl = null;

    function closeMenu() {
      if (openEl) {
        openEl.classList.remove('open');
        const d = openEl.querySelector('.dropdown');
        if (d) { d.remove(); }
        openEl = null;
        document.body.style.minHeight = '';
      }
    }

    function render(spec) {
      closeMenu();
      bar.innerHTML = '';

      for (const g of spec.menus) {
        const m = document.createElement('div');
        m.className = 'menu';
        m.setAttribute('role', 'menuitem');
        m.dataset.id = g.id;
        m.innerHTML = '<span>' + g.label + '</span><span class="caret">\\u25BE</span>';

        m.addEventListener('click', (ev) => {
          ev.stopPropagation();
          if (openEl === m) { closeMenu(); return; }
          closeMenu();

          const dd = document.createElement('div');
          dd.className = 'dropdown';
          dd.setAttribute('role', 'menu');
          for (const it of g.items) {
            const mi = document.createElement('div');
            mi.className = 'mi' + (it.enabled ? '' : ' disabled');
            mi.setAttribute('role', 'menuitem');
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
          // 让 body 至少容纳下拉高度，尽量不被视图裁剪
          document.body.style.minHeight = (24 + dd.offsetHeight + 4) + 'px';
        });

        // 悬停即展开（与普通软件菜单栏一致）
        m.addEventListener('mouseenter', () => {
          if (openEl && openEl !== m) { m.click(); }
        });

        bar.appendChild(m);
      }

      const st = document.createElement('div');
      st.className = 'status';
      st.textContent = spec.status;
      st.title = spec.status;
      bar.appendChild(st);
    }

    document.addEventListener('click', closeMenu);
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

/**
 * 图标：**内联 SVG**（16×16，`currentColor`）。
 *
 * 为什么不用 codicon 字体：侧边栏 Webview 不加载 VS Code 的 codicon 字体表，
 * 用字体会出现空白方块；内联 SVG 在任何环境都能渲染，且跟随主题色。
 */
export const ICON_SVG: Readonly<Record<string, string>> = {
    // 连接（插头）
    plug: '<svg viewBox="0 0 16 16"><path d="M6 1v3H5a1 1 0 0 0-1 1v2a4 4 0 0 0 3 3.87V14a1 1 0 1 0 2 0v-3.13A4 4 0 0 0 12 7V5a1 1 0 0 0-1-1h-1V1H6zm6 6H4V6h8v1z"/></svg>',
    // 断开连接
    'debug-disconnect': '<svg viewBox="0 0 16 16"><path d="M2.5 1.5 1.5 2.5 6 7v2a4 4 0 0 0 3 3.87V15a1 1 0 1 0 2 0v-2.13c.36-.09.7-.22 1-.4l3 3 .5-.5-6-6-7-7zM10 5V4h2v3.13c0 .5-.12.96-.32 1.37L10 6.82V5zM6 4 5 3V2h3v3h-1V4H6z"/></svg>',
    // 下载到控制器
    'cloud-upload': '<svg viewBox="0 0 16 16"><path d="M8 1 4.5 4.5l.7.7L7.5 2.9V10h1V2.9l2.3 2.3.7-.7L8 1zM2 11v3h12v-3h-1v2H3v-2H2z"/></svg>',
    // 运行
    play: '<svg viewBox="0 0 16 16"><path d="M3 1.7v12.6L13.5 8 3 1.7z"/></svg>',
    // 停止
    'debug-stop': '<svg viewBox="0 0 16 16"><path d="M3 3h10v10H3z"/></svg>',
    // 曲线
    graph: '<svg viewBox="0 0 16 16"><path d="M1 1h1v13h13v1H1V1zm3 11 3-5 2.5 3L13 4l.8.6-4 7-2.5-3L4.6 13 4 12z"/></svg>',
    // 轴状态（状态表）
    table: '<svg viewBox="0 0 16 16"><path d="M1 2h14v12H1V2zm1 1v3h12V3H2zm0 4v3h4V7H2zm5 0v3h7V7H7zm-5 4v2h4v-2H2zm5 0v2h7v-2H7z"/></svg>',
    // Modbus 寄存器（地址网格）
    registers: '<svg viewBox="0 0 16 16"><path d="M1 1h14v14H1V1zm1 1v3h5V2H2zm6 0v3h6V2H8zM2 6v3h5V6H2zm6 0v3h6V6H8zm-6 4v4h5v-4H2zm6 0v4h6v-4H8z"/></svg>',
    // 刷新
    refresh: '<svg viewBox="0 0 16 16"><path d="M8 2.5a5.5 5.5 0 1 0 5.2 3.7h-1.1A4.5 4.5 0 1 1 8 3.5c1 0 1.9.3 2.6.9l-1.9.1v1l3.4-.2-.2-3.4h-1l.1 2A5.5 5.5 0 0 0 8 2.5z"/></svg>',
    // 信息
    info: '<svg viewBox="0 0 16 16"><path d="M8 1a7 7 0 1 0 0 14A7 7 0 0 0 8 1zm0 1a6 6 0 1 1 0 12A6 6 0 0 1 8 2zm-.9 2.2a.9.9 0 1 0 0 1.8.9.9 0 0 0 0-1.8zM7 7v5h2V7H7z"/></svg>',
    // 修改 IP（地球）
    globe: '<svg viewBox="0 0 16 16"><path d="M8 1a7 7 0 1 0 0 14A7 7 0 0 0 8 1zM2.1 7h2.2c.1-1.3.3-2.5.7-3.4A6 6 0 0 0 2.1 7zm0 2a6 6 0 0 0 2.9 3.4c-.4-.9-.6-2.1-.7-3.4H2.1zm2.2 0c.1 1.5.4 2.8.9 3.6.4.7.9 1 1.3 1s.9-.3 1.3-1c.5-.8.8-2.1.9-3.6H4.3zm0-2h4.4c-.1-1.5-.4-2.8-.9-3.6-.4-.7-.9-1-1.3-1s-.9.3-1.3 1c-.5.8-.8 2.1-.9 3.6zm5.4 0h2.2a6 6 0 0 0-2.9-3.4c.4.9.6 2.1.7 3.4zm0 2c-.1 1.3-.3 2.5-.7 3.4a6 6 0 0 0 2.9-3.4h-2.2z"/></svg>',
    // 设备/从站（用于「工具 → Modbus 主站」：机箱 + 指向箭头）
    mbdev: '<svg viewBox="0 0 16 16"><path d="M1.5 3h7v10h-7V3zm1 1v8h5V4h-5zM10.2 5 13 8l-2.8 3V9.2H8.4V6.8h1.8V5z"/></svg>',
    // 参数滑杆（用于「工具 → Modbus 配置」）
    sliders: '<svg viewBox="0 0 16 16"><path d="M2 3.2h12v1.2H2zM4.4 2.4h2v2.8h-2zM2 7.4h12v1.2H2zM8.4 6.6h2v2.8h-2zM2 11.6h12v1.2H2zM5.6 10.8h2v2.8h-2z"/></svg>',
    // 本地同步（双向箭头）
    sync: '<svg viewBox="0 0 16 16"><path d="M5 2 2.5 4.5 5 7V5.5h6v1.5L13.5 4.5 11 1.5V3H5V2zM11 14l2.5-2.5L11 9v1.5H5V9L2.5 11.5 5 14.5V13h6v1z"/></svg>',
    // 通讯状态（无线电塔：塔身 + 两侧信号波；「工具 → 通讯状态」用）
    'radio-tower': '<svg viewBox="0 0 16 16"><path d="M8 1.2a.9.9 0 1 0 0 1.8.9.9 0 0 0 0-1.8zM8 3l-2.6 9h1.4l.5-2h1.4l.5 2h1.4L8 3zM4.6 4.6 3.8 6l.8 1.4h.9L4.7 6l.8-1.4H4.6zM3.3 3.8 2.5 6l.8 2.2h.9L3.4 6l.8-2.2H3.3zM11.4 4.6l.8 1.4-.8 1.4h-.9L11.3 6l-.8-1.4h.9zM12.7 3.8l.8 2.2-.8 2.2h-.9L12.6 6l-.8-2.2h.9z"/></svg>',
    // 重启控制器（逆时针环形箭头；refresh 的镜像，两者不同向以示区分）
    'debug-restart': '<svg viewBox="0 0 16 16"><g transform="scale(-1,1) translate(-16,0)"><path d="M8 2.5a5.5 5.5 0 1 0 5.2 3.7h-1.1A4.5 4.5 0 1 1 8 3.5c1 0 1.9.3 2.6.9l-1.9.1v1l3.4-.2-.2-3.4h-1l.1 2A5.5 5.5 0 0 0 8 2.5z"/></g></svg>',
    // 端口数量（三层服务器机架：横板 + 指示灯镂空）
    'server-process': '<svg viewBox="0 0 16 16"><path fill-rule="evenodd" d="M2 2h12v3.4H2V2zm1.2 1.1v1.2h1.6V3.1H3.2zM2 6.3h12v3.4H2V6.3zm1.2 1.1v1.2h1.6V7.4H3.2zM2 10.6h12V14H2v-3.4zm1.2 1.1v1.2h1.6v-1.2H3.2z"/></svg>',
    // 设备终端
    terminal: '<svg viewBox="0 0 16 16"><path d="M1 2h14v12H1V2zm1 1v10h12V3H2zm1.8 2.2.7.7L5.7 7l-1.2 1.1-.7-.7L4.9 6l-1.1-1.1v.3zm2.5 2.8h4v1h-4V8z"/></svg>',
    // 控制器文件（文件夹）
    folder: '<svg viewBox="0 0 16 16"><path d="M1.5 3h4.6l1.2 1.5h7.2V13h-13V3zm1 1v8h11V5.5H6.8L5.6 4H2.5z"/></svg>',
    // 运行（三角，用于「运行脚本」菜单项）
    'debug-start': '<svg viewBox="0 0 16 16"><path d="M3 1.7v12.6L13.5 8 3 1.7z"/></svg>',
};

/**
 * 按钮渲染模型：把「定义 + 可用性 + 图标」合成前端直接可用的形态。
 * 单独抽出来是为了能被 Node 单测断言（不需要 vscode，也不需要跑 Webview）。
 */
export interface ToolbarRenderButton {
    id: string;
    command: string;
    tooltip: string;
    /** 按钮上的可见短文字 */
    text: string;
    enabled: boolean;
    svg: string;
    /** 该按钮之后是否画一条竖分隔线（用于「连接」组与脚本组之间） */
    separator: boolean;
}

/** 分隔线插在这些按钮**之后**（视觉分组：连接 | 脚本 | 工具） */
const SEPARATOR_AFTER = new Set(['connect', 'disconnect', 'stop']);

export function toolbarRenderButtons(spec: ToolbarSpec): ToolbarRenderButton[] {
    return toolbarButtons(spec).map((b) => ({
        id: b.id,
        command: b.command,
        tooltip: b.tooltip,
        text: b.text,
        enabled: toolbarEnabled(b.id, spec),
        svg: ICON_SVG[b.icon] ?? '',
        separator: SEPARATOR_AFTER.has(b.id),
    }));
}
