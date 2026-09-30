// subscribe.ts —— 订阅管理（docs/planA/15 T-18 / FR-6.4·6.5、NFR-3）
//
// 职责：
//   * **引用计数**：多个消费者（输出面板、轴面板、总线面板…）可各自 retain 同一 topic，
//     计数归零才真正 `unsubscribe`，避免互相踩踏；
//   * **合并请求**：同一轮（同一 tick）内的多次 retain/release 合并成**一次** RPC；
//   * **限频**：`hz` 统一夹在 [1, maxHz]（NFR-3：默认 ≤20Hz）；
//   * **降级不伪装**：控制器未实现订阅（`13` D3 未就绪）时返回 `NOT_SUPPORTED`/`UNKNOWN_METHOD`，
//     标记 `unavailable` 供 UI 明示，而不是静默假装已推送。
//
// 本文件不依赖 vscode，可在 Node 下直接单测。

import { KxConnection } from './connection';
import { ErrorCodes, KxRpcError } from './protocol';

/** 可降级为「控制器未提供订阅」的错误码 */
const DEGRADE_CODES: ReadonlySet<string> = new Set([ErrorCodes.NOT_SUPPORTED, ErrorCodes.UNKNOWN_METHOD]);

export function clampHz(hz: number, maxHz = 50): number {
    if (!Number.isFinite(hz)) {
        return 20;
    }
    return Math.min(Math.max(Math.trunc(hz), 1), maxHz);
}

export class SubscribeManager {
    private readonly counts = new Map<string, number>();
    private subscribed = new Set<string>();
    private flushTimer: NodeJS.Timeout | undefined;
    private flushing = false;
    private dirty = false;
    private unavailable = false;
    private disabled = false;
    private disposed = false;
    private lastError = '';

    constructor(
        private readonly conn: KxConnection,
        private readonly hz: number,
        private readonly onDegrade?: (reason: string) => void,
    ) {}

    /** 服务端已确认订阅的 topic */
    get active(): string[] {
        return [...this.subscribed];
    }

    /** 订阅是否不可用（控制器未实现 `13` D3 / 通道未连接） */
    get isUnavailable(): boolean {
        return this.unavailable;
    }

    get degradeReason(): string {
        return this.lastError;
    }

    /** 标记为需要某 topic（+1） */
    retain(topics: string[]): void {
        for (const t of topics) {
            this.counts.set(t, (this.counts.get(t) ?? 0) + 1);
        }
        this.schedule();
    }

    /** 释放某 topic（-1，归零则真正退订） */
    release(topics: string[]): void {
        for (const t of topics) {
            const n = (this.counts.get(t) ?? 0) - 1;
            if (n <= 0) {
                this.counts.delete(t);
            } else {
                this.counts.set(t, n);
            }
        }
        this.schedule();
    }

    /**
     * 直接标记订阅不可用（例如 `sys.info.caps` 未声明 `d3`），**不发任何 RPC**。
     * 对应 `16` §7.1「能力声明：插件启动即定 UI」；粘性生效，直到 `reset()`（重连）。
     */
    disable(reason: string): void {
        this.disabled = true;
        this.unavailable = true;
        this.lastError = reason;
        this.counts.clear();
        this.subscribed.clear();
    }

    /** 断线后清理（连接层已关，服务端订阅随连接一起消失） */
    reset(): void {
        this.subscribed.clear();
        this.unavailable = false;
        this.disabled = false;
        this.lastError = '';
    }

    /**
     * 重新对齐一次：丢弃「已订阅」本地视图后按现有引用计数重发差量。
     * 用于手动刷新 / 重连后（也解掉 `disable()` 的粘性，允许再试一次）。
     */
    resubscribe(): void {
        this.subscribed.clear();
        this.unavailable = false;
        this.disabled = false;
        this.lastError = '';
        this.schedule();
    }

    dispose(): void {
        this.disposed = true;
        if (this.flushTimer) {
            clearTimeout(this.flushTimer);
            this.flushTimer = undefined;
        }
        this.counts.clear();
        this.subscribed.clear();
    }

    private schedule(): void {
        if (this.disposed || this.disabled || this.flushTimer) {
            return;
        }
        // 合并同一 tick 内的多次变更（面板可见性切换常常一次动多个 topic）
        this.flushTimer = setTimeout(() => {
            this.flushTimer = undefined;
            void this.flush();
        }, 0);
    }

    private async flush(): Promise<void> {
        if (this.flushing) {
            this.dirty = true;
            return;
        }
        this.flushing = true;
        try {
            do {
                this.dirty = false;
                await this.applyOnce();
            } while (this.dirty && !this.disposed);
        } finally {
            this.flushing = false;
        }
    }

    private async applyOnce(): Promise<void> {
        const desired = new Set(this.counts.keys());
        const add = [...desired].filter((t) => !this.subscribed.has(t));
        const del = [...this.subscribed].filter((t) => !desired.has(t));
        if (add.length === 0 && del.length === 0) {
            return;
        }
        if (!this.conn.connected) {
            return;
        }
        try {
            if (del.length > 0) {
                await this.conn.request('unsubscribe', { topics: del });
                for (const t of del) {
                    this.subscribed.delete(t);
                }
            }
            if (add.length > 0) {
                await this.conn.request('subscribe', { topics: add, hz: clampHz(this.hz) });
                for (const t of add) {
                    this.subscribed.add(t);
                }
            }
            if (this.unavailable) {
                this.unavailable = false;
                this.lastError = '';
            }
        } catch (e) {
            const code = e instanceof KxRpcError ? e.code : '';
            if (DEGRADE_CODES.has(code)) {
                // 控制器未实现 D3：本地记为「已订阅」以避免反复重试刷日志，同时暴露降级状态给 UI
                this.unavailable = true;
                this.lastError = e instanceof Error ? e.message : String(e);
                for (const t of add) {
                    this.subscribed.add(t);
                }
                this.onDegrade?.(this.lastError);
                return;
            }
            this.lastError = e instanceof Error ? e.message : String(e);
        }
    }
}
