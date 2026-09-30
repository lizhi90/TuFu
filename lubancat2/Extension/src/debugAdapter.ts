// debugAdapter.ts —— Debug Adapter（DAP）
//
// 对应 docs/planA/15 §8：
//   T-13 会话骨架（initialize / launch / configurationDone / disconnect）
//   T-14 变量/监视（FR-4.5/4.6/4.7 → scopes / variables / setVariable / evaluate）
//   T-15 输出与终止映射（FR-4.9/4.10 → output / stopped / exited / terminated，映射表见 §6.3）
// T-16 热更新（卡 `13` D4）、T-17 断点/单步（卡 `13` D5）见后续迭代，当前**降级不伪装**。
//
// 形态：**同包内 TS**（开工决议表 D-02）——以 `DebugAdapterInlineImplementation` 嵌入扩展进程，
// 与扩展共用同一份连接层/配置/协议类型，不再单起 adapter 进程。
//
// 协议分层：VS Code ──DAP(in-process)──▶ 本类 ──JSON-Lines(TCP)──▶ 控制器 DebugServer（docs/planA/13）
//
// 「入口暂停」说明：D5 未就绪时没有真正的挂起点，而 FR-4.5 变量视图**只在「已停止」状态**下才会被
// VS Code 请求。因此脚本尚未运行时停一次（DAP 标准的 entry stop，真实状态，非伪造），
// 让用户先看/改变量再 `continue` 启动。

import * as vscode from 'vscode';

import { readConfig } from './config';
import { KxConnection } from './connection';
import { baseName, d5Notice, parseLiteral, parseVarLine, typeOfValue, VarItem } from './dapPure';
import { resolveEngine } from './engineResolve';
import { engineDisplay } from './engineRule';
import { KxRpcError } from './protocol';
import { S } from './strings';

// --- 最小 DAP 类型（@types/vscode 不导出 DebugProtocol 命名空间，就地声明子集） ---

interface DapRequest {
    seq: number;
    type: 'request';
    command: string;
    arguments?: unknown;
}

interface DapSetBreakpointsArguments {
    source?: { name?: string; path?: string };
    breakpoints?: Array<{ line: number; column?: number; condition?: string }>;
}

/** `launch.json` 的 launch 参数（键名对齐 config/app.conf，见 D-04） */
export interface KxLaunchArgs {
    host?: string;
    port?: number;
    token?: string;
    engine?: string;
    program?: string;
    /** 脚本启动前是否先停一次（默认 true：便于 FR-4.5 变量视图可用） */
    stopOnEntry?: boolean;
    waitBus?: boolean;
    maxSteps?: number;
    trace?: boolean;
}

interface SysInfoLite {
    ver?: string;
    engine?: string;
    axis_count?: number;
    caps?: string[];
}

interface KxScriptEvent {
    e: string;
    s?: string;
    lvl?: string;
    status?: string;
    steps?: number;
    error_line?: number;
    /** D5 挂起/单步时的当前行（字段名待 `13` 冻结，见 `15` §8.5 注） */
    line?: number;
}

/** none=未启动 / entry=入口暂停 / running=运行中 / paused=D5 挂起 / error=异常停止 / ended=已结束 */
type ThreadState = 'none' | 'entry' | 'running' | 'paused' | 'error' | 'ended';

interface StopInfo {
    line: number;
    detail: string;
}

const THREAD_ID = 1;
const LOCALS_REF = 1;
/** `variables` 一次最多 enrich 的变量数（逐项 `var.get` 取类型，避免大变量表拖慢面板） */
const MAX_VARS = 64;

export class KxDebugAdapter implements vscode.DebugAdapter {
    private readonly emitter = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
    readonly onDidSendMessage: vscode.Event<vscode.DebugProtocolMessage> = this.emitter.event;

    private seq = 0;
    private conn: KxConnection | undefined;
    private caps = new Set<string>();
    private controllerEngine: string | undefined;
    private explicitEngine: string | undefined;
    private program: string | undefined;
    private programSrc: string | undefined;
    private state: ThreadState = 'none';
    private stopInfo: StopInfo | undefined;
    private stopOnEntry = true;
    private launched = false;
    private lastStatus = '';
    private breakpointNoticeSent = false;
    /** 本地登记的行断点（D5 未就绪时仅用于「未校验」提示；就绪时与控制器 `breakpoint.*` 同步） */
    private breakpointLines: number[] = [];

