#!/usr/bin/env node
// mock-debug-server.mjs —— Mock 调试通道服务端（状态机模拟，见 docs/planA/16 §5）
//
// 目的：控制器侧 DebugServer（docs/planA/13）尚未实施时，让插件可跑通
//       「连接→编译→运行→变量→面板→事件→错误/降级」全链路。
// 边界：不接 EtherCAT、不驱动电机；轴数据均为**设定值**，仅模拟协议行为。
//
// 用法：
//   node tools/mock-debug-server.mjs [--port 5000] [--engine basic|lua|auto] [--caps d1,d2,d3,d4,d5] [--verbose]
//   --engine auto：对齐控制器 2026-09-25 的语言绑定——空文件目录两种语言都收，带 name 落盘后绑定
//
// 能力开关（--caps，模拟 13 的 D1~D5 就绪度）：
//   d1 基础 RPC（sys.info / var.* / axis.snapshot / cmd）
//   d2 脚本生命周期（script.compile / run / stop / status）
//   d3 事件订阅（subscribe / unsubscribe + 周期推送：axis / log / bus / mb）
//   d4 热更新（script.compile 带 p.swap=true：编译到新实例并原子替换，运行中不中断），缺省关闭 → 回 NOT_SUPPORTED
//   d5 引擎暂停钩子（pause / resume / step / breakpoint.*），缺省关闭 → 回 NOT_SUPPORTED
//
// 运行态（d5 打开时）：
//   * 运行中命中 `breakpoint.add` 登记的行 → 推 `script PAUSED` 事件（带 line）并挂起；
//   * `script.pause` → 挂起并推 PAUSED；`script.step` → 行号 +1 后推 PAUSED；`script.resume` → 推 READY 继续。
//
// 注入错误的约定（便于测降级/诊断路径）：
//   * 某行仅写 `MOVEABS`（参数不足）→ COMPILE_ERROR，line=该行号
//   * 源码含 `@runtime-error`      → 运行结束为 RUNTIME_ERROR
//   * 源码含 `@budget`             → 运行结束为 BUDGET_EXCEEDED
//   * 请求 engine 与当前绑定不一致  → ENGINE_MISMATCH（绑定规则同上；mixed 一律拒绝）

import * as net from 'node:net';

const argv = process.argv.slice(2);
function opt(name, def) {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 && i + 1 < argv.length ? argv[i + 1] : def;
}
const PORT = Number(opt('port', '5000'));
// --engine basic|lua|auto（auto = 对齐控制器 2026-09-25：语言由已保存脚本推导，空目录两种都收）
const ENGINE_ARG = String(opt('engine', 'basic')).toLowerCase();
const ENGINE = ENGINE_ARG === 'lua' ? 'lua' : ENGINE_ARG === 'auto' ? 'auto' : 'basic';
const CAPS = new Set(
    String(opt('caps', 'd1,d2,d3'))
        .split(',')
        .map((s) => s.trim().toLowerCase())
        .filter(Boolean),
);
// D6/D7 恒报（与板端一致）：D6 文件管理恒实装，D7 重启在 Mock 中恒定声明（sys.restart 只记录）
CAPS.add('d6');
CAPS.add('d7');
CAPS.add('d8');   // D8 主文件（开机运行）：Mock 恒声明，boot.* 只改内存状态
CAPS.add('d9');   // D9 端口数量上限：Mock 恒声明，port.max.* 只改内存状态（默认 16）
CAPS.add('d10');  // D10 通讯状态：Mock 恒声明（conn 订阅返回演示连接/从站数据）
const VERBOSE = argv.includes('--verbose');
const VER = '0.9.0-mock';
const AXIS_COUNT = 1;

const has = (cap) => CAPS.has(cap);
const now = () => Date.now();

/** 语言绑定（对齐控制器 debug_server.cpp scan_lang_bind）：--engine auto 时由已保存脚本推导 */
/** FNV-1a 64（十六进制 16 字符）——与控制器 debug_server.cpp / 插件 filesPanelPure.fnv1a64Hex 同算法 */
function fnv1a64Hex(text) {
    const PRIME = 0x100000001b3n;
    const MASK = 0xffffffffffffffffn;
    let h = 0xcbf29ce484222325n;
    for (const b of Buffer.from(text, 'utf8')) {
        h ^= BigInt(b);
        h = (h * PRIME) & MASK;
    }
    return h.toString(16).padStart(16, '0');
}

