// store.ts —— 面板数据仓库（docs/planA/15 T-19/T-20）
//
// 只做一件事：把调试通道推来的**事件**（`13` §4.4 ③）与一次性快照的应答，收敛成面板可直接渲染的
// 「最新值」。纪律：
//   * **事件驱动、不轮询**（FR-6.5）——快照仅在面板首次可见时取一次，之后全靠推送；
//   * **字段缺失即未知**：不猜、不补 0，UI 显示「未收到数据」；
//   * 订阅不可用（`13` D3 未就绪）时由外层标记 `subUnavailable`，面板**明示**而非静默空表。
//
// 本文件不依赖 vscode，可在 Node 下直接单测。

import { KxEvent } from './protocol';

export interface AxisSample {
    axis: number;
    bus_ok?: number;
    enabled?: number;
    idle?: number;
    alarm?: number;
    mpos?: number;
    dpos?: number;
    axis_status?: number;
    err_code?: number;
    /** 当前生效的脉冲当量（inc/mm；0 = 未设置，由脚本 UNITS 设置，v0.8.1） */
    inc_per_mm?: number;
}

export interface BusNode {
    index: number;
    axis_count?: number;
    status?: number;
    io?: number;
    aio?: number;
}

export interface BusSample {
    node_count: number;
    nodes: BusNode[];
}

/** Modbus 4x 寄存器（`mb_regs[256]`，见 FR-6.3） */
export const MB_REG_COUNT = 256;

/** D10「通讯状态」：一条对外连接（conn 事件） */
export interface ConnRow {
    port: number;         // 端口句柄
    kind: string;         // TCP_SERVER / TCP_CLIENT
    conn: boolean;        // 已连接
    listen: number;       // server：监听端口；client：0
    peerPort: number;     // server：客户端端口；client：远端端口
    target: string;
    peer: string;         // 对端 IP
    tag: string;          // PORT_INFO 用途
    role: string;         // PORT_INFO 主/从
}

/** D10：EtherCAT 从站明细 */
export interface BusSlaveRow {
    index: number;
    axis: number;         // -1 = 非轴从站
    online: boolean;
    alState: number;
    vid: number;
    pid: number;
    rev: number;
    name: string;
}

/** D10：EtherCAT 主站健康 */
export interface BusHealth {
    linkUp: boolean;
    slavesResponding: number;
    masterAl: number;
    slaveAl: number;
    slaveOnline: boolean;
    slaveOp: boolean;
}

export type PanelId = 'axes' | 'bus' | 'regs' | 'conn';

export class KxStatusStore {
    engine = '';
    axisCount = 0;
    connected = false;

    readonly axes = new Map<number, AxisSample>();
    bus: BusSample | undefined;
    regs: number[] = new Array(MB_REG_COUNT).fill(0);
    /** 是否收到过寄存器数据（全 0 与「没数据」必须区分） */
    regsLoaded = false;
    /** v0.8.6：控制器脚本访问过（读/写）的寄存器位图（mb 事件 `used`）——「已用寄存器」监视 */
    regsUsed: boolean[] = new Array(MB_REG_COUNT).fill(false);

    /** D10「通讯状态」：对外连接 + EtherCAT 主/从（conn 事件） */
    conns: ConnRow[] = [];
    connsLoaded = false;
    busHealth: BusHealth | undefined;
    busSlaves: BusSlaveRow[] = [];

    /** 订阅不可用（控制器未实现 `13` D3） */
    subUnavailable = false;
    subDegradeReason = '';

    onConnect(engine: string, axisCount: number): void {
        this.engine = engine;
        this.axisCount = axisCount;
        this.connected = true;
    }

    markDisconnected(): void {
        this.connected = false;
        this.axes.clear();
        this.bus = undefined;
        this.regs = new Array(MB_REG_COUNT).fill(0);
        this.regsLoaded = false;
        this.regsUsed = new Array(MB_REG_COUNT).fill(false);
        this.conns = [];
        this.connsLoaded = false;
        this.busHealth = undefined;
        this.busSlaves = [];
        this.subUnavailable = false;
        this.subDegradeReason = '';
    }

    markSubUnavailable(reason: string): void {
        this.subUnavailable = true;
        this.subDegradeReason = reason;
    }

    /** 订阅恢复可用（手动刷新后重试订阅） */
    markSubAvailable(): void {
        this.subUnavailable = false;
        this.subDegradeReason = '';
    }

    /** `axis.snapshot` 应答 → 收敛到同一个轴样本（一次性快照，FR-6.5 首次可见时取一次） */
    applyAxisSnapshot(axis: number, raw: unknown): void {
        this.upsertAxis(axisFromObj(axis, asObj(raw)));
    }

    /** 轴快照 / `axis` 事件同 schema（见 `16` §2.4 与本文 T-19 约定） */
    upsertAxis(s: AxisSample): void {
        const prev = this.axes.get(s.axis) ?? { axis: s.axis };
        this.axes.set(s.axis, { ...prev, ...stripUndefined(s) });
    }

    mergeRegs(start: number, values: number[]): void {
        for (let i = 0; i < values.length; i++) {
            const idx = start + i;
            if (idx >= 0 && idx < this.regs.length) {
                this.regs[idx] = values[i];
            }
        }
        this.regsLoaded = true;
    }

