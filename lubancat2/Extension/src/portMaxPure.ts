// portMaxPure.ts —— 「修改端口数量」（D9）的纯逻辑（不 import vscode，可 Node 直接单测）
//
// 控制器端口编号 0..上限-1 只是句柄（无 ZMC 式通道规划）；上限默认 16、静态容量 64，
// 由 `port.max.get/set` 读写并持久化在控制器脚本目录 `.portmax`（重启后保留）。
// 本模块只做：输入解析（纯函数）+ 展示文案组装；UI/请求在 extension.ts。

import { S } from './strings';

export interface PortMaxParsed {
    /** 合法时的整数值 */
    value?: number;
    /** 非法时的错误文案（直接进 showInputBox 的 validateInput） */
    error?: string;
}

/** 解析端口数量输入：去空白、纯十进制整数、范围 min..max（含）；否则给出 strings.ts 文案 */
export function parsePortMax(input: string, min: number, max: number): PortMaxParsed {
    const s = String(input ?? '').trim();
    if (s.length === 0 || !/^\d+$/.test(s)) return { error: S.portMax.invalid(min, max) };
    const v = Number(s);
    if (!Number.isInteger(v) || v < min || v > max) return { error: S.portMax.invalid(min, max) };
    return { value: v };
}

/** 「当前上限」摘要（提示/控制器信息用；used 空数组显示“无”） */
export function portMaxSummary(max: number, slots: number, used: number[]): string {
    return S.portMax.summary(max, slots, used.length > 0 ? used.join(', ') : '无');
}