function bindLang(sess) {
    if (ENGINE !== 'auto') return ENGINE;
    let bas = false;
    let lua = false;
    for (const n of sess.files.keys()) {
        if (n.endsWith('.bas')) bas = true;
        else if (n.endsWith('.lua')) lua = true;
    }
    return bas && lua ? 'mixed' : bas ? 'basic' : lua ? 'lua' : 'auto';
}

/** D8 主文件清单状态（对齐控制器 boot.get） */
function bootInfo(sess) {
    const dir = '/userdata/kine-x/scripts';
    const name = sess.boot || '';
    if (!name) return { dir, name: '', valid: true };
    if (!sess.files.has(name)) {
        return { dir, name, valid: false, reason: `主文件在控制器上不存在：${name}` };
    }
    if (bindLang(sess) === 'mixed') {
        return { dir, name, valid: false, reason: '脚本目录同时存在 .bas 与 .lua，语言不唯一' };
    }
    return { dir, name, valid: true };
}
const log = (...a) => {
    if (VERBOSE) console.log(`[mock ${new Date().toISOString().slice(11, 23)}]`, ...a);
};

// ---------------------------------------------------------------------------
// 会话状态
// ---------------------------------------------------------------------------

function newSession(socket) {
    return {
        socket,
        buf: '',
        compiled: null, // { src, engine, labels }
        boot: '',       // D8：主文件（开机运行）清单
        portMax: 16,    // D9：端口数量上限（运行期；Mock 不落盘）
        status: 'IDLE', // IDLE|READY|PAUSED|DONE|ABORTED|RUNTIME_ERROR|BUDGET_EXCEEDED|COMPILE_ERROR
        steps: 0,
        errorLine: 0,
        /** 当前执行行（d5 挂起/单步用，`script` 事件里带出为 `line`） */
        line: 0,
        breakpoints: [],
        vars: new Map([
            ['SPEED', 100],
            ['POS', 12.5],
            ['FLAG', 1],
        ]),
        /** D6 文件管理：name → src（模拟 DEBUG_SCRIPT_DIR，仅内存） */
        files: new Map(),
        subs: new Map(), // topic -> interval
        runTimers: [],
        runSeq: 0,
        t0: now(),
    };
}

const varType = (v) => (typeof v === 'number' ? 'num' : typeof v === 'string' ? 'str' : 'nil');

function axisSnapshot(sess) {
    const since = (now() - sess.t0) / 1000;
    const moving = sess.status === 'READY';
    const mpos = moving ? 12.5 + since * 3 : 12.5;
    return {
        bus_ok: 1,
        mpos: Number(mpos.toFixed(3)),
        dpos: Number(mpos.toFixed(3)),
        idle: moving ? 0 : 1,
        enabled: 1,
        alarm: 0,
        // 15 FR-6.1：轴状态位（位表见 §4.6）与错误码。运行中置 bit1（随动误差告警），便于验证面板解码。
        axis_status: moving ? 1 << 1 : 0,
        err_code: 0,
    };
}

/** 15 FR-6.2：总线节点（设定值，仅演示协议行为） */
function busSnapshot() {
    return {
        node_count: 2,
        nodes: [
            { index: 0, axis_count: 1, status: 8, io: 0x0f, aio: 0 },
            { index: 1, axis_count: 4, status: 4, io: 0x00, aio: 2 },
        ],
    };
}

/** 15 FR-6.3：Modbus 4x 分段推送（start + regs[]），验证插件侧分段合并 */
// v0.8.6：演示「已用寄存器」位图（与板端脚本的典型集合一致；4×16 位十六进制）
const MB_USED_DEMO = [3, 10, 11, 12, 13, 120, 122, 123, 140];
function mbUsedWords() {
    const w = [0n, 0n, 0n, 0n];
    for (const a of MB_USED_DEMO) {
        w[a >> 6] |= 1n << BigInt(a & 63);
    }
    return w.map((v) => v.toString(16).padStart(16, '0'));
}

