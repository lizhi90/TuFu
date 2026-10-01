// extension.ts —— 扩展入口
//
// 对应 docs/planA/15 §8：
//   T-07 连接命令与状态栏（FR-1.1/1.2/1.4）
//   T-08 输出通道（脚本输出 / 日志 / 原始报文分栏，FR-7.2/7.3）
//   T-09 下载与运行/停止（FR-3.1~3.3/3.6/3.7）
//   T-10 编译即诊断（FR-2.4：err.line/msg → Problems）
//   T-11 引擎一致性校验（FR-2.6、§6.4）
//   T-12 设备终端（FR-5.1/5.2，走 `cmd`）
//   T-13 Debug Adapter 注册（D-02：同包内 TS，`DebugAdapterInlineImplementation`）
//   T-18 订阅管理（`subscribe`/`unsubscribe` + 引用计数 + 限频）
//   T-19/T-20 状态面板（轴 / 总线 / Modbus 寄存器，事件驱动，FR-6.1~6.5）
//
// 本文件只做「装配」：命令注册、状态栏、输出通道、事件分发、诊断落点、DAP 工厂、面板与订阅。
// 传输细节在 connection.ts，配置在 config.ts，规则在 engineRule.ts，报文类型在 protocol.ts，
// 调试适配器在 debugAdapter.ts，订阅在 subscribe.ts，数据仓库在 store.ts；状态 UI 为 Webview 面板。

import * as vscode from 'vscode';

import {
    isLoopback,
    parseTarget,
    readConfig,
    sshForwardHint,
    KxConfig,
    KxTarget,
    ScriptEngine,
} from './config';
import { registerCompletion } from './completion';
import { KxConnection } from './connection';
import { KxCurvePanel } from './curve';
import { KxDebugAdapter } from './debugAdapter';
import { controllerEngineConflict, engineDisplay, engineLabel } from './engineRule';
import { resolveEngine } from './engineResolve';
import { buildConsoleSpec } from './consolePure';
import { KxConsoleView } from './console';
import { KxFilesTreeItem, KxFilesTreeProvider } from './filesTree';
import { AxisControlMsg, KxAxisPanel, AxisPanelHooks } from './axisPanel';
import { buildAxisPanelSpec } from './axisPanelPure';
import { KxModbusPanel, ModbusPanelHooks } from './modbusPanel';
import { buildModbusPanelSpec } from './modbusPanelPure';
import { FilesTreeState, FileSyncState, fnv1a64Hex, compareFileHash } from './filesPanelPure';
import { parsePortMax, portMaxSummary } from './portMaxPure';
import { KxCommPanel, CommPanelHooks } from './commPanel';
import { KxMbmapPanel, MbmapPanelHooks } from './mbmapPanel';
import { buildMbmapSpec, validateD12Text } from './mbmapPanelPure';
import { KxMbdevPanel, MbdevPanelHooks } from './mbdevPanel';
import { buildMbdevSpec } from './mbdevPanelPure';
import { buildCommPanelSpec } from './commPanelPure';
import { KxConnectPanel } from './connectPanel';
import { ErrorCodes, KxEvent, KxRpcError } from './protocol';
import { S } from './strings';
import { clampHz, SubscribeManager } from './subscribe';
import { PanelId, KxStatusStore } from './store';

let conn: KxConnection | undefined;
/**
 * 调试会话占用连接（v0.7.0）：DebugServer 单客户端，DAP 与主连接必须**交接**——
 * 会话开始即断开主连接、结束再连回；期间手动「连接」被明确拒绝（不抢通道）。
 */
let debugSessionActive = false;
/** 断线自动重连（v0.7.0）：指数退避；主动断开/重启/调试交接期间挂起 */
let autoReconnectTimer: ReturnType<typeof setTimeout> | undefined;
let autoReconnectAttempts = 0;
let autoReconnectInfo = '';
let suppressAutoReconnect = false;
let out: vscode.OutputChannel;
let rawOut: vscode.OutputChannel;
let diagnostics: vscode.DiagnosticCollection;
let extCtx: vscode.ExtensionContext | undefined;

/**
 * 状态栏分区（FR-1.4）：
 *   - 左区：品牌 `statusBrandItem` + 运行概要 `statusRuntimeItem`（引擎 / 轴数 / 脚本状态）
 *   - 右区：连接状态 `statusConnItem`，**固定最右**（Requirement：连接成功在最右侧显示「已连接」）
 * VS Code 状态栏只有左/右两个停靠区，没有「中间区」；此处按左区两段 + 右区一段实现分区。
 */
let statusBrandItem: vscode.StatusBarItem;
let statusRuntimeItem: vscode.StatusBarItem;
let statusConnItem: vscode.StatusBarItem;

/**
 * 状态栏**文字动作条**（v0.2.0 最终方案）：底部一排可点击的文字按钮
 * `连接/断开 · 下载 · 运行 · 停止 · 曲线 · 刷新`，点击直接执行命令——
 * 一条真正的横排菜单栏，无区块、无折叠、标准 API（绝不依赖 Webview 的显示成功率）。
 * 未连接时脚本类按钮隐藏（降级不伪装）。
 */
let tbConnect: vscode.StatusBarItem;
let tbDownload: vscode.StatusBarItem;
let tbRun: vscode.StatusBarItem;
let tbStop: vscode.StatusBarItem;
let tbCurve: vscode.StatusBarItem;
let tbRefresh: vscode.StatusBarItem;

/** 最近一次连接目标（弹窗输入的结果；用于状态栏显示与「连接」二次确认） */
let target: KxTarget | undefined;
/** 控制器上报的引擎 / 轴数（sys.info 落地；本地同步据此过滤脚本扩展名） */
let controllerEngine: ScriptEngine | '' = '';
/** sys.info.engine 原值（含 auto/mixed；仅用于显示与诊断，判定仍走 controllerEngine） */
let controllerEngineRaw = '';
let controllerAxisCount = 0;

/** `globalState` 键：记住上次输入的 IP / 端口，作为下次弹窗默认值 */
const LAST_TARGET_KEY = 'kine-x.lastTarget';

/** 面板数据仓库与订阅管理器（T-18/T-19/T-20） */
const store = new KxStatusStore();
let subs: SubscribeManager | undefined;

/** 「Kine-X 控制台」Webview 视图（侧边栏：菜单栏 + 状态行；v0.4.2 起不含状态表格） */
let consoleView: KxConsoleView | undefined;
let filesTree: KxFilesTreeProvider | undefined;

/**
 * 「轴状态」右侧面板 hooks（extension.ts 单一构造点，注册命令与重连复用）。
 * 面板可见才订阅 axis/bus（引用计数），并取一次轴快照（FR-6.5）；
 * v0.4.3 起寄存器拆至 Modbus 面板，此处不再订阅 mb。
 */
function axisPanelHooks(): AxisPanelHooks {
    return {
        buildSpec: () => buildAxisPanelSpec(store, { connected: link === 'connected' }),
        onVisible: () => {
            subs?.retain(['axis', 'bus']);
            void snapshotAxes();
        },
        onHidden: () => subs?.release(['axis', 'bus']),
        // v0.7.0：面板在线调试按钮（使能/去使能/停止/点动/定位）→ D1 `cmd`
        onControl: (msg) => void runAxisControl(msg),
        log: (line) => out?.appendLine(line),
    };
}

/**
 * 轴面板在线调试（v0.7.0）：把面板动作翻译为设备命令经 `cmd` 下发。
 * 说明：移动用 `MOVEABS/MOVE …,0`（wait=0 异步）避免阻塞调试会话；点动按住期间由面板
 * 持续下发，松开执行 `CANCEL 2`；EN/DIS 为阻塞命令（通常 <100ms，最长 5s 超时）。
 */
/** 点动看门狗：面板 1.5s 无心跳（关闭/卡死）→ 自动 CANCEL，防止轴一直走 */
let jogWatchdog: ReturnType<typeof setTimeout> | undefined;

function stopJogWatchdog(): void {
    if (jogWatchdog) {
        clearTimeout(jogWatchdog);
        jogWatchdog = undefined;
    }
}

async function runAxisControl(msg: AxisControlMsg): Promise<void> {
    const c = conn;
    if (!c || !c.connected) {
        void vscode.window.showWarningMessage(S.extension.notConnectedWithHint);
        return;
    }
    const axis = Number.isInteger(msg.axis) && msg.axis >= 0 ? msg.axis : 0;
    if (msg.action === 'jog' || msg.action === 'jogTick') {
        stopJogWatchdog();
        jogWatchdog = setTimeout(() => {
            jogWatchdog = undefined;
            out.appendLine(S.axisPanel.jogWatchdog);
            void conn?.request('cmd', { line: `BASE ${axis}` }).catch(() => undefined);
            void conn?.request('cmd', { line: 'CANCEL 2' }).catch(() => undefined);
        }, 1500);
        if (msg.action === 'jogTick') {
            return;   // 心跳只续期，不重复下发命令
        }
    } else {
        stopJogWatchdog();
    }
    const speed = typeof msg.speed === 'number' && Number.isFinite(msg.speed) && msg.speed > 0 ? msg.speed : 50;
    const lines: string[] = [`BASE ${axis}`];
    let label = '';
    switch (msg.action) {
        case 'enable':
            lines.push('EN');
            label = S.axisPanel.ctlEnable;
            break;
        case 'disable':
            lines.push('DIS');
            label = S.axisPanel.ctlDisable;
            break;
        case 'stop':
            lines.push('STOP');
            label = S.axisPanel.ctlStop;
            break;
        case 'jog': {
            const dir = msg.dir === -1 ? -1 : 1;
            lines.push(`SPEED ${axis}, ${Math.abs(speed)}`);
            lines.push(`VMOVE ${dir}`);
            label = S.axisPanel.ctlJog(dir);
            break;
        }
        case 'jogStop':
            lines.push('CANCEL 2');
            label = S.axisPanel.ctlJogStop;
            break;
        case 'setScale': {
            const v = typeof msg.value === 'number' ? msg.value : Number.NaN;
            if (!Number.isFinite(v) || v <= 0) {
                void vscode.window.showWarningMessage(S.axisPanel.ctlScaleRequired);
                return;
            }
            lines.push(`UNITS ${axis}, ${v}`);
            label = S.axisPanel.ctlScale(v);
            break;
        }
        case 'move': {
            const t = typeof msg.target === 'number' ? msg.target : Number.NaN;
            if (!Number.isFinite(t)) {
                void vscode.window.showWarningMessage(S.axisPanel.ctlTargetRequired);
                return;
            }
            lines.push(`SPEED ${axis}, ${speed}`);
            lines.push(`${msg.mode === 'rel' ? 'MOVE' : 'MOVEABS'} ${t}, ${speed}, 0, 0`);
            label = S.axisPanel.ctlMove(msg.mode === 'rel' ? S.axisPanel.rel : S.axisPanel.abs, t, speed);
            break;
        }
        default:
            return;
    }
    try {
        for (const line of lines) {
            await c.request('cmd', { line });
        }
    } catch (e) {
        const m = stripCode(errText(e));
        out.appendLine(`[axisCtl] ${m}`);
        void vscode.window.showWarningMessage(m);
        return;
    }
    out.appendLine(`[axisCtl] ${label}`);
}

