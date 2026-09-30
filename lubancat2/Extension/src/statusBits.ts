// statusBits.ts —— 轴状态位表与解码（docs/planA/15 §4.6）
//
// 单一事实来源：控制器 `state.h` 的 `AxisStatusBit`。
// 纪律（§4.6）：**只有列出的位有数据源**；其余位在本控制器上恒 0，
// 面板必须标注「空数据源」，**不得**把恒 0 渲染成「正常」。
//
// 本文件不依赖 vscode，可在 Node 下直接单测（见 test/connection.smoke.cjs）。

import { S } from './strings';

export interface AxisStatusBit {
    bit: number;
    name: string;
}

/** 与 `state.h` `AxisStatusBit` 逐位对齐；顺序即 UI 展示顺序 */
export const AXIS_STATUS_BITS: ReadonlyArray<AxisStatusBit> = [
    { bit: 1, name: '随动误差告警' },
    { bit: 2, name: '通讯错' },
    { bit: 3, name: '驱动器报错' },
    { bit: 8, name: '随动误差出错' },
    { bit: 22, name: '告警输入' },
];

export interface DecodedBit extends AxisStatusBit {
    on: boolean;
}

export interface DecodedAxisStatus {
    /** 已定义的位及其状态 */
    bits: DecodedBit[];
    /** 原始值中**置位但没有数据源**的位（正常应为空；非空说明控制器新增了位，需同步位表） */
    unknownBits: number[];
    /** 十六进制展示（便于与 ZDevelop / 手册对照） */
    hex: string;
}

export function decodeAxisStatus(raw: number): DecodedAxisStatus {
    const v = Number.isFinite(raw) ? Math.trunc(raw) : 0;
    const known = new Set(AXIS_STATUS_BITS.map((b) => b.bit));
    const bits = AXIS_STATUS_BITS.map((b) => ({ ...b, on: (v & (1 << b.bit)) !== 0 }));

    const unknownBits: number[] = [];
    // 逐位扫描：仅收集**置位**的未知位（恒 0 的未知位不构成信息，不逐个列出）
    for (let i = 0; i < 32; i++) {
        if ((v & (1 << i)) !== 0 && !known.has(i)) {
            unknownBits.push(i);
        }
    }
    return { bits, unknownBits, hex: '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0') };
}

/** EtherCAT AL 状态（`status` 字段）显示名；未知值原样返回，不自造语义 */
export function alStateName(status: number): string {
    switch (status) {
        case 1:
            return 'INIT';
        case 2:
            return 'PREOP';
        case 3:
            return 'BOOT';
        case 4:
            return 'SAFEOP';
        case 8:
            return 'OP';
        default:
            return String(status);
    }
}

/** 布尔型轴标志（bus_ok/enabled/idle/alarm）的展示：未知不猜 */
export function flagText(v: number | undefined, onText: string, offText: string): string {
    if (v === undefined) {
        return S.common.noData;
    }
    return v !== 0 ? onText : offText;
}

export function numText(v: number | undefined, digits = 3): string {
    if (v === undefined) {
        return S.common.noData;
    }
    return Number.isInteger(v) ? String(v) : v.toFixed(digits);
}