// D10：通讯状态演示数据（标签与板端脚本 port_tag_declare 一致）
function connPayload() {
    return {
        conns: [
            { port: 10, kind: 'TCP_SERVER', conn: true, listen: 4321, peer_port: 51234,
              target: 'listen:4321', peer: '192.168.1.3', tag: '4321 ASCII 服务端', role: '—' },
            { port: 11, kind: 'TCP_CLIENT', conn: true, listen: 0, peer_port: 10123,
              target: '192.168.1.80:10123', peer: '192.168.1.80', tag: '称重网关 TCP 客户端', role: 'Modbus 主站' },
            { port: 12, kind: 'TCP_CLIENT', conn: true, listen: 0, peer_port: 4320,
              target: '192.168.1.221:4320', peer: '192.168.1.221', tag: '机器人 TCP 客户端', role: '—' },
            { port: 13, kind: 'TCP_SERVER', conn: true, listen: 502, peer_port: 51235,
              target: 'listen:502', peer: '192.168.1.3', tag: 'Modbus-TCP 从站(502)', role: '从站' },
        ],
        bus: {
            link_up: 1, slaves_responding: 1, master_al: 8, slave_al: 8,
            slave_online: 1, slave_op: 1, node_count: 1,
            slaves: [
                { index: 0, axis: 0, online: 1, al_state: 8, vid: 0x00100000, pid: 0x000c0112,
                  rev: 2, name: 'SV630NS1R6I' },
            ],
        },
    };
}

function mbSegment(n) {
    const start = (n % 2) * 16;
    const regs = [];
    for (let i = 0; i < 16; i++) {
        regs.push(start + i + ((n * 7) % 100));
    }
    return { start, regs, used: mbUsedWords() };
}

function sendEvent(sess, ev) {
    const s = sess.socket;
    if (!s || s.destroyed) return;
    s.write(JSON.stringify(ev) + '\n');
}

function clearRun(sess) {
    for (const t of sess.runTimers) clearTimeout(t);
    sess.runTimers = [];
}

function emitScript(sess, status, steps, errorLine, line) {
    sess.status = status;
    sess.steps = steps;
    if (errorLine !== undefined) sess.errorLine = errorLine;
    if (line !== undefined) sess.line = line;
    const ev = { e: 'script', t: now(), status, steps, error_line: sess.errorLine };
    // d5 挂起/单步：带上当前执行行（字段名待 13 冻结，见 15 §8.5 注）
    if (status === 'PAUSED') ev.line = sess.line;
    sendEvent(sess, ev);
}

function emitLog(sess, s, lvl = 'info') {
    sendEvent(sess, { e: 'log', t: now(), s, lvl });
}

// ---------------------------------------------------------------------------
// 方法实现（返回 { r } 或 { err }）
// ---------------------------------------------------------------------------

function doCompile(sess, p) {
    const src = typeof p?.src === 'string' ? p.src : '';
    const bind = bindLang(sess);
    const fallback = bind === 'basic' || bind === 'lua' ? bind : 'basic';
    const engine = String(p?.engine ?? fallback).toLowerCase();
    const swap = p?.swap === true;
    if (bind === 'mixed') {
        return { err: { code: 'ENGINE_MISMATCH', msg: '控制器脚本目录同时存在 .bas 与 .lua：请先删除其中一种再编译' } };
    }
    if ((bind === 'basic' || bind === 'lua') && engine !== bind) {
        return { err: { code: 'ENGINE_MISMATCH', msg: `控制器脚本目录中为 ${bind} 脚本，不能编译 ${engine}（语言由控制器现有脚本决定）` } };
    }
    if (swap && !has('d4')) {
        // 热更新需 13 D4：未声明时**明确拒绝**，插件据此提示「先停止再下载」（不伪装）
        return { err: { code: 'NOT_SUPPORTED', msg: '热更新需运行中原子替换(13 D4)' } };
    }
    const lines = src.split('\n');
    for (let i = 0; i < lines.length; i++) {
        if (lines[i].trim() === 'MOVEABS') {
            return { err: { code: 'COMPILE_ERROR', msg: '参数不足: MOVEABS', line: i + 1 } };
        }
    }
    const labels = engine === 'lua' ? ['init', 'loop'] : ['main'];
    const wasRunning = sess.status === 'READY' || sess.status === 'PAUSED';
    // 编译到**新实例**（不碰运行中实例），swap=true 时再原子替换（13 §5.1）
    sess.compiled = { src, engine, labels };
    sess.errorLine = 0;
    if (swap) {
        emitLog(sess, `[hotswap] 已原子替换运行中脚本（labels: ${labels.join(', ')}）`, 'info');
    }
    // D6：可选 name——编译成功后落盘（内存模拟），供 file.list/get 拉取
    const r = { labels, swapped: swap && wasRunning };
    const name = typeof p?.name === 'string' ? p.name : '';
    if (name.length > 0) {
        if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(name)) {
            return { err: { code: 'BAD_PARAM', msg: 'name 只能是纯文件名（字母/数字/._-，不以 . 开头）' } };
        }
        sess.files.set(name, src);
        r.saved = true;
    }
    return { r };
}