/** 「Modbus 寄存器」右侧面板 hooks：可见才订阅 mb 主题（不背 axis/bus 带宽） */
function modbusPanelHooks(): ModbusPanelHooks {
    return {
        buildSpec: () =>
            buildModbusPanelSpec(store, {
                connected: link === 'connected',
                usedOnly: KxModbusPanel.instance?.showUsedOnly === true,   // v0.8.6
            }),
        onVisible: () => subs?.retain(['mb']),
        onHidden: () => subs?.release(['mb']),
        log: (line) => out?.appendLine(line),
    };
}

/** 连接目标标签（`host:port`）；未连接时为空串，供控制台状态行使用 */
function currentTargetLabel(): string {
    if (!target || !target.host) {
        return '';
    }
    return `${target.host}:${target.port}`;
}

/** 控制器 `sys.info.caps` 声明（`16` §7.1）：`d3` 决定订阅、`d4` 决定热更新（T-16）、`d5` 决定断点（T-17） */
const controllerCaps = new Set<string>();

type LinkState = 'disconnected' | 'connecting' | 'connected';
let link: LinkState = 'disconnected';
let scriptStatus = '';
let scriptSteps = 0;

interface SysInfo {
    ver?: string;
    engine?: string;
    axis_count?: number;
    /** 能力位（`16` §7.1：`["d1","d2","d3","d5"]`）；缺省表示老版本服务端，按「未知」处理 */
    caps?: string[];
}

interface ScriptEventPayload {
    status?: string;
    steps?: number;
}

interface CmdResult {
    ret?: number;
    out?: string[];
}

export function activate(context: vscode.ExtensionContext): void {
    extCtx = context;
    out = vscode.window.createOutputChannel(S.extension.outputDebug);
    rawOut = vscode.window.createOutputChannel(S.extension.outputRaw);

    // 状态栏左区：品牌（最左） + 运行概要；右区：连接状态（最右）
    statusBrandItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    // 插件版本号取自 package.json（随 VSIX 走，不硬编码；v0.8.2 起显示在底部状态栏）
    const extVersion = String(context.extension.packageJSON.version ?? '?');
    statusBrandItem.text = S.extension.statusBrand(extVersion);
    statusBrandItem.tooltip = S.extension.statusBrandTooltip(extVersion);
    statusBrandItem.command = 'kine-x.sysInfo';
    statusRuntimeItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
    statusRuntimeItem.text = S.extension.statusRuntimeIdle;
    statusRuntimeItem.tooltip = S.extension.statusRuntimeTooltipIdle;
    statusRuntimeItem.command = 'kine-x.panel.refresh';
    statusConnItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);

    diagnostics = vscode.languages.createDiagnosticCollection('kine-x');
    context.subscriptions.push(
        out,
        rawOut,
        statusBrandItem,
        statusRuntimeItem,
        statusConnItem,
        diagnostics,
    );

    restoreLastTarget();
    render();
    statusBrandItem.show();
    statusRuntimeItem.show();
    statusConnItem.show();
    registerPanels(context);
    registerFilesTree(context);
    registerToolbar(context);
    // FR-2.5 命令名补全（.bas/.lua）；数据缺失时自动跳过，不阻塞扩展
    const completionReg = registerCompletion(context);
    if (completionReg) {
        context.subscriptions.push(completionReg);
    }
    out.appendLine(S.extension.activated);

    context.subscriptions.push(
        vscode.commands.registerCommand('kine-x.connect', () => connect()),
        vscode.commands.registerCommand('kine-x.disconnect', () => disconnect()),
        vscode.commands.registerCommand('kine-x.sync', () => syncFromController()),
        // 旧命令 id 保留（指向同一实现）：旧版菜单/键位不失效
        vscode.commands.registerCommand('kine-x.sync.local', () => syncFromController()),
        vscode.commands.registerCommand('kine-x.ip.set', () => setIpAddress()),
        vscode.commands.registerCommand('kine-x.sysInfo', () => sysInfo(false)),
        vscode.commands.registerCommand('kine-x.restart', () => restartController()),
        vscode.commands.registerCommand('kine-x.portMax', () => setPortMax()),
        vscode.commands.registerCommand('kine-x.cmd', () => runCmd()),
        vscode.commands.registerCommand('kine-x.panel.refresh', () => refreshPanelsCmd()),
        // 「控制器文件」TreeView（v0.4.9）：刷新 / 全部拉取 / 单文件拉取 / 删除
        vscode.commands.registerCommand('kine-x.files.refresh', () => void refreshControllerFiles()),
        vscode.commands.registerCommand('kine-x.files.pullAll', () => syncFromController()),
        vscode.commands.registerCommand('kine-x.files.pull', (item?: KxFilesTreeItem) =>
            item?.fileName ? pullFileToWorkspace(item.fileName) : undefined,
        ),
        vscode.commands.registerCommand('kine-x.files.delete', (item?: KxFilesTreeItem) =>
            item?.fileName ? deleteControllerFile(item.fileName) : undefined,
        ),
        // D8：主文件（开机运行）设置/取消（TreeView 行内按钮与右键菜单）
        vscode.commands.registerCommand('kine-x.files.setMain', (item?: KxFilesTreeItem) =>
            item?.fileName ? setMainFile(item.fileName) : undefined,
        ),
        vscode.commands.registerCommand('kine-x.files.clearMain', () => clearMainFile()),
        vscode.commands.registerCommand('kine-x.curve.open', () => openCurve()),
        vscode.commands.registerCommand('kine-x.axis.open', () => openAxisPanel()),
        vscode.commands.registerCommand('kine-x.modbus.open', () => openModbusPanel()),
        vscode.commands.registerCommand('kine-x.comm.open', () => openCommPanel()),
        vscode.commands.registerCommand('kine-x.mbmap.open', () => openMbmapPanel(context)),
        vscode.commands.registerCommand('kine-x.mbdev.open', () => openMbdevPanel(context)),
        vscode.commands.registerCommand('kine-x.download', () => withScript((doc) => download(doc, false))),
        vscode.commands.registerCommand('kine-x.downloadAndRun', () =>
            withScript((doc) => download(doc, true)),
        ),
        vscode.commands.registerCommand('kine-x.run', () => runScript()),
        vscode.commands.registerCommand('kine-x.stop', () => stopScript()),
        // 脚本语言环境切换（菜单「控制器 → 脚本语言」）：写入本项目设置 kine-x.engine
        vscode.commands.registerCommand('kine-x.engine.basic', () => setScriptEngine('basic')),
        vscode.commands.registerCommand('kine-x.engine.lua', () => setScriptEngine('lua')),
        vscode.workspace.onDidOpenTextDocument((doc) => warnEngineMismatch(doc.uri.fsPath)),
        vscode.workspace.onDidChangeTextDocument((e) => diagnostics.delete(e.document.uri)),
        // v0.7.0：调试会话与主连接**单客户端交接**（DAP 自建连接，主连接让位/会话结束连回）
        vscode.debug.onDidStartDebugSession((s) => void onDebugSessionStart(s)),
        vscode.debug.onDidTerminateDebugSession((s) => void onDebugSessionEnd(s)),
        // T-13（D-02）：同包内 TS Adapter，与扩展同进程，不再单起 adapter 进程
        vscode.debug.registerDebugAdapterDescriptorFactory('kine-x', {
            createDebugAdapterDescriptor: () =>
                new vscode.DebugAdapterInlineImplementation(new KxDebugAdapter()),
        }),
    );
}

export function deactivate(): void {
    teardown();
}

// ---------------------------------------------------------------------------
// 连接生命周期
// ---------------------------------------------------------------------------

async function connect(): Promise<void> {
    const cfg = readConfig();
    if (conn && conn.connected) {
        const t = target ?? { host: cfg.host, port: cfg.port };
        void vscode.window.showInformationMessage(S.extension.alreadyConnectedAt(t.host, t.port));
        return;
    }

    // FR-1.1（v0.3.2）：打开「连接控制器」表单弹窗（IP + 端口 + 连接按钮 + 就地校验）。
    // 表单提交 → submitConnect()；取消/关标签页 → 记日志即止（不猜地址，不静默回退）。
    KxConnectPanel.showOrReveal(
        { host: target?.host || cfg.host, port: target?.port || cfg.port },
        {
            onSubmit: (host, port) => void submitConnect(cfg, host, port),
            onCancel: () => out.appendLine(S.extension.connectCancelled),
        },
    );
}

/** 表单「连接」回调：扩展端 parseTarget 兜底校验 → 走原有连接流程，结果回显到表单 */
async function submitConnect(cfg: KxConfig, hostRaw: string, portRaw: string): Promise<void> {
    const panel = KxConnectPanel.instance;
    // 调试会话占用连接时禁止抢占（单客户端：抢了会把调试器踢下线）
    if (debugSessionActive) {
        panel?.setState({ connecting: false, error: S.extension.debugBusy });
        void vscode.window.showWarningMessage(S.extension.debugBusy);
        return;
    }
    suppressAutoReconnect = false;
    autoReconnectAttempts = 0;
    autoReconnectInfo = '';
    const parsed = parseTarget(hostRaw, portRaw);
    if (!parsed.target) {
        // 表单前端已就地校验，此处兜底：错误回显到表单内（不弹 toast，输入保留便于改）
        panel?.setState({ connecting: false, error: parsed.error ?? S.extension.connectFailed('', '') });
        return;
    }
    const picked = parsed.target;
    target = picked;
    await saveLastTarget(picked);
    panel?.setState({ connecting: true, host: picked.host, port: picked.port, error: '' });

    if (cfg.sshForward && !isLoopback(picked.host)) {
        const hint = sshForwardHint(picked);
        out.appendLine(S.extension.sshForward(hint));
        void vscode.env.clipboard.writeText(hint);
    }

    const c = getConnection(cfg, picked);
    setLink('connecting');
    out.appendLine(S.extension.connecting(picked.host, picked.port));
    try {
        await c.connect();
    } catch (e) {
        setLink('disconnected');
        const msg = errText(e);
        out.appendLine(S.extension.connectFailedLog(msg));
        const extra = cfg.sshForward ? S.extension.sshForwardSuffix(sshForwardHint(picked)) : '';
        panel?.setState({ connecting: false, error: S.extension.connectFailed(msg, extra) });
        return;
    }
    setLink('connected');
    panel?.close();
    await sysInfo(true);
    subscribeLogs(cfg);
    reapplySubscriptions();
}

