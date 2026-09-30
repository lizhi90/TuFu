// connection.ts —— 调试通道连接层（JSON-Lines over TCP，见 docs/planA/13 §4）
//
// 职责：TCP 连接、按行分帧、请求/应答按 id 关联、超时、事件分发。
// 事件（供上层监听）：
//   'state'  (s: 'connected' | 'disconnected')
//   'event'  (ev: KxEvent)
//   'raw'    (dir: 'C' | 'S', line: string)   原始报文，供排查
//   'log'    (msg: string)                    内部诊断
//   'error'  (err: Error)                     传输层错误

import { EventEmitter } from 'node:events';
import * as net from 'node:net';
import { StringDecoder } from 'node:string_decoder';

import { ErrorCodes, isEvent, isResponse, RpcRequest, RpcResponse, KxEvent, KxRpcError } from './protocol';
import { S } from './strings';

interface Pending {
    resolve: (v: unknown) => void;
    reject: (e: Error) => void;
    timer: NodeJS.Timeout;
}

export interface KxConnectionOptions {
    host: string;
    port: number;
    token?: string;
    timeoutMs?: number;
}

export class KxConnection extends EventEmitter {
    private socket: net.Socket | undefined;
    private buffer = '';
    private nextId = 1;
    private readonly pending = new Map<number, Pending>();

    private readonly host: string;
    private readonly port: number;
    private readonly token: string;
    private readonly timeoutMs: number;

    constructor(opts: KxConnectionOptions) {
        super();
        this.host = opts.host;
        this.port = opts.port;
        this.token = opts.token ?? '';
        this.timeoutMs = opts.timeoutMs ?? 5000;
    }

    get connected(): boolean {
        return this.socket !== undefined && !this.socket.destroyed;
    }

    connect(): Promise<void> {
        if (this.connected) {
            return Promise.resolve();
        }
        return this.openSocket().then(() => {
            this.startKeepalive();
            return this.authenticate();
        });
    }

    /**
     * 保活（V-C-01 拍板）：每 10s 发一条 `sys.ping`。
     * 板端会话有 **60s 空闲超时**（半开连接会永久占用单客户端槽位），保活同时兼作
     * 「我还在线」的心跳——静置观看面板也不会被板端断开。
     */
    private keepaliveTimer: NodeJS.Timeout | undefined;

    private startKeepalive(): void {
        this.stopKeepalive();
        this.keepaliveTimer = setInterval(() => {
            if (this.socket && !this.socket.destroyed) {
                this.request('sys.ping', {}, this.timeoutMs).catch(() => undefined);
            }
        }, 10_000);
    }

    private stopKeepalive(): void {
        if (this.keepaliveTimer) {
            clearInterval(this.keepaliveTimer);
            this.keepaliveTimer = undefined;
        }
    }

    /**
     * 可选鉴权：docs/planA/13 §4.5「一次性 token（连接首帧校验）」。
     * 该方法名/字段为**待冻结项**（docs/planA/16 §7）：服务端未实现时返回 `UNKNOWN_METHOD`，
     * 此时按「未启用鉴权」放行（向前兼容）；返回 `AUTH_FAILED` 则**硬失败**，不静默降级。
     */
    private authenticate(): Promise<void> {
        if (this.token.length === 0) {
            return Promise.resolve();
        }
        return this.request('auth', { token: this.token }, this.timeoutMs).then(
            () => undefined,
            (e: Error) => {
                if (e.message.includes('UNKNOWN_METHOD')) {
                    this.emit('log', S.connection.warnNoAuth);
                    return;
                }
                throw e;
            },
        );
    }

    private openSocket(): Promise<void> {
        return new Promise<void>((resolve, reject) => {
            const socket = net.createConnection({ host: this.host, port: this.port });
            this.socket = socket;

            const onError = (err: Error): void => {
                cleanup();
                this.socket = undefined;
                reject(err);
            };
            const onConnect = (): void => {
                cleanup();
                socket.on('data', (chunk: Buffer) => this.onData(chunk));
                socket.on('error', (err: Error) => this.emit('error', err));
                socket.on('close', () => this.onClose());
                this.emit('state', 'connected');
                resolve();
            };
            const cleanup = (): void => {
                socket.removeListener('error', onError);
                socket.removeListener('connect', onConnect);
            };

            socket.once('error', onError);
            socket.once('connect', onConnect);
        });
    }

