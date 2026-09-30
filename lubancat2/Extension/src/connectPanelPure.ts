// connectPanelPure.ts —— 「连接控制器」表单弹窗的纯逻辑（v0.3.2 起替代两步 InputBox）
//
// 为什么用 WebviewPanel：VS Code 扩展 API **没有**带表单的原生对话框（InputBox 一次只收
// 一个值，QuickPick 只能选列表）。要「IP + 端口 + 连接按钮」的完善表单，标准做法就是
// 一个 WebviewPanel（编辑器区标签页形态）。
//
// 本文件不 import vscode：HTML 生成可被 Node 单测；适配层在 connectPanel.ts。

import { S } from './strings';

/** 表单模式：连接（默认） / 修改IP地址 + 端口（v0.5.1，同一表单、按钮为「保存」） */
export type ConnectPanelMode = 'connect' | 'settings';

/** 表单初始值（上次连接目标） */
export interface ConnectPanelInit {
    host: string;
    port: number;
    /** 不传 = connect（保持 v0.3.2 行为不变） */
    mode?: ConnectPanelMode;
}

/** 表单运行态（扩展端推送给前端） */
export interface ConnectPanelState {
    /** 连接进行中：按钮禁用、输入只读 */
    connecting?: boolean;
    /** 错误信息（红字显示在按钮上方；空 = 无错） */
    error?: string;
    host?: string;
    port?: number;
    /** 切换模式（同一面板被「连接」/「修改地址」复用时更新标题与按钮文案） */
    mode?: ConnectPanelMode;
}

/** 前端即时校验（与 config.ts 的 hostError/portError 同语义，提前到输入侧给反馈） */
export function validateConnectInputs(host: string, port: string): string {
    const h = host.trim();
    const p = port.trim();
    if (h.length === 0) {
        return S.connectPanel.errHostEmpty;
    }
    if (!/^(\d{1,3})(\.\d{1,3}){3}$/.test(h)) {
        return S.connectPanel.errHostFormat;
    }
    if (p.length === 0) {
        return S.connectPanel.errPortEmpty;
    }
    const n = Number(p);
    if (!Number.isInteger(n) || n < 1 || n > 65535) {
        return S.connectPanel.errPortRange;
    }
    return '';
}