// ---------------------------------------------------------------------------
// 连接目标（IP / 端口）弹窗与记忆（FR-1.1）
// ---------------------------------------------------------------------------

function restoreLastTarget(): void {
    const saved = extCtx?.globalState.get<Partial<KxTarget>>(LAST_TARGET_KEY);
    const cfg = readConfig();
    const host = typeof saved?.host === 'string' && saved.host.length > 0 ? saved.host : cfg.host;
    const port = typeof saved?.port === 'number' && saved.port > 0 ? saved.port : cfg.port;
    target = { host, port };
}

async function saveLastTarget(t: KxTarget): Promise<void> {
    try {
        await extCtx?.globalState.update(LAST_TARGET_KEY, { host: t.host, port: t.port });
    } catch (e) {
        out.appendLine(S.extension.errorLog('globalState.update', errText(e)));
    }
}

/** `kine-x.ip.set`：修改IP地址 + 端口（表单复用 connectPanel 设置模式，v0.5.1） */
async function setIpAddress(): Promise<void> {
    const cfg = readConfig();
    const cur = target ?? { host: cfg.host, port: cfg.port };
    KxConnectPanel.showOrReveal(
        { host: cur.host, port: cur.port, mode: 'settings' },
        {
            onSubmit: (host, port) => void saveControllerAddress(host, port),
            onCancel: () => out.appendLine(S.extension.setIpCancelled),
        },
    );
}

/** 设置模式提交：parseTarget 兜底校验 → 写入 kine-x.host / kine-x.debugPort（Global） */
async function saveControllerAddress(hostRaw: string, portRaw: string): Promise<void> {
    const panel = KxConnectPanel.instance;
    const parsed = parseTarget(hostRaw, portRaw);
    if (!parsed.target) {
        // 前端已就地校验，此处兜底：错误回显到表单内（不弹 toast，输入保留便于改）
        panel?.setState({ connecting: false, error: parsed.error ?? '' });
        return;
    }
    const t = parsed.target;
    try {
        const conf = vscode.workspace.getConfiguration('kine-x');
        await conf.update('host', t.host, vscode.ConfigurationTarget.Global);
        await conf.update('debugPort', t.port, vscode.ConfigurationTarget.Global);
    } catch (e) {
        const msg = errText(e);
        panel?.setState({ connecting: false, error: S.extension.setIpFailed(msg) });
        out.appendLine(S.extension.setIpFailed(msg));
        return;
    }
    target = { host: t.host, port: t.port };
    await saveLastTarget(target);
    out.appendLine(S.extension.setIpDone(t.host, t.port));
    void vscode.window.showInformationMessage(S.extension.setIpDone(t.host, t.port));
    panel?.close();
    render();
}

function disconnect(): void {
    if (!conn || !conn.connected) {
        void vscode.window.showInformationMessage(S.extension.notConnected);
        return;
    }
    suppressAutoReconnect = true;      // 用户主动断开：不自动重连
    autoReconnectAttempts = 0;
    autoReconnectInfo = '';
    if (autoReconnectTimer) {
        clearTimeout(autoReconnectTimer);
        autoReconnectTimer = undefined;
    }
    out.appendLine(S.extension.disconnecting);
    conn.disconnect();
}

// ---------------------------------------------------------------------------
// 断线自动重连（v0.7.0）与调试会话交接
// ---------------------------------------------------------------------------

/** 意外断线 → 指数退避重连（2/5/10/20/30s 封顶，随后每 30s 重试；成功即恢复订阅） */
function scheduleAutoReconnect(): void {
    if (suppressAutoReconnect || debugSessionActive) {
        return;
    }
    if (!readConfig().autoReconnect) {
        return;
    }
    if (!target || autoReconnectTimer) {
        return;
    }
    autoReconnectAttempts += 1;
    const delays = [2000, 5000, 10000, 20000, 30000];
    const wait = delays[Math.min(autoReconnectAttempts - 1, delays.length - 1)];
    autoReconnectInfo = S.extension.autoReconnecting(autoReconnectAttempts, Math.round(wait / 1000));
    out.appendLine(`[conn] ${autoReconnectInfo}`);
    render();
    autoReconnectTimer = setTimeout(() => {
        autoReconnectTimer = undefined;
        void attemptAutoReconnect();
    }, wait);
}

async function attemptAutoReconnect(): Promise<void> {
    if (suppressAutoReconnect || debugSessionActive || !target) {
        return;
    }
    const cfg = readConfig();
    const c = getConnection(cfg, target);
    setLink('connecting');
    try {
        await c.connect();
    } catch {
        setLink('disconnected');
        scheduleAutoReconnect();
        return;
    }
    setLink('connected');
    autoReconnectAttempts = 0;
    autoReconnectInfo = '';
    out.appendLine(S.extension.autoReconnected(target.host, target.port));
    await sysInfo(true);
    subscribeLogs(cfg);
    reapplySubscriptions();
    render();
    refreshToolbar();
}

/** 调试会话开始：单客户端交接——断开主连接让位（DAP 自建连接） */
async function onDebugSessionStart(s: vscode.DebugSession): Promise<void> {
    if (s.type !== 'kine-x' || debugSessionActive) {
        return;
    }
    debugSessionActive = true;
    if (autoReconnectTimer) {
        clearTimeout(autoReconnectTimer);
        autoReconnectTimer = undefined;
    }
    autoReconnectInfo = '';
    if (conn && conn.connected) {
        suppressAutoReconnect = true;   // 交接期间不自动抢回
        out.appendLine(S.extension.debugHandover);
        conn.disconnect();
    }
    render();
    refreshToolbar();
}

/** 调试会话结束：把主连接接回来（调试期间引擎/连接已被 DAP 占用） */
async function onDebugSessionEnd(s: vscode.DebugSession): Promise<void> {
    if (s.type !== 'kine-x') {
        return;
    }
    debugSessionActive = false;
    suppressAutoReconnect = false;
    const t = target ?? { host: readConfig().host, port: readConfig().port };
    out.appendLine(S.extension.debugRestore);
    const ok = await reconnectUntil(t, 15_000);
    if (!ok) {
        out.appendLine(S.extension.debugRestoreFail);
        void vscode.window.showWarningMessage(S.extension.debugRestoreFail);
    }
    render();
    refreshToolbar();
}

// ---------------------------------------------------------------------------
// 重启控制器（D7，`13` §7；菜单「控制器 → 重启控制器」）
// 确认 → sys.restart → 主动断开 → 轮询自动重连（服务优雅退出 + RestartSec=2 + 启动）
// ---------------------------------------------------------------------------

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/** 重启后的自动重连：每 1.5s 试一次；成功则恢复 sys.info / 日志订阅 / 面板订阅 */
async function reconnectUntil(t: KxTarget, budgetMs: number): Promise<boolean> {
    const deadline = Date.now() + budgetMs;
    while (Date.now() < deadline) {
        await delay(1500);
        if (conn && conn.connected) {
            return true;
        }
        const cfg = readConfig();
        const c = getConnection(cfg, t);
        setLink('connecting');
        try {
            await c.connect();
        } catch {
            setLink('disconnected');
            continue;
        }
        setLink('connected');
        await sysInfo(true);
        subscribeLogs(cfg);
        reapplySubscriptions();
        return true;
    }
    return false;
}

/** `kine-x.portMax`：修改控制器端口数量上限（D9；缺 d9 明确拒绝，不伪装可用；重启后保留） */
async function setPortMax(): Promise<void> {
    const c = conn;
    if (!c || !c.connected) {
        void vscode.window.showWarningMessage(S.extension.notConnectedWithHint);
        return;
    }
    if (!controllerCaps.has('d9')) {
        out.appendLine(S.portMax.noD9);
        void vscode.window.showWarningMessage(S.portMax.noD9);
        return;
    }
    let cur = 0;
    let slots = 0;
    let used: number[] = [];
    try {
        const r = (await c.request('port.max.get')) as
            | { max?: number; slots?: number; used?: number[] }
            | undefined;
        cur = Number(r?.max ?? 0);
        slots = Number(r?.slots ?? 0);
        used = Array.isArray(r?.used) ? r.used.map((n) => Number(n)) : [];
    } catch (e) {
        reportError('port.max.get', e);
        return;
    }
    out.appendLine(portMaxSummary(cur, slots, used));
    const input = await vscode.window.showInputBox({
        prompt: S.portMax.prompt(cur, slots),
        value: String(cur),
        validateInput: (v) => parsePortMax(v, 1, slots).error ?? null,
    });
    if (input === undefined) {
        return;   // 用户取消
    }
    const parsed = parsePortMax(input, 1, slots);
    if (parsed.value === undefined) {
        void vscode.window.showWarningMessage(parsed.error ?? S.portMax.invalid(1, slots));
        return;
    }
    if (parsed.value === cur) {
        void vscode.window.showInformationMessage(S.portMax.unchanged(cur));
        return;
    }
    try {
        const r = (await c.request('port.max.set', { max: parsed.value })) as
            | { max?: number; prev?: number; file?: string }
            | undefined;
        const n = Number(r?.max ?? parsed.value);
        out.appendLine(S.portMax.logDone(n, String(r?.file ?? '')));
        void vscode.window.showInformationMessage(S.portMax.done(n, Number(r?.prev ?? cur)));
    } catch (e) {
        if (e instanceof KxRpcError && e.code === ErrorCodes.BUSY) {
            const msg = S.portMax.busy(e.message);
            out.appendLine(msg);
            void vscode.window.showWarningMessage(msg);
            return;
        }
        reportError('port.max.set', e);
    }
}

