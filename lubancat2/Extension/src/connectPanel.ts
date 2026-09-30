// connectPanel.ts —— 「连接控制器」表单弹窗适配层（WebviewPanel，编辑器区标签页形态）
//
// 单例：重复点「连接控制器」时聚焦已有面板并刷新表单值，不会开出一堆标签页。
// 职责：建面板、转发提交/取消、把连接过程状态（连接中/失败原因）回显到表单。
// HTML 在 connectPanelPure.ts（纯逻辑，可 Node 单测）。

import * as vscode from 'vscode';

import { S } from './strings';
import { ConnectPanelInit, ConnectPanelState, connectPanelHtml } from './connectPanelPure';

export interface ConnectPanelHooks {
    /** 用户点了「连接」（表单已通过前端校验；扩展端再做 parseTarget 兜底） */
    onSubmit(host: string, port: string): void;
    /** 用户取消（点取消按钮 / 关闭标签页）。注意：连接成功关闭**不**触发 */
    onCancel(): void;
}

function makeNonce(): string {
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    let s = '';
    for (let i = 0; i < 24; i++) {
        s += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return s;
}

export class KxConnectPanel {
    static instance: KxConnectPanel | undefined;

    private panel: vscode.WebviewPanel;
    private hooks: ConnectPanelHooks;
    private readonly nonce = makeNonce();
    /** 连接成功后的主动关闭不算「取消」 */
    private closing = false;

    private constructor(init: ConnectPanelInit, hooks: ConnectPanelHooks) {
        this.hooks = hooks;
        this.panel = vscode.window.createWebviewPanel(
            'kine-x.connectPanel',
            init.mode === 'settings' ? S.connectPanelSettingsTitle : S.connectPanelTitle,
            vscode.ViewColumn.Active,
            { enableScripts: true, retainContextWhenHidden: false },
        );
        this.panel.webview.html = connectPanelHtml(this.nonce, init);
        this.panel.webview.onDidReceiveMessage((m: unknown) => this.onMessage(m));
        this.panel.onDidDispose(() => {
            if (KxConnectPanel.instance === this) {
                KxConnectPanel.instance = undefined;
            }
            if (!this.closing) {
                this.hooks.onCancel();
            }
        });
        KxConnectPanel.instance = this;
    }

    /** 打开（或聚焦已有）表单；复用实例时按本次调用方更新 hooks 与模式（连接 ↔ 修改地址） */
    static showOrReveal(init: ConnectPanelInit, hooks: ConnectPanelHooks): void {
        if (KxConnectPanel.instance) {
            const p = KxConnectPanel.instance;
            p.hooks = hooks;
            p.panel.title = init.mode === 'settings' ? S.connectPanelSettingsTitle : S.connectPanelTitle;
            p.panel.reveal();
            p.setState({
                connecting: false,
                error: '',
                host: init.host,
                port: init.port,
                mode: init.mode ?? 'connect',
            });
            return;
        }
        new KxConnectPanel(init, hooks);
    }

    /** 连接过程状态回显（connecting / 错误信息） */
    setState(s: ConnectPanelState): void {
        void this.panel.webview.postMessage({ type: 'state', state: s });
    }

    /** 连接成功后由扩展端调用：先置 closing 再 dispose，onCancel 不触发 */
    close(): void {
        this.closing = true;
        this.panel.dispose();
    }

    private onMessage(m: unknown): void {
        const msg = (typeof m === 'object' && m !== null ? m : {}) as {
            type?: string;
            host?: unknown;
            port?: unknown;
        };
        if (msg.type === 'submit') {
            this.hooks.onSubmit(String(msg.host ?? ''), String(msg.port ?? ''));
            return;
        }
        if (msg.type === 'cancel') {
            this.panel.dispose(); // 触发 onDidDispose → onCancel
        }
    }
}
