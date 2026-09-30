// console.ts —— 「Kine-X 控制台」WebviewView 适配层（v0.3.0 起的唯一侧边栏视图）
//
// 一个视图 = 菜单栏（顶） + 状态行（v0.4.9 起文件列表迁至原生 TreeView `kine-x.files`）。
// 状态表格（轴/总线/寄存器）不在此视图：它们迁至编辑器区的 Webview 面板
// （axisPanel.ts / modbusPanel.ts），避免高频事件重建 DOM 冲掉菜单交互（v0.4.2 根因修复）。
// 职责：建视图、转发菜单点击、按低频状态变化重绘。
// 渲染数据与 HTML 在 consolePure.ts（纯逻辑，可 Node 单测）。

import * as vscode from 'vscode';

import { ConsoleSpec, consoleHtml } from './consolePure';

export interface ConsoleHooks {
    /** 组装渲染数据（extension.ts 用 buildConsoleSpec(store, …) 构造） */
    buildSpec(): ConsoleSpec;
    /** 控制台可见（订阅 retain：axis/bus/mb） */
    onVisible(): void;
    /** 控制台隐藏（订阅 release） */
    onHidden(): void;
    /** 诊断日志 */
    log?(line: string): void;
}

function makeNonce(): string {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let s = '';
    for (let i = 0; i < 24; i++) {
        s += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return s;
}

export class KxConsoleView implements vscode.WebviewViewProvider {
    public static readonly viewType = 'kine-x.console';

    private view: vscode.WebviewView | undefined;
    private readonly nonce = makeNonce();
    private log: (line: string) => void = () => {};

    constructor(private readonly hooks: ConsoleHooks) {}

    setLogger(fn: (line: string) => void): void {
        this.log = fn;
    }

    resolveWebviewView(view: vscode.WebviewView): void {
        this.log('[console] resolveWebviewView');
        this.view = view;
        view.webview.options = { enableScripts: true };
        view.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m));
        // 先挂监听再设 html：ready 握手不会丢（详见 toolbar 教训）
        view.webview.html = consoleHtml(this.nonce);
        view.onDidChangeVisibility(() => {
            this.log(`[console] visible=${view.visible}`);
            if (view.visible) {
                this.hooks.onVisible();
            } else {
                this.hooks.onHidden();
            }
        });
        if (view.visible) {
            // 注册时即可见的兜底（首次激活）
            this.hooks.onVisible();
        }
    }

    /** store/连接态变化 → 重绘（含 ready 时的首帧） */
    update(): void {
        if (!this.view) {
            return;
        }
        const spec = this.hooks.buildSpec();
        void this.view.webview.postMessage({ type: 'render', spec });
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as {
            type?: string;
            command?: string;
            name?: string;
        };
        if (msg.type === 'ready') {
            this.log('[console] ready 握手 → 首帧渲染');
            this.update();
            // 补发兜底：Webview 重载/时序抖动时防首帧丢失
            setTimeout(() => this.update(), 150);
            return;
        }
        if (msg.type === 'cmd' && typeof msg.command === 'string') {
            void vscode.commands.executeCommand(msg.command);
        }
    }
}