/** `kine-x.restart`：重启控制器（缺 d7 明确拒绝；不伪装可用） */
async function restartController(): Promise<void> {
    const c = conn;
    if (!c || !c.connected) {
        void vscode.window.showWarningMessage(S.extension.notConnectedWithHint);
        return;
    }
    if (!controllerCaps.has('d7')) {
        out.appendLine(S.extension.restartNoD7);
        void vscode.window.showWarningMessage(S.extension.restartNoD7);
        return;
    }
    const t = target ?? { host: readConfig().host, port: readConfig().port };
    const pick = await vscode.window.showWarningMessage(
        S.extension.restartConfirm(t.host, t.port),
        { modal: true },
        S.extension.restartBtn,
    );
    if (pick !== S.extension.restartBtn) {
        return;
    }
    try {
        await c.request('sys.restart');
    } catch (e) {
        reportError('sys.restart', e);
        return;
    }
    out.appendLine(S.extension.restartSent);
    void vscode.window.showInformationMessage(S.extension.restartSentToast);
    suppressAutoReconnect = true;   // 重启交接：由 reconnectUntil 负责连回
    autoReconnectInfo = '';
    c.disconnect();   // 主动断开：不等半开连接超时（服务即将退出）
    const ok = await reconnectUntil(t, 20_000);
    suppressAutoReconnect = false;
    autoReconnectAttempts = 0;
    if (ok) {
        out.appendLine(S.extension.restartBack(t.host, t.port));
        void vscode.window.showInformationMessage(S.extension.restartBackToast(t.host, t.port));
    } else {
        out.appendLine(S.extension.restartTimeout);
        void vscode.window.showWarningMessage(S.extension.restartTimeout);
    }
}

function getConnection(cfg: KxConfig, t: KxTarget): KxConnection {
    if (conn && conn.connected) {
        return conn;
    }
    if (conn) {
        conn.removeAllListeners();
    }
    const c = new KxConnection({
        host: t.host,
        port: t.port,
        token: cfg.token,
        timeoutMs: cfg.requestTimeoutMs,
    });
    c.on('state', (s: string) => {
        setLink(s === 'connected' ? 'connected' : 'disconnected');
        out.appendLine(S.extension.linkState(s === 'connected'));
        if (s !== 'connected') {
            scriptStatus = '';
            scriptSteps = 0;
            controllerEngine = '';
            controllerEngineRaw = '';
            controllerAxisCount = 0;
            render();
            refreshToolbar();   // 断线 → 脚本类按钮置灰、徽标回「未连接」
            // 面板与订阅随连接一起归零：服务端订阅已随连接消失，本地不得留「假在线」
            store.markDisconnected();
            controllerCaps.clear();
            subs?.reset();
            // 曲线窗口清空：断线后不得把上一段会话的曲线当作当前数据继续画
            KxCurvePanel.instance?.reset();
            refreshAllPanels();
            // 控制器文件列表回到「未连接」占位（清缓存 + 重绘 TreeView）
            void refreshControllerFiles();
            // 意外断线 → 自动重连（主动断开/重启/调试交接已置 suppress，不会触发）
            scheduleAutoReconnect();
        }
    });
    c.on('event', (ev: KxEvent) => onEvent(ev));
    c.on('raw', (dir: 'C' | 'S', line: string) => {
        if (readConfig().showRawProtocol) {
            rawOut.appendLine(`${dir} ${line}`);
        }
    });
    c.on('log', (m: string) => out.appendLine(m));
    c.on('error', (e: Error) => out.appendLine(S.extension.transportError(e.message)));

    subs?.dispose();
    subs = new SubscribeManager(c, cfg.subscribeHz, (reason) => {
        store.markSubUnavailable(reason);
        out.appendLine(S.extension.subscribeUnavailable(reason));
        refreshAllPanels();
    });

    conn = c;
    return c;
}

function teardown(): void {
    if (subs) {
        subs.dispose();
        subs = undefined;
    }
    if (conn) {
        conn.removeAllListeners();
        conn.disconnect();
        conn = undefined;
    }
}

// ---------------------------------------------------------------------------
// 命令实现
// ---------------------------------------------------------------------------

async function sysInfo(silent: boolean): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    try {
        const raw = (await c.request('sys.info')) as SysInfo | undefined;
        const info = raw ?? {};
        const caps = Array.isArray(info.caps) ? info.caps.map((x) => String(x).toLowerCase()) : undefined;
        const line =
            S.extension.sysInfoLine(
                info.ver ?? '?',
                info.engine ?? '?',
                String(info.axis_count ?? '?'),
            ) +
            (caps
                ? S.extension.sysInfoCaps(caps.length > 0 ? caps.join(',') : S.extension.capsNone)
                : '');
        out.appendLine(`[sys.info] ${line}`);
        applySysInfo(info, caps);
        if (typeof info.engine === 'string') {
            checkEngineAgainstWorkspace(info.engine);
        }
        if (!silent) {
            void vscode.window.showInformationMessage(S.extension.controllerLine(line));
        }
    } catch (e) {
        reportError('sys.info', e);
    }
}

let cmdHistory: string[] = [];

async function runCmd(): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    const line = await vscode.window.showInputBox({
        title: S.extension.cmdTitle,
        prompt: S.extension.cmdPrompt,
        value: cmdHistory.length > 0 ? cmdHistory[0] : '',
        ignoreFocusOut: false,
    });
    if (line === undefined) {
        return;
    }
    const trimmed = line.trim();
    if (trimmed.length === 0) {
        return;
    }
    cmdHistory = [trimmed, ...cmdHistory.filter((x) => x !== trimmed)].slice(0, 50);

    try {
        const r = (await c.request('cmd', { line: trimmed })) as CmdResult | undefined;
        const ret = r?.ret ?? 0;
        const lines = r?.out ?? [];
        out.appendLine(`> ${trimmed}`);
        if (lines.length === 0) {
            out.appendLine(S.extension.cmdNoOutput(ret));
        } else {
            for (const l of lines) {
                out.appendLine(`  ${l}`);
            }
        }
        out.show(true);
    } catch (e) {
        reportError(`cmd(${trimmed})`, e);
    }
}

// ---------------------------------------------------------------------------
// 下载与运行（T-09 / FR-3.1~3.3/3.6/3.7）
// ---------------------------------------------------------------------------

interface CompileResult {
    labels?: string[];
}

async function withScript(fn: (doc: vscode.TextDocument) => Promise<void>): Promise<void> {
    const doc = activeScriptDocument();
    if (doc) {
        await fn(doc);
    }
}

function activeScriptDocument(): vscode.TextDocument | undefined {
    const doc = vscode.window.activeTextEditor?.document;
    if (!doc) {
        void vscode.window.showWarningMessage(S.extension.openScriptFirst);
        return undefined;
    }
    if (doc.languageId !== 'kx-basic' && doc.languageId !== 'kx-lua') {
        void vscode.window.showWarningMessage(S.extension.notScriptFile);
        return undefined;
    }
    return doc;
}

/**
 * 下载 = `script.compile`（编译到**新引擎实例**，不碰运行中实例，docs/planA/13 §5.1）。
 * 引擎与扩展名不一致时**前置拦截**（FR-2.6、§6.4），绝不静默回退。
 */
async function download(doc: vscode.TextDocument, runAfter: boolean): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    const rel = vscode.workspace.asRelativePath(doc.uri);
    const resolved = resolveEngine(doc.uri.fsPath);
    if (!resolved.engine) {
        out.appendLine(S.extension.engineCheck(resolved.reason ?? ''));
        void vscode.window.showErrorMessage(resolved.reason ?? S.extension.engineMismatchFallback);
        return;
    }
    const engine = resolved.engine;

    // v0.4.7：控制器引擎（板端 SCRIPT_ENGINE，与目录里有无文件无关）与本地文件引擎冲突时
    // **本地拦截**，不发一条必然被 `ENGINE_MISMATCH` 拒绝的 script.compile；
    // 错误里直接给出「换脚本 / 改板端配置」两条出路（降级不伪装）。
    const conflict = controllerEngineConflict(controllerEngine, engine);
    if (conflict) {
        out.appendLine(S.extension.engineCheck(conflict));
        void vscode.window.showErrorMessage(conflict);
        return;
    }

    clearDiagnostics(doc);

    // T-16 热更新（`13` D4）：脚本运行中即「下载到运行中的引擎并原子替换」，不中断总线。
    // 控制器未声明 d4 时**明确提示先停止**，绝不静默失败（§4.4 降级纪律）。
    if (scriptStatus === 'READY' || scriptStatus === 'PAUSED') {
        if (!controllerCaps.has('d4')) {
            out.appendLine(S.extension.hotswapNoD4);
            void vscode.window.showWarningMessage(S.extension.hotswapNoD4Toast);
            return;
        }
        try {
            const r = (await c.request('script.compile', {
                src: doc.getText(),
                engine,
                swap: true,
                name: rel,          // D6：编译成功后控制器侧落盘（供「控制器文件/同步」拉取）
            })) as CompileResult | undefined;
            const labels = r?.labels ?? [];
            const labelText = labels.length > 0 ? labels.join(', ') : S.common.none;
            out.appendLine(S.extension.hotswapOk(rel, labelText));
            void vscode.window.showInformationMessage(S.extension.hotswapOkToast);
            void refreshControllerFiles();   // 下载成功 → 控制器文件列表自动出现新文件
            void sysInfo(true);              // 语言可能刚被绑定（auto→basic/lua）/ 能力位变化 → 刷新
        } catch (e) {
            if (e instanceof KxRpcError && e.code === ErrorCodes.COMPILE_ERROR) {
                handleCompileFailure(doc, e);
                return;
            }
            reportError('script.compile(swap)', e);
        }
        return;
    }

    try {
        const r = (await c.request('script.compile', {
            src: doc.getText(),
            engine,
            name: rel,          // D6：编译成功后控制器侧落盘（供「控制器文件/同步」拉取）
        })) as CompileResult | undefined;
        const labels = r?.labels ?? [];
        const labelText = labels.length > 0 ? labels.join(', ') : S.common.none;
        out.appendLine(S.extension.downloadOk(rel, labelText));
        void refreshControllerFiles();       // 下载成功 → 控制器文件列表自动出现新文件
        void sysInfo(true);                  // 语言可能刚被绑定（auto→basic/lua）/ 能力位变化 → 刷新
        if (runAfter) {
            await runScript();
        }
    } catch (e) {
        handleCompileFailure(doc, e);
    }
}

async function runScript(): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    try {
        const r = (await c.request('script.run')) as { status?: string } | undefined;
        out.appendLine(S.extension.runStatus(r?.status ?? 'READY'));
    } catch (e) {
        reportError('script.run', e);
    }
}

async function stopScript(): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    try {
        const r = (await c.request('script.stop')) as { status?: string } | undefined;
        out.appendLine(S.extension.stopStatus(r?.status ?? 'ABORTED'));
    } catch (e) {
        reportError('script.stop', e);
    }
}

// ---------------------------------------------------------------------------
// 同步（kine-x.sync）：控制器文件 → VSCodium 工作区（用户拍板 v0.4.5）
// 「下载」= 本地 → 控制器（单文件，编译并落盘）；「同步」= 控制器 → 本地（批量拉取）。
// 数据源 = D6 file.list / file.get；落点 = <工作区>/controller-sync/<文件名>。
// ---------------------------------------------------------------------------