/** 生成表单面板 HTML（初值内联；状态经 postMessage 更新） */
export function connectPanelHtml(nonce: string, init: ConnectPanelInit): string {
    const csp = `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';`;
    const host = init.host.replace(/"/g, '&quot;');
    const port = Number.isFinite(init.port) && init.port > 0 ? String(init.port) : '5000';
    const mode: ConnectPanelMode = init.mode === 'settings' ? 'settings' : 'connect';
    const title = mode === 'settings' ? S.connectPanel.settingsTitle : S.connectPanel.title;
    const goLabel = mode === 'settings' ? S.connectPanel.saveBtn : S.connectPanel.connectBtn;
    const hint = mode === 'settings' ? S.connectPanel.settingsHint : S.connectPanel.hint;
    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="${csp}" />
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 24px;
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    color: var(--vscode-foreground);
    background: transparent;
    display: flex;
    justify-content: center;
  }
  .card {
    width: 100%;
    max-width: 380px;
    padding: 18px 20px 16px;
    border: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.3));
    border-radius: 6px;
    background: var(--vscode-editor-background);
  }
  h2 { margin: 0 0 14px; font-size: 14px; font-weight: 600; }
  label {
    display: block;
    margin: 10px 0 4px;
    font-size: 12px;
    color: var(--vscode-descriptionForeground);
  }
  input {
    width: 100%;
    height: 26px;
    padding: 0 8px;
    border: 1px solid var(--vscode-input-border, rgba(128,128,128,0.4));
    border-radius: 3px;
    background: var(--vscode-input-background);
    color: var(--vscode-input-foreground);
    font-family: inherit;
    font-size: 13px;
    outline: none;
  }
  input:focus { border-color: var(--vscode-focusBorder); }
  input[readonly] { opacity: 0.7; }
  .err {
    margin: 10px 0 0;
    min-height: 16px;
    font-size: 12px;
    color: var(--vscode-errorForeground, #f14c4c);
    white-space: pre-wrap;
  }
  .hint {
    margin: 12px 0 0;
    font-size: 11px;
    color: var(--vscode-descriptionForeground);
    line-height: 1.5;
  }
  .btns {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 16px;
  }
  button {
    height: 26px;
    padding: 0 14px;
    border: 1px solid transparent;
    border-radius: 3px;
    font-family: inherit;
    font-size: 12px;
    cursor: pointer;
    color: var(--vscode-button-foreground);
    background: var(--vscode-button-background);
  }
  button:hover { background: var(--vscode-button-hoverBackground); }
  button.secondary {
    background: transparent;
    color: var(--vscode-button-secondaryForeground, var(--vscode-foreground));
    border-color: var(--vscode-button-secondaryBackground, rgba(128,128,128,0.4));
  }
  button.secondary:hover { background: var(--vscode-button-secondaryHoverBackground, rgba(128,128,128,0.2)); }
  button[disabled] { opacity: 0.55; cursor: default; }
</style>
</head>
<body>
  <div class="card">
    <h2>${title}</h2>
    <form id="f">
      <label for="host">${S.connectPanel.hostLabel}</label>
      <input id="host" name="host" value="${host}" placeholder="192.168.1.11" autocomplete="off" spellcheck="false" />
      <label for="port">${S.connectPanel.portLabel}</label>
      <input id="port" name="port" value="${port}" placeholder="5000" autocomplete="off" spellcheck="false" />
      <div class="err" id="err"></div>
      <div class="btns">
        <button type="button" class="secondary" id="cancel">${S.connectPanel.cancelBtn}</button>
        <button type="submit" id="go">${goLabel}</button>
      </div>
    </form>
    <div class="hint" id="hint">${hint}</div>
  </div>
  <script nonce="${nonce}">
    const vscode = acquireVsCodeApi();
    const form = document.getElementById('f');
    const hostEl = document.getElementById('host');
    const portEl = document.getElementById('port');
    const errEl = document.getElementById('err');
    const goBtn = document.getElementById('go');
    const cancelBtn = document.getElementById('cancel');
    const hintEl = document.getElementById('hint');

    // 模式：connect（连接）/ settings（修改控制器地址，v0.5.1）——同一表单复用，由扩展端消息切换
    let mode = '${mode}';
    const LABELS = {
      connect: { go: '${S.connectPanel.connectBtn}', busy: '${S.connectPanel.connectingBtn}' },
      settings: { go: '${S.connectPanel.saveBtn}', busy: '${S.connectPanel.saveBtn}' },
    };
    const TITLES = { connect: '${S.connectPanel.title}', settings: '${S.connectPanel.settingsTitle}' };
    const HINTS = { connect: '${S.connectPanel.hint}', settings: '${S.connectPanel.settingsHint}' };
    function applyMode() {
      const h2 = document.querySelector('h2');
      if (h2) { h2.textContent = TITLES[mode]; }
      if (hintEl) { hintEl.textContent = HINTS[mode]; }
      if (!goBtn.disabled) { goBtn.textContent = LABELS[mode].go; }
    }

    form.addEventListener('submit', (ev) => {
      ev.preventDefault();
      const host = hostEl.value.trim();
      const port = portEl.value.trim();
      // 前端即时校验（与扩展端 parseTarget 同语义；错误就地表单内显示，不弹 toast）
      const local = localValidate(host, port);
      if (local) { errEl.textContent = local; return; }
      errEl.textContent = '';
      vscode.postMessage({ type: 'submit', host, port });
    });
    cancelBtn.addEventListener('click', () => vscode.postMessage({ type: 'cancel' }));

    function localValidate(host, port) {
      if (host.length === 0) { return '${S.connectPanel.errHostEmpty}'; }
      if (!/^(\\d{1,3})(\\.\\d{1,3}){3}$/.test(host)) { return '${S.connectPanel.errHostFormat}'; }
      if (port.length === 0) { return '${S.connectPanel.errPortEmpty}'; }
      const n = Number(port);
      if (!Number.isInteger(n) || n < 1 || n > 65535) { return '${S.connectPanel.errPortRange}'; }
      return '';
    }

    function setConnecting(on) {
      goBtn.disabled = on;
      cancelBtn.disabled = on;
      hostEl.readOnly = on;
      portEl.readOnly = on;
      goBtn.textContent = on ? LABELS[mode].busy : LABELS[mode].go;
    }

    window.addEventListener('message', (ev) => {
      const m = ev.data;
      if (!m || m.type !== 'state') { return; }
      const s = m.state || {};
      if (s.mode === 'connect' || s.mode === 'settings') {
        mode = s.mode;
        applyMode();
      }
      if (s.host !== undefined) { hostEl.value = s.host; }
      if (s.port !== undefined) { portEl.value = String(s.port); }
      errEl.textContent = s.error || '';
      setConnecting(Boolean(s.connecting));
    });
  </script>
</body>
</html>`;
}
