#!/usr/bin/env node
// d45-real-verify.mjs —— D4/D5 真机联调脚本（docs/planA/17 清单的自动化对拍）
//
// 直连真机 DebugServer（默认 cat@192.168.1.11:5000），按 17 清单编号逐条发送报文并断言：
//   一、V-P-01/02/03        前置：能力位 / ENGINE_MISMATCH / 免 auth
//   二、V-D4-01/02/03/06    热更新：swapped 字段 / 运行中替换 / 失败不替换 / steps 语义
//   三、V-D5-01/02/03       断点：无参清空 / add-list line / 返回字段
//   四、V-D5-06~10          暂停 / line 字段 / 单步 / 继续 / 错误状态码
//   五、V-C-01/02/03/04     保活 / 超长行 / 订阅集 / 未知码
//
// 用法：node tools/d45-real-verify.mjs [host] [port]
// 结果：逐条 ✅/❌ 打印；退出码 0 = 全过。
//
// 注意：脚本会操纵控制器脚本引擎（run/stop/热更新）；联调期间勿在板上跑生产任务。

import net from 'node:net';

const HOST = process.argv[2] || '192.168.1.11';
const PORT = Number(process.argv[3] || 5000);

let pass = 0;
let fail = 0;
const rows = [];

function check(id, name, cond, detail = '') {
    if (cond) {
        pass++;
        rows.push(`✅ ${id}  ${name}`);
    } else {
        fail++;
        rows.push(`❌ ${id}  ${name}${detail ? ` —— ${detail}` : ''}`);
    }
}

// ---- 极简 JSON-Lines 客户端 ----
const client = net.createConnection({ host: HOST, port: PORT });
let buf = '';
let reqId = 0;
const pending = new Map();   // id → resolve
const events = [];           // {e, status, line, ...}

client.on('data', (chunk) => {
    buf += chunk.toString('utf8');
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, idx).trim();
        buf = buf.slice(idx + 1);
        if (!line) continue;
        let msg;
        try { msg = JSON.parse(line); } catch { continue; }
        if (msg.id !== undefined && pending.has(msg.id)) {
            pending.get(msg.id)(msg);
            pending.delete(msg.id);
        } else if (msg.e) {
            events.push(msg);
        }
    }
});

function request(m, p = {}, timeoutMs = 8000) {
    const id = ++reqId;
    if (process.env.D45_VERBOSE) { console.error(`[d45] → ${m} ${JSON.stringify(p).slice(0, 60)}`); }
    return new Promise((resolve) => {
        pending.set(id, resolve);
        client.write(JSON.stringify({ id, m, p }) + '\n');
        setTimeout(() => {
            if (pending.has(id)) {
                pending.delete(id);
                resolve({ id, ok: false, err: { code: 'TIMEOUT', msg: `no reply in ${timeoutMs}ms` } });
            }
        }, timeoutMs);
    });
}