/** 控制器侧文件清单条目（D6 file.list 回包；hash=FNV-1a 64 内容哈希，v0.8.4 起） */
interface ControllerFileEntry {
    name?: string;
    size?: number;
    hash?: string;
}

/** 工作区落点目录（相对首个工作区根） */
const SYNC_DIR = 'controller-sync';

/** 工作区落点 Uri；无工作区返回 undefined（明示，不猜落点） */
function syncTargetDir(): vscode.Uri | undefined {
    const ws = vscode.workspace.workspaceFolders?.[0];
    return ws ? vscode.Uri.joinPath(ws.uri, SYNC_DIR) : undefined;
}

/** 拉取一个控制器文件写入 controller-sync/；返回 null = 成功，否则为失败原因 */
/** 单文件拉取（file.get → 写工作区）。超时 15s（大文件/总线忙时 5s 易误判）+ 超时自动重试一次 */
async function pullControllerFile(c: KxConnection, name: string, dir: vscode.Uri): Promise<string | null> {
    const FETCH_TIMEOUT_MS = 15000;
    for (let attempt = 1; attempt <= 2; attempt++) {
        try {
            const f = (await c.request('file.get', { name }, FETCH_TIMEOUT_MS)) as
                | { src?: string }
                | undefined;
            await vscode.workspace.fs.writeFile(
                vscode.Uri.joinPath(dir, name),
                Buffer.from(f?.src ?? '', 'utf8'),
            );
            return null;
        } catch (e) {
            const isTimeout = e instanceof KxRpcError && e.code === ErrorCodes.TIMEOUT;
            if (isTimeout && attempt === 1) {
                out.appendLine(`[files] ${name}: 拉取超时，自动重试一次…`);
                continue;
            }
            return errText(e);
        }
    }
    return 'unreachable';
}

/**
 * 同步：把控制器中保存的脚本文件拉取到工作区。
 * 纪律（§4.4 降级不伪装）：缺 D6 → 明确提示需升级板端；无工作区 → 明确提示落点不可用。
 */
async function syncFromController(): Promise<void> {
    const c = requireConnected();
    if (!c) {
        return;
    }
    if (!controllerCaps.has('d6')) {
        out.appendLine(S.extension.syncNoD6Cap);
        void vscode.window.showWarningMessage(S.extension.syncNoD6Cap);
        return;
    }
    const dir = syncTargetDir();
    if (!dir) {
        void vscode.window.showWarningMessage(S.extension.syncNoWorkspace);
        return;
    }
    try {
        const r = (await c.request('file.list')) as { files?: ControllerFileEntry[] } | undefined;
        const names = (r?.files ?? [])
            .map((f) => String(f.name ?? ''))
            .filter((n) => n.length > 0);
        if (names.length === 0) {
            void vscode.window.showWarningMessage(S.extension.syncEmpty);
            return;
        }
        await vscode.workspace.fs.createDirectory(dir);
        let ok = 0;
        const failures: string[] = [];
        for (const n of names) {
            const err = await pullControllerFile(c, n, dir);
            if (err === null) {
                ok++;
            } else {
                failures.push(`${n}: ${err}`);
                out.appendLine(S.extension.syncFileFail(n, err));
            }
        }
        const summary =
            failures.length > 0
                ? S.extension.syncSummary(ok, failures.length)
                : S.extension.syncSummaryClean(ok);
        out.appendLine(`${summary} → ${SYNC_DIR}/`);
        out.show(true);
        void vscode.window.showInformationMessage(`${summary}（${SYNC_DIR}/）`);
        void refreshControllerFiles();   // 本地副本已更新 → 重算一致性标识
    } catch (e) {
        reportError('file.list', e);
    }
}

// ---------------------------------------------------------------------------
// 「控制器文件」原生 TreeView（v0.4.9；侧边栏 `kine-x.files`，不再画在控制台 Webview 里）
// 数据低频：仅 打开/手动刷新/下载成功/删除 后经 file.list 拉一次。
// ---------------------------------------------------------------------------

/** 文件列表数据缓存（refreshControllerFiles 拉取后落这里；filesTree 渲染用） */
const filesState = {
    dir: '',
    files: [] as Array<{ name: string; size: number; hash?: string; sync?: FileSyncState }>,
    error: '',
    /** D8 主文件（开机运行）清单（未连接/无 d8 时 name 为空） */
    boot: { name: '', valid: true, reason: '' },
};

/**
 * 逐文件比对本地副本（v0.8.4）：读 `controller-sync/<名>` 算同款 FNV-1a 64 哈希与远端比较。
 * 无工作区 / 读失败 → unknown；文件不存在 → missing。只在刷新时调用（文件少、低频）。
 */
async function localSyncStates(
    entries: Array<{ name: string; hash?: string }>,
): Promise<Map<string, FileSyncState>> {
    const out = new Map<string, FileSyncState>();
    const dir = syncTargetDir();
    if (!dir) {
        for (const e of entries) out.set(e.name, 'unknown');
        return out;
    }
    for (const e of entries) {
        try {
            const data = await vscode.workspace.fs.readFile(vscode.Uri.joinPath(dir, e.name));
            out.set(e.name, compareFileHash(e.hash, fnv1a64Hex(data)));
        } catch (err) {
            const notFound = err instanceof vscode.FileSystemError && err.code === 'FileNotFound';
            out.set(e.name, compareFileHash(e.hash, notFound ? null : undefined));
        }
    }
    return out;
}

/** filesState + 连接/能力 → TreeView 渲染输入（行模型在 filesPanelPure.buildFilesTreeRows） */
function filesTreeState(): FilesTreeState {
    return {
        connected: link === 'connected',
        hasD6: controllerCaps.has('d6'),
        hasD8: controllerCaps.has('d8'),
        boot: filesState.boot,
        cache: { dir: filesState.dir, files: filesState.files, error: filesState.error },
    };
}

/** 拉取控制器文件列表 + 主文件清单 → 更新缓存并重绘 TreeView */
async function refreshControllerFiles(): Promise<void> {
    const c = conn;
    if (!c || !c.connected || !controllerCaps.has('d6')) {
        filesState.dir = '';
        filesState.files = [];
        filesState.error = '';
        filesState.boot = { name: '', valid: true, reason: '' };
        filesTree?.refresh();
        return;
    }
    try {
        const r = (await c.request('file.list')) as
            | { dir?: string; files?: ControllerFileEntry[] }
            | undefined;
        filesState.dir = r?.dir ?? '';
        const entries = (r?.files ?? [])
            .map((f) => ({
                name: String(f.name ?? ''),
                size: Number(f.size ?? 0),
                hash: typeof f.hash === 'string' && f.hash.length > 0 ? f.hash : undefined,
            }))
            .filter((f) => f.name.length > 0);
        const syncMap = await localSyncStates(entries);
        filesState.files = entries.map((f) => ({ ...f, sync: syncMap.get(f.name) ?? 'unknown' }));
        filesState.error = '';
    } catch (e) {
        filesState.error = errText(e);
    }
    if (controllerCaps.has('d8')) {
        try {
            const b = (await c.request('boot.get')) as
                | { name?: string; valid?: boolean; reason?: string }
                | undefined;
            filesState.boot = {
                name: String(b?.name ?? ''),
                valid: b?.valid !== false,
                reason: String(b?.reason ?? ''),
            };
        } catch (e) {
            filesState.boot = { name: '', valid: true, reason: '' };
            out.appendLine(S.extension.errorLog('boot.get', errText(e)));
        }
    } else {
        filesState.boot = { name: '', valid: true, reason: '' };
    }
    filesTree?.refresh();
}

/** D8：设为主文件（开机运行）——下次启动生效；提供「立即重启应用」 */
async function setMainFile(name: string): Promise<void> {
    const c = conn;
    if (!c || !c.connected || !controllerCaps.has('d8')) {
        void vscode.window.showWarningMessage(S.filesPanel.noD8);
        return;
    }
    try {
        await c.request('boot.set', { name });
    } catch (e) {
        const err = errText(e);
        out.appendLine(S.filesPanel.setMainFail(name, err));
        void vscode.window.showErrorMessage(S.filesPanel.setMainFail(name, stripCode(err)));
        return;
    }
    out.appendLine(S.filesPanel.setMainDone(name));
    await refreshControllerFiles();
    const pick = await vscode.window.showInformationMessage(
        S.filesPanel.setMainDone(name),
        S.extension.restartBtn,
    );
    if (pick === S.extension.restartBtn) {
        void vscode.commands.executeCommand('kine-x.restart');
    }
}

/** D8：取消主文件（开机回退 SCRIPT_FILE 配置，未配置则不自动运行） */
async function clearMainFile(): Promise<void> {
    const c = conn;
    if (!c || !c.connected || !controllerCaps.has('d8')) {
        void vscode.window.showWarningMessage(S.filesPanel.noD8);
        return;
    }
    try {
        await c.request('boot.clear');
    } catch (e) {
        const err = errText(e);
        out.appendLine(S.filesPanel.clearMainFail(err));
        void vscode.window.showErrorMessage(S.filesPanel.clearMainFail(stripCode(err)));
        return;
    }
    out.appendLine(S.filesPanel.clearMainDone);
    await refreshControllerFiles();
}

/** 拉取单个文件到工作区（TreeView 行点击 / 行内「拉取」按钮） */
/** 单文件拉取在途集合（防抖：连点/行点击+行内按钮同时触发时只跑一次） */
const filePullInflight = new Set<string>();

async function pullFileToWorkspace(name: string): Promise<void> {
    const c = requireConnected();
    const dir = syncTargetDir();
    if (!c || !dir) {
        void vscode.window.showWarningMessage(
            c ? S.extension.syncNoWorkspace : S.extension.notConnectedWithHint,
        );
        return;
    }
    if (filePullInflight.has(name)) {
        out.appendLine(`[files] ${name}: 拉取进行中，忽略重复点击`);
        return;                          // 重复点击：静默合并（进度已在进行）
    }
    filePullInflight.add(name);
    try {
        await vscode.workspace.fs.createDirectory(dir);
        const err = await pullControllerFile(c, name, dir);
        if (err === null) {
            out.appendLine(`[files] ${S.filesPanel.pulled(name)}`);
            void refreshControllerFiles();   // 本地副本已更新 → 重算一致性标识
            void vscode.window.showInformationMessage(S.filesPanel.pulled(name));
        } else {
            out.appendLine(S.extension.syncFileFail(name, err));
            void vscode.window.showWarningMessage(S.extension.syncFileFail(name, err));
        }
    } finally {
        filePullInflight.delete(name);
    }
}

