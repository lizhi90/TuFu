// mbdevPanel.ts —— 「工具 → Modbus 主站」右侧面板（D13，v0.12.0；planA/21）
//
// 与轴状态/Modbus/通讯状态面板同形：编辑器区 WebviewPanel，同一时刻一个实例。
// 数据：打开/刷新时拉一次（控制器 mbdev.get 组态 + mbdev.status 在线状态）。
// 保存：{type:'save', text} → extension 调 mbdev.set。

import * as vscode from 'vscode';

import { MbdevSpec, mbdevPanelHtml } from './mbdevPanelPure';
import { S } from './strings';

export interface MbdevPanelHooks {
    fetch(): Promise<MbdevSpec>;
    save?(text: string): Promise<void>;
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

export class KxMbdevPanel {
    private static current: KxMbdevPanel | undefined;

    private readonly panel: vscode.WebviewPanel;
    private readonly nonce = makeNonce();
    private readonly disposables: vscode.Disposable[] = [];
    private disposed = false;

    static createOrShow(hooks: MbdevPanelHooks): KxMbdevPanel {
        if (KxMbdevPanel.current) {
            KxMbdevPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            void KxMbdevPanel.current.refresh();
            return KxMbdevPanel.current;
        }
        const panel = vscode.window.createWebviewPanel(
            'kine-x.mbdevPanel',
            S.mbdevPanel.title,
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        KxMbdevPanel.current = new KxMbdevPanel(panel, hooks);
        return KxMbdevPanel.current;
    }

    static get instance(): KxMbdevPanel | undefined {
        return KxMbdevPanel.current;
    }

    private constructor(
        panel: vscode.WebviewPanel,
        private readonly hooks: MbdevPanelHooks,
    ) {
        this.panel = panel;
        this.panel.webview.html = mbdevPanelHtml(
            { source: 'none', sourceText: S.mbdevPanel.sourceLoading, editable: false, devices: [], status: [] },
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
        if (this.disposed) return;
        try {
            const spec = await this.hooks.fetch();
            if (!this.disposed) {
                this.panel.webview.html = mbdevPanelHtml(spec, this.nonce);
            }
        } catch (e) {
            this.hooks.log?.('[mbdev] 刷新失败: ' + String(e));
            if (!this.disposed) {
                this.panel.webview.html = mbdevPanelHtml(
                    { source: 'none', sourceText: S.mbdevPanel.sourceNone, editable: false, devices: [], status: [] },
                    this.nonce,
                );
            }
        }
    }

    private async save(text: string): Promise<void> {
        if (!this.hooks.save) return;
        try {
            await this.hooks.save(text);
        } catch (e) {
            this.hooks.log?.('[mbdev] 保存失败: ' + String(e));
            void vscode.window.showErrorMessage(S.mbdevPanel.saveFail(String(e)));
            return;
        }
        void vscode.window.showInformationMessage(S.mbdevPanel.saveOk);
        await this.refresh();
    }

    private dispose(): void {
        this.disposed = true;
        KxMbdevPanel.current = undefined;
        while (this.disposables.length > 0) {
            this.disposables.pop()?.dispose();
        }
    }
}