    /**
     * v0.8.6：mb 事件 `used` = 4×16 位十六进制（第 i 位 = 寄存器 i 被脚本访问过）。
     * 逐半字节解析——不经 53 位精度的整数转换，256 位全部准确。
     */
    mergeUsed(words: unknown): void {
        if (!Array.isArray(words) || words.length !== 4) {
            return;   // 旧固件无 `used`：保持原集合（不重置，避免闪烁）
        }
        for (let w = 0; w < 4; w++) {
            const hex = String(words[w] ?? '');
            if (hex.length !== 16) {
                continue;
            }
            for (let c = 0; c < 16; c++) {
                const nib = parseInt(hex.charAt(c), 16);
                if (!Number.isFinite(nib) || nib === 0) {
                    continue;
                }
                for (let b = 0; b < 4; b++) {
                    if (nib & (1 << b)) {
                        this.regsUsed[w * 64 + (15 - c) * 4 + b] = true;
                    }
                }
            }
        }
    }

    /** 消费一条事件，返回**受影响的面板**（供 UI 按需刷新，避免整树重绘） */
    update(ev: KxEvent): PanelId[] {
        switch (ev.e) {
            case 'axis': {
                const axis = numOr(ev['axis'], 0);
                this.upsertAxis(axisFromObj(axis, ev));
                return ['axes'];
            }
            case 'bus': {
                this.bus = {
                    node_count: numOr(ev['node_count'], 0),
                    nodes: Array.isArray(ev['nodes'])
                        ? (ev['nodes'] as unknown[])
                              .map(asObj)
                              .map((n, i) => ({
                                  index: numOr(n['index'], i),
                                  axis_count: optNum(n['axis_count']),
                                  status: optNum(n['status']),
                                  io: optNum(n['io']),
                                  aio: optNum(n['aio']),
                              }))
                        : [],
                };
                return ['bus'];
            }
            case 'conn': {
                const rows = Array.isArray(ev['conns']) ? (ev['conns'] as unknown[]) : [];
                this.conns = rows.map((c) => {
                    const o = asObj(c);
                    return {
                        port: numOr(o['port'], -1),
                        kind: typeof o['kind'] === 'string' ? String(o['kind']) : '',
                        conn: o['conn'] === true,
                        listen: numOr(o['listen'], 0),
                        peerPort: numOr(o['peer_port'], 0),
                        target: typeof o['target'] === 'string' ? String(o['target']) : '',
                        peer: typeof o['peer'] === 'string' ? String(o['peer']) : '',
                        tag: typeof o['tag'] === 'string' ? String(o['tag']) : '',
                        role: typeof o['role'] === 'string' ? String(o['role']) : '',
                    };
                });
                this.connsLoaded = true;
                const bus = asObj(ev['bus']);
                this.busHealth = {
                    linkUp: numOr(bus['link_up'], 0) === 1,
                    slavesResponding: numOr(bus['slaves_responding'], 0),
                    masterAl: numOr(bus['master_al'], 0),
                    slaveAl: numOr(bus['slave_al'], 0),
                    slaveOnline: numOr(bus['slave_online'], 0) === 1,
                    slaveOp: numOr(bus['slave_op'], 0) === 1,
                };
                const slaves = Array.isArray(bus['slaves']) ? (bus['slaves'] as unknown[]) : [];
                this.busSlaves = slaves.map((s2) => {
                    const o = asObj(s2);
                    return {
                        index: numOr(o['index'], 0),
                        axis: numOr(o['axis'], -1),
                        online: numOr(o['online'], 0) === 1,
                        alState: numOr(o['al_state'], 0),
                        vid: numOr(o['vid'], 0),
                        pid: numOr(o['pid'], 0),
                        rev: numOr(o['rev'], 0),
                        name: typeof o['name'] === 'string' ? String(o['name']) : '',
                    };
                });
                return ['conn'];
            }
            case 'mb': {
                const start = numOr(ev['start'], 0);
                const values = Array.isArray(ev['regs']) ? (ev['regs'] as unknown[]).map(numOr0) : [];
                this.mergeRegs(start, values);
                this.mergeUsed(ev['used']);
                return ['regs'];
            }
            default:
                return [];
        }
    }
}

function asObj(v: unknown): Record<string, unknown> {
    return typeof v === 'object' && v !== null ? (v as Record<string, unknown>) : {};
}

/**
 * 轴字段的**唯一**收敛点：`axis.snapshot` 应答与 `axis` 事件同 schema（`16` §2.4）。
 * 字段缺失即 `undefined`（面板显示「未收到数据」），**不补 0** 冒充正常值。
 */
function axisFromObj(axis: number, o: Record<string, unknown>): AxisSample {
    return {
        axis,
        bus_ok: optNum(o['bus_ok']),
        enabled: optNum(o['enabled']),
        idle: optNum(o['idle']),
        alarm: optNum(o['alarm']),
        mpos: optNum(o['mpos']),
        dpos: optNum(o['dpos']),
        axis_status: optNum(o['axis_status']),
        err_code: optNum(o['err_code']),
        inc_per_mm: optNum(o['inc_per_mm']),
    };
}

function optNum(v: unknown): number | undefined {
    return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}

function numOr(v: unknown, def: number): number {
    const n = optNum(v);
    return n === undefined ? def : n;
}

function numOr0(v: unknown): number {
    return numOr(v, 0);
}

/** 只保留有值的键，避免 `{...prev, ...s}` 用 undefined 覆盖掉已有数据 */
function stripUndefined<T extends object>(o: T): Partial<T> {
    const out: Partial<T> = {};
    for (const [k, v] of Object.entries(o)) {
        if (v !== undefined) {
            (out as Record<string, unknown>)[k] = v;
        }
    }
    return out;
}
