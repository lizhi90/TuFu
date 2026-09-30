// protocol.ts —— 调试通道报文类型与判定（依据 docs/planA/13 §4.4 / docs/planA/16）
//
// 三类报文，同一条 TCP 连接上以 JSON-Lines 承载（每行一条 JSON）：
//   请求 {id,m,p}  /  应答 {id,ok,r|err}  /  事件 {e,t,...}
//
// 本文件只描述类型与判定，不含传输实现（见 connection.ts）。

/** 请求：{ id, m, p? } */
export interface RpcRequest {
    id: number;
    m: string;
    p?: unknown;
}

/** 错误体：{ code, msg, line? } */
export interface RpcError {
    code: string;
    msg: string;
    line?: number;
}

/** 应答：{ id, ok, r? | err? } */
export interface RpcResponse {
    id: number;
    ok: boolean;
    r?: unknown;
    err?: RpcError;
}

/** 事件：{ e, t, ... }（服务端主动推送，无 id） */
export interface KxEvent {
    e: string;
    t: number;
    [key: string]: unknown;
}

/** 错误码（稳定字符串；插件分支只看 code，不解析 msg）。见 docs/planA/16 §3 */
export const ErrorCodes = {
    BAD_REQUEST: 'BAD_REQUEST',
    UNKNOWN_METHOD: 'UNKNOWN_METHOD',
    BAD_PARAM: 'BAD_PARAM',
    AUTH_FAILED: 'AUTH_FAILED',
    ENGINE_MISMATCH: 'ENGINE_MISMATCH',
    COMPILE_ERROR: 'COMPILE_ERROR',
    RUNTIME_ERROR: 'RUNTIME_ERROR',
    BUSY: 'BUSY',
    NOT_SUPPORTED: 'NOT_SUPPORTED',
    TIMEOUT: 'TIMEOUT',
} as const;

export type ErrorCode = (typeof ErrorCodes)[keyof typeof ErrorCodes];

/**
 * 带错误码的 RPC 异常。
 * 纪律（docs/planA/16 §3）：插件分支**只看 `code`**，不解析 `msg` 文案；`line` 仅编译/运行错误有值。
 */
export class KxRpcError extends Error {
    readonly code: string;
    readonly line: number | undefined;

    constructor(code: string, msg: string, line?: number) {
        super(`[${code}] ${msg}${line !== undefined ? ` (line ${line})` : ''}`);
        this.name = 'KxRpcError';
        this.code = code;
        this.line = line;
    }
}

/** 是否为应答报文 */
export function isResponse(obj: unknown): obj is RpcResponse {
    return (
        typeof obj === 'object' &&
        obj !== null &&
        'id' in obj &&
        'ok' in obj
    );
}

/** 是否为事件报文（有 e、无 id） */
export function isEvent(obj: unknown): obj is KxEvent {
    return (
        typeof obj === 'object' &&
        obj !== null &&
        'e' in obj &&
        !('id' in obj)
    );
}
