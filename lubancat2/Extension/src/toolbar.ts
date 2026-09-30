// toolbar.ts —— 侧边栏顶部**横向菜单条**（WebviewView 适配层）
//
// 位置：Kine-X 容器内、**轴状态视图之上**（`package.json` 的 views 里排在第一位）。
// 形态：固定高度的 Webview，内部横排一排图标按钮（真·横条，非竖排文字）。
//
// 职责边界：
//   * 本文件**只做 vscode 适配**：建 WebviewView、转发点击、按状态重绘；
//   * 按钮定义 / 可用性 / HTML 全在 `toolbarPure.ts`（可 Node 单测）；
//   * 点击 → `vscode.commands.executeCommand(command)`，与命令面板同一条路。

import * as vscode from 'vscode';

import { ToolbarSpec, toolbarHtml, toolbarMenus, toolbarStatusText } from './toolbarPure';

/** 横条需要的外部状态（由 extension.ts 提供，避免本模块反向依赖全局变量） */
export interface ToolbarHooks {
    /** 当前是否已连接 */
    isConnected(): boolean;
    /** 当前脚本运行态（`READY` / `PAUSED` / `DONE`…；空串 = 未运行） */
    scriptStatus(): string;
    /** 控制器能力位（`sys.info.caps`） */
    caps(): ReadonlySet<string>;
    /** 当前连接目标 `host:port`（未连接时为空串） */
    target(): string;
}

/** 生成 nonce（CSP 用）：随机串即可，无需密码学强度 */
function makeNonce(): string {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let s = '';
    for (let i = 0; i < 24; i++) {
        s += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return s;
}

export class KxToolbarView implements vscode.WebviewViewProvider {
    public static readonly viewType = 'kine-x.toolbar';

    private view: vscode.WebviewView | undefined;
    private readonly nonce = makeNonce();
    /** 诊断日志落点（Output 面板「Kine-X 调试」）；缺失时静默，不影响功能 */
    private log: (line: string) => void = () => {};

    constructor(private readonly hooks: ToolbarHooks) {}

    /** 注入日志通道（extension.ts 调用；用于排查「菜单条不显示」类问题） */
    setLogger(fn: (line: string) => void): void {
        this.log = fn;
    }

    /** 由 extension.ts 在激活时注册；返回的 provider 可之后调用 `refresh()` */
    resolveWebviewView(view: vscode.WebviewView): void {
        this.log('[toolbar] resolveWebviewView 被调用（视图已创建）');
        this.view = view;
        view.webview.options = { enableScripts: true };
        view.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m));
        // ⚠ 顺序要点：**先挂监听，再设 html**。
        // 若先 setHtml 再 onDidReceiveMessage，Webview 里的 `ready` 握手可能先到而无人接收，
        // 导致首帧渲染永远丢失（界面空白）。
        view.webview.html = toolbarHtml(this.nonce);
        this.log('[toolbar] html 已注入，等待 Webview 的 ready 握手…');
        view.onDidChangeVisibility(() => {
            this.log(`[toolbar] 视图可见性变化 visible=${view.visible}`);
        });
        // 不做「主动 refresh」：此刻 Webview 脚本尚未执行，postMessage 会被丢弃。
        // 渲染**只由 ready 握手触发**（见 onMessage），重载/重建视图都会重新握手一次。
    }

    /** 状态变化（连接/运行态/能力位）时重绘按钮可用性与状态徽标 */
    refresh(): void {
        if (!this.view) {
            this.log('[toolbar] refresh 跳过：视图尚未创建（resolveWebviewView 未触发）');
            return;
        }
        const spec: ToolbarSpec = {
            connected: this.hooks.isConnected(),
            scriptStatus: this.hooks.scriptStatus(),
            caps: this.hooks.caps(),
            target: this.hooks.target(),
            configEngine: '',
        };
        const menus = toolbarMenus(spec);
        const payload = {
            type: 'render',
            spec: {
                menus,
                status: toolbarStatusText(spec),
            },
        };
        this.log(
            `[toolbar] 推送 render（${menus.length} 个顶层菜单，connected=${spec.connected}）`,
        );
        void this.view.webview.postMessage(payload);
    }

    // -----------------------------------------------------------------------

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as {
            type?: string;
            command?: string;
            ok?: boolean;
        };
        if (msg.type === 'ready') {
            this.log('[toolbar] 收到 Webview 的 ready 握手 → 开始渲染');
            // Webview 初次加载 / 重载：立即发一次渲染，并在稍后再补发一次。
            // 补发是**防丢兜底**：Webview 脚本可能刚注册监听就被 CSP/时序影响，
            // 单次 postMessage 丢失会让界面永久空白（NFR：宁可多发一次也不空窗）。
            this.refresh();
            setTimeout(() => this.refresh(), 150);
            return;
        }
        if (msg.type === 'cmd' && typeof msg.command === 'string') {
            this.log(`[toolbar] 点击按钮 → 执行命令 ${msg.command}`);
            void vscode.commands.executeCommand(msg.command);
        }
    }
}
