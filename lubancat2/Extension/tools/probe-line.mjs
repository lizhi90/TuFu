#!/usr/bin/env node
// probe-line.mjs —— 行号语义探针：对 4 行脚本分别下断点 1..4，记录命中行
import net from 'node:net';
const HOST = process.argv[2] || '192.168.1.11';
const PORT = Number(process.argv[3] || 5000);
const client = net.createConnection({ host: HOST, port: PORT });
let buf = ''; let reqId = 0;
const pending = new Map(); const events = [];
client.on('data', (c) => {
    buf += c.toString('utf8'); let i;
    while ((i = buf.indexOf('\n')) >= 0) {
        const l = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
        if (!l) continue;
        let m; try { m = JSON.parse(l); } catch { continue; }
        if (m.id !== undefined && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
        else if (m.e) events.push(m);
    }
});
function request(m, p = {}, t = 8000) {
    const id = ++reqId;
    return new Promise((res) => {
        pending.set(id, res);
        client.write(JSON.stringify({ id, m, p }) + '\n');
        setTimeout(() => { if (pending.has(id)) { pending.delete(id); res({ ok: false, err: { code: 'TIMEOUT' } }); } }, t);
    });
}
const waitEv = (status, t = 6000) => new Promise((res) => {
    const t0 = Date.now();
    const tick = () => {
        const hit = [...events].reverse().find((e) => e.e === 'script' && (!status || e.status === status));
        if (hit) return res(hit);
        if (Date.now() - t0 > t) return res(undefined);
        setTimeout(tick, 40);
    };
    tick();
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await new Promise((r) => client.on('connect', r));
const SRC = 'A = 0\nA = A + 1\nA = A + 1\nEND';
await request('script.compile', { src: SRC, engine: 'basic' });

for (const bp of [1, 2, 3, 4]) {
    events.length = 0;
    await request('breakpoint.del', {});
    await request('breakpoint.add', { line: bp });
    await request('script.run');
    const ev = await waitEv('PAUSED', 4000);
    const st = await request('script.status');
    console.log(`断点=${bp} → 事件line=${ev?.line ?? '—'} status=${st?.r?.status} line=${st?.r?.line} steps=${st?.r?.steps}`);
    await request('script.stop');
    await sleep(300);
    await request('breakpoint.del', {});
}
client.destroy();