    handleMessage(message: vscode.DebugProtocolMessage): void {
        const req = message as DapRequest;
        if (req.type !== 'request') {
            return;
        }
        void this.dispatch(req).catch((e: unknown) => this.respondError(req, errText(e)));
    }

    dispose(): void {
        this.closeConnection();
        this.emitter.dispose();
    }

    private async dispatch(req: DapRequest): Promise<void> {
        switch (req.command) {
            case 'initialize':
                this.respond(req, {
                    supportsConfigurationDoneRequest: true,
                    supportsTerminateRequest: true,
                    supportTerminateDebuggee: true,
                    // FR-4.6 已实装（var.set），故据实声明
                    supportsSetVariable: true,
                    supportsEvaluateForHovers: true,
                    // D5 未就绪：以下能力**据实为 false**，不让 UI 假装支持（§4.4 降级）
                    supportsConditionalBreakpoints: false,
                    supportsFunctionBreakpoints: false,
                    supportsStepBack: false,
                    supportsRestartRequest: false,
                    exceptionBreakpointFilters: [],
                });
                break;
            case 'launch':
                await this.launch(req, (req.arguments ?? {}) as KxLaunchArgs);
                break;
            case 'configurationDone':
                this.respond(req);
                void this.afterConfiguration();
                break;
            case 'threads':
                this.respond(req, { threads: [{ id: THREAD_ID, name: this.threadName() }] });
                break;
            case 'stackTrace':
                this.stackTrace(req);
                break;
            case 'scopes':
                this.scopes(req);
                break;
            case 'variables':
                await this.variables(req);
                break;
            case 'setVariable':
                await this.setVariable(req);
                break;
            case 'evaluate':
                await this.evaluate(req);
                break;
            case 'continue':
                await this.continueRun(req);
                break;
            case 'next':
            case 'stepIn':
            case 'stepOut':
                await this.stepRun(req);
                break;
            case 'pause':
                await this.pauseRun(req);
                break;
            case 'setBreakpoints':
                await this.setBreakpoints(req);
                break;
            case 'setExceptionBreakpoints':
                this.respond(req, { breakpoints: [] });
                break;
            case 'source':
                this.respond(req, { content: this.programSrc ?? '', mimeType: 'text/plain' });
                break;
            case 'terminate':
                await this.terminate(req);
                break;
            case 'disconnect':
                await this.disconnect(req);
                break;
            case 'trace':
                this.respond(req);
                break;
            default:
                this.respondError(req, S.debugAdapter.notSupported(req.command));
                break;
        }
    }

    // -----------------------------------------------------------------------
    // T-13 会话骨架
    // -----------------------------------------------------------------------

    private async launch(req: DapRequest, args: KxLaunchArgs): Promise<void> {
        const cfg = readConfig();
        const host = args.host ?? cfg.host;
        const port = args.port ?? cfg.port;
        const token = args.token ?? cfg.token;

        this.stopOnEntry = args.stopOnEntry ?? true;
        this.explicitEngine = args.engine;
        this.state = 'none';
        this.stopInfo = undefined;
        this.breakpointNoticeSent = false;
        this.breakpointLines = [];
        this.program = await this.resolveProgram(args.program);

        this.closeConnection();
        const conn = new KxConnection({ host, port, token, timeoutMs: cfg.requestTimeoutMs });
        conn.on('event', (ev) => this.onKxEvent(ev as KxScriptEvent));
        conn.on('log', (m: string) => this.output(m, 'console'));
        conn.on('error', (e: Error) => this.output(S.debugAdapter.transportError(e.message), 'stderr'));
        conn.on('state', (s: string) => {
            if (s === 'disconnected' && this.state !== 'ended') {
                this.output(S.debugAdapter.channelClosed, 'stderr');
            }
        });
        this.conn = conn;

        try {
            await conn.connect();
            const info = ((await conn.request('sys.info')) ?? {}) as SysInfoLite;
            this.controllerEngine = typeof info.engine === 'string' ? info.engine.toLowerCase() : undefined;
            this.caps = new Set((info.caps ?? []).map((c) => String(c).toLowerCase()));
            const caps = this.caps.size > 0 ? [...this.caps].join(',') : S.debugAdapter.capsUndeclared;
            this.output(
                S.debugAdapter.connectedLine(
                    host,
                    port,
                    info.ver ?? '?',
                    info.engine ?? '?',
                    String(info.axis_count ?? '?'),
                    caps,
                ),
                'console',
            );
            if (!this.d5Ready()) {
                this.output(S.debugAdapter.noD5, 'console');
            }
            if (!this.d4Ready()) {
                this.output(S.debugAdapter.noD4, 'console');
            }
            this.output(
                S.debugAdapter.launchArgs(
                    this.program ?? S.debugAdapter.programUnresolved,
                    args.engine ?? S.debugAdapter.engineInferred,
                    String(this.stopOnEntry),
                    String(args.waitBus ?? true),
                    String(args.maxSteps ?? 5000000),
                    String(args.trace ?? false),
                ),
                'console',
            );
        } catch (e) {
            this.closeConnection();
            const msg = e instanceof KxRpcError ? e.message : errText(e);
            this.respondError(req, S.debugAdapter.connectFailed(msg));
            return;
        }

        this.launched = true;
        this.respond(req);
        this.sendEvent('initialized');
    }

