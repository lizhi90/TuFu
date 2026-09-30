// modbusPanel.ts —— 「Modbus 寄存器」右侧面板（v0.4.3 自轴状态面板拆出）
//
// 用户拍板：Modbus 寄存器与轴状态分开——「工具 → Modbus」独立打开。
// 形态与「轴状态」「实时曲线」一致：编辑器区 WebviewPanel，同一时刻一个实例。
//
// 数据：复用 T-18 的订阅，面板可见才 retain `mb` 主题（引用计数），隐藏/关闭即 release；
// 只看寄存器时不背 axis/bus 的推送带宽。

import * as vscode from 'vscode';

import { ModbusPanelSpec, modbusPanelHtml } from './modbusPanelPure';
import { S } from './strings';

export interface ModbusPanelHooks {
    /** 组装渲染数据（extension.ts 用 buildModbusPanelSpec(store, …) 构造） */
    buildSpec(): ModbusPanelSpec;
    /** 面板可见（订阅 retain：mb） */
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

export class KxModbusPanel {
    private static current: KxModbusPanel | undefined;

    private readonly panel: vscode.WebviewPanel;
    private readonly nonce = makeNonce();
    private visible = false;
    /** v0.8.6：「仅显示已用寄存器」模式（按钮切换；过滤在 buildModbusPanelSpec 内） */
    private usedOnly = false;
    private readonly disposables: vscode.Disposable[] = [];

    /** 打开（或前置）面板；同一时刻只有一个实例 */
    static createOrShow(hooks: ModbusPanelHooks): KxModbusPanel {
        if (KxModbusPanel.current) {
            KxModbusPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            return KxModbusPanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.modbusPanel',
            S.modbusPanel.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxModbusPanel.current = new KxModbusPanel(panel, hooks);
        return KxModbusPanel.current;
    }

    static get instance(): KxModbusPanel | undefined {
        return KxModbusPanel.current;
    }

    private constructor(
        panel: vscode.WebviewPanel,
        private readonly hooks: ModbusPanelHooks,
    ) {
        this.panel = panel;
        panel.webview.options = { enableScripts: true };
        // 先挂监听再设 html：ready 握手不会丢（详见 console.ts 教训）
        panel.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m), undefined, this.disposables);
        panel.webview.html = modbusPanelHtml(this.nonce);
        panel.onDidDispose(() => this.dispose(), undefined, this.disposables);
        panel.onDidChangeViewState(() => this.syncVisibility(), undefined, this.disposables);
        this.syncVisibility();
    }

    /** 面板是否可见（扩展据此维护订阅引用计数） */
    get isVisible(): boolean {
        return this.visible;
    }

    /** v0.8.6：当前是否「仅显示已用寄存器」（extension.ts 组装 spec 时读取） */
    get showUsedOnly(): boolean {
        return this.usedOnly;
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
        if (KxModbusPanel.current === this) {
            KxModbusPanel.current = undefined;
        }
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
        this.panel.dispose();
    }

    private syncVisibility(): void {
        if (this.panel.visible && !this.visible) {
            this.visible = true;
            this.hooks.log?.('[modbusPanel] 可见 → retain mb 订阅');
            this.hooks.onVisible();
            this.update();
        } else if (!this.panel.visible && this.visible) {
            this.visible = false;
            this.hooks.log?.('[modbusPanel] 隐藏 → release mb 订阅');
            this.hooks.onHidden();
        }
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as { type?: string };
        if (msg.type === 'ready') {
            this.hooks.log?.('[modbusPanel] ready 握手 → 首帧渲染');
            this.update();
            // 补发兜底：Webview 重载/时序抖动时防首帧丢失
            setTimeout(() => this.update(), 150);
        } else if (msg.type === 'toggleUsedOnly') {
            this.usedOnly = !this.usedOnly;
            this.hooks.log?.(`[modbusPanel] 仅显示已用 = ${this.usedOnly}`);
            this.update();
        }
    }
}