function doRun(sess) {
    if (!sess.compiled) {
        return { err: { code: 'RUNTIME_ERROR', msg: '尚未编译脚本（script.compile）' } };
    }
    if (sess.status === 'READY' || sess.status === 'PAUSED') {
        return { err: { code: 'BUSY', msg: '脚本正在运行' } };
    }
    clearRun(sess);
    sess.errorLine = 0;
    sess.status = 'READY';
    sess.steps = 0;
    sess.line = 1;

    const src = sess.compiled.src;
    const seq = ++sess.runSeq;
    const at = (ms, fn) =>
        sess.runTimers.push(
            setTimeout(() => {
                if (seq !== sess.runSeq) return;
                fn();
            }, ms),
        );

    at(0, () => emitScript(sess, 'READY', 0));
    if (/PRINT/i.test(src)) {
        at(40, () => emitLog(sess, 'hi', 'info'));
    }
    // d5：登记了断点 → 运行到该行**挂起**（真实命中路径，供 T-17 验证）
    const bpLine = has('d5') ? bpLines(sess)[0] : undefined;
    if (bpLine !== undefined) {
        at(300, () => emitScript(sess, 'PAUSED', 150, undefined, bpLine));
    } else if (src.includes('@runtime-error')) {
        at(600, () => emitScript(sess, 'RUNTIME_ERROR', 3210, 42));
    } else if (src.includes('@budget')) {
        at(600, () => emitScript(sess, 'BUDGET_EXCEEDED', 5000000));
    } else {
        at(800, () => emitScript(sess, 'DONE', 8123));
    }
    return { r: { status: 'READY' } };
}

function doStop(sess) {
    if (sess.status !== 'READY' && sess.status !== 'PAUSED') {
        return { r: { status: sess.status } };
    }
    clearRun(sess);
    sess.runSeq++;
    emitScript(sess, 'ABORTED', Math.max(sess.steps, 4200));
    return { r: { status: 'ABORTED' } };
}

function startSub(sess, topic, hz) {
    if (sess.subs.has(topic)) return;
    const period = Math.max(50, Math.round(1000 / Math.max(1, hz || 20)));
    let n = 0;
    const timer = setInterval(() => {
        n++;
        if (topic === 'axis') {
            // 多轴控制器需在事件里带轴号（topic 只有 'axis'）——此字段待 13 冻结，见 15 §8.5 注
            sendEvent(sess, { e: 'axis', t: now(), axis: 0, ...axisSnapshot(sess) });
        } else if (topic === 'log') {
            sendEvent(sess, { e: 'log', t: now(), s: `[script] loop=${n}`, lvl: 'info' });
        } else if (topic === 'bus') {
            sendEvent(sess, { e: 'bus', t: now(), ...busSnapshot() });
        } else if (topic === 'mb') {
            sendEvent(sess, { e: 'mb', t: now(), ...mbSegment(n) });
        } else if (topic === 'conn') {
            sendEvent(sess, { e: 'conn', t: now(), ...connPayload() });
        } else {
            sendEvent(sess, { e: topic, t: now(), seq: n });
        }
    }, period);
    sess.subs.set(topic, timer);
}

function doSubscribe(sess, p) {
    const topics = Array.isArray(p?.topics) ? p.topics.map(String) : [];
    const hz = Number(p?.hz ?? 20);
    for (const t of topics) startSub(sess, t, hz);
    return { r: { sub: [...sess.subs.keys()] } };
}

function doUnsubscribe(sess, p) {
    const topics = Array.isArray(p?.topics) ? p.topics.map(String) : [];
    for (const t of topics) {
        const timer = sess.subs.get(t);
        if (timer) clearInterval(timer);
        sess.subs.delete(t);
    }
    return { r: { sub: [...sess.subs.keys()] } };
}

const CMD_WHITELIST = new Set(['STA', 'POS', 'MPOS', 'EN', 'DIS', 'STOP']);