    private async afterConfiguration(): Promise<void> {
        if (!this.launched) {
            return;
        }
        if (this.stopOnEntry) {
            this.enterEntryStop();
            return;
        }
        await this.startRun();
    }

    /** 入口暂停：真实状态（脚本尚未运行），用于露出 FR-4.5 变量视图 */
    private enterEntryStop(): void {
        this.state = 'entry';
        this.stopInfo = { line: 1, detail: S.debugAdapter.entryPauseDetail };
        this.output(S.debugAdapter.entryPauseOutput(this.d5Ready()), 'console');
        this.sendEvent('stopped', {
            reason: 'entry',
            threadId: THREAD_ID,
            allThreadsStopped: true,
            description: S.debugAdapter.entryPauseLabel,
            text: S.debugAdapter.entryPauseDetail,
        });
    }

    private async continueRun(req: DapRequest): Promise<void> {
        if (this.state === 'entry' || this.state === 'none') {
            this.respond(req, { allThreadsContinued: true });
            await this.startRun();
            return;
        }
        if (this.state === 'running') {
            this.respond(req, { allThreadsContinued: true });
            return;
        }
        if (this.state === 'paused') {
            // D5 就绪：真正的「继续」（T-17）
            if (!this.d5Ready()) {
                this.respondError(req, d5Notice('continue'));
                return;
            }
            const conn = this.conn;
            if (!conn) {
                this.respondError(req, S.debugAdapter.channelNotConnected);
                return;
            }
            try {
                await conn.request('script.resume');
            } catch (e) {
                this.respondError(req, S.debugAdapter.resumeFailed(errText(e)));
                return;
            }
            this.state = 'running';
            this.respond(req, { allThreadsContinued: true });
            // 不再自发 `continued`：以控制器的 `script READY` 事件为准（单一事实来源）
            return;
        }
        this.respondError(
            req,
            S.debugAdapter.endedCannotResume(this.lastStatus || this.state, this.d5Ready()),
        );
    }

    /**
     * T-17：单步（`next` / `stepIn` / `stepOut`）。
     * 作用域是行级，三者语义相同（协议只有 `script.step`，见 `15` §8.5 注）。
     */
    private async stepRun(req: DapRequest): Promise<void> {
        if (!this.d5Ready()) {
            this.respondError(req, d5Notice(req.command));
            return;
        }
        if (this.state !== 'paused' && this.state !== 'entry') {
            this.respondError(req, S.debugAdapter.stepOnlyPaused);
            return;
        }
        const conn = this.conn;
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        try {
            await conn.request('script.step');
        } catch (e) {
            this.respondError(req, S.debugAdapter.stepFailed(errText(e)));
            return;
        }
        this.respond(req);
        // 真正的 `stopped` 由控制器回推的 `script PAUSED` 事件产生
    }