/** 删除控制器文件（TreeView 行内「删除」/ 右键菜单；先模态确认——删空目录会解绑语言） */
async function deleteControllerFile(name: string): Promise<void> {
    const c = conn;
    if (!c || !c.connected || !controllerCaps.has('d6')) {
        void vscode.window.showWarningMessage(S.filesPanel.notConnected);
        return;
    }
    const isMain =
        controllerCaps.has('d8') && filesState.boot.valid && filesState.boot.name === name;
    const pick = await vscode.window.showWarningMessage(
        isMain ? S.filesPanel.deleteConfirmMain(name) : S.filesPanel.deleteConfirm(name),
        { modal: true },
        S.filesPanel.deleteBtn,
    );
    if (pick !== S.filesPanel.deleteBtn) {
        return;
    }
    try {
        await c.request('file.del', { name });
        out.appendLine(`[files] ${S.filesPanel.deleteDone(name)}`);
    } catch (e) {
        const err = errText(e);
        out.appendLine(S.filesPanel.deleteFail(name, err));
        void vscode.window.showErrorMessage(S.filesPanel.deleteFail(name, stripCode(err)));
    }
    await refreshControllerFiles();   // 重新 file.list：目录清空与否影响脚本语言绑定
}

// ---------------------------------------------------------------------------
// 编译诊断（T-10 / FR-2.4）
// ---------------------------------------------------------------------------

function clearDiagnostics(doc: vscode.TextDocument): void {
    diagnostics.delete(doc.uri);
}

/** `COMPILE_ERROR` 落到 `err.line`；其余错误按协议人话提示（FR-7.4） */
function handleCompileFailure(doc: vscode.TextDocument, e: unknown): void {
    if (e instanceof KxRpcError && e.code === ErrorCodes.COMPILE_ERROR) {
        const line = e.line !== undefined && e.line > 0 ? e.line : 1;
        reportDiagnostic(doc, line, e.message);
        out.appendLine(
            S.extension.compileFailedLog(vscode.workspace.asRelativePath(doc.uri), line, e.message),
        );
        void vscode.window.showErrorMessage(S.extension.compileFailedToast(line, stripCode(e.message)));
        return;
    }
    reportError('script.compile', e);
}

/** 去掉 `[CODE]` 前缀与 `(line N)` 后缀，只留人话（诊断气泡不重复码） */
function stripCode(msg: string): string {
    return msg.replace(/^\[[A-Z_]+\]\s*/, '').replace(/\s*\(line \d+\)$/, '');
}

function reportDiagnostic(doc: vscode.TextDocument, line: number, msg: string): void {
    const idx = Math.min(Math.max(line - 1, 0), Math.max(doc.lineCount - 1, 0));
    const diag = new vscode.Diagnostic(doc.lineAt(idx).range, stripCode(msg), vscode.DiagnosticSeverity.Error);
    diag.source = 'kine-x';
    diagnostics.set(doc.uri, [diag]);
    revealLine(doc, idx);
}

function revealLine(doc: vscode.TextDocument, idx: number): void {
    const editor = vscode.window.visibleTextEditors.find(
        (e) => e.document.uri.toString() === doc.uri.toString(),
    );
    if (!editor) {
        return;
    }
    const pos = new vscode.Position(idx, 0);
    editor.selection = new vscode.Selection(pos, pos);
    editor.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
}

// ---------------------------------------------------------------------------
// 事件与订阅
// ---------------------------------------------------------------------------

function subscribeLogs(cfg: KxConfig): void {
    if (!subs) {
        return;
    }
    if (subs.isUnavailable) {
        out.appendLine(S.extension.subscribeDegrade(subs.degradeReason ?? ''));
        return;
    }
    subs.retain(['log']);
    out.appendLine(S.extension.subscribeRequest(clampHz(cfg.subscribeHz)));
}

function onEvent(ev: KxEvent): void {
    switch (ev.e) {
        case 'script': {
            const p = ev as unknown as ScriptEventPayload;
            scriptStatus = p.status ?? '';
            scriptSteps = typeof p.steps === 'number' ? p.steps : 0;
            out.appendLine(
                S.extension.scriptEvent(
                    scriptStatus,
                    scriptSteps > 0 ? ` (steps=${scriptSteps})` : '',
                ),
            );
            render();
            refreshToolbar();   // 运行/暂停/结束 → 菜单条状态徽标跟随
            flushConsole();     // 控制台状态行/菜单可用性跟随脚本状态（低频事件）
            break;
        }
        case 'log': {
            const s = typeof ev.s === 'string' ? ev.s : JSON.stringify(ev);
            out.appendLine(s);
            break;
        }
        case 'axis': {
            // 面板数据只进仓库、不落输出（避免 20Hz 刷屏）；按受影响面板定向刷新
            const changed = store.update(ev);
            if (changed.length > 0) {
                refreshPanels(changed);
            }
            // T-21：曲线可见时喂一帧；面板内部按「当前轴」过滤，不可见直接丢弃
            KxCurvePanel.instance?.push(numField(ev['axis']), ev);
            break;
        }
        case 'bus':
        case 'mb':
        case 'conn': {   // v0.8.8 修复：D10 通讯状态事件此前落 default（只记日志），面板无数据
            const changed = store.update(ev);
            if (changed.length > 0) {
                refreshPanels(changed);
            }
            break;
        }
        default:
            out.appendLine(S.extension.rawEvent(ev.e, JSON.stringify(ev)));
            break;
    }
}

// ---------------------------------------------------------------------------
// 状态面板与订阅装配（T-18 / T-19 / T-20）
// ---------------------------------------------------------------------------

/**
 * 注册「Kine-X 控制台」侧边栏视图（v0.4.2 起只承载菜单栏 + 状态行）。
 *
 * 为什么不再放状态表格：连接后 axis/bus/mb 最高 20Hz 推送，与菜单栏同视图会
 * 高频重建 DOM 冲掉菜单交互（「连接后菜单点不动」的根因）。
 * 表格迁至右侧「轴状态」面板（`kine-x.axis.open`，见 openAxisPanel）。
 *
 * 订阅策略：**轴状态面板可见才订阅**（引用计数 retain axis/bus/mb），不可见即释放（NFR-3）；
 * 面板首次可见时对轴取一次 `axis.snapshot`（FR-6.5：之后全靠事件推送，不轮询）。
 */
function registerPanels(context: vscode.ExtensionContext): void {
    consoleView = new KxConsoleView({
        buildSpec: () =>
            buildConsoleSpec(store, {
                connected: link === 'connected',
                target: currentTargetLabel(),
                scriptStatus,
                caps: controllerCaps,
                configEngine: readConfig().engine,
            }),
        // 控制台已无状态表格：可见性不再驱动订阅（订阅归轴状态面板与曲线）
        onVisible: () => {},
        onHidden: () => {},
    });
    consoleView.setLogger((line) => out?.appendLine(line));
    context.subscriptions.push(
        vscode.window.registerWebviewViewProvider(KxConsoleView.viewType, consoleView, {
            webviewOptions: { retainContextWhenHidden: true },
        }),
    );
}

/**
 * 注册「控制器文件」原生 TreeView（v0.4.9，侧边栏 `kine-x.files`）。
 *
 * 为什么用原生 TreeView 而非控制台内自绘：原生列表自带键盘导航/无障碍/主题、
 * 行内按钮（view/item/context inline）与右键菜单；文件数据低频（file.list），
 * 不会触发控制台那条「高频重绘冲掉菜单」的老问题。
 */
function registerFilesTree(context: vscode.ExtensionContext): void {
    filesTree = new KxFilesTreeProvider(() => filesTreeState());
    const view = vscode.window.createTreeView(KxFilesTreeProvider.viewType, {
        treeDataProvider: filesTree,
        showCollapseAll: false,
    });
    context.subscriptions.push(view);
}

/**
 * 侧边栏顶部**横向菜单条**（WebviewView，位于轴状态视图之上）。
 *
 * 为什么用 Webview：VS Code 侧边栏没有「容器标题下插自定义横条」的贡献点，
 * 唯一能做出真·横排按钮的办法就是固定高度的 Webview 视图（见 toolbarPure.ts 顶部说明）。
 *
 * 数据来源：本模块的全局状态（`link` / `scriptStatus` / `controllerCaps` / 连接目标），
 * 通过 hooks 注入，避免 toolbar.ts 反向依赖这些变量。
 */
function registerToolbar(context: vscode.ExtensionContext): void {
    // 菜单入口说明（v0.2.0 终版）：
    //   * **状态栏文字动作条**（本函数）：底部一排「连接/断开 · 下载 · 运行 · 停止 · 曲线 · 刷新」，
    //     点击直接执行命令——普通软件菜单栏的形态，标准 API，100% 可见；
    //   * **视图标题栏下拉**（package.json 的 submenus + view/title）：侧边栏内的悬停入口；
    //   * Webview 方案已废弃：侧边栏任何视图都被框架套上带折叠箭头的 section 标题行，
    //     与「普通菜单栏」观感相悖（toolbar.ts/toolbarPure.ts 保留备用，不再注册）。
    const mk = (priority: number, cmd: string): vscode.StatusBarItem => {
        const item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, priority);
        item.command = cmd;
        context.subscriptions.push(item);
        return item;
    };
    tbConnect = mk(97, 'kine-x.connect');
    tbDownload = mk(96, 'kine-x.download');
    tbRun = mk(95, 'kine-x.run');
    tbStop = mk(94, 'kine-x.stop');
    tbCurve = mk(93, 'kine-x.curve.open');
    tbRefresh = mk(92, 'kine-x.panel.refresh');
    refreshToolbar();
}

/**
 * 状态栏动作条同步（连接态 / 运行态 / 能力位变化时调用）：
 *   * 连接 ↔ 断开：同一槽位换文字与命令；
 *   * 脚本类（下载/运行/停止）：未连接 → 隐藏；已连接但调试口不能独占引擎（缺 d2）→ 隐藏；
 *   * 曲线/刷新：已连接才显示。
 */