    disconnect(): void {
        this.stopKeepalive();
        const socket = this.socket;
        this.socket = undefined;
        this.buffer = '';
        this.failAll(new Error(S.connection.closed));
        if (socket) {
            socket.removeAllListeners();
            socket.destroy();
        }
        this.emit('state', 'disconnected');
    }

    /** 发送请求；成功 resolve 应答的 r，失败 reject（含错误码） */
    request(method: string, params?: unknown, timeoutMs?: number): Promise<unknown> {
        const socket = this.socket;
        if (!socket || socket.destroyed) {
            return Promise.reject(new Error(S.connection.notConnected));
        }
        const id = this.nextId++;
        const req: RpcRequest = { id, m: method };
        if (params !== undefined) {
            req.p = params;
        }
        const line = JSON.stringify(req);
        this.emit('raw', 'C', line);

        const timeout = timeoutMs ?? this.timeoutMs;
        return new Promise<unknown>((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new KxRpcError(ErrorCodes.TIMEOUT, S.connection.timeout(method, timeout)));
            }, timeout);
            this.pending.set(id, { resolve, reject, timer });
            socket.write(line + '\n', (err) => {
                if (err) {
                    const p = this.pending.get(id);
                    if (p) {
                        clearTimeout(p.timer);
                        this.pending.delete(id);
                    }
                    reject(err);
                }
            });
        });
    }

    /**
     * UTF-8 安全解码器（v0.8.13 修复）：TCP 分片可能切在多字节字符（中文）中间，
     * `chunk.toString('utf8')` 会把被截断的序列变成 U+FFFD（内容永久损坏而 JSON 仍可解析）——
     * 表现为「拉取成功但本地文件与控制器不一致」。StringDecoder 会缓存半个序列到下一块。
     */
    private readonly decoder = new StringDecoder('utf8');

    private onData(chunk: Buffer): void {
        this.buffer += this.decoder.write(chunk);
        let idx = this.buffer.indexOf('\n');
        while (idx >= 0) {
            const line = this.buffer.slice(0, idx).replace(/\r$/, '');
            this.buffer = this.buffer.slice(idx + 1);
            if (line.trim().length > 0) {
                this.emit('raw', 'S', line);
                this.handleLine(line);
            }
            idx = this.buffer.indexOf('\n');
        }
    }

    private handleLine(line: string): void {
        let obj: unknown;
        try {
            obj = JSON.parse(line);
        } catch {
            this.emit('log', S.connection.warnBadJson(line));
            return;
        }
        if (isEvent(obj)) {
            this.emit('event', obj as KxEvent);
            return;
        }
        if (isResponse(obj)) {
            this.resolveResponse(obj as RpcResponse);
            return;
        }
        this.emit('log', S.connection.warnUnknownMessage(line));
    }

    private resolveResponse(resp: RpcResponse): void {
        const p = this.pending.get(resp.id);
        if (!p) {
            this.emit('log', S.connection.warnUnmatchedId(String(resp.id)));
            return;
        }
        this.pending.delete(resp.id);
        clearTimeout(p.timer);
        if (resp.ok) {
            p.resolve(resp.r);
        } else {
            const code = resp.err?.code ?? 'UNKNOWN';
            const msg = resp.err?.msg ?? S.connection.unknownError;
            p.reject(new KxRpcError(code, msg, resp.err?.line));
        }
    }

    private onClose(): void {
        this.stopKeepalive();
        this.socket = undefined;
        this.failAll(new Error(S.connection.closedByPeer));
        this.emit('state', 'disconnected');
    }

    private failAll(err: Error): void {
        for (const p of this.pending.values()) {
            clearTimeout(p.timer);
            p.reject(err);
        }
        this.pending.clear();
    }
}