    /** T-17：运行中挂起（`pause`）。已在挂起态则视为成功（幂等） */
    private async pauseRun(req: DapRequest): Promise<void> {
        if (!this.d5Ready()) {
            this.respondError(req, d5Notice('pause'));
            return;
        }
        if (this.state === 'paused') {
            this.respond(req);
            return;
        }
        if (this.state !== 'running') {
            this.respondError(req, S.debugAdapter.pauseOnlyRunning);
            return;
        }
        const conn = this.conn;
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        try {
            await conn.request('script.pause');
        } catch (e) {
            this.respondError(req, S.debugAdapter.pauseFailed(errText(e)));
            return;
        }
        this.respond(req);
    }

    /** 控制器是否声明了 D5（引擎 pause 钩子） */
    private d5Ready(): boolean {
        return this.caps.has('d5');
    }

    /** 控制器是否声明了 D4（运行中原子替换 / 热更新） */
    private d4Ready(): boolean {
        return this.caps.has('d4');
    }

    /**
     * 编译并运行 `program`。
     * 引擎裁决按 D-03（`engineRule.resolveEngine`）；与控制器引擎冲突时**明确拦截**（§6.4）。
     */
    private async startRun(): Promise<void> {
        const conn = this.conn;
        if (!conn) {
            this.enterErrorStop('NOT_CONNECTED', 1, S.debugAdapter.channelNotConnectedBare);
            return;
        }
        const program = this.program;
        if (!program) {
            this.enterErrorStop('NO_PROGRAM', 1, S.debugAdapter.noProgram);
            return;
        }

        const resolved = resolveEngine(program);
        if (!resolved.engine) {
            this.enterErrorStop('ENGINE_MISMATCH', 1, resolved.reason ?? '');
            return;
        }
        const engine = resolved.engine;
        if (this.explicitEngine && this.explicitEngine.toLowerCase() !== engine) {
            this.enterErrorStop(
                'ENGINE_MISMATCH',
                1,
                S.debugAdapter.engineArgConflict(this.explicitEngine, engine),
            );
            return;
        }
        // 语言由控制器脚本目录推导：auto/mixed/未知 = 未绑定 → 本地不拦（由控制器裁决，
        // 空目录时两种语言均可编译，见 debug_server.cpp scan_lang_bind）。
        if (
            (this.controllerEngine === 'basic' || this.controllerEngine === 'lua') &&
            this.controllerEngine !== engine
        ) {
            this.enterErrorStop(
                'ENGINE_MISMATCH',
                1,
                S.debugAdapter.engineControllerMismatch(this.controllerEngine, engine),
            );
            return;
        }

        let src: string;
        try {
            src = new TextDecoder('utf-8').decode(await vscode.workspace.fs.readFile(vscode.Uri.file(program)));
        } catch (e) {
            this.enterErrorStop('READ_FAILED', 1, S.debugAdapter.readScriptFailed(errText(e)));
            return;
        }

        try {
            const r = (await conn.request('script.compile', { src, engine })) as
                | { labels?: string[] }
                | undefined;
            this.programSrc = src;
            const labels = r?.labels ?? [];
            const labelText = labels.length > 0 ? labels.join(', ') : S.common.none;
            this.output(S.extension.downloadOk(baseName(program), labelText), 'console');
        } catch (e) {
            const line = e instanceof KxRpcError && e.line !== undefined ? e.line : 1;
            this.enterErrorStop('COMPILE_ERROR', line, e instanceof Error ? e.message : '');
            return;
        }

        // 编译成功可能刚刚绑定语言（控制器空目录 → 首次编译），或该语言下能力位有变（如 D5）：
        // 刷新一次 sys.info，保证后续断点/单步判定据实（不拿旧 caps 冒充）。
        try {
            const info = ((await conn.request('sys.info')) ?? {}) as SysInfoLite;
            this.controllerEngine = typeof info.engine === 'string' ? info.engine.toLowerCase() : undefined;
            this.caps = new Set((info.caps ?? []).map((c) => String(c).toLowerCase()));
        } catch {
            // 刷新失败不阻断运行：沿用启动时的能力位快照
        }

        try {
            await conn.request('script.run');
        } catch (e) {
            this.enterErrorStop('RUNTIME_ERROR', 1, errText(e));
            return;
        }
        this.state = 'running';
        this.sendEvent('continued', { threadId: THREAD_ID, allThreadsContinued: true });
    }