function refreshToolbar(): void {
    if (!tbConnect) {
        return;
    }
    const connected = link === 'connected';
    const scriptAllowed = connected && controllerCaps.has('d2');

    tbConnect.text = connected ? S.toolbar.tbDisconnect : S.toolbar.tbConnect;
    tbConnect.tooltip = connected ? S.toolbar.disconnectTip : S.toolbar.connectTip;
    tbConnect.command = connected ? 'kine-x.disconnect' : 'kine-x.connect';

    tbDownload.text = S.toolbar.tbDownload;
    tbDownload.tooltip = S.toolbar.downloadTip;
    tbRun.text = S.toolbar.tbRun;
    tbRun.tooltip = S.toolbar.runTip;
    tbStop.text = S.toolbar.tbStop;
    tbStop.tooltip = S.toolbar.stopTip;
    tbCurve.text = S.toolbar.tbCurve;
    tbCurve.tooltip = S.toolbar.curveTip;
    tbRefresh.text = S.toolbar.tbRefresh;
    tbRefresh.tooltip = S.toolbar.refreshTip;

    scriptAllowed ? tbDownload.show() : tbDownload.hide();
    scriptAllowed ? tbRun.show() : tbRun.hide();
    scriptAllowed ? tbStop.show() : tbStop.hide();
    connected ? tbCurve.show() : tbCurve.hide();
    connected ? tbRefresh.show() : tbRefresh.hide();
    tbConnect.show();
}

/**
 * 切换本项目脚本语言环境（菜单「控制器 → 脚本语言 ▸ Basic / Lua」）。
 *
 * 落点：**本项目**（工作区）设置 `kine-x.engine`；无工作区时回退全局。
 * 生效链：`readConfig()` 每次现读 → 下一次连接/编译即用新语言；
 * 若控制器已在运行另一种引擎，连接/编译会收到 ENGINE_MISMATCH（明示，不静默）。
 */
async function setScriptEngine(lang: 'basic' | 'lua'): Promise<void> {
    const cfg = vscode.workspace.getConfiguration('kine-x');
    const scope = vscode.workspace.workspaceFolders?.length
        ? vscode.ConfigurationTarget.Workspace
        : vscode.ConfigurationTarget.Global;
    const scopeName =
        scope === vscode.ConfigurationTarget.Workspace
            ? S.toolbar.engineSetScopeWorkspace
            : S.toolbar.engineSetScopeGlobal;

    await cfg.update('engine', lang, scope);
    out.appendLine(S.extension.engineChanged(lang, scopeName));

    // 与控制器当前引擎比对：不一致要明示（连接/编译会被 ENGINE_MISMATCH 拦下）
    const notice =
        controllerEngine && controllerEngine !== lang ? S.toolbar.engineSetMismatch(engineLabel(controllerEngine)) : '';
    void vscode.window.showInformationMessage(S.toolbar.engineSetDone(lang.toUpperCase(), scopeName) + notice);
    render();
    flushConsole();   // 菜单「脚本语言」勾选标记（✓）跟随配置
}

/**
 * T-21：打开「实时曲线」Webview（Canvas）。曲线是**订阅驱动**的——
 * 面板可见才 retain `axis`，隐藏/关闭即 release（引用计数，不额外占带宽，FR-6.5）。
 */
function openCurve(): void {
    KxCurvePanel.createOrShow(store.axisCount, {
        retain: () => subs?.retain(['axis']),
        release: () => subs?.release(['axis']),
        notice: (m: string) => out.appendLine(S.extension.curveNotice(m)),
    });
}

/**
 * 打开右侧「轴状态」面板（菜单「工具 → 轴状态」）。
 * 形态与「工具 → 曲线」一致：编辑器区 WebviewPanel，同一时刻一个实例，再次点击即前置。
 * 面板可见时 retain axis/bus/mb 订阅 + 取一次轴快照（hooks 见 axisPanelHooks）。
 */
function openAxisPanel(): void {
    KxAxisPanel.createOrShow(axisPanelHooks());
}

/**
 * 打开右侧「Modbus 寄存器」面板（菜单「工具 → Modbus」）。
 * 与「轴状态」分开的独立面板（用户拍板 v0.4.3），可见才订阅 mb 主题。
 */
function openModbusPanel(): void {
    KxModbusPanel.createOrShow(modbusPanelHooks());
}

/** 「工具 → 通讯状态」（D10；缺 d10 由菜单置灰 + 命令内明确提示，降级不伪装） */
function openCommPanel(): void {
    if (controllerCaps.size > 0 && !controllerCaps.has('d10')) {
        out.appendLine(S.commPanel.noD10);
        void vscode.window.showWarningMessage(S.commPanel.noD10);
        return;
    }
    KxCommPanel.createOrShow(commPanelHooks());
}

function commPanelHooks(): CommPanelHooks {
    return {
        buildSpec: () => buildCommPanelSpec(store, { connected: link === 'connected' }),
        onVisible: () => subs?.retain(['conn']),
        onHidden: () => subs?.release(['conn']),
        log: (line) => out?.appendLine(line),
    };
}

/** 「工具 → Modbus 主站」（D13；planA/21） */
function openMbdevPanel(context: vscode.ExtensionContext): void {
    if (link !== 'connected') {
        void vscode.window.showWarningMessage(S.toolbar.statusOffline);
        return;
    }
    if (controllerCaps.size > 0 && !controllerCaps.has('d13')) {
        out.appendLine(S.mbdevPanel.sourceNone);
        void vscode.window.showWarningMessage(S.mbdevPanel.sourceNone);
        return;
    }
    KxMbdevPanel.createOrShow(mbdevPanelHooks(context));
}

function mbdevPanelHooks(context: vscode.ExtensionContext): MbdevPanelHooks {
    void context;
    return {
        async fetch() {
            if (link === 'connected' && conn && controllerCaps.has('d13')) {
                try {
                    const r = (await conn.request('mbdev.get')) as
                        | { exists?: boolean; text?: string }
                        | undefined;
                    let statusJson = '';
                    try {
                        const st = (await conn.request('mbdev.status')) as unknown;
                        statusJson = JSON.stringify(st ?? {});
                    } catch (e) {
                        out?.appendLine('[mbdev] mbdev.status 失败: ' + String(e));
                    }
                    const text = r && r.exists && r.text ? r.text : '{"version":1,"devices":[]}';
                    return buildMbdevSpec('controller', text, statusJson);
                } catch (e) {
                    out?.appendLine('[mbdev] mbdev.get 失败: ' + String(e));
                }
            }
            return buildMbdevSpec('none', '');
        },
        async save(text: string) {
            if (!(conn && link === 'connected')) {
                throw new Error('未连接控制器');
            }
            await conn.request('mbdev.set', { text });
            out?.appendLine(`[mbdev] 主站组态已下发（${text.length} 字节；固件即时热加载）`);
        },
        log: (line) => out?.appendLine(line),
    };
}

/** 「工具 → Modbus 配置」（D11）：控制器在线用 mbmap.get；否则用内置本地副本（面板内标注来源） */
function openMbmapPanel(context: vscode.ExtensionContext): void {
    if (link !== 'connected') {
        void vscode.window.showWarningMessage(S.toolbar.statusOffline);
        return;
    }
    if (controllerCaps.size > 0 && !controllerCaps.has('d12') && !controllerCaps.has('d11')) {
        out.appendLine(S.mbmapPanel.noD11);
        void vscode.window.showWarningMessage(S.mbmapPanel.noD11);
        return;
    }
    KxMbmapPanel.createOrShow(mbmapPanelHooks(context));
}

async function readLocalRegmap(context: vscode.ExtensionContext): Promise<string> {
    try {
        const buf = await vscode.workspace.fs.readFile(
            vscode.Uri.joinPath(context.extensionUri, 'data', 'regmap.json'),
        );
        return Buffer.from(buf).toString('utf8');
    } catch (e) {
        out?.appendLine('[mbmap] 读本地副本失败: ' + String(e));
        return '';
    }
}

function mbmapPanelHooks(context: vscode.ExtensionContext): MbmapPanelHooks {
    return {
        async fetch() {
            const hasD12 = controllerCaps.has('d12');
            const proto = hasD12 ? ('d12' as const) : ('d11' as const);
            const method = hasD12 ? 'mbreg.get' : 'mbmap.get';
            if (link === 'connected' && conn && (hasD12 || controllerCaps.has('d11'))) {
                try {
                    const r = (await conn.request(method)) as
                        | { exists?: boolean; text?: string }
                        | undefined;
                    if (r && r.exists && r.text) {
                        return buildMbmapSpec('controller', r.text, undefined, proto);
                    }
                    out?.appendLine(`[mbmap] ${method} 返回空（${method === 'mbreg.get' ? 'config/modbus.json' : '.mbmap'} 未写入）`);
                } catch (e) {
                    out?.appendLine(`[mbmap] ${method} 失败: ` + String(e));
                }
            }
            return buildMbmapSpec('local', await readLocalRegmap(context), undefined, proto);
        },
        async save(text: string) {
            if (!(conn && link === 'connected')) {
                throw new Error('未连接控制器');
            }
            const hasD12 = controllerCaps.has('d12');
            if (hasD12) {
                const bad = validateD12Text(text);          // 宿主侧权威校验（不依赖 webview 内联脚本）
                if (bad) {
                    out?.appendLine(`[mbmap] 组态校验未通过：${bad}`);
                    throw new Error(bad);
                }
            }
            await conn.request(hasD12 ? 'mbreg.set' : 'mbmap.set', { text });
            out?.appendLine(
                `[mbmap] 组态已下发（${hasD12 ? 'D12 mbreg.set' : 'D11 mbmap.set'}，${text.length} 字节；` +
                    '固件/脚本热加载生效）',
            );
        },
        log: (line) => out?.appendLine(line),
    };
}

/** 重连后按「当前可见的消费者」重建订阅引用计数（新 `SubscribeManager` 计数从零开始） */
function reapplySubscriptions(): void {
    if (!subs) {
        return;
    }
    if (KxAxisPanel.instance?.isVisible) {
        subs.retain(['axis', 'bus']);
    }
    if (KxModbusPanel.instance?.isVisible) {
        subs.retain(['mb']);
    }
    if (KxCommPanel.instance?.isVisible) {
        subs.retain(['conn']);
    }
    if (KxCurvePanel.instance?.isVisible) {
        subs.retain(['axis']);
    }
}