function doCmd(sess, p) {
    const line = String(p?.line ?? '').trim();
    if (line.length === 0) {
        return { err: { code: 'BAD_PARAM', msg: '命令为空' } };
    }
    if (line.toUpperCase() === 'MOVEABS') {
        return { err: { code: 'RUNTIME_ERROR', msg: '参数不足: MOVEABS' } };
    }
    const head = line.split(/\s+/)[0].toUpperCase();
    if (!CMD_WHITELIST.has(head)) {
        return { err: { code: 'RUNTIME_ERROR', msg: `未知命令: ${head}` } };
    }
    const snap = axisSnapshot(sess);
    const out = head === 'POS' || head === 'MPOS' ? [snap.mpos.toFixed(3)] : [];
    return { r: { ret: 0, out } };
}

/** 已登记断点的行号（忽略只给了 label 的项） */
function bpLines(sess) {
    return sess.breakpoints.map((b) => Number(b.line)).filter((n) => Number.isInteger(n) && n > 0);
}

/** 续跑后重新武装「结束」定时器（resume 用；与 doRun 尾部同语义） */
function armCompletion(sess) {
    const src = sess.compiled?.src ?? '';
    const seq = sess.runSeq;
    const at = (ms, fn) =>
        sess.runTimers.push(
            setTimeout(() => {
                if (seq !== sess.runSeq) return;
                fn();
            }, ms),
        );
    if (/PRINT/i.test(src)) at(40, () => emitLog(sess, 'hi', 'info'));
    if (src.includes('@runtime-error')) {
        at(600, () => emitScript(sess, 'RUNTIME_ERROR', 3210, 42));
    } else if (src.includes('@budget')) {
        at(600, () => emitScript(sess, 'BUDGET_EXCEEDED', 5000000));
    } else {
        at(800, () => emitScript(sess, 'DONE', 8123));
    }
}

function doD5(method, sess, p) {
    if (!has('d5')) {
        return { err: { code: 'NOT_SUPPORTED', msg: `${method} 需引擎 pause 钩子(13 D5)` } };
    }
    switch (method) {
        case 'breakpoint.list':
            return { r: { breakpoints: sess.breakpoints } };
        case 'breakpoint.add': {
            const bp = p?.line !== undefined ? { line: Number(p.line) } : { label: String(p?.label ?? '') };
            sess.breakpoints.push(bp);
            return { r: { breakpoints: sess.breakpoints } };
        }
        case 'breakpoint.del':
            // 不指定 line/label = 清空全部（供 DAP setBreakpoints 的「全量替换」语义使用）
            if (p?.line === undefined && p?.label === undefined) {
                sess.breakpoints = [];
            } else {
                sess.breakpoints = sess.breakpoints.filter((b) =>
                    p?.line !== undefined ? b.line !== Number(p.line) : b.label !== String(p?.label ?? ''),
                );
            }
            return { r: { breakpoints: sess.breakpoints } };
        case 'script.pause':
            // 挂起：停掉待发的结束定时器，在最近断点行（无则当前行）停住
            clearRun(sess);
            emitScript(sess, 'PAUSED', sess.steps, undefined, bpLines(sess)[0] ?? (sess.line || 1));
            return { r: { status: 'PAUSED' } };
        case 'script.step':
            // 单步：行号 +1 后再次挂起
            clearRun(sess);
            emitScript(sess, 'PAUSED', sess.steps, undefined, Math.max(1, (sess.line || 1) + 1));
            return { r: { status: 'PAUSED' } };
        case 'script.resume':
            sess.status = 'READY';
            emitScript(sess, 'READY', sess.steps);
            armCompletion(sess);
            return { r: { status: 'READY' } };
        default:
            return { err: { code: 'UNKNOWN_METHOD', msg: method } };
    }
}