    // -----------------------------------------------------------------------
    // T-14 变量与监视（FR-4.5/4.6/4.7）
    // -----------------------------------------------------------------------

    /** 仅在「已停止」状态提供调用栈；真实运行中没有可枚举的栈（D5 未就绪，不编造） */
    private stackTrace(req: DapRequest): void {
        if (!this.isStopped()) {
            this.respondError(req, S.debugAdapter.stackUnavailable);
            return;
        }
        const line = this.stopInfo?.line ?? 1;
        const name = this.program
            ? `${baseName(this.program)} · ${this.stopInfo?.detail ?? ''}`
            : S.debugAdapter.scriptFallback;
        this.respond(req, {
            totalFrames: 1,
            stackFrames: [
                {
                    id: 1,
                    name,
                    line,
                    column: 1,
                    source: this.sourceRef(),
                },
            ],
        });
    }

    private scopes(req: DapRequest): void {
        if (!this.isStopped()) {
            this.respond(req, { scopes: [] });
            return;
        }
        this.respond(req, {
            scopes: [
                {
                    name: S.debugAdapter.scopeLocals,
                    presentationHint: 'locals',
                    variablesReference: LOCALS_REF,
                    expensive: false,
                },
            ],
        });
    }

    /**
     * FR-4.5：`list_vars()` 解析 `NAME = 值`，再用 `get_var` 取**精确类型/值**。
     * 变量名与值是引擎给的原文，插件不改写（§6.3 纪律：不自造语义）。
     */
    private async variables(req: DapRequest): Promise<void> {
        const conn = this.conn;
        const args = (req.arguments ?? {}) as { variablesReference?: number };
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        if (args.variablesReference !== LOCALS_REF) {
            this.respond(req, { variables: [] });
            return;
        }
        if (!this.isStopped()) {
            this.respond(req, { variables: [] });
            return;
        }

        let list: string[];
        try {
            list = ((await conn.request('var.list')) as string[] | undefined) ?? [];
        } catch (e) {
            this.respondError(req, S.debugAdapter.listVarsFailed(errText(e)));
            return;
        }

        const items = list.slice(0, MAX_VARS).map(parseVarLine);
        const children = await Promise.all(
            items.map(async (it) => {
                if (!it.name) {
                    return undefined;
                }
                const enriched = await this.enrichVar(it);
                return {
                    name: enriched.name,
                    value: enriched.value,
                    type: enriched.type,
                    variablesReference: 0,
                    evaluateName: enriched.name,
                };
            }),
        );
        this.respond(req, { variables: children.filter((c) => c !== undefined) });
    }

    /** FR-4.6：`set_var()`。按字面量推断数值/字符串，避免把 `100` 写成 `"100"` */
    private async setVariable(req: DapRequest): Promise<void> {
        const conn = this.conn;
        const args = (req.arguments ?? {}) as {
            variablesReference?: number;
            name?: string;
            value?: string;
        };
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        const name = args.name ?? '';
        const literal = args.value ?? '';
        if (args.variablesReference !== LOCALS_REF || name.length === 0) {
            this.respondError(req, S.debugAdapter.setVarScopeOnly);
            return;
        }
        const v = parseLiteral(literal);
        try {
            await conn.request('var.set', { name, v });
        } catch (e) {
            this.respondError(req, S.debugAdapter.setVarFailed(errText(e)));
            return;
        }
        this.output(S.debugAdapter.varAssigned(name, literal), 'console');
        this.respond(req, {
            value: String(v),
            type: typeOfValue(v),
            variablesReference: 0,
        });
        // 让监视/变量视图立即重取（否则要等用户手动刷新）
        this.sendEvent('invalidated', { areas: ['variables'], threadId: THREAD_ID });
    }