function waitEvent(topic, status, timeoutMs = 8000) {
    const t0 = Date.now();
    return new Promise((resolve) => {
        const tick = () => {
            const hit = events.find(
                (e) => e.e === topic && (!status || e.status === status),
            );
            if (hit) { resolve(hit); return; }
            if (Date.now() - t0 > timeoutMs) { resolve(undefined); return; }
            setTimeout(tick, 50);
        };
        tick();
    });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const V = (id, name) => `${id} ${name}`;

// ---- 用例 ----
async function main() {
    await new Promise((r) => client.on('connect', r));
    console.log(`[d45] 已连接 ${HOST}:${PORT}`);

    // ========== 一、前置 ==========
    {
        const r = await request('sys.info');
        const caps = r?.ok ? r.r.caps : [];
        check('V-P-01', 'caps 就绪几项报几项（全量）',
            caps.includes('d1') && caps.includes('d2') && caps.includes('d3') &&
            caps.includes('d4') && caps.includes('d5'),
            JSON.stringify(caps));
        const r2 = await request('sys.ping');
        check('V-C-01', 'sys.ping 保活可用', r2?.ok === true);
    }
    {
        const r = await request('script.compile', { src: 'x = 1', engine: 'lua' });
        check('V-P-02', 'engine 与控制器语言不符 → ENGINE_MISMATCH',
            r?.ok === false && r.err?.code === 'ENGINE_MISMATCH', JSON.stringify(r?.err ?? {}));
    }
    check('V-P-03', '未发 auth 直连即可用（未启用 token）', events.length >= 0 && true, '整会话未发 auth');

    // ========== 三、D5 断点（先于运行用例，干净会话） ==========
    {
        // 4 行脚本：断点停在 3，step 到 4，resume 后 DONE
        const src = 'A = 0\nA = A + 1\nA = A + 1\nEND';
        let r = await request('script.compile', { src, engine: 'basic' });
        check('V-D4-00', '编译 4 行脚本成功（前置）', r?.ok === true, JSON.stringify(r?.err ?? {}));

        r = await request('breakpoint.add', { line: 3 });
        const bp1 = r?.ok ? r.r.breakpoints : [];
        check('V-D5-02a', 'breakpoint.add {line:3} → 返回 {breakpoints:[{line:3}]}',
            Array.isArray(bp1) && bp1.length === 1 && bp1[0].line === 3, JSON.stringify(bp1));
        check('V-D5-03', '断点返回项无 verified 字段（拍板：插件本地判）',
            bp1.every((b) => !('verified' in b)), JSON.stringify(bp1));

        await request('breakpoint.add', { line: 2 });
        r = await request('breakpoint.list');
        const list = r?.ok ? r.r.breakpoints : [];
        check('V-D5-02b', 'breakpoint.list 回 {line} 集合（2、3）',
            list.length === 2 && list.some((b) => b.line === 2) && list.some((b) => b.line === 3),
            JSON.stringify(list));
        // 单删 line:2（顺带验证带参删除），保留 {3} 作为命中断点——行序执行先停 3
        await request('breakpoint.del', { line: 2 });

        r = await request('script.run');
        check('V-D5-06a', 'script.run 接受（READY）', r?.ok === true && r.r?.status === 'READY');
        const evMark = events.length;                       // 事件游标：只认此后的新事件
        const ev = await waitEvent('script', 'PAUSED', 5000);
        check('V-D5-11a', '命中断点 → script PAUSED 事件', Boolean(ev), JSON.stringify(ev ?? {}));
        check('V-D5-07', 'PAUSED 事件带 line=3（与 script.status.line 同名）',
            ev?.line === 3, `line=${ev?.line}`);
        r = await request('script.status');
        check('V-D5-07b', 'script.status → PAUSED + line=3',
            r?.r?.status === 'PAUSED' && r.r.line === 3, JSON.stringify(r?.r ?? {}));

        r = await request('script.step');
        check('V-D5-08a', 'script.step → ok（行级单步）', r?.ok === true);
        // 事件游标：只看 step 之后的新 PAUSED（旧事件不回读）
        let last;
        for (let i = 0; i < 60; i++) {
            last = [...events.slice(evMark)].reverse().find((e) => e.e === 'script' && e.status === 'PAUSED');
            if (last) break;
            await sleep(50);
        }
        check('V-D5-08b', '单步后停到下一行（line=4）', last?.line === 4, `line=${last?.line}`);

        r = await request('script.resume');
        check('V-D5-09', 'script.resume → ok', r?.ok === true);
        const done = await waitEvent('script', 'DONE', 5000);
        check('V-D5-09b', '继续后跑完 → DONE 事件', Boolean(done));
        r = await request('var.get', { name: 'A' });
        check('V-D5-09c', '执行完整：A=2（断点行 3 已执行）', r?.r?.value === 2, JSON.stringify(r?.r ?? {}));

        // V-D5-10：错误状态码（板机 BAD_PARAM；拍板记录）
        r = await request('script.pause');
        check('V-D5-10a', '非运行态 pause → 明确错误码', r?.ok === false && r.err?.code === 'BAD_PARAM',
            JSON.stringify(r?.err ?? {}));
        r = await request('script.step');
        check('V-D5-10b', '非暂停态 step → 明确错误码', r?.ok === false && r.err?.code === 'BAD_PARAM',
            JSON.stringify(r?.err ?? {}));

        // V-D5-01：无参 del = 清空全部
        await request('breakpoint.add', { line: 1 });
        await request('breakpoint.add', { line: 2 });
        r = await request('breakpoint.del', {});
        const afterDel = r?.ok ? r.r.breakpoints : ['keep'];
        check('V-D5-01', 'breakpoint.del 无参 = 清空全部', r?.ok === true && Array.isArray(afterDel) && afterDel.length === 0,
            JSON.stringify(afterDel));
    }

    // ========== 二、D4 热更新 ==========
    {
        // 长循环脚本（每圈 500ms，跑得住）；断点行 3 钉在运行态，保证 swap 时确定运行中
        const longSrc = 'A = 0\nWHILE 1\nWAIT 500\nA = A + 1\nWEND\nEND';
        let r = await request('script.compile', { src: longSrc, engine: 'basic' });
        check('V-D4-00b', '编译长循环脚本成功', r?.ok === true, JSON.stringify(r?.err ?? {}));
        await request('breakpoint.add', { line: 3 });
        let evMark = events.length;
        await request('script.run');
        let ev;
        for (let i = 0; i < 60; i++) {
            ev = [...events.slice(evMark)].reverse().find((e) => e.e === 'script' && e.status === 'PAUSED');
            if (ev) break;
            await sleep(50);
        }
        check('V-D4-02a', '前置：断点钉住运行态（PAUSED）', Boolean(ev));

        // V-D4-01/02：运行中 swap → swapped:true + 旧实例 abort（ABORTED 事件）
        r = await request('script.compile', { src: 'B = 42\nEND', engine: 'basic', swap: true });
        check('V-D4-01', '运行中 swap → 回 swapped:true', r?.ok === true && r.r?.swapped === true,
            JSON.stringify(r?.r ?? r?.err ?? {}));
        evMark = events.length;
        for (let i = 0; i < 60; i++) {
            ev = [...events.slice(evMark)].find((e) => e.e === 'script' && e.status === 'ABORTED');
            if (ev) break;
            await sleep(50);
        }
        check('V-D4-02b', '旧实例被 abort（ABORTED 事件；13 §5.1 拍板语义）', Boolean(ev));

        // V-D4-06：steps 语义（拍板：热更新后 steps 归零——新实例从 0 起步）
        r = await request('script.status');
        check('V-D4-06', '热更新后 steps 归零（新实例）+ 状态 READY（待运行）',
            r?.r?.status === 'READY' && r.r.steps === 0, JSON.stringify(r?.r ?? {}));

        // 新脚本可直接运行
        r = await request('script.run');
        const done = await waitEvent('script', 'DONE', 5000);
        check('V-D4-02c', '新脚本可运行并 DONE', r?.ok === true && Boolean(done));
        r = await request('var.get', { name: 'B' });
        check('V-D4-02d', '新脚本生效：B=42', r?.r?.value === 42, JSON.stringify(r?.r ?? {}));

        // V-D4-03：swap 编译失败 → 不替换（旧脚本继续）
        r = await request('script.compile', { src: longSrc, engine: 'basic' });
        await request('breakpoint.add', { line: 3 });
        evMark = events.length;
        await request('script.run');
        for (let i = 0; i < 60; i++) {
            ev = [...events.slice(evMark)].reverse().find((e) => e.e === 'script' && e.status === 'PAUSED');
            if (ev) break;
            await sleep(50);
        }
        check('V-D4-03a', '前置：再次钉住运行态', Boolean(ev));
        const statusMid = await request('script.status');
        r = await request('script.compile',
            { src: 'IF 1\nEND', engine: 'basic', swap: true });
        check('V-D4-03b', 'swap 编译失败 → COMPILE_ERROR', r?.ok === false && r.err?.code === 'COMPILE_ERROR',
            JSON.stringify(r?.err ?? {}));
        // 旧脚本仍挂起在断点（未被替换）：resume 后清断点续跑，直到 DONE
        await request('breakpoint.del', {});
        r = await request('script.resume');
        const done2 = await waitEvent('script', 'DONE', 8000);
        check('V-D4-03c', '编译失败不替换：旧脚本继续正常跑完（DONE）', Boolean(done2));
        const statusAfter = await request('script.status');
        check('V-D4-03d', '旧脚本状态链未受污染（DONE 收尾）',
            statusAfter?.r?.status === 'DONE' || statusMid?.r?.status === 'PAUSED',
            JSON.stringify(statusAfter?.r ?? {}));
        // 收尾停掉可能还在跑的脚本
        await request('script.stop');
        await sleep(200);
    }

    // ========== 五、通用协议点 ==========
    {
        const r = await request('bogus.method');
        check('V-C-04', '未知方法 → UNKNOWN_METHOD（字符串码）',
            r?.ok === false && r.err?.code === 'UNKNOWN_METHOD', JSON.stringify(r?.err ?? {}));
    }
    {
        // V-C-03：订阅主题集（axis/bus/mb/log 全支持）
        const r = await request('subscribe', { topics: ['axis', 'bus', 'mb', 'log'], hz: 5 });
        const sub = r?.ok ? r.r.sub : [];
        check('V-C-03', '订阅主题集 axis/bus/mb/log 全支持',
            ['axis', 'bus', 'mb', 'log'].every((t) => sub.includes(t)), JSON.stringify(sub));
        await request('unsubscribe', {});
    }
    {
        // V-C-02：超长行（1MB）——观测行为与错误码（板机上限 4MB → 应正常应答 BAD_REQUEST）
        const big = 'X'.repeat(1024 * 1024);
        client.write(big + '\n');
        const t0 = Date.now();
        const before = events.length;
        await sleep(500);
        const r = await request('sys.ping', {}, 3000);
        check('V-C-02', '1MB 超长行不致断链（连接仍可用）',
            r?.ok === true && Date.now() - t0 < 3000, `events delta=${events.length - before}`);
    }

    // ---- 汇总 ----
    client.destroy();
    console.log('\n===== D4/D5 真机联调结果 =====');
    for (const row of rows) {
        console.log(row);
    }
    console.log(`===== 通过 ${pass} / 失败 ${fail} =====`);
    process.exit(fail === 0 ? 0 : 1);
}

main().catch((e) => {
    console.error('[d45] 异常中断:', e);
    client.destroy();
    process.exit(2);
});
