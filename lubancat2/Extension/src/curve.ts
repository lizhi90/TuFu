// curve.ts —— 实时曲线面板（docs/planA/15 T-21 / FR-6.6）
//
// 形态：**Webview + Canvas**（`14` §6.3：DAP 不覆盖波形，曲线必须单开 Webview）。
// 数据：复用 T-18 的订阅（`axis` 主题），**事件驱动、不轮询**（FR-6.5）——
//   本面板可见时才 retain `axis` 订阅，关闭即 release（引用计数，不额外占用带宽）。
// 频率：按订阅 `hz`（默认 ≤20Hz）绘制；更高频采样属 `13` §9 的独立通道，按 D-06「首版不做」。
//
// 本文件依赖 vscode；纯逻辑（缓冲 / 页面文档）在 `curvePure.ts`，可 Node 单测。

import * as vscode from 'vscode';

import { CurveRing, CurveSeries, curveHtml, curveToCsv } from './curvePure';
import { S } from './strings';

/**
 * 曲线序列：`mpos`/`dpos` 直接来自 `axis` 事件；
 * `followerr`（DPOS-MPOS）与 `speed`（ΔMPOS/Δt，mm/s）为**客户端推算**（v0.7.0），
 * 图例可点击显隐（默认只显示两条位置曲线；导出 CSV 始终含全部通道）。
 */
export const CURVE_SERIES: CurveSeries[] = [
    { key: 'mpos', label: S.curve.targetFb, color: '#4fc9ff' },       // 反馈位置 = MPOS
    { key: 'dpos', label: S.curve.targetCmd, color: '#ffb74d' },      // 指令位置 = DPOS
    { key: 'followerr', label: S.curve.followerr, color: '#e57373' }, // 推算：跟随误差
    { key: 'speed', label: S.curve.speed, color: '#81c784' },         // 推算：速度
];

/** 滚动窗口保留的样点数（≈ 20Hz × 30s） */
const MAX_POINTS = 600;

export interface CurveHooks {
    /** 面板可见 → 保留 `axis` 订阅 */
    retain(): void;
    /** 面板隐藏/关闭 → 释放 `axis` 订阅 */
    release(): void;
    /** 状态提示（落输出面板，不弹窗） */
    notice?(msg: string): void;
}

interface CurveSamplePoint {
    t: number;
    v: Array<number | null>;
}

export class KxCurvePanel {
    private static current: KxCurvePanel | undefined;

    private readonly ring = new CurveRing(MAX_POINTS);
    private readonly disposables: vscode.Disposable[] = [];
    private axisCount: number;
    private axis = 0;
    private visible = false;
    /** 推算通道的状态（速度 = ΔMPOS/Δt） */
    private lastMpos: number | undefined;
    private lastT: number | undefined;