function handle(sess, req) {
    const m = String(req.m);
    const p = req.p;
    switch (m) {
        case 'sys.info':
            // d6/d7/d8 与板端一致：恒实装、恒声明
            return {
                r: {
                    ver: VER,
                    engine: bindLang(sess),
                    boot: sess.boot,
                    axis_count: AXIS_COUNT,
                    caps: [...new Set([...CAPS, 'd6', 'd7', 'd8', 'd9'])],
                },
            };
        // ---- D6 文件管理（板端恒实装，caps 恒含 d6；目录仅为内存 Map）----
        case 'file.list': {
            const files = [...sess.files.keys()]
                .sort()
                .map((name) => ({
                    name,
                    size: Buffer.byteLength(sess.files.get(name) ?? ''),
                    hash: fnv1a64Hex(sess.files.get(name) ?? ''),   // v0.8.4：插件一致性标识用
                }));
            return { r: { dir: '/userdata/kine-x/scripts (mock)', files } };
        }
        case 'file.get': {
            const name = String(p?.name ?? '');
            // 与板端一致：先做纯文件名校验（防路径穿越 → BAD_PARAM），再查存在性（NOT_FOUND）
            if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(name)) {
                return { err: { code: 'BAD_PARAM', msg: 'name 只能是纯文件名（字母/数字/._-，不以 . 开头）' } };
            }
            if (!sess.files.has(name)) {
                return { err: { code: 'NOT_FOUND', msg: `文件不存在: ${name}` } };
            }
            return { r: { name, src: sess.files.get(name) } };
        }
        case 'file.del': {
            const name = String(p?.name ?? '');
            if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(name)) {
                return { err: { code: 'BAD_PARAM', msg: 'name 只能是纯文件名（字母/数字/._-，不以 . 开头）' } };
            }
            if (!sess.files.has(name)) {
                return { err: { code: 'NOT_FOUND', msg: `文件不存在: ${name}` } };
            }
            sess.files.delete(name);
            return { r: { deleted: true } };
        }
        // ---- D8 主文件（开机运行）：只改内存清单，不真的重启 ----
        case 'port.max.get':
            return {
                r: {
                    max: sess.portMax,
                    slots: 64,
                    default: 16,
                    dir: '/userdata/kine-x/scripts (mock)',
                    used: [],
                },
            };
        case 'port.max.set': {
            const n = p?.max;
            if (typeof n !== 'number' || !Number.isInteger(n)) {
                return { err: { code: 'BAD_PARAM', msg: '缺少 max（端口数量上限，1..64 的整数）' } };
            }
            if (n < 1 || n > 64) {
                return { err: { code: 'BAD_PARAM', msg: '端口数量必须在 1..64 之间' } };
            }
            const prev = sess.portMax;
            sess.portMax = n;
            return {
                r: { max: n, prev, saved: true, file: '/userdata/kine-x/scripts (mock)/.portmax' },
            };
        }
        case 'boot.get':
            return { r: bootInfo(sess) };
        case 'boot.set': {
            const name = String(p?.name ?? '');
            if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(name)) {
                return { err: { code: 'BAD_PARAM', msg: 'name 只能是纯文件名（字母/数字/._-，不以 . 开头）' } };
            }
            if (!/\.(bas|lua)$/.test(name)) {
                return { err: { code: 'BAD_PARAM', msg: `主文件必须是 .bas 或 .lua：${name}` } };
            }
            if (!sess.files.has(name)) {
                return { err: { code: 'NOT_FOUND', msg: `控制器上没有该文件: ${name}（先「下载」保存）` } };
            }
            const bind = bindLang(sess);
            if (bind === 'mixed') {
                return { err: { code: 'ENGINE_MISMATCH', msg: '脚本目录同时存在 .bas 与 .lua，请先删除一种' } };
            }
            const lang = name.endsWith('.lua') ? 'lua' : 'basic';
            if ((bind === 'basic' || bind === 'lua') && lang !== bind) {
                return { err: { code: 'ENGINE_MISMATCH', msg: `控制器脚本目录中为 ${bind} 脚本，不能把 ${lang} 文件设为主文件` } };
            }
            sess.boot = name;
            return { r: { name, valid: true } };
        }
        case 'boot.clear':
            sess.boot = '';
            return { r: { name: '' } };
        case 'sys.ping':
            return { r: {} };
        case 'sys.restart':
            // D7：Mock 只回应答 + 落一条日志，不真的重启进程（真机由 DEBUG_RESTART_CMD 执行）
            if (!has('d7')) {
                return { err: { code: 'NOT_SUPPORTED', msg: '重启需 13 D7（能力开关 --caps 含 d7）' } };
            }
            emitLog(sess, '[restart] 已请求控制器重启（Mock：仅记录，不重启）', 'info');
            return { r: { restarting: true } };
        case 'auth':
            // docs/planA/13 §4.5：首帧可选 token 校验。Mock 一律放行（--token 未实现）。
            return { r: {} };
        case 'script.compile':
            return has('d2') ? doCompile(sess, p) : { err: { code: 'NOT_SUPPORTED', msg: '编译需 13 D2' } };
        case 'script.run':
            return has('d2') ? doRun(sess) : { err: { code: 'NOT_SUPPORTED', msg: '运行需 13 D2' } };
        case 'script.stop':
            return has('d2') ? doStop(sess) : { err: { code: 'NOT_SUPPORTED', msg: '停止需 13 D2' } };
        case 'script.status':
            return { r: { status: sess.status, steps: sess.steps, error_line: sess.errorLine, line: sess.line } };
        case 'var.list':
            return { r: [...sess.vars.entries()].map(([k, v]) => `${k} = ${v}`) };
        case 'var.get': {
            const name = String(p?.name ?? '');
            if (!sess.vars.has(name)) {
                return { err: { code: 'BAD_PARAM', msg: `变量不存在: ${name}` } };
            }
            const v = sess.vars.get(name);
            return { r: { name, type: varType(v), value: v } };
        }
        case 'var.set': {
            const name = String(p?.name ?? '');
            if (!sess.vars.has(name)) {
                return { err: { code: 'BAD_PARAM', msg: `变量不存在: ${name}` } };
            }
            sess.vars.set(name, p?.v);
            return { r: {} };
        }
        case 'axis.snapshot': {
            const axis = Number(p?.axis ?? 0);
            if (!Number.isInteger(axis) || axis < 0 || axis >= AXIS_COUNT) {
                return { err: { code: 'BAD_PARAM', msg: `axis out of range: ${axis}` } };
            }
            return { r: axisSnapshot(sess) };
        }
        case 'cmd':
            return doCmd(sess, p);
        case 'subscribe':
            return has('d3') ? doSubscribe(sess, p) : { err: { code: 'NOT_SUPPORTED', msg: '订阅需 13 D3' } };
        case 'unsubscribe':
            return has('d3') ? doUnsubscribe(sess, p) : { err: { code: 'NOT_SUPPORTED', msg: '订阅需 13 D3' } };
        case 'script.pause':
        case 'script.resume':
        case 'script.step':
        case 'breakpoint.add':
        case 'breakpoint.del':
        case 'breakpoint.list':
            return doD5(m, sess, p);
        default:
            return { err: { code: 'UNKNOWN_METHOD', msg: `未知方法: ${m}` } };
    }
}

