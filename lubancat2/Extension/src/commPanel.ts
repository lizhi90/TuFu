// commPanel.ts —— 「工具 → 通讯状态」右侧面板（D10，v0.8.7）
//
// 形态与轴状态/Modbus 面板一致：编辑器区 WebviewPanel，同一时刻一个实例；
// 面板可见才 retain `conn` 订阅（引用计数），隐藏/关闭即 release。
// 数据：conn 事件（TCP 连接快照 + EtherCAT 主/从明细），2Hz。

import * as vscode from 'vscode';

import { CommPanelSpec, commPanelHtml } from './commPanelPure';
import { S } from './strings';

export interface CommPanelHooks {
    /** 组装渲染数据（extension.ts 用 buildCommPanelSpec(store, …) 构造） */
    buildSpec(): CommPanelSpec;
    /** 面板可见（订阅 retain：conn） */
    onVisible(): void;
    /** 面板隐藏/关闭（订阅 release） */
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

export class KxCommPanel {
    private static current: KxCommPanel | undefined;

    private readonly panel: vscode.WebviewPanel;
    private readonly nonce = makeNonce();
    private visible = false;
    private readonly disposables: vscode.Disposable[] = [];

    /** 打开（或前置）面板；同一时刻只有一个实例 */
    static createOrShow(hooks: CommPanelHooks): KxCommPanel {
        if (KxCommPanel.current) {
            KxCommPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            return KxCommPanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.commPanel',
            S.commPanel.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxCommPanel.current = new KxCommPanel(panel, hooks);
        return KxCommPanel.current;
    }

    static get instance(): KxCommPanel | undefined {
        return KxCommPanel.current;
    }

    private constructor(
        panel: vscode.WebviewPanel,
        private readonly hooks: CommPanelHooks,
    ) {
        this.panel = panel;
        panel.webview.options = { enableScripts: true };
        panel.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m), undefined, this.disposables);
        panel.webview.html = commPanelHtml(this.nonce);
        panel.onDidDispose(() => this.dispose(), undefined, this.disposables);
        panel.onDidChangeViewState(() => this.syncVisibility(), undefined, this.disposables);
        this.syncVisibility();
    }

    get isVisible(): boolean {
        return this.visible;
    }

    /** store/连接态变化 → 重绘（含 ready 时的首帧） */
    update(): void {
        void this.panel.webview.postMessage({ type: 'render', spec: this.hooks.buildSpec() });
    }

    dispose(): void {
        if (this.visible) {
            this.visible = false;
            this.hooks.onHidden();
        }
        if (KxCommPanel.current === this) {
            KxCommPanel.current = undefined;
        }
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
        this.panel.dispose();
    }

    private syncVisibility(): void {
        if (this.panel.visible && !this.visible) {
            this.visible = true;
            this.hooks.log?.('[commPanel] 可见 → retain conn 订阅');
            this.hooks.onVisible();
            this.update();
        } else if (!this.panel.visible && this.visible) {
            this.visible = false;
            this.hooks.log?.('[commPanel] 隐藏 → release conn 订阅');
            this.hooks.onHidden();
        }
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as { type?: string };
        if (msg.type === 'ready') {
            this.hooks.log?.('[commPanel] ready 握手 → 首帧渲染');
            this.update();
            setTimeout(() => this.update(), 150);   // 兜底防首帧丢失
        }
    }
}
