// dapPure.ts —— Debug Adapter 的**纯函数**部分（不依赖 vscode / 网络，可直接在 Node 下单测）
//
// 为什么要单独一个文件：`debugAdapter.ts` 顶部 `import * as vscode from 'vscode'`，
// 无法在普通 Node 进程里 require。把「变量行解析 / 字面量解析 / 类型推断 / 文案」这类
// 纯逻辑分离出来，就能用 `test/connection.smoke.cjs` 直接对编译产物做断言。

import { S } from './strings';

export interface VarItem {
    name: string;
    type: string;
    value: string;
}

/** `list_vars()` 的一行 `NAME = 值`（FR-4.5：`NAME = 3` 原文解析） */
export function parseVarLine(line: string): VarItem {
    const i = line.indexOf('=');
    if (i < 0) {
        return { name: line.trim(), type: 'unknown', value: '' };
    }
    const name = line.slice(0, i).trim();
    const value = line.slice(i + 1).trim();
    return { name, type: inferType(value), value: unquote(value) };
}

/** 字面量 → 引擎可接受的值：数值转 number，`true/false` 转 1/0，其余按字符串（去引号） */
export function parseLiteral(text: string): string | number {
    const t = text.trim();
    if (/^[+-]?\d+$/.test(t)) {
        return Number(t);
    }
    if (/^[+-]?(\d+\.\d*|\.\d+)([eE][+-]?\d+)?$/.test(t) || /^[+-]?\d+[eE][+-]?\d+$/.test(t)) {
        return Number(t);
    }
    if (/^true$/i.test(t)) {
        return 1;
    }
    if (/^false$/i.test(t)) {
        return 0;
    }
    return unquote(t);
}

export function inferType(raw: string): string {
    const t = raw.trim();
    if (/^".*"$/.test(t) || /^'.*'$/.test(t)) {
        return 'str';
    }
    if (/^[+-]?\d+$/.test(t)) {
        return 'int';
    }
    if (/^[+-]?(\d+\.\d*|\.\d+)([eE][+-]?\d+)?$/.test(t)) {
        return 'float';
    }
    return 'unknown';
}

export function typeOfValue(v: string | number): string {
    return typeof v === 'number' ? (Number.isInteger(v) ? 'int' : 'float') : 'str';
}

export function unquote(t: string): string {
    if (t.length >= 2 && ((t.startsWith('"') && t.endsWith('"')) || (t.startsWith("'") && t.endsWith("'")))) {
        return t.slice(1, -1);
    }
    return t;
}

export function baseName(p: string): string {
    const i = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'));
    return i >= 0 ? p.slice(i + 1) : p;
}

/** D5 未就绪时对 `pause`/`next`/`stepIn`/`stepOut` 的统一降级文案（UI **不伪装**） */
export function d5Notice(command: string): string {
    return S.dapPure.d5Notice(command);
}
