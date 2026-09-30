#!/usr/bin/env node
// probe-del.mjs —— 最小复现：compile → add3 → add2 → list → del{2}，逐步打印响应
import net from 'node:net';
const HOST = process.argv[2] || '192.168.1.11';
const PORT = Number(process.argv[3] || 5000);
const client = net.createConnection({ host: HOST, port: PORT });
let buf = ''; let reqId = 0;
const pending = new Map();
client.on('data', (c) => {
    buf += c.toString('utf8'); let i;
    while ((i = buf.indexOf('\n')) >= 0) {
        const l = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
        if (!l) continue;
        let m; try { m = JSON.parse(l); } catch { continue; }
        if (m.id !== undefined && pending.has(m.id)) {
            const r = pending.get(m.id);
            pending.delete(m.id);
            r(m);
        }
    }
});
function request(m, p = {}, t = 4000) {
    const id = ++reqId;
    return new Promise((res) => {
        pending.set(id, res);
        client.write(JSON.stringify({ id, m, p }) + '\n');
        setTimeout(() => {
            if (pending.has(id)) { pending.delete(id); res({ TIMEOUT: true }); }
        }, t);
    });
}
await new Promise((r) => client.on('connect', r));
console.log('connected');
let r = await request('script.compile', { src: 'A = 0\nA = A + 1\nA = A + 1\nEND', engine: 'basic' });
console.log('compile:', JSON.stringify(r));
r = await request('breakpoint.add', { line: 3 });
console.log('add3:', JSON.stringify(r));
r = await request('breakpoint.add', { line: 2 });
console.log('add2:', JSON.stringify(r));
r = await request('breakpoint.list');
console.log('list:', JSON.stringify(r));
r = await request('breakpoint.del', { line: 2 });
console.log('del2:', JSON.stringify(r));
client.destroy();
