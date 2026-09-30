// axisPanel.ts —— 「轴状态」右侧面板（docs/planA/15 T-19/T-20，v0.4.2 自控制台迁出）
//
// 形态：**编辑器区 WebviewPanel**，与「实时曲线」（curve.ts）同形态——
// 菜单「工具 → 轴状态」打开，同一时刻只有一个实例，再次点击即前置。
// 为什么迁出侧边栏：连接后 axis/bus/mb 最高 20Hz 推送，与菜单栏同视图会
// 高频重建 DOM 冲掉菜单交互（「连接后菜单点不动」的根因）。
//
// 数据：复用 T-18 的订阅（axis/bus/mb 主题），事件驱动、不轮询（FR-6.5）——
// 面板可见时 retain，隐藏/关闭即 release（引用计数）；首次可见取一次 `axis.snapshot`。

import * as vscode from 'vscode';

import { AxisPanelSpec, axisPanelHtml } from './axisPanelPure';
import { S } from './strings';

/** 在线调试动作（v0.7.0，Webview → 扩展；扩展经 D1 `cmd` 下发） */
export interface AxisControlMsg {
    action: 'enable' | 'disable' | 'stop' | 'jog' | 'jogTick' | 'jogStop' | 'move' | 'setScale';
    axis: number;
    /** 点到点/点动速度（mm/s） */
    speed?: number;
    /** 点动方向：+1 / -1 */
    dir?: number;
    /** 目标位置（mm） */
    target?: number;
    mode?: 'abs' | 'rel';
    /** setScale：脉冲当量（inc/mm） */
    value?: number;
}

export interface AxisPanelHooks {
    /** 组装渲染数据（extension.ts 用 buildAxisPanelSpec(store, …) 构造） */
    buildSpec(): AxisPanelSpec;
    /** 面板在线调试动作（v0.7.0） */
    onControl?(msg: AxisControlMsg): void;
    /** 面板可见（订阅 retain：axis/bus/mb + 轴快照） */
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

export class KxAxisPanel {
    private static current: KxAxisPanel | undefined;

    private readonly panel: vscode.WebviewPanel;
    private readonly nonce = makeNonce();
    private visible = false;
    private readonly disposables: vscode.Disposable[] = [];

    /** 打开（或前置）面板；同一时刻只有一个实例 */
    static createOrShow(hooks: AxisPanelHooks): KxAxisPanel {
        if (KxAxisPanel.current) {
            KxAxisPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            return KxAxisPanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.axisPanel',
            S.axisPanel.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxAxisPanel.current = new KxAxisPanel(panel, hooks);
        return KxAxisPanel.current;
    }

    static get instance(): KxAxisPanel | undefined {
        return KxAxisPanel.current;
    }

    private constructor(
        panel: vscode.WebviewPanel,
        private readonly hooks: AxisPanelHooks,
    ) {
        this.panel = panel;
        panel.webview.options = { enableScripts: true };
        // 先挂监听再设 html：ready 握手不会丢（详见 console.ts 教训）
        panel.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m), undefined, this.disposables);
        panel.webview.html = axisPanelHtml(this.nonce);
        panel.onDidDispose(() => this.dispose(), undefined, this.disposables);
        panel.onDidChangeViewState(() => this.syncVisibility(), undefined, this.disposables);
        this.syncVisibility();
    }

    /** 面板是否可见（扩展据此维护订阅引用计数） */
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
        if (KxAxisPanel.current === this) {
            KxAxisPanel.current = undefined;
        }
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
        this.panel.dispose();
    }

    private syncVisibility(): void {
        if (this.panel.visible && !this.visible) {
            this.visible = true;
            this.hooks.log?.('[axisPanel] 可见 → retain 订阅 + 轴快照');
            this.hooks.onVisible();
            this.update();
        } else if (!this.panel.visible && this.visible) {
            this.visible = false;
            this.hooks.log?.('[axisPanel] 隐藏 → release 订阅');
            this.hooks.onHidden();
        }
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as { type?: string } & Record<string, unknown>;
        if (msg.type === 'axisCtl') {
            this.hooks.onControl?.(msg as unknown as AxisControlMsg);
            return;
        }
        if (msg.type === 'ready') {
            this.hooks.log?.('[axisPanel] ready 握手 → 首帧渲染');
            this.update();
            // 补发兜底：Webview 重载/时序抖动时防首帧丢失
            setTimeout(() => this.update(), 150);
        }
    }
}