    /** 打开（或前置）曲线面板；同一时刻只有一个实例 */
    static createOrShow(axisCount: number, hooks: CurveHooks): KxCurvePanel {
        if (KxCurvePanel.current) {
            KxCurvePanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            KxCurvePanel.current.setAxisCount(axisCount);
            return KxCurvePanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.curve',
            S.curve.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxCurvePanel.current = new KxCurvePanel(panel, axisCount, hooks);
        return KxCurvePanel.current;
    }

    static get instance(): KxCurvePanel | undefined {
        return KxCurvePanel.current;
    }

    private constructor(
        private readonly panel: vscode.WebviewPanel,
        axisCount: number,
        private readonly hooks: CurveHooks,
    ) {
        this.axisCount = axisCount;
        this.panel.webview.html = curveHtml({
            title: S.curve.title,
            series: CURVE_SERIES,
            maxPoints: MAX_POINTS,
        });
        this.panel.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m), undefined, this.disposables);
        this.panel.onDidDispose(() => this.dispose(), undefined, this.disposables);
        this.panel.onDidChangeViewState(() => this.syncVisibility(), undefined, this.disposables);
        this.syncVisibility();
    }

    /** 控制器报告轴数（`sys.info`）后同步下拉框，并把当前轴夹到合法范围 */
    setAxisCount(axisCount: number): void {
        const n = Number.isFinite(axisCount) && axisCount > 0 ? Math.trunc(axisCount) : 0;
        if (n === this.axisCount) {
            return;
        }
        this.axisCount = n;
        if (this.axis >= Math.max(n, 1)) {
            this.axis = 0;
            this.ring.clear();
            this.lastMpos = undefined;
            this.lastT = undefined;
            void this.panel.webview.postMessage({ type: 'reset' });
        }
        if (this.visible) {
            void this.postConfig();
        }
    }

    /** 面板是否可见（扩展据此在重连后重建 `axis` 订阅） */
    get isVisible(): boolean {
        return this.visible;
    }

    /** 喂一帧轴采样（由扩展的 `axis` 事件分发调用） */
    push(axis: number, values: Record<string, unknown>): void {
        if (!this.visible || axis !== this.axis) {
            return;
        }
        // 只取本面板声明的序列键：`t`/`axis` 等杂项不进曲线（也不占环形缓冲的列）
        const sample: Record<string, number | undefined> = {};
        for (const s of CURVE_SERIES) {
            const v = values[s.key];
            sample[s.key] = typeof v === 'number' && Number.isFinite(v) ? v : undefined;
        }
        const t = Date.now();
        // 客户端推算：跟随误差与速度（不新增协议字段；速度按相邻 MPOS 差分）
        const mpos = sample['mpos'];
        const dpos = sample['dpos'];
        if (mpos !== undefined && dpos !== undefined) {
            sample['followerr'] = dpos - mpos;
        }
        if (mpos !== undefined && this.lastMpos !== undefined && this.lastT !== undefined) {
            const dt = (t - this.lastT) / 1000;
            if (dt > 0) {
                sample['speed'] = (mpos - this.lastMpos) / dt;
            }
        }
        if (mpos !== undefined) {
            this.lastMpos = mpos;
            this.lastT = t;
        }
        this.ring.push(t, sample);
        void this.panel.webview.postMessage({ type: 'data', t, values: sample });
    }

    /** 断线：清空窗口，避免把上一段会话的曲线当成当前数据继续画（不伪装） */
    reset(): void {
        this.ring.clear();
        this.lastMpos = undefined;
        this.lastT = undefined;
        void this.panel.webview.postMessage({ type: 'reset' });
    }

    /** 导出当前窗口为 CSV（v0.7.0）：保存对话框 → 写文件；空数据明确提示 */
    private async exportCsv(): Promise<void> {
        const times = this.ring.timesSnapshot();
        if (times.length === 0) {
            this.hooks.notice?.(S.curve.exportEmpty);
            return;
        }
        const cols = CURVE_SERIES.map((s) => this.ring.series(s.key));
        const csv = curveToCsv(times, CURVE_SERIES, cols);
        const d = new Date();
        const pad = (x: number): string => String(x).padStart(2, '0');
        const name = `kine-x-curve-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-` +
            `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.csv`;
        const base = vscode.workspace.workspaceFolders?.[0]?.uri;
        const uri = await vscode.window.showSaveDialog({
            defaultUri: base ? vscode.Uri.joinPath(base, name) : vscode.Uri.file(name),
            filters: { CSV: ['csv'] },
            saveLabel: S.curvePure.exportCsv,
        });
        if (!uri) {
            return;
        }
        try {
            await vscode.workspace.fs.writeFile(uri, Buffer.from(csv, 'utf8'));
            this.hooks.notice?.(S.curve.exported(uri.fsPath));
        } catch (e) {
            const msg = e instanceof Error ? e.message : String(e);
            this.hooks.notice?.(S.curve.exportFailed(msg));
        }
    }

    /** 手动清空当前窗口（订阅继续采样） */
    private clearWindow(): void {
        this.ring.clear();
        this.lastMpos = undefined;
        this.lastT = undefined;
        void this.panel.webview.postMessage({ type: 'reset' });
        this.hooks.notice?.(S.curve.cleared);
    }

    dispose(): void {
        if (this.visible) {
            this.visible = false;
            this.hooks.release();
        }
        if (KxCurvePanel.current === this) {
            KxCurvePanel.current = undefined;
        }
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
        this.panel.dispose();
    }

    // -----------------------------------------------------------------------

    private syncVisibility(): void {
        if (this.panel.visible && !this.visible) {
            this.visible = true;
            this.hooks.retain();
            this.hooks.notice?.(S.curve.opened);
            void this.postConfig();
        } else if (!this.panel.visible && this.visible) {
            this.visible = false;
            this.hooks.release();
        }
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as { type?: string; axis?: number };
        if (msg.type === 'export') {
            void this.exportCsv();
            return;
        }
        if (msg.type === 'clear') {
            this.clearWindow();
            return;
        }
        if (msg.type === 'ready') {
            // Webview 重新加载：补发配置与已有窗口（retainContextWhenHidden 之外的情形）
            void this.postConfig();
            void this.postSnapshot();
            return;
        }
        if (msg.type === 'axis') {
            const a = Number(msg.axis);
            if (Number.isInteger(a) && a >= 0 && a < Math.max(this.axisCount, 1)) {
                this.axis = a;
                this.ring.clear();
                this.lastMpos = undefined;
                this.lastT = undefined;
                void this.panel.webview.postMessage({ type: 'reset' });
            }
        }
    }

    private async postConfig(): Promise<void> {
        const count = Math.max(this.axisCount, 1);
        await this.panel.webview.postMessage({
            type: 'config',
            series: CURVE_SERIES,
            axes: Array.from({ length: count }, (_, i) => i),
            axis: this.axis,
            maxPoints: MAX_POINTS,
        });
    }

    private async postSnapshot(): Promise<void> {
        const times = this.ring.timesSnapshot();
        if (times.length === 0) {
            return;
        }
        const cols = CURVE_SERIES.map((s) => this.ring.series(s.key));
        const points: CurveSamplePoint[] = times.map((t, i) => ({
            t,
            v: cols.map((c) => (c && c[i] !== undefined ? c[i] : null)),
        }));
        await this.panel.webview.postMessage({ type: 'snapshot', points });
    }
}