/** `sys.info` 落地到仓库与订阅开关（`16` §7.1 能力声明：启动即定 UI） */
function applySysInfo(info: SysInfo, caps: string[] | undefined): void {
    const axisCount = typeof info.axis_count === 'number' && info.axis_count > 0 ? info.axis_count : 0;
    store.onConnect(typeof info.engine === 'string' ? info.engine : '', axisCount);

    // 状态栏左区与「本地同步」都要用：引擎未知时不猜，按空处理（降级不伪装）。
    // 语言由控制器脚本目录推导（2026-09-25 拍板）：空目录=auto、两种并存=mixed 均视为
    // 「未绑定」，本地不拦截（由控制器裁决并给出明确错误）。
    const engineRaw = typeof info.engine === 'string' ? info.engine.toLowerCase() : '';
    controllerEngineRaw = engineRaw;
    controllerEngine = engineRaw === 'lua' ? 'lua' : engineRaw === 'basic' ? 'basic' : '';
    controllerAxisCount = axisCount;
    render();

    controllerCaps.clear();
    for (const c of caps ?? []) {
        controllerCaps.add(c);
    }
    refreshToolbar();   // 能力位（尤其 d2）变了 → 菜单条脚本类按钮可用性跟随
    // 轴数变化同步给曲线面板（下拉框项 + 当前轴夹取）
    KxCurvePanel.instance?.setAxisCount(axisCount);

    if (caps && !caps.includes('d3')) {
        // 控制器已明说没有 D3：不试、不重试、不刷日志，直接按降级渲染
        subs?.disable(S.extension.noD3);
        store.markSubUnavailable(subs?.degradeReason ?? S.extension.noSubscription);
    }
    refreshAllPanels();
    void refreshControllerFiles();   // 连接成功 → 控制器文件列表自动加载
}

let snapshotting = false;

/** 轴一次性快照（`axis.snapshot`）；越界/不支持只留日志，面板保持「未收到数据」 */
async function snapshotAxes(): Promise<void> {
    const c = conn;
    if (!c || !c.connected || snapshotting || store.axisCount <= 0) {
        return;
    }
    snapshotting = true;
    try {
        for (let i = 0; i < store.axisCount; i++) {
            try {
                const r = (await c.request('axis.snapshot', { axis: i })) as unknown;
                store.applyAxisSnapshot(i, r);
            } catch (e) {
                out.appendLine(S.extension.snapshotFailed(i, errText(e)));
            }
        }
    } finally {
        snapshotting = false;
    }
    refreshPanels(['axes']);
}

async function refreshPanelsCmd(): Promise<void> {
    const c = conn;
    if (!c || !c.connected) {
        refreshAllPanels();
        void vscode.window.showWarningMessage(S.extension.notConnectedWithHint);
        return;
    }
    // 手动刷新 = 重新对齐订阅（可能之前因 D3 缺失被标记降级）+ 重取轴快照
    subs?.resubscribe();
    store.markSubAvailable();
    await snapshotAxes();
    refreshAllPanels();
    void vscode.window.showInformationMessage(S.extension.panelsRefreshed);
}

/**
 * 控制台重绘（节流 80ms）。v0.4.2 起控制台只有菜单栏 + 状态行（低频数据），
 * 只在连接态/引擎/脚本状态变化时调用——**不随 axis/bus/mb 事件刷新**：
 * 这是「连接后菜单点不动」的根治手段（高频表格已迁至右侧轴状态面板）。
 */
let consoleFlushPending = false;

function flushConsole(): void {
    if (consoleFlushPending) {
        return;
    }
    consoleFlushPending = true;
    setTimeout(() => {
        consoleFlushPending = false;
        consoleView?.update();
    }, 80);
}

/**
 * 轴状态面板重绘：**稳压刷新**（v0.6.2）。
 * 订阅照旧按 kine-x.subscribeHz 收事件，但面板按 kine-x.axisRefreshHz（默认 10Hz，2~20）重绘，
 * 等待期间事件只置脏、不排队，到点总取最新快照——避免 20Hz 全量重绘的闪烁与节奏抖动。
 */
let axisFlushPending = false;
let axisLastRender = 0;

function flushAxisPanel(): void {
    if (axisFlushPending) {
        return;   // 已排程：到点取最新 spec
    }
    const hz = Math.min(20, Math.max(2, readConfig().axisRefreshHz || 10));
    const interval = Math.round(1000 / hz);
    const wait = Math.max(0, interval - (Date.now() - axisLastRender));
    axisFlushPending = true;
    setTimeout(() => {
        axisFlushPending = false;
        axisLastRender = Date.now();
        KxAxisPanel.instance?.update();
    }, wait);
}

/**
 * Modbus 面板重绘：与轴状态面板同款**稳压刷新**（v0.6.4）。
 * 按 kine-x.modbusRefreshHz（默认 10Hz，2~20）重绘，等待期间事件只置脏、到点取最新快照。
 */
let mbFlushPending = false;
let mbLastRender = 0;

function flushModbusPanel(): void {
    if (mbFlushPending) {
        return;   // 已排程：到点取最新 spec
    }
    const hz = Math.min(20, Math.max(2, readConfig().modbusRefreshHz || 10));
    const interval = Math.round(1000 / hz);
    const wait = Math.max(0, interval - (Date.now() - mbLastRender));
    mbFlushPending = true;
    setTimeout(() => {
        mbFlushPending = false;
        mbLastRender = Date.now();
        KxModbusPanel.instance?.update();
    }, wait);
}

/** 连接态/引擎/手动刷新等全局面板变化：控制台 + 两个面板都刷 */
function refreshAllPanels(): void {
    flushConsole();
    flushAxisPanel();
    flushModbusPanel();
}

/**
 * 状态事件落点：**按受影响面板定向刷新**（store.update 返回 PanelId[]）——
 * axes/bus 只刷轴状态面板，regs 只刷 Modbus 面板；都不碰控制台菜单。
 */
function refreshPanels(panels: PanelId[]): void {
    if (panels.includes('axes') || panels.includes('bus')) {
        flushAxisPanel();
    }
    if (panels.includes('regs')) {
        flushModbusPanel();
    }
    if (panels.includes('conn')) {
        KxCommPanel.instance?.update();
    }
}

// ---------------------------------------------------------------------------
// 引擎一致性（FR-2.6 / §6.4）
// ---------------------------------------------------------------------------

/**
 * 引擎裁决（D-03）已下沉到 `engineRule.resolveEngine`（`debugAdapter.ts` 也要用，避免循环依赖）。
 */
function warnEngineMismatch(fsPath: string): void {
    const r = resolveEngine(fsPath);
    if (r.reason) {
        out.appendLine(S.extension.engineCheck(r.reason));
    }
}

async function checkEngineAgainstWorkspace(engineRaw: string): Promise<void> {
    const raw = engineRaw.trim().toLowerCase();
    if (raw !== 'basic' && raw !== 'lua') {
        // auto/mixed/未知 = 语言未绑定：不做「工作区有另一种语言文件」告警（不猜）
        return;
    }
    const engine = raw === 'lua' ? 'lua' : 'basic';
    const cfg = readConfig();
    if (cfg.engineExplicit && cfg.engine !== engine) {
        out.appendLine(S.extension.engineSettingMismatch(engine, cfg.engine));
    }
    const wrongGlob = engine === 'lua' ? '**/*.bas' : '**/*.lua';
    const found = await vscode.workspace.findFiles(wrongGlob, '**/node_modules/**', 5);
    if (found.length > 0) {
        const names = found
            .map((u) => vscode.workspace.asRelativePath(u))
            .join(S.common.listSep);
        out.appendLine(
            S.extension.engineWorkspaceMismatch(engineLabel(engine), names) +
                S.extension.engineWorkspaceHint,
        );
    }
}

// ---------------------------------------------------------------------------
// 状态栏与工具函数
// ---------------------------------------------------------------------------

function requireConnected(): KxConnection | undefined {
    if (!conn || !conn.connected) {
        void vscode.window.showWarningMessage(S.extension.notConnectedWithHint);
        return undefined;
    }
    return conn;
}

function setLink(s: LinkState): void {
    link = s;
    render();
    refreshToolbar();
    flushConsole();   // 控制台状态行与菜单可用性跟随连接态（低频，不会冲菜单）
}

/**
 * 状态栏渲染（FR-1.4）：左区 = 品牌 + 运行概要，右区 = 连接状态。
 * 连接状态**永远在最右侧**，且连接成功后文字必须是「已连接 <ip>:<port>」。
 */
function render(): void {
    if (!statusConnItem) {
        return;
    }
    // 面板标题栏「连接 / 断开」按钮的 when 条件（package.json view/title）
    void vscode.commands.executeCommand('setContext', 'kine-x.connected', link === 'connected');

    const cfg = readConfig();
    const t = target ?? { host: cfg.host, port: cfg.port };
    const targetText = S.extension.targetLine(t.host, t.port);

    // —— 左区 · 运行概要（连接后才有内容；无内容时不显示假数据）——
    const parts: string[] = [];
    if (controllerEngineRaw !== '') {
        parts.push(S.extension.statusRuntimeEngine(engineDisplay(controllerEngineRaw), controllerAxisCount));
    }
    if (scriptStatus) {
        parts.push(S.extension.statusRuntimeScript(scriptStatus, scriptSteps));
    }
    if (parts.length === 0) {
        statusRuntimeItem.text = S.extension.statusRuntimeIdle;
        statusRuntimeItem.tooltip = S.extension.statusRuntimeTooltipIdle;
    } else {
        statusRuntimeItem.text = parts.join('  ·  ');
        statusRuntimeItem.tooltip = S.extension.statusRuntimeTooltip(
            controllerEngineRaw !== '' ? engineDisplay(controllerEngineRaw) : '?',
            controllerAxisCount,
            scriptStatus || '?',
            scriptSteps,
        );
    }

    // —— 右区 · 连接状态（固定最右）——
    switch (link) {
        case 'connecting':
            statusConnItem.text = S.extension.connConnecting(targetText);
            statusConnItem.tooltip = S.extension.connTooltipConnecting(targetText);
            statusConnItem.command = undefined;
            break;
        case 'connected':
            statusConnItem.text = S.extension.connConnected(targetText);
            statusConnItem.tooltip = S.extension.connTooltipConnected(targetText, scriptStatus);
            statusConnItem.command = 'kine-x.sysInfo';
            break;
        default:
            if (debugSessionActive) {
                statusConnItem.text = S.extension.connDebugOccupied;
                statusConnItem.tooltip = S.extension.connDebugOccupiedTooltip;
                statusConnItem.command = undefined;
                break;
            }
            statusConnItem.text = autoReconnectInfo !== '' ? autoReconnectInfo : S.extension.connDisconnected;
            statusConnItem.tooltip = S.extension.connTooltipDisconnected;
            statusConnItem.command = 'kine-x.connect';
            break;
    }
}

function reportError(method: string, e: unknown): void {
    const msg = errText(e);
    out.appendLine(S.extension.errorLog(method, msg));
    void vscode.window.showErrorMessage(S.extension.errorToast(method, msg));
}

function errText(e: unknown): string {
    return e instanceof Error ? e.message : String(e);
}

/** 事件里的轴号（缺失/非法按轴 0 处理；曲线面板会再按「当前轴」过滤） */
function numField(v: unknown): number {
    return typeof v === 'number' && Number.isFinite(v) ? Math.trunc(v) : 0;
}