    /**
     * FR-4.7：REPL / 监视求值。
     * - `NAME`        → `var.get`
     * - `NAME = 值`   → `var.set`
     * - 其它表达式    → **仅 repl 上下文**回退为设备命令 `cmd`（与终端同一条路，`13` §4.4）；
     *                   hover/watch 绝不执行命令（避免鼠标悬停产生副作用）。
     */
    private async evaluate(req: DapRequest): Promise<void> {
        const conn = this.conn;
        const args = (req.arguments ?? {}) as { expression?: string; context?: string };
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        const expr = (args.expression ?? '').trim();
        if (expr.length === 0) {
            this.respondError(req, S.debugAdapter.emptyExpression);
            return;
        }

        const assign = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$/.exec(expr);
        if (assign) {
            const v = parseLiteral(assign[2].trim());
            try {
                await conn.request('var.set', { name: assign[1], v });
            } catch (e) {
                this.respondError(req, S.debugAdapter.setVarFailed(errText(e)));
                return;
            }
            this.output(S.debugAdapter.varAssigned(assign[1], String(v)), 'console');
            this.respond(req, { result: `${assign[1]} = ${String(v)}`, type: typeOfValue(v), variablesReference: 0 });
            this.sendEvent('invalidated', { areas: ['variables'], threadId: THREAD_ID });
            return;
        }

        if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(expr)) {
            try {
                const g = (await conn.request('var.get', { name: expr })) as
                    | { type?: string; value?: unknown }
                    | undefined;
                const value = g?.value;
                this.respond(req, {
                    result: value === undefined ? String(g?.type ?? '') : String(value),
                    type: g?.type,
                    variablesReference: 0,
                });
            } catch (e) {
                this.respondError(req, S.debugAdapter.getVarFailed(expr, errText(e)));
            }
            return;
        }

        if (args.context !== 'repl') {
            this.respondError(req, S.debugAdapter.exprNotSupported);
            return;
        }

