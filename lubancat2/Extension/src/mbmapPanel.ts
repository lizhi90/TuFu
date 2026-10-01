// mbmapPanel.ts —— 「工具 → Modbus 配置」右侧面板（D11，v0.9.0）
//
// 形态与轴状态/Modbus/通讯状态面板一致：编辑器区 WebviewPanel，同一时刻一个实例。
// 数据：打开时/点「刷新」时拉取一次（控制器 `mbmap.get`，失败则用内置本地副本，界面标注来源）。
// 只读展示（「已存在的寄存器 = 已配置」，2026-09-30 定案；用户自定义编辑留待未来项目）。

import * as vscode from 'vscode';

import { MbmapSpec, mbmapPanelHtml } from './mbmapPanelPure';
import { S } from './strings';

export interface MbmapPanelHooks {
    /** 拉取数据（extension.ts 内实现：控制器 d11 → 否则本地副本） */
    fetch(): Promise<MbmapSpec>;
    /** 保存用户寄存器表（mbmap.set；仅在线且 d11 时可用） */
    save?(text: string): Promise<void>;
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

export class KxMbmapPanel {
    private static current: KxMbmapPanel | undefined;

    private readonly panel: vscode.WebviewPanel;
    private readonly nonce = makeNonce();
    private readonly disposables: vscode.Disposable[] = [];
    private disposed = false;

    static createOrShow(hooks: MbmapPanelHooks): KxMbmapPanel {
        if (KxMbmapPanel.current) {
            KxMbmapPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            void KxMbmapPanel.current.refresh();
            return KxMbmapPanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.mbmapPanel',
            S.mbmapPanel.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxMbmapPanel.current = new KxMbmapPanel(panel, hooks);
        return KxMbmapPanel.current;
    }

    static get instance(): KxMbmapPanel | undefined {
        return KxMbmapPanel.current;
    }

    private constructor(
        panel: vscode.WebviewPanel,
        private readonly hooks: MbmapPanelHooks,
    ) {
        this.panel = panel;
        this.panel.webview.html = mbmapPanelHtml(
            { source: 'none', sourceText: S.mbmapPanel.sourceLoading, groups: [], total: 0, editable: false, proto: 'd11', station: 1 },
            this.nonce,
        );
        this.disposables.push(
            panel.webview.onDidReceiveMessage((msg: { type?: string; text?: string }) => {
                if (msg && msg.type === 'refresh') {
                    void this.refresh();
                } else if (msg && msg.type === 'save' && typeof msg.text === 'string') {
                    void this.save(msg.text);
                }
            }),
            panel.onDidDispose(() => this.dispose()),
        );
        void this.refresh();
    }

    private async refresh(): Promise<void> {
        if (this.disposed) {
            return;
        }
        try {
            const spec = await this.hooks.fetch();
            if (!this.disposed) {
                this.panel.webview.html = mbmapPanelHtml(spec, this.nonce);
            }
        } catch (e) {
            this.hooks.log?.('[mbmap] 刷新失败: ' + String(e));
            if (!this.disposed) {
                this.panel.webview.html = mbmapPanelHtml(
                    { source: 'none', sourceText: S.mbmapPanel.sourceNone, groups: [], total: 0, editable: false, proto: 'd11', station: 1 },
                    this.nonce,
                );
            }
        }
    }

    private async save(text: string): Promise<void> {
        if (!this.hooks.save) {
            return;
        }
        try {
            await this.hooks.save(text);
        } catch (e) {
            this.hooks.log?.('[mbmap] 保存失败: ' + String(e));
            void vscode.window.showErrorMessage(S.mbmapPanel.saveFail(String(e)));
            return;
        }
        void vscode.window.showInformationMessage(S.mbmapPanel.saveOk);
        await this.refresh();
    }

    private dispose(): void {
        this.disposed = true;
        KxMbmapPanel.current = undefined;
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
    }
}