// ---------------------------------------------------------------------------
// 传输：JSON-Lines over TCP（仅 127.0.0.1）
// ---------------------------------------------------------------------------

function dispatch(sess, line) {
    let req;
    try {
        req = JSON.parse(line);
    } catch {
        log('非法 JSON', line);
        sendEvent(sess, { id: 0, ok: false, err: { code: 'BAD_REQUEST', msg: 'invalid JSON line' } });
        return;
    }
    const id = typeof req?.id === 'number' ? req.id : 0;
    if (typeof req?.m !== 'string') {
        sendEvent(sess, { id, ok: false, err: { code: 'BAD_REQUEST', msg: 'missing method' } });
        return;
    }
    log('→', req.m);
    const res = handle(sess, req);
    const reply = res.err ? { id, ok: false, err: res.err } : { id, ok: true, r: res.r };
    sess.socket.write(JSON.stringify(reply) + '\n');
}

const server = net.createServer((socket) => {
    const sess = newSession(socket);
    sess.t0 = now();
    socket.setNoDelay(true);
    log('客户端接入', `${socket.remoteAddress}:${socket.remotePort}`);

    socket.on('data', (chunk) => {
        sess.buf += chunk.toString('utf8');
        let idx = sess.buf.indexOf('\n');
        while (idx >= 0) {
            const line = sess.buf.slice(0, idx).replace(/\r$/, '');
            sess.buf = sess.buf.slice(idx + 1);
            if (line.trim().length > 0) dispatch(sess, line);
            idx = sess.buf.indexOf('\n');
        }
    });

    socket.on('error', (e) => log('连接错误', e.message));
    socket.on('close', () => {
        clearRun(sess);
        for (const t of sess.subs.values()) clearInterval(t);
        sess.subs.clear();
        log('客户端断开');
    });
});

server.on('error', (e) => {
    console.error('[mock] 服务端错误：', e.message);
    process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
    console.log(`[mock] Kine-X Mock DebugServer 监听 127.0.0.1:${PORT}`);
    console.log(`[mock] 引擎=${ENGINE}  能力开关=${[...CAPS].join(',') || '(无)'}`);
    console.log('[mock] 仅用于插件联调：不接 EtherCAT、不驱动电机（docs/planA/16 §5.1）。Ctrl+C 退出。');
});