        try {
            const r = (await conn.request('cmd', { line: expr })) as { ret?: number; out?: string[] } | undefined;
            const text = (r?.out ?? []).join('\n');
            const ret = r?.ret ?? 0;
            this.respond(req, { result: text.length > 0 ? text : `ret=${ret}`, variablesReference: 0 });
        } catch (e) {
            this.respondError(req, S.debugAdapter.cmdFailed(errText(e)));
        }
    }

    private async enrichVar(it: VarItem): Promise<VarItem> {
        const conn = this.conn;
        if (!conn) {
            return it;
        }
        try {
            const g = (await conn.request('var.get', { name: it.name })) as
                | { type?: string; value?: unknown }
                | undefined;
            if (!g) {
                return it;
            }
            return {
                name: it.name,
                type: g.type ?? it.type,
                value: g.value === undefined ? it.value : String(g.value),
            };
        } catch {
            // get_var 失败时以 list_vars 的解析结果为准（不吞掉变量本身）
            return it;
        }
    }

    /**
     * T-17 断点。DAP 语义 = **全量替换**（本次请求的列表即当前断点全集）。
     *
     * - D5 就绪：与控制器 `breakpoint.*` 同步，命中即挂起 → `verified:true`；
     * - D5 未就绪：**明确标注未校验**，不伪装（§4.4 降级），仅本地登记。
     *
     * 注：协议无「源文件」概念（单脚本会话），故断点为**会话级**行号集合（`15` §8.5 注）。
     */
    private async setBreakpoints(req: DapRequest): Promise<void> {
        const args = (req.arguments ?? {}) as DapSetBreakpointsArguments;
        const src = args.source?.path ?? '';
        const requested = args.breakpoints ?? [];
        const lines = requested.map((bp) => bp.line).filter((n) => Number.isInteger(n) && n > 0);
        this.breakpointLines = [...new Set(lines)].sort((a, b) => a - b);

        const source = { name: baseName(src), path: src };
        if (!this.d5Ready()) {
            const reason = S.debugAdapter.bpUnverifiedReason;
            this.respond(req, {
                breakpoints: requested.map((bp) => ({
                    verified: false,
                    line: bp.line,
                    message: reason,
                    source,
                })),
            });
            if (!this.breakpointNoticeSent && requested.length > 0) {
                this.breakpointNoticeSent = true;
                this.output(S.debugAdapter.bpRegistered(requested.length, reason), 'console');
            }
            return;
        }

        const conn = this.conn;
        if (!conn) {
            this.respondError(req, S.debugAdapter.channelNotConnected);
            return;
        }
        try {
            // 全量替换：先清空，再逐行 add（协议只有 add/del/list）
            await conn.request('breakpoint.del', {});
            for (const line of this.breakpointLines) {
                await conn.request('breakpoint.add', { line });
            }
        } catch (e) {
            this.respondError(req, S.debugAdapter.bpSetFailed(errText(e)));
            return;
        }
        this.respond(req, {
            breakpoints: requested.map((bp) => ({ verified: true, line: bp.line, source })),
        });
        if (!this.breakpointNoticeSent && requested.length > 0) {
            this.breakpointNoticeSent = true;
            this.output(S.debugAdapter.bpRegisteredReady(this.breakpointLines.length), 'console');
        }
    }

    // -----------------------------------------------------------------------
    // T-15 输出与终止映射（§6.3）
    // -----------------------------------------------------------------------

    /**
     * 状态名**直接取控制器给的字符串**，插件不自造（§6.3）。
     * `READY`→continued、`DONE`→exited(0)+terminated、`ABORTED`→terminated、
     * 错误态→stopped(exception) + `error_line` 定位。
     */
    private onKxEvent(ev: KxScriptEvent): void {
        switch (ev.e) {
            case 'log':
                // 订阅的日志：错误级进 stderr，其余进 stdout（FR-7.2 分栏）
                this.output(ev.s ?? '', ev.lvl === 'error' || ev.lvl === 'warn' ? 'stderr' : 'stdout');
                break;
            case 'script':
                this.onScriptStatus(ev);
                break;
            case 'axis':
                // 状态面板（T-19）用；DAP 通道不落输出，避免刷屏
                break;
            default:
                this.output(S.debugAdapter.unknownEvent(ev.e), 'console');
                break;
        }
    }

    private onScriptStatus(ev: KxScriptEvent): void {
        const status = ev.status ?? '';
        this.lastStatus = status;
        const steps = typeof ev.steps === 'number' ? ` steps=${ev.steps}` : '';
        const line = typeof ev.error_line === 'number' && ev.error_line > 0 ? ` line=${ev.error_line}` : '';
        this.output(S.debugAdapter.scriptStatus(status, steps, line), 'console');

        switch (status) {
            case 'READY':
                this.state = 'running';
                this.sendEvent('continued', { threadId: THREAD_ID, allThreadsContinued: true });
                break;
            case 'PAUSED': {
                // D5 就绪：引擎在断点/单步/暂停处挂起（T-17）
                this.state = 'paused';
                const line = typeof ev.line === 'number' && ev.line > 0 ? ev.line : 1;
                const hit = this.breakpointLines.includes(line);
                const detail = hit
                    ? S.debugAdapter.bpHit(line)
                    : S.debugAdapter.pausedAt(line);
                this.stopInfo = { line, detail };
                this.output(S.debugAdapter.debuggerMsg(detail), 'console');
                this.sendEvent('stopped', {
                    reason: hit ? 'breakpoint' : 'pause',
                    threadId: THREAD_ID,
                    allThreadsStopped: true,
                    description: detail,
                    text: S.debugAdapter.atLine(line),
                    hitBreakpointIds: hit ? [line] : [],
                });
                break;
            }
            case 'DONE':
                this.state = 'ended';
                this.sendEvent('exited', { exitCode: 0 });
                this.sendEvent('terminated');
                break;
            case 'ABORTED':
                this.state = 'ended';
                this.sendEvent('terminated');
                break;
            case 'RUNTIME_ERROR':
                this.enterErrorStop(status, ev.error_line ?? 1, steps.trim());
                break;
            case 'BUDGET_EXCEEDED':
                this.enterErrorStop(status, ev.error_line ?? 1, steps.trim());
                break;
            case 'COMPILE_ERROR':
                this.enterErrorStop(status, ev.error_line ?? 1, '');
                break;
            default:
                // 未知状态不猜测语义，仅落输出（便于对控制器升级保持前向兼容）
                break;
        }
    }

    private async terminate(req: DapRequest): Promise<void> {
        await this.stopScript();
        this.respond(req);
        this.state = 'ended';
        this.sendEvent('terminated');
    }

    private async disconnect(req: DapRequest): Promise<void> {
        const args = (req.arguments ?? {}) as { terminateDebuggee?: boolean };
        if (args.terminateDebuggee) {
            await this.stopScript();
        }
        this.closeConnection();
        this.respond(req);
        this.state = 'ended';
        this.sendEvent('terminated');
    }

    private async stopScript(): Promise<void> {
        const conn = this.conn;
        if (!conn || !conn.connected) {
            return;
        }
        try {
            await conn.request('script.stop');
        } catch (e) {
            this.output(S.debugAdapter.stopFailed(errText(e)), 'stderr');
        }
    }

    // -----------------------------------------------------------------------
    // 内部工具
    // -----------------------------------------------------------------------

    private isStopped(): boolean {
        return this.state === 'entry' || this.state === 'error' || this.state === 'paused';
    }

    private enterErrorStop(status: string, line: number, extra: string): void {
        this.state = 'error';
        const l = line > 0 ? line : 1;
        this.lastStatus = status;
        const detail = `${status}${extra.length > 0 ? `：${extra}` : ''}`;
        this.stopInfo = { line: l, detail };
        this.output(S.debugAdapter.debuggerMsgLine(detail, l), 'stderr');
        this.sendEvent('stopped', {
            reason: 'exception',
            threadId: THREAD_ID,
            allThreadsStopped: true,
            description: detail,
            text: S.debugAdapter.atLine(l),
            hitBreakpointIds: [],
        });
        // 不自动 terminated：让用户看到错误行与变量现场。
        // 恢复不可用（D5 未就绪）：再次 continue 会被明确拒绝，而不是假装能继续。
    }

    private threadName(): string {
        return this.controllerEngine
            ? S.debugAdapter.threadName(engineDisplay(this.controllerEngine))
            : S.debugAdapter.threadNameFallback;
    }

    private sourceRef(): { name?: string; path?: string } | undefined {
        const p = this.program;
        return p ? { name: baseName(p), path: p } : undefined;
    }

    /** `program` 解析：显式路径 > 活动脚本编辑器 > （目录时）唯一脚本 */
    private async resolveProgram(programArg?: string): Promise<string | undefined> {
        if (programArg && /\.(bas|lua)$/i.test(programArg)) {
            return programArg;
        }
        const doc = vscode.window.activeTextEditor?.document;
        if (doc && doc.uri.scheme === 'file' && (doc.languageId === 'kx-basic' || doc.languageId === 'kx-lua')) {
            return doc.uri.fsPath;
        }
        if (programArg) {
            try {
                const found = await vscode.workspace.findFiles(
                    new vscode.RelativePattern(vscode.Uri.file(programArg), '*.{bas,lua}'),
                    null,
                    2,
                );
                if (found.length === 1) {
                    return found[0].fsPath;
                }
            } catch {
                // programArg 既不是脚本也不是可搜索目录：按「未指定」处理，交给上层报错
            }
        }
        return undefined;
    }

    private closeConnection(): void {
        if (this.conn) {
            this.conn.removeAllListeners();
            this.conn.disconnect();
            this.conn = undefined;
        }
    }

    // --- DAP 出站消息 ---

    private respond(req: DapRequest, body?: unknown): void {
        this.emitter.fire({
            seq: ++this.seq,
            type: 'response',
            request_seq: req.seq,
            success: true,
            command: req.command,
            body,
        } as unknown as vscode.DebugProtocolMessage);
    }

    private respondError(req: DapRequest, message: string): void {
        this.emitter.fire({
            seq: ++this.seq,
            type: 'response',
            request_seq: req.seq,
            success: false,
            command: req.command,
            message,
        } as unknown as vscode.DebugProtocolMessage);
    }

    private sendEvent(event: string, body?: unknown): void {
        this.emitter.fire({
            seq: ++this.seq,
            type: 'event',
            event,
            body,
        } as unknown as vscode.DebugProtocolMessage);
    }

    /** 输出到「调试控制台」（FR-7.2 分栏：console=调试器消息 / stdout=脚本输出 / stderr=错误） */
    private output(text: string, category: 'console' | 'stdout' | 'stderr'): void {
        if (text.length === 0) {
            return;
        }
        this.sendEvent('output', { category, output: text + '\n' });
    }
}

// 纯函数（`parseVarLine` / `parseLiteral` / `inferType` / `typeOfValue` / `baseName` / `d5Notice`）
// 已抽到 `dapPure.ts`：不依赖 vscode，可在 Node 下直接单测（见 test/connection.smoke.cjs）。

function errText(e: unknown): string {
    return e instanceof Error ? e.message : String(e);
}
