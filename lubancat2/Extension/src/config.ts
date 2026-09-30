// config.ts —— 扩展设置读取（docs/planA/15 §7.1）
//
// 键名 kine-x.*；与控制器 config/app.conf 的 SCRIPT_* 语义同源（对照表见 docs/planA/15 §11.2 D-04）。

import * as vscode from 'vscode';
import { S } from './strings';

export type ScriptEngine = 'basic' | 'lua';

/**
 * 默认调试通道地址（FR-1.1）。
 * 插件运行在 **PC** 上、控制器调试口是**服务端**，故默认值是**控制器业务网 IP**（出厂固定 `192.168.1.11`），
 * 而不是 `127.0.0.1`——`127.0.0.1` 只有板子自己（或本机 Mock）连得上，与「以另一台电脑远程调试」的用法矛盾。
 */
export const DEFAULT_HOST = '192.168.1.11';
/** 控制器调试通道默认端口（对应控制器 config/app.conf 的 DEBUG_PORT） */
export const DEFAULT_PORT = 5000;
/** 调试通道端口合法区间（TCP 端口） */
export const MIN_PORT = 1;
export const MAX_PORT = 65535;

/** 一次连接的目标端点（来自连接弹窗，不落盘，除 IP 外只保留在会话内） */
export interface KxTarget {
    host: string;
    port: number;
}

const HOSTNAME_RE = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;

/** 纯函数：校验主机（IPv4 字面量或主机名）。返回 null 表示合法。 */
export function hostError(raw: string): string | null {
    const v = raw.trim();
    if (v.length === 0) {
        return S.configCheck.hostEmpty;
    }
    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(v)) {
        const ok = v.split('.').every((seg) => {
            const n = Number(seg);
            return seg.length > 0 && String(n) === String(Number(seg)) && n >= 0 && n <= 255;
        });
        return ok ? null : S.configCheck.hostIpv4Range;
    }
    return HOSTNAME_RE.test(v) ? null : S.configCheck.hostNotHost;
}

/** 纯函数：校验端口。返回 null 表示合法。 */
export function portError(raw: string): string | null {
    const v = raw.trim();
    if (!/^\d+$/.test(v)) {
        return S.configCheck.portNotNumber;
    }
    const n = Number(v);
    return n >= MIN_PORT && n <= MAX_PORT ? null : S.configCheck.portRange(MIN_PORT, MAX_PORT);
}

/**
 * 纯函数：把弹窗里的一对输入解析成连接目标。
 * 不合法则 `error` 给出**人话原因**（降级不伪装：宁可拒绝，也不连一个猜出来的地址）。
 */
export function parseTarget(rawHost: string, rawPort: string): { target?: KxTarget; error?: string } {
    const hostErr = hostError(rawHost);
    if (hostErr !== null) {
        return { error: S.configCheck.hostReject(hostErr) };
    }
    const portErr = portError(rawPort);
    if (portErr !== null) {
        return { error: S.configCheck.portReject(portErr) };
    }
    return { target: { host: rawHost.trim(), port: Number(rawPort.trim()) } };
}

export interface KxConfig {
    host: string;
    port: number;
    token: string;
    engine: ScriptEngine;
    /** `kine-x.engine` 是否被用户**显式**配置（未配置时仅按文档默认值参与展示，不参与裁决） */
    engineExplicit: boolean;
    sshForward: boolean;
    subscribeHz: number;
    /** 轴状态面板界面刷新率（Hz，2~20；默认 10。订阅仍按 subscribeHz，面板只按此频率重绘） */
    axisRefreshHz: number;
    /** Modbus 面板界面刷新率（Hz，2~20；默认 10） */
    modbusRefreshHz: number;
    /** 意外断线自动重连（指数退避；默认开） */
    autoReconnect: boolean;
    showRawProtocol: boolean;
    requestTimeoutMs: number;
}

export function readConfig(): KxConfig {
    const c = vscode.workspace.getConfiguration('kine-x');
    const inspect = c.inspect<string>('engine');
    const engineExplicit =
        inspect?.workspaceFolderValue !== undefined ||
        inspect?.workspaceValue !== undefined ||
        inspect?.globalValue !== undefined;
    const engineRaw = (c.get<string>('engine') ?? 'basic').toLowerCase();
    return {
        host: c.get<string>('host', DEFAULT_HOST),
        port: c.get<number>('debugPort', DEFAULT_PORT),
        token: c.get<string>('token', ''),
        engine: engineRaw === 'lua' ? 'lua' : 'basic',
        engineExplicit,
        sshForward: c.get<boolean>('sshForward', false),
        subscribeHz: c.get<number>('subscribeHz', 20),
        axisRefreshHz: c.get<number>('axisRefreshHz', 10),
        modbusRefreshHz: c.get<number>('modbusRefreshHz', 10),
        autoReconnect: c.get<boolean>('autoReconnect', true),
        showRawProtocol: c.get<boolean>('showRawProtocol', false),
        requestTimeoutMs: c.get<number>('requestTimeoutMs', 5000),
    };
}

/**
 * 远程访问时给出 SSH 端口转发提示（docs/planA/13 §4.5：不裸奔业务网）。
 * 端口与地址取自**用户当场输入的连接目标**，而不是配置文件默认值。
 */
export function sshForwardHint(target: KxTarget): string {
    return `ssh -N -L ${target.port}:${target.host}:${target.port} <user>@<board-ip>`;
}

/** 回环地址不需要端口转发（本机直连 mock / 本机控制器）。 */
export function isLoopback(host: string): boolean {
    const v = host.trim().toLowerCase();
    return v === 'localhost' || v === '::1' || v.startsWith('127.');
}
