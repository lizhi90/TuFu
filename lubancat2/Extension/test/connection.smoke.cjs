#!/usr/bin/env node
// connection.smoke.cjs —— 无 UI 冒烟测试（NFR-10）
//
// 用**真实连接层**（out/connection.js）打 tools/mock-debug-server.mjs，
// 验证 docs/planA/15 T-09/T-10 依赖的协议语义：
//   握手 / 编译成功 / 编译失败(err.line) / 引擎不符 / 运行→事件→停止 /
//   变量读写 / 轴越界 / D5 降级(NOT_SUPPORTED) / cmd / 未知方法 / 断开
//
// 用法：npm run test:smoke   （脚本自行拉起并关闭 Mock，无需手工起服务）
// 前置：npm run compile（用到 out/）

const { spawn } = require('node:child_process');
const path = require('node:path');

const { KxConnection } = require('../out/connection.js');
const { controllerEngineConflict, engineDisplay, engineFromFile, fileMatchesEngine, inferEngineName, decideEngine } = require('../out/engineRule.js');
const { KxStatusStore } = require('../out/store.js');

const PORT = Number(process.env.KX_SMOKE_PORT || 5097);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let pass = 0;
let fail = 0;
function check(name, cond, extra) {
    if (cond) {
        pass++;
        console.log(`  \u2713 ${name}`);
    } else {
        fail++;
        console.log(`  \u2717 ${name}${extra !== undefined ? ` → ${extra}` : ''}`);
    }
}

async function expectError(name, expectedCode, fn, expectedLine) {
    try {
        await fn();
        check(name, false, '未抛出错误');
    } catch (e) {
        const codeOk = e && e.code === expectedCode;
        const lineOk = expectedLine === undefined || e.line === expectedLine;
        check(name, codeOk && lineOk, `code=${e && e.code} line=${e && e.line}`);
    }
}

function startMock(port, caps, engine) {
    const script = path.join(__dirname, '..', 'tools', 'mock-debug-server.mjs');
    const args = [script, '--port', String(port), '--caps', caps];
    if (engine) args.push('--engine', engine);
    const child = spawn(process.execPath, args, {
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Mock 启动超时')), 5000);
        child.stdout.on('data', (d) => {
            if (String(d).includes('监听')) {
                clearTimeout(timer);
                resolve(child);
            }
        });
        child.on('error', (e) => {
            clearTimeout(timer);
            reject(e);
        });
    });
}

// engineRule 是纯逻辑（无 vscode 依赖），可脱离控制器直接断言（对齐 src/script/engine_rule.h）
function checkEngineRule() {
    console.log('[smoke] 引擎规则（engine_rule.h 镜像）');
    check('engineFromFile(.bas) → basic', engineFromFile('/p/demo.bas') === 'basic');
    check('engineFromFile(.lua) → lua', engineFromFile('/p/demo.lua') === 'lua');
    check('engineFromFile(无扩展名) → basic', engineFromFile('/p/demo') === 'basic');
    check('lua 引擎 + .bas → 拦截', fileMatchesEngine('lua', '/p/demo.bas').ok === false);
    // v0.8.11：非脚本文件（.md/.txt/无扩展名）不得误报“引擎冲突”（D-03 裁决）
    check(
        'inferEngineName：.lua→lua / .bas→basic / 其它→null（无法判断）',
        inferEngineName('/p/a.lua') === 'lua' &&
            inferEngineName('/p/a.bas') === 'basic' &&
            inferEngineName('/p/notes.md') === null &&
            inferEngineName('/p/demo') === null,
    );
    check(
        'decideEngine：.md + 显式 lua → 不冲突（放行 lua）',
        decideEngine('lua', true, '/p/notes.md').conflict !== true &&
            decideEngine('lua', true, '/p/notes.md').engine === 'lua',
    );
    check(
        'decideEngine：.md + 显式 basic → 不冲突（放行 basic）',
        decideEngine('basic', true, '/p/notes.md').conflict !== true &&
            decideEngine('basic', true, '/p/notes.md').engine === 'basic',
    );
    check(
        'decideEngine：.lua 与显式 basic 冲突 / .bas 与显式 lua 冲突',
        decideEngine('basic', true, '/p/a.lua').conflict === true &&
            decideEngine('lua', true, '/p/a.bas').conflict === true,
    );
    check(
        'decideEngine：非显式时按扩展名（.lua→lua / .bas→basic / 无法判断→basic）',
        decideEngine('lua', false, '/p/a.lua').engine === 'lua' &&
            decideEngine('lua', false, '/p/a.bas').engine === 'basic' &&
            decideEngine('basic', false, '/p/notes.md').engine === 'basic',
    );
    check('basic 引擎 + .lua → 拦截', fileMatchesEngine('basic', '/p/demo.lua').ok === false);
    check('lua 引擎 + .lua → 放行', fileMatchesEngine('lua', '/p/demo.lua').ok === true);
    check('basic 引擎 + 无扩展名 → 放行（无法判断）', fileMatchesEngine('basic', '/p/demo').ok === true);

    // v0.4.7：控制器引擎（SCRIPT_ENGINE，与有无文件无关）× 本地文件引擎 → 本地拦截
    const c1 = controllerEngineConflict('basic', 'lua');
    check('控制器 basic + 本地 lua → 拦截（含改法提示）', typeof c1 === 'string' && c1.includes('SCRIPT_ENGINE'));
    check('控制器 lua + 本地 basic → 拦截', controllerEngineConflict('lua', 'basic') !== null);
    check('控制器 basic + 本地 basic → 放行', controllerEngineConflict('basic', 'basic') === null);
    check('控制器引擎未知（空）→ 不拦截，交服务端裁决', controllerEngineConflict('', 'lua') === null);
    check('控制器引擎未知（异常值）→ 不拦截', controllerEngineConflict('weird', 'lua') === null);
    check(
        '控制器 basic + .lua → 提示改用 .bas 或改板端配置',
        typeof c1 === 'string' && c1.includes('.bas') && c1.includes('kine-x.service'),
    );

    // v0.4.8：sys.info.engine 新值 auto/mixed 的显示（语言由控制器脚本目录推导）
    check('engineDisplay: basic/lua 常规', engineDisplay('basic') === 'BASIC' && engineDisplay('lua') === 'Lua');
    check(
        'engineDisplay: auto/mixed 有明确显示（不显示英文原值）',
        engineDisplay('auto') !== '' && engineDisplay('auto') !== 'auto' &&
            engineDisplay('mixed') !== '' && engineDisplay('mixed') !== 'mixed',
    );
    check('engineDisplay: 未知原样报未知', engineDisplay('weird').length > 0);
}

// dapPure 同样无 vscode 依赖：把 T-14 的变量解析逻辑（FR-4.5/4.6/4.7）直接单测
// v0.8.13：TCP 分片跨多字节字符时的 UTF-8 解码（回归：曾用 chunk.toString 造成内容损坏 → “拉取成功但不一致”）
function checkUtf8ChunkDecode() {
    console.log('[smoke] UTF-8 分片解码（StringDecoder）');
    const { KxConnection } = require('../out/connection.js');
    const c = new KxConnection({ host: '127.0.0.1', port: 1 });
    let got = null;
    c.on('raw', (dir, line) => {
        if (dir === 'S') {
            got = line;
        }
    });
    const payload = JSON.stringify({ id: 1, ok: true, r: { src: '中文测试🙂tail' } }) + '\n';
    const buf = Buffer.from(payload, 'utf8');
    const cut = buf.indexOf(Buffer.from('中', 'utf8')) + 1;   // 切在“中”的 UTF-8 中间
    c.onData(buf.subarray(0, cut));
    c.onData(buf.subarray(cut));
    check(
        '跨包中文不损坏（StringDecoder 缓存半个序列）',
        got !== null && JSON.parse(got).r.src === '中文测试🙂tail',
        got ?? '(无输出)',
    );
}

function checkDapPure() {
    const { parseVarLine, parseLiteral, inferType, typeOfValue, baseName } = require('../out/dapPure.js');
    console.log('[smoke] DAP 纯逻辑（变量解析 / 字面量 / 类型）');

    const a = parseVarLine('SPEED = 100');
    check('parseVarLine: SPEED = 100', a.name === 'SPEED' && a.value === '100' && a.type === 'int', JSON.stringify(a));
    const b = parseVarLine('POS = 12.5');
    check('parseVarLine: POS = 12.5 → float', b.name === 'POS' && b.value === '12.5' && b.type === 'float', JSON.stringify(b));
    const c = parseVarLine('MSG = "hi"');
    check('parseVarLine: 去引号 + str', c.name === 'MSG' && c.value === 'hi' && c.type === 'str', JSON.stringify(c));
    const d = parseVarLine('BARE');
    check('parseVarLine: 无等号 → unknown', d.name === 'BARE' && d.value === '' && d.type === 'unknown', JSON.stringify(d));

    check('parseLiteral: 100 → number', parseLiteral('100') === 100);
    check('parseLiteral: -2.5 → number', parseLiteral('-2.5') === -2.5);
    check('parseLiteral: true → 1', parseLiteral('true') === 1);
    check('parseLiteral: false → 0', parseLiteral('false') === 0);
    check('parseLiteral: "abc" → 去引号字符串', parseLiteral('"abc"') === 'abc');
    check('parseLiteral: 1e3 → number', parseLiteral('1e3') === 1000);

    check('inferType: 引号优先于数字', inferType('"123"') === 'str', inferType('"123"'));
    check('typeOfValue: 整数/小数/字符串', typeOfValue(3) === 'int' && typeOfValue(3.5) === 'float' && typeOfValue('s') === 'str');
    check('baseName: 兼容 / 与 \\', baseName('/a/b/demo.bas') === 'demo.bas' && baseName('C:\\x\\y.lua') === 'y.lua');
}

// store / subscribe / statusBits 均无 vscode 依赖：仓库、订阅降级与位表可直接单测（T-18）
// 注：树形面板模型（panelModel/panels）已于 v0.4.7 移除——状态 UI 全部由 Webview 面板承载。
function checkStoreAndBits() {
    const { clampHz } = require('../out/subscribe.js');
    const { KxStatusStore } = require('../out/store.js');
    const { decodeAxisStatus, alStateName } = require('../out/statusBits.js');
    console.log('[smoke] 状态纯逻辑（位表 / 仓库 / 订阅）');

    check('clampHz: 夹取到 [1,50]', clampHz(0) === 1 && clampHz(200) === 50 && clampHz(20) === 20);
    check('clampHz: NaN → 默认 20', clampHz(Number.NaN) === 20);

    const allOff = decodeAxisStatus(0);
    check(
        'decodeAxisStatus(0): 全部未置位、无未知位',
        allOff.bits.every((b) => !b.on) && allOff.unknownBits.length === 0 && allOff.hex === '0x00000000',
        allOff.hex,
    );
    const two = decodeAxisStatus((1 << 1) | (1 << 22));
    check('decodeAxisStatus: bit1/bit22 置位', two.bits.find((b) => b.bit === 1).on && two.bits.find((b) => b.bit === 22).on);
    check('decodeAxisStatus: 位表外的位置位 → unknownBits', decodeAxisStatus(1 << 5).unknownBits.includes(5));
    check('alStateName: 8→OP / 未知值原样', alStateName(8) === 'OP' && alStateName(7) === '7');

    const st = new KxStatusStore();
    st.onConnect('basic', 2);
    check('update(axis) → 只影响 axes', JSON.stringify(st.update({ e: 'axis', axis: 0, mpos: 5 })) === '["axes"]');
    st.update({ e: 'axis', axis: 0, bus_ok: 1 });
    check('轴样本增量合并：不覆盖已有字段', st.axes.get(0).mpos === 5 && st.axes.get(0).bus_ok === 1);
    st.update({ e: 'mb', start: 4, regs: [7, 8] });
    check('寄存器分段合并：regs[4]/regs[5] 且 regsLoaded', st.regs[4] === 7 && st.regs[5] === 8 && st.regsLoaded === true);
    st.update({
        e: 'bus',
        node_count: 1,
        nodes: [{ index: 0, axis_count: 1, status: 8 }],
    });
    check('总线事件收敛：node_count/nodes', st.bus.node_count === 1 && st.bus.nodes[0].status === 8);
    check('update(未知事件) → 不影响任何面板', st.update({ e: 'nope' }).length === 0);
    st.applyAxisSnapshot(1, {
        bus_ok: 1,
        mpos: 12.5,
        dpos: 12.5,
        idle: 1,
        enabled: 1,
        alarm: 0,
        axis_status: 2,
        err_code: 0,
    });
    check('axis.snapshot → 收敛为轴样本', st.axes.get(1).mpos === 12.5 && st.axes.get(1).axis_status === 2);
    st.markSubUnavailable('订阅需 13 D3');
    check('订阅降级：store 明示原因', st.subUnavailable === true && st.subDegradeReason.includes('D3'));
    st.markDisconnected();
    check(
        '断线：store 清空轴/总线/寄存器（不留假在线）',
        st.axes.size === 0 && st.bus === undefined && st.regsLoaded === false,
    );
}

// 侧边栏顶部「横向菜单条」纯逻辑（toolbarPure.ts，方案 B）
function checkToolbarPure() {
    const {
        toolbarButtons,
        toolbarEnabled,
        toolbarRenderButtons,
        toolbarMenus,
        toolbarStatusText,
        toolbarHtml,
    } = require('../out/toolbarPure.js');
    console.log('[smoke] 菜单条（按钮 / 可用性 / HTML）');

    const offline = {
        connected: false,
        scriptStatus: '',
        caps: new Set(),
        target: '',
        configEngine: 'basic',
    };
    const online = {
        connected: true,
        scriptStatus: 'READY',
        caps: new Set(['d1', 'd2', 'd3', 'd4', 'd5']),
        target: '192.168.1.11:5000',
        configEngine: 'basic',
    };

    // 按钮顺序固定（连接 | 下载 | 运行 | 停止 | 曲线 | 刷新 | 信息）
    check(
        '菜单条：按钮顺序固定（7 个）',
        toolbarButtons(online).map((b) => b.id).join(',') ===
            'disconnect,download,downloadAndRun,stop,curve,refresh,sysInfo',
    );
    check(
        '菜单条：未连接时首按钮为「连接」',
        toolbarButtons(offline)[0].id === 'connect' && toolbarButtons(offline)[0].command === 'kine-x.connect',
    );

    // 降级不伪装：未连接 → 脚本类置灰，连接/信息可用
    check('菜单条：未连接 → 脚本类按钮禁用', toolbarEnabled('download', offline) === false);
    check('菜单条：未连接 → 连接按钮可用', toolbarEnabled('connect', offline) === true);
    check('菜单条：未连接 → 控制器信息可用', toolbarEnabled('sysInfo', offline) === true);
    // 已连接但无 d2（自动脚本占用引擎）→ 脚本类仍禁用
    check(
        '菜单条：已连接但缺 d2 → 脚本类仍禁用',
        toolbarEnabled('download', { ...online, caps: new Set(['d1', 'd3']) }) === false,
    );
    check(
        '菜单条：已连接且有 d2 → 脚本类可用',
        toolbarEnabled('download', online) === true && toolbarEnabled('stop', online) === true,
    );

    // D7 重启（v0.5.0）：菜单「控制器 → 重启控制器」，缺 d7 明确置灰（降级不伪装）
    const ctlMenu = toolbarMenus(online).find((g) => g.id === 'controller');
    check(
        '菜单栏：控制器菜单含「重启控制器」项',
        Boolean(ctlMenu) &&
            ctlMenu.items.some((i) => i.id === 'restart' && i.command === 'kine-x.restart'),
    );
    check('菜单条：未连接 → 重启禁用', toolbarEnabled('restart', offline) === false);
    check(
        '菜单条：已连接但缺 d7 → 重启禁用',
        toolbarEnabled('restart', { ...online, caps: new Set(['d1', 'd2', 'd3']) }) === false,
    );
    check(
        '菜单条：已连接且有 d7 → 重启可用',
        toolbarEnabled('restart', { ...online, caps: new Set([...online.caps, 'd7']) }) === true,
    );
    check(
        '菜单栏：控制器菜单含「修改端口数量」项',
        Boolean(ctlMenu) &&
            ctlMenu.items.some((i) => i.id === 'portMax' && i.command === 'kine-x.portMax'),
    );
    check('菜单条：未连接 → 修改端口数量禁用', toolbarEnabled('portMax', offline) === false);
    check(
        '菜单条：已连接但缺 d9 → 修改端口数量禁用',
        toolbarEnabled('portMax', { ...online, caps: new Set(['d1', 'd2', 'd3']) }) === false,
    );
    check(
        '菜单条：已连接且有 d9 → 修改端口数量可用',
        toolbarEnabled('portMax', { ...online, caps: new Set([...online.caps, 'd9']) }) === true,
    );
    const toolsMenu = toolbarMenus(online).find((g) => g.id === 'tools');
    check(
        '菜单栏：工具菜单含「通讯状态」项',
        Boolean(toolsMenu) &&
            toolsMenu.items.some((i) => i.id === 'comm' && i.command === 'kine-x.comm.open'),
    );
    check('菜单条：未连接 → 通讯状态禁用', toolbarEnabled('comm', offline) === false);
    check(
        '菜单条：已连接但缺 d10 → 通讯状态禁用',
        toolbarEnabled('comm', { ...online, caps: new Set(['d1', 'd2', 'd3']) }) === false,
    );
    check(
        '菜单条：已连接且有 d10 → 通讯状态可用',
        toolbarEnabled('comm', { ...online, caps: new Set([...online.caps, 'd10']) }) === true,
    );

    // 渲染模型：图标内联 SVG（不依赖 codicon 字体）、分隔线分组
    const rb = toolbarRenderButtons(online);
    check('菜单条：每个按钮都带内联 SVG 图标', rb.every((b) => b.svg.startsWith('<svg')));
    // v0.8.17：菜单项图标覆盖守卫——「通讯状态」曾漏配 ICON_SVG['radio-tower'] 导致无图标
    const menuItemsAll = toolbarMenus(online)
        .flatMap((g) => g.items)
        .filter((i) => i.kind === 'item');
    check(
        '菜单栏：每个菜单项都带内联 SVG 图标（防漏配 ICON_SVG）',
        menuItemsAll.every((i) => typeof i.svg === 'string' && i.svg.startsWith('<svg')),
    );
    check('菜单条：每个按钮都有可见文字（SVG 兜底）', rb.every((b) => b.text.length > 0));
    check('菜单条：连接组后画分隔线', rb.find((b) => b.id === 'disconnect').separator === true);

    // 状态徽标
    check('菜单条：未连接徽标', toolbarStatusText(offline) === '未连接');
    check('菜单条：已连接徽标含目标 + 运行态', toolbarStatusText(online) === '192.168.1.11:5000 · READY');

    // HTML：菜单栏样式（横排 flex + 下拉浮层）+ CSP + 只 postMessage 不直接执行
    const html = toolbarHtml('NONCE123');
    check('菜单条 HTML：.menubar 横排（display: flex）', html.includes('.menubar') && html.includes('display: flex'));
    check('菜单条 HTML：含下拉浮层 .dropdown', html.includes('.dropdown') && html.includes('position: absolute'));
    check('菜单条 HTML：顶层菜单带展开三角 caret', html.includes('caret') && html.includes('\\u25BE'));
    check('菜单条 HTML：含 CSP nonce', html.includes("script-src 'nonce-NONCE123'"));
    check('菜单条 HTML：点击只 postMessage，不内联执行命令', html.includes("type: 'cmd'"));

    // 菜单栏结构（v0.3.1）：顶层 = 控制器 / 工具；控制器内含「脚本语言」二级子菜单（Basic/Lua）
    const menus = toolbarMenus(offline);
    check('菜单栏：2 个顶层菜单（控制器/工具）', menus.length === 2 && menus.map((g) => g.id).join(',') === 'controller,tools');
    check('菜单栏：顶层菜单名为中文', menus.map((g) => g.label).join('') === '控制器工具');
    const ctl = menus.find((g) => g.id === 'controller');
    const sub = ctl.items.find((i) => i.kind === 'submenu');
    check(
        '菜单栏：控制器菜单含「脚本语言」子菜单（Basic/Lua）',
        Boolean(sub) && sub.label === '脚本语言' && sub.children.map((c) => c.id).join(',') === 'engine.basic,engine.lua',
    );
    check('菜单栏：语言切换项恒可用（写本项目配置，不需连接）', sub.children.every((c) => c.enabled === true));
    check(
        '菜单栏：未连接时脚本类命令（下载/运行/停止）置灰',
        ctl.items
            .filter((i) => i.kind === 'item' && ['download', 'run', 'stop'].includes(i.id))
            .every((i) => i.enabled === false),
    );
    check(
        '菜单栏：未连接时连接/改IP/信息可用',
        ctl.items
            .filter((i) => i.kind === 'item' && ['connect', 'setIp', 'sysInfo'].includes(i.id))
            .every((i) => i.enabled === true),
    );
    check(
        '菜单栏：同步在未连接时置灰（拉取控制器文件需连接）',
        ctl.items.filter((i) => i.kind === 'item' && i.id === 'sync').every((i) => i.enabled === false),
    );
    const tools = menus.find((g) => g.id === 'tools');
    check(
        '菜单栏：工具组含 轴状态/Modbus/设备命令/曲线/刷新/通讯状态（控制器文件已常驻控制台）',
        tools.items.map((i) => i.id).join(',') === 'axisPanel,modbus,cmd,curve,refresh,comm',
    );
    check(
        '菜单栏：工具组未连接时全部置灰（均需连接）',
        tools.items.every((i) => i.enabled === false),
    );
    // 语言勾选：configEngine 决定 ✓ 标记（选择即切换本项目语言环境）
    const menusBasic = toolbarMenus({ ...online, configEngine: 'basic' });
    const subBasic = menusBasic.find((g) => g.id === 'controller').items.find((i) => i.kind === 'submenu');
    check(
        '菜单栏：configEngine=basic → Basic 打勾',
        subBasic.children.find((c) => c.id === 'engine.basic').text.startsWith('✓') &&
            !subBasic.children.find((c) => c.id === 'engine.lua').text.startsWith('✓'),
    );
    const menusLua = toolbarMenus({ ...online, configEngine: 'lua' });
    const subLua = menusLua.find((g) => g.id === 'controller').items.find((i) => i.kind === 'submenu');
    check(
        '菜单栏：configEngine=lua → Lua 打勾',
        subLua.children.find((c) => c.id === 'engine.lua').text.startsWith('✓'),
    );

    const menusOn = toolbarMenus(online);
    check(
        '菜单栏：已连接时下载/运行/停止可用（caps 含 d2）',
        menusOn
            .find((g) => g.id === 'controller')
            .items.filter((i) => i.kind === 'item' && ['download', 'run', 'stop'].includes(i.id))
            .every((i) => i.enabled === true),
    );
}

// 控制台 Webview 纯逻辑（consolePure.ts）：菜单栏 + 二级子菜单易用性（v0.3.3）
// 「修改端口数量」（D9）纯逻辑：输入解析（v0.8.5）
function checkPortMaxPure() {
    const { parsePortMax, portMaxSummary } = require('../out/portMaxPure.js');
    console.log('[smoke] 端口数量（解析 / 摘要）');

    check('端口数量：合法整数', parsePortMax('24', 1, 64).value === 24 && parsePortMax(' 16 ', 1, 64).value === 16);
    check(
        '端口数量：0 / 65 / 空 / 小数 / 负数 全部拒绝并给出文案',
        parsePortMax('0', 1, 64).error !== undefined &&
            parsePortMax('65', 1, 64).error !== undefined &&
            parsePortMax('', 1, 64).error !== undefined &&
            parsePortMax('1.5', 1, 64).error !== undefined &&
            parsePortMax('-3', 1, 64).error !== undefined &&
            parsePortMax('abc', 1, 64).error !== undefined,
    );
    check(
        '端口数量：摘要含上限/容量/占用',
        portMaxSummary(16, 64, []).includes('16') &&
            portMaxSummary(16, 64, []).includes('64') &&
            portMaxSummary(16, 64, [10, 11]).includes('10, 11'),
    );
}

// 「通讯状态」（D10）纯逻辑：连接行 / 主从 / 从站明细
function checkCommPanelPure() {
    // 源码级回归守卫（v0.8.8）：conn 事件必须在 extension.ts 的事件分发里进仓库（曾落 default → 空表）
    {
        const fs = require('node:fs');
        const path2 = require('node:path');
        const src = fs.readFileSync(path2.join(__dirname, '..', 'src', 'extension.ts'), 'utf8');
        check(
            'conn 事件已在扩展事件分发中处理（store.update）',
            /case 'conn'/.test(src),
        );
    }

    const { commPanelHtml, buildCommPanelSpec, alStateText } = require('../out/commPanelPure.js');
    const { KxStatusStore } = require('../out/store.js');
    console.log('[smoke] 通讯状态面板（D10：连接 / 主从 / 从站明细）');

    const html = commPanelHtml('NONCEC');
    check(
        '通讯状态面板 HTML：CSP nonce + 两张表（TCP / EtherCAT）',
        html.includes("script-src 'nonce-NONCEC'") && html.includes('对外连接') &&
            html.includes('EtherCAT') && html.includes('从站'),
    );
    check(
        'AL 状态映射（OP/SAFEOP/PREOP/—）',
        alStateText(8) === 'OP' && alStateText(4) === 'SAFEOP' && alStateText(2) === 'PREOP' &&
            alStateText(0) === '—',
    );

    const store = new KxStatusStore();
    store.onConnect('lua', 1);
    store.update({
        e: 'conn',
        t: 1,
        conns: [
            { port: 10, kind: 'TCP_SERVER', conn: true, listen: 4321, peer_port: 51000,
              target: 'listen:4321', peer: '192.168.1.3', tag: '4321 ASCII 服务端', role: '—' },
            { port: 12, kind: 'TCP_CLIENT', conn: false, listen: 0, peer_port: 4320,
              target: '192.168.1.221:4320', peer: '192.168.1.221', tag: '机器人 TCP 客户端' },
        ],
        bus: {
            link_up: 1, slaves_responding: 1, slave_al: 8, slave_online: 1, slave_op: 1, node_count: 1,
            slaves: [{ index: 0, axis: 0, online: 1, al_state: 8, vid: 0x00100000, pid: 0x000c0112,
                       rev: 2, name: 'SV630NS1R6I' }],
        },
    });
    check(
        'conn 事件 → 仓库装载（2 连接 + 1 从站）',
        store.connsLoaded === true && store.conns.length === 2 && store.busSlaves.length === 1,
    );
    const spec = buildCommPanelSpec(store, { connected: true });
    check(
        '行视图：服务端行（用途/角色/端口/已连接）',
        spec.rows[0].kindText.includes('4321 ASCII 服务端') && spec.rows[0].roleText === '服务端' &&
            spec.rows[0].portText === '4321' && spec.rows[0].stateText === '已连接' &&
            spec.rows[0].stateOk === true,
    );
    check(
        '行视图：客户端未连 → 连接中；主从缺省 —',
        spec.rows[1].roleText === '客户端' && spec.rows[1].stateText === '连接中' &&
            spec.rows[1].masterText === '—' && spec.rows[1].portText === '4320',
    );
    check(
        '从站明细：身份/AL/在线/轴/名称',
        spec.slaves[0].identityText === '0x100000:0xc0112 r2' && spec.slaves[0].alText === 'OP' &&
            spec.slaves[0].onlineOk === true && spec.slaves[0].axisText === '轴0' &&
            spec.slaves[0].nameText === 'SV630NS1R6I',
    );
    check('总线摘要：link/从站数/AL 聚合', spec.busOk === true && spec.busText.includes('从站=1'));
    // ★多客户端（2026-09-28）：同端口两条连接 → 行 id 必须唯一（否则渲染互相覆盖）
    {
        const st2 = new KxStatusStore();
        st2.update({
            e: 'conn', t: 1,
            conns: [
                { port: 13, kind: 'TCP_SERVER', conn: true, listen: 502, peer_port: 58161, target: 'listen:502', peer: '192.168.1.3', tag: 'Modbus-TCP 从站(502)', role: '从站' },
                { port: 13, kind: 'TCP_SERVER', conn: true, listen: 502, peer_port: 50608, target: 'listen:502', peer: '192.168.1.221', tag: 'Modbus-TCP 从站(502)', role: '从站' },
            ],
        });
        const sp2 = buildCommPanelSpec(st2, { connected: true });
        check(
            '多客户端：同端口两行 id 唯一且各自显示对端 IP',
            sp2.rows.length === 2 && sp2.rows[0].id !== sp2.rows[1].id &&
                sp2.rows[0].peerText === '192.168.1.3' && sp2.rows[1].peerText === '192.168.1.221',
        );
    }
    const empty = buildCommPanelSpec(new KxStatusStore(), { connected: false });
    check(
        '无数据 → 明示等待（不补 0 伪装）',
        empty.loaded === false && empty.rows.length === 0 && empty.slaves.length === 0,
    );
}

function checkConsolePure() {
    const { consoleHtml, buildConsoleSpec } = require('../out/consolePure.js');
    console.log('[smoke] 控制台（菜单栏 / 二级子菜单 / 表格渲染数据）');

    const html = consoleHtml('NONCE7');
    check('控制台 HTML：菜单栏 sticky 顶行', html.includes('.menubar') && html.includes('position: sticky'));
    check('控制台 HTML：含 CSP nonce', html.includes("script-src 'nonce-NONCE7'"));

    // 脚本语言子分组（v0.3.4）：内联平铺，不再用浮层/flip（那会被视图裁剪导致「消失」）
    check(
        '控制台 HTML：无浮层子菜单（.mi.sub 已弃用）',
        !html.includes('.mi.sub'),
    );
    check(
        '控制台 HTML：无 flip（不再把子菜单推出可视区）',
        !html.includes('.mi.sub.flip'),
    );
    check(
        '控制台 HTML：子分组标题 + 子项缩进 + 勾选标记样式',
        html.includes('.mi.head') && html.includes('.mi.child') && html.includes('.child-mark'),
    );
    check(
        '控制台 HTML：延迟关闭缓冲（scheduleClose + 600ms）',
        html.includes('scheduleClose') && html.includes('600'),
    );
    check('控制台 HTML：移入下拉取消待关闭', /mouseenter[^;]*clearTimeout/.test(html));
    check('控制台 HTML：点击菜单栏内不关闭（closest(.menubar)）', html.includes("closest('.menubar')"));

    // 修「连接后菜单/表格经常没反应」（v0.3.5）：事件推送高频重绘会冲掉展开的菜单与寄存器展开态
    check(
        '控制台 HTML：菜单展开期间丢弃渲染帧（防重绘冲掉下拉）',
        /function render\(spec\) \{[\s\S]{0,400}if \(openEl\) \{ return; \}/.test(html),
    );

    // v0.4.2 根治「连接后菜单点不动」：状态表格（20Hz 高频重绘源）整体迁出控制台
    check(
        '控制台 HTML：不再含状态表格（轴/总线/寄存器已迁至轴状态面板）',
        !html.includes('<table') && !html.includes('Modbus') && !html.includes('details'),
    );
    // 渲染数据：未连接时也能拿到菜单（含语言子分组）；文件列表已迁出到原生 TreeView
    const { KxStatusStore } = require('../out/store.js');
    const store = new KxStatusStore();
    const spec = buildConsoleSpec(store, {
        connected: false,
        target: '',
        scriptStatus: '',
        caps: new Set(),
        configEngine: 'lua',
    });

    check('控制台数据：未连接仍提供菜单栏', Array.isArray(spec.menus) && spec.menus.length === 2);
    const langSub = spec.menus[0].items.find((i) => i.kind === 'submenu');
    check(
        '控制台数据：语言子分组存在且勾选 lua',
        Boolean(langSub) && langSub.label === '脚本语言' &&
            langSub.children.find((c) => c.id === 'engine.lua').text.startsWith('✓'),
    );
    check(
        '控制台数据：不再承载状态表格（axes/bus/regs 已迁出）',
        spec.axes === undefined && spec.bus === undefined && spec.regs === undefined,
    );
    // v0.4.9：文件列表迁至原生 TreeView `kine-x.files`，控制台不再承载
    check(
        '控制台数据：文件列表已迁出（无 files 字段）',
        spec.files === undefined && !html.includes('refreshFiles') && !html.includes("type: 'fetch'"),
    );
}

// 「轴状态」右侧面板纯逻辑（axisPanelPure.ts，v0.4.2 自控制台迁出；v0.4.3 寄存器拆出）
function checkAxisPanelPure() {
    const { axisPanelHtml, buildAxisPanelSpec } = require('../out/axisPanelPure.js');
    console.log('[smoke] 轴状态面板（轴/总线表格 / 降级明示，寄存器已拆出）');

    const html = axisPanelHtml('NONCE8');
    check('轴面板 HTML：含 CSP nonce', html.includes("script-src 'nonce-NONCE8'"));
    check('轴面板 HTML：含轴/总线两张表', html.includes('轴状态') && html.includes('总线'));
    check(
        '轴面板 HTML：不含寄存器段（v0.4.3 已拆至 Modbus 面板）',
        !html.includes('Modbus') && !html.includes('details'),
    );

    const { KxStatusStore } = require('../out/store.js');
    const store = new KxStatusStore();
    store.onConnect('basic', 1);
    store.update({ e: 'axis', axis: 0, bus_ok: 1, enabled: 1, idle: 0, alarm: 0, mpos: 1.5, dpos: 1.4, axis_status: 0x21, err_code: 0 });
    store.update({ e: 'bus', node_count: 2, nodes: [{ index: 0, axis_count: 1, status: 0x08 }] });
    const spec = buildAxisPanelSpec(store, { connected: true });
    check('轴面板数据：1 行轴且字段齐全', spec.axes.length === 1 && spec.axes[0].mpos === 1.5 && spec.axes[0].errCode === 0);
    // v0.6.1：指令位置=DPOS 保留 4 位小数，且列顺序与表头一致（指令位置在前）
    check(
        '轴面板 HTML：指令位置=DPOS 且 toFixed(4)',
        html.includes('pos4(a.dpos)') && html.includes('toFixed(4)') && html.includes('指令位置'),
    );
    check(
        '轴面板 HTML：指令位置列在反馈位置列之前（与表头一致）',
        html.indexOf('pos4(a.dpos') > 0 && html.indexOf('pos4(a.dpos') < html.indexOf('pos4(a.mpos'),
    );
    check(
        '轴面板 HTML：反馈位置 MPOS 同样 4 位小数',
        html.includes('pos4(a.mpos)') && !html.includes('posN('),
    );
    // v0.6.2：增量渲染 + 暂停刷新（不再每帧全量重建 DOM）
    check(
        '轴面板 HTML：增量渲染（setText 仅在变化时写 DOM）',
        html.includes('const setText =') && html.includes("el.textContent !== txt") && html.includes('ensureAxisTable'),
    );
    check(
        '轴面板 HTML：含「暂停刷新」按钮与最后更新时间',
        html.includes('id="pause"') && html.includes('暂停刷新') && html.includes('最后更新 '),
    );
    check('轴面板 HTML：不再全量重建（无 content.innerHTML 清空式渲染）', !html.includes("content.innerHTML = ''"));
    // v0.7.0：在线调试控件（经 D1 cmd 下发；点动按住有效）
    check(
        '轴面板 HTML：含在线调试控件（使能/去使能/停止/点动/定位）',
        ['en', 'dis', 'stop', 'jogn', 'jogp', 'target', 'mode', 'speed', 'go'].every((id) => html.includes('id="' + id + '"')) &&
            html.includes("type: 'axisCtl'") && html.includes("ctl('jogStop')"),
    );
    check('轴面板 HTML：移动经 wait=0 异步下发（注释与提示）', html.includes('wait=0'));
    // v0.8.1：脉冲当量由脚本设置——面板提供输入与应用
    check(
        '轴面板 HTML：含脉冲当量输入与应用按钮',
        html.includes('id="scale"') && html.includes('id="applyScale"') &&
            html.includes('id="scaleNow"') && html.includes("'setScale'"),
    );
    check('轴面板数据：AL 状态已解码出名称', spec.axes[0].statusName.length > 0, spec.axes[0].statusName);
    check('轴面板数据：总线 1 行', spec.bus !== null && spec.bus.rows.length === 1 && spec.bus.nodeCount === 2);
    check(
        '轴面板数据：不再承载寄存器（regs 字段已移除）',
        spec.regs === undefined,
    );

    const empty = buildAxisPanelSpec(new KxStatusStore(), { connected: false });
    check(
        '轴面板数据：未连接不伪造数据（空表 + 无降级横幅）',
        empty.axes.length === 0 && empty.bus === null && empty.subDegradeReason === '',
    );

    const st2 = new KxStatusStore();
    st2.markSubUnavailable('订阅需 13 D3');
    const degraded = buildAxisPanelSpec(st2, { connected: true });
    check('轴面板数据：订阅降级明示原因（不伪装）', degraded.subDegradeReason === '订阅需 13 D3');
}

// 「控制器文件」列表纯逻辑（filesPanelPure.ts，v0.4.9 起为侧边栏原生 TreeView）
function checkFilesPanelPure() {
    const {
        langOfName,
        toControllerFileRow,
        formatSize,
        fileIconSpec,
        buildFilesTreeRows,
        fnv1a64Hex,
        compareFileHash,
        syncLabel,
    } = require('../out/filesPanelPure.js');
    console.log('[smoke] 控制器文件（语言判定 / 行构造 / 图标 / TreeView 行模型）');

    check('语言判定：.bas→basic / .lua→lua / 其他', langOfName('demo.bas') === 'basic' && langOfName('a.LUA') === 'lua' && langOfName('x.txt') === 'other');
    const row = toControllerFileRow('loop.lua', 2048);
    check('行构造：语言标记 Lua + 尺寸透传', row.lang === 'lua' && row.langLabel === 'Lua' && row.size === 2048);

    check('尺寸格式化：B / KB / 非法值', formatSize(512) === '512 B' && formatSize(2048) === '2.0 KB' && formatSize(-1) === '0 B');
    check(
        '图标规格：basic/lua 用 file-code + 主题色，其他用 file',
        fileIconSpec('basic').id === 'file-code' &&
            fileIconSpec('basic').colorId !== fileIconSpec('lua').colorId &&
            fileIconSpec('other').id === 'file',
    );

    const noBoot = { name: '', valid: true, reason: '' };
    const empty = {
        connected: true,
        hasD6: true,
        hasD8: true,
        boot: noBoot,
        cache: { dir: '/x', files: [], error: '' },
    };
    const rows = buildFilesTreeRows({
        connected: true,
        hasD6: true,
        hasD8: true,
        boot: { name: 'a.bas', valid: true, reason: '' },
        cache: { dir: '/x', files: [{ name: 'a.bas', size: 100 }, { name: 'b.lua', size: 2048 }], error: '' },
    });
    check(
        'TreeView 行：两文件 → 两行且带名称/语言/大小描述',
        rows.length === 2 && rows[0].kind === 'file' && rows[0].name === 'a.bas' &&
            rows[1].description.includes('Lua') && rows[1].description.includes('2.0 KB'),
    );
    check('TreeView 行：id 稳定（file:名称）', rows[0].id === 'file:a.bas' && rows[1].id === 'file:b.lua');
    check(
        'TreeView 行：主文件带 main 标记与「主文件」描述',
        rows[0].main === true && rows[0].description.includes('主文件') && rows[1].main !== true,
    );

    // 状态构造器（v0.6.0 起含 D8 主文件字段）
    const st = (over) => ({
        connected: true,
        hasD6: true,
        hasD8: true,
        boot: noBoot,
        cache: { dir: '/x', files: [], error: '' },
        ...over,
    });
    check(
        '占位行：未连接 / 缺 d6 / 错误 / 空目录 各一条且不伪装',
        buildFilesTreeRows(st({ connected: false }))[0].kind === 'placeholder' &&
            buildFilesTreeRows(st({ hasD6: false }))[0].label.includes('d6') &&
            buildFilesTreeRows(st({ cache: { dir: '', files: [], error: 'boom' } }))[0].label.includes('boom') &&
            buildFilesTreeRows(st({})).length === 1 &&
            buildFilesTreeRows(st({}))[0].kind === 'placeholder',
    );
    const invalid = buildFilesTreeRows(
        st({
            boot: { name: 'gone.bas', valid: false, reason: '文件不存在' },
            cache: { dir: '/x', files: [{ name: 'a.bas', size: 1 }], error: '' },
        }),
    );
    check(
        '主文件失效 → 顶部占位明示原因（不静默）',
        invalid.length === 2 && invalid[0].kind === 'placeholder' && invalid[0].label.includes('文件不存在'),
    );
    const noD8 = buildFilesTreeRows(
        st({ hasD8: false, boot: { name: 'a.bas', valid: true, reason: '' }, cache: { dir: '/x', files: [{ name: 'a.bas', size: 1 }], error: '' } }),
    );
    check('无 d8 → 不显示主文件标记（降级不伪装）', noD8.every((r) => r.main !== true));

    // v0.8.4：控制器文件 ↔ 本地副本一致性标识（file.list.hash + 同款 FNV-1a 64）
    check(
        'FNV-1a 64：已知向量（空串 / "a" / "A = 42\\nEND"）',
        fnv1a64Hex(Buffer.from('')) === 'cbf29ce484222325' &&
            fnv1a64Hex(Buffer.from('a')) === 'af63dc4c8601ec8c' &&
            fnv1a64Hex(Buffer.from('A = 42\nEND')) === '3dcf4319e81d1824',
    );
    check(
        '一致性判定：same / diff / missing / unknown（旧固件无 hash / 读失败）',
        compareFileHash('h1', 'h1') === 'same' &&
            compareFileHash('h1', 'h2') === 'diff' &&
            compareFileHash('h1', null) === 'missing' &&
            compareFileHash('h1', undefined) === 'unknown' &&
            compareFileHash(undefined, 'h1') === 'unknown',
    );
    check(
        '一致性文案：四态各有标识（strings.ts 集中）',
        syncLabel('same') === '一致' &&
            syncLabel('diff') === '不一致' &&
            syncLabel('missing') === '本地无副本' &&
            syncLabel('unknown') === '未比较',
    );
    const syncRows = buildFilesTreeRows(
        st({
            cache: {
                dir: '/x',
                files: [
                    { name: 'a.bas', size: 1, hash: 'h', sync: 'same' },
                    { name: 'b.lua', size: 1, hash: 'h', sync: 'diff' },
                    { name: 'c.lua', size: 1, hash: 'h', sync: 'missing' },
                    { name: 'd.lua', size: 1, sync: 'unknown' },
                ],
                error: '',
            },
        }),
    );
    check(
        'TreeView 行：一致性标识进 description，提示进 tooltip',
        syncRows[0].description.includes('一致') &&
            !syncRows[0].description.includes('不一致') &&
            syncRows[1].description.includes('不一致') &&
            syncRows[2].description.includes('本地无副本') &&
            syncRows[3].description.includes('未比较') &&
            syncRows[1].tooltip.includes('重新拉取') &&
            syncRows[3].tooltip.includes('未提供'),
    );
}

// 「Modbus 寄存器」右侧面板纯逻辑（modbusPanelPure.ts，v0.4.3 自轴状态面板拆出）
function checkModbusPanelPure() {
    const { modbusPanelHtml, buildModbusPanelSpec } = require('../out/modbusPanelPure.js');
    console.log('[smoke] Modbus 面板（寄存器分组 / 展开态保留 / 降级明示）');

    const html = modbusPanelHtml('NONCEB');
    check('Modbus 面板 HTML：含 CSP nonce', html.includes("script-src 'nonce-NONCEB'"));
    check('Modbus 面板 HTML：含寄存器标题与折叠分组', html.includes('Modbus 寄存器（4x）') && html.includes('details'));
    check(
        'Modbus 面板 HTML：增量渲染（ensureGroups 只在结构变化时建 + setText 变化才写）',
        html.includes('function ensureGroups') && html.includes('const setText =') &&
            html.includes('el.textContent !== txt'),
    );
    check(
        'Modbus 面板 HTML：分组展开状态天然保留（不再重建后恢复）',
        !html.includes('opened.has(') && html.includes('details'),
    );
    check(
        'Modbus 面板 HTML：含「暂停刷新」按钮与最后更新时间',
        html.includes('id="pause"') && html.includes('暂停刷新') && html.includes('最后更新 '),
    );
    check('Modbus 面板 HTML：不再每帧全量重建', !html.includes("content.innerHTML = ''"));
    check('Modbus 面板 HTML：不含轴/总线表（与轴状态面板分开）', !html.includes('轴状态') && !html.includes('>总线<'));
    check(
        'Modbus 面板 HTML：含「仅显示已用」按钮与已用高亮样式（v0.8.6）',
        html.includes('id="usedonly"') && html.includes('仅显示已用') &&
            html.includes('reg used') && html.includes("cellUsed[k] !== c.used"),
    );

    const { KxStatusStore } = require('../out/store.js');
    const store = new KxStatusStore();
    store.onConnect('basic', 1);
    // v0.8.6：mb 事件带 used 位图（4×16 位十六进制）——3/5（字 0）、224（字 3）被标记
    store.update({
        e: 'mb',
        start: 0,
        regs: [0, 1, 2, 3],
        used: ['0000000000000028', '0000000000000000', '0000000000000000', '0000000100000000'],
    });
    const spec = buildModbusPanelSpec(store, { connected: true });
    check(
        'Modbus 面板数据：256 寄存器分 16 组 × 16',
        spec.regs.loaded && spec.regs.groups.length === 16 && spec.regs.groups[0].cells.length === 16,
    );
    check('Modbus 面板数据：只承载寄存器（无轴/总线字段）', spec.axes === undefined && spec.bus === undefined);
    check(
        '已用寄存器：位图解析（字 0 的 3/5 与字 3 的 224；未标记的不置位）',
        store.regsUsed[3] === true && store.regsUsed[5] === true && store.regsUsed[224] === true &&
            store.regsUsed[4] === false && store.regsUsed[200] === false,
    );
    check('已用寄存器：计数与单元格标记', spec.regs.usedCount === 3 &&
        spec.regs.groups[0].cells[3].used === true && spec.regs.groups[0].cells[4].used === false);
    const usedSpec = buildModbusPanelSpec(store, { connected: true, usedOnly: true });
    check(
        '仅显示已用：只剩含已用寄存器的组，且单元格按地址过滤',
        usedSpec.regs.groups.length === 2 &&
            usedSpec.regs.groups[0].cells.map((c) => c.addr).join(',') === '3,5' &&
            usedSpec.regs.groups[1].cells.map((c) => c.addr).join(',') === '224',
    );
    check('结构指纹：全量模式恒定（"a"）；仅已用模式随集合变化',
        spec.regs.shapeKey === 'a' && usedSpec.regs.shapeKey.startsWith('u:') &&
            usedSpec.regs.shapeKey !== buildModbusPanelSpec(new KxStatusStore(), { connected: true, usedOnly: true }).regs.shapeKey);

    const empty = buildModbusPanelSpec(new KxStatusStore(), { connected: false });
    check(
        'Modbus 面板数据：未收到数据明示等待（不补 0 伪装）',
        empty.regs.loaded === false && empty.regs.groups.length === 0 && empty.subDegradeReason === '',
    );

    const st2 = new KxStatusStore();
    st2.markSubUnavailable('订阅需 13 D3');
    const degraded = buildModbusPanelSpec(st2, { connected: true });
    check('Modbus 面板数据：订阅降级明示原因（不伪装）', degraded.subDegradeReason === '订阅需 13 D3');
}

// 「连接控制器」表单弹窗纯逻辑（connectPanelPure.ts，v0.3.2 替代两步 InputBox）
function checkConnectPanelPure() {
    const { connectPanelHtml, validateConnectInputs } = require('../out/connectPanelPure.js');
    console.log('[smoke] 连接表单弹窗（字段 / 校验 / HTML）');

    check('连接表单：IP 空 → 提示不能为空', validateConnectInputs('', '5000') !== '');
    check('连接表单：IP 格式错 → 提示格式', validateConnectInputs('192.168', '5000') !== '');
    check('连接表单：端口超范围 → 提示范围', validateConnectInputs('192.168.1.11', '70000') !== '');
    check('连接表单：合法输入 → 无错误', validateConnectInputs('192.168.1.11', '5000') === '');

    const html = connectPanelHtml('NONCE9', { host: '192.168.1.11', port: 5000 });    check('连接表单 HTML：含 IP 输入框（带初值）', html.includes('id="host"') && html.includes('192.168.1.11'));
    check('连接表单 HTML：含端口输入框（带初值）', html.includes('id="port"') && html.includes('5000'));
    check('连接表单 HTML：含连接/取消按钮', html.includes('id="go"') && html.includes('id="cancel"'));
    check('连接表单 HTML：错误区就地表单内（不弹 toast）', html.includes('id="err"'));
    check('连接表单 HTML：含 CSP nonce', html.includes("script-src 'nonce-NONCE9'"));
    check('连接表单 HTML：提交走 postMessage', html.includes("type: 'submit'"));

    // v0.5.1：设置模式（修改控制器地址，IP + 端口）——同一表单，标题/按钮/提示按模式切换
    const htmlSet = connectPanelHtml('NONCE9S', { host: '192.168.1.11', port: 5000, mode: 'settings' });
    check('设置表单 HTML：标题「修改IP地址」', htmlSet.includes('<h2>修改IP地址</h2>'));
    check('设置表单 HTML：按钮「保存」', htmlSet.includes('id="go">保存</button>'));
    check('设置表单 HTML：提示写明写入两项设置且不立刻连接', htmlSet.includes('不会立刻连接'));
    check(
        '连接表单：默认模式标题/按钮保持原样',
        html.includes('<h2>连接控制器</h2>') && html.includes('id="go">连接</button>'),
    );
    check(
        '设置表单：含运行期模式切换钩子（连接 ↔ 设置复用同一面板）',
        htmlSet.includes("s.mode === 'connect' || s.mode === 'settings'"),
    );
}

// 订阅管理：引用计数 + 同 tick 合并 + 限频 + D3 缺失降级（FR-6.4/6.5、NFR-3）
async function checkSubscribePure() {
    const { SubscribeManager } = require('../out/subscribe.js');
    const { KxRpcError } = require('../out/protocol.js');
    console.log('[smoke] 订阅管理（引用计数 / 合并 / 限频 / 降级）');

    const calls = [];
    const fake = {
        connected: true,
        request: (m, p) => {
            calls.push({ m, p });
            if (m === 'subscribe') {
                return Promise.reject(new KxRpcError('NOT_SUPPORTED', '订阅需 13 D3'));
            }
            return Promise.resolve({});
        },
    };
    let degraded = '';
    const mgr = new SubscribeManager(fake, 200, (r) => {
        degraded = r;
    });
    mgr.retain(['axis', 'log']);
    mgr.release(['log']);
    await sleep(50);
    const subs = calls.filter((c) => c.m === 'subscribe');
    check('同 tick 合并成一次 subscribe，且归零的 topic 不发', subs.length === 1 && subs[0].p.topics.join(',') === 'axis', JSON.stringify(subs));
    check('hz 夹取到上限 50（NFR-3）', subs[0].p.hz === 50, JSON.stringify(subs[0].p));
    check('NOT_SUPPORTED → 标记不可用并回调原因', mgr.isUnavailable === true && degraded.length > 0, degraded);
    mgr.dispose();

    const calls2 = [];
    const fake2 = { connected: true, request: (m, p) => (calls2.push({ m, p }), Promise.resolve({})) };
    const mgr2 = new SubscribeManager(fake2, 20);
    mgr2.disable('sys.info.caps 未声明 d3');
    mgr2.retain(['axis']);
    await sleep(30);
    check('disable 后不再发任何 RPC（不试、不刷日志）', calls2.length === 0 && mgr2.isUnavailable === true);
    mgr2.resubscribe();
    await sleep(30);
    check('resubscribe 解除粘性 → 重新发 subscribe', calls2.length === 1 && calls2[0].p.topics.join(',') === 'axis');
    mgr2.dispose();
}

// T-21：曲线纯逻辑（环形缓冲 / Webview 文档）无 vscode 依赖，可 Node 直测
function checkCurvePure() {
    const { CurveRing, curveHtml } = require('../out/curvePure.js');
    console.log('[smoke] 实时曲线纯逻辑（环形缓冲 / Webview 文档）');

    const ring = new CurveRing(3);
    ring.push(1, { a: 10, b: 100 });
    ring.push(2, { a: 11 }); // 本帧无 b → 断线
    ring.push(3, { a: 12, b: 102 });
    check('CurveRing: 时间轴与序列等长', ring.timesSnapshot().length === 3 && ring.series('a').length === 3);
    check('CurveRing: 缺采样补 null（不补 0、不插值）', ring.series('b')[1] === null && ring.series('b')[0] === 100);
    ring.push(4, { a: 13, b: 103 });
    check('CurveRing: 超容量裁剪最旧', ring.size === 3 && ring.timesSnapshot()[0] === 2);

    const r2 = new CurveRing(5);
    r2.push(1, { a: 1 });
    r2.push(2, { a: 2, c: 9 });
    check('CurveRing: 中途出现的新序列历史补 null', r2.series('c')[0] === null && r2.series('c')[1] === 9);
    ring.clear();
    check('CurveRing: clear 后清空', ring.size === 0 && ring.series('a').length === 0);

    const html = curveHtml({ title: 'T', series: [{ key: 'mpos', label: 'm', color: '#fff' }], maxPoints: 600 });
    check(
        'curveHtml: 自包含（CSP 禁外部资源 + acquireVsCodeApi）',
        html.includes("default-src 'none'") && html.includes('acquireVsCodeApi'),
    );
    check('curveHtml: 无外部 http(s) 引用', !/src\s*=\s*["']https?:/i.test(html) && !/href\s*=\s*["']https?:/i.test(html));
    check('curveHtml: 含 canvas 且注入 maxPoints', html.includes('<canvas id="chart">') && html.includes('600'));
    check('curveHtml: 标题 HTML 转义', curveHtml({ title: '<b>', series: [], maxPoints: 1 }).includes('&lt;b&gt;'));

    // v0.7.0：导出 CSV / 清空 / 图例显隐
    check(
        'curveHtml: 含「导出 CSV」「清空」按钮与图例显隐',
        html.includes('id="export"') && html.includes('id="clear"') &&
            html.includes("state.hidden") && html.includes('点击显示/隐藏'),
    );
    const { curveToCsv } = require('../out/curvePure.js');
    const csv = curveToCsv(
        [1000, 2000],
        [{ key: 'mpos', label: 'm', color: '#fff' }, { key: 'dpos', label: 'd', color: '#000' }],
        [[1.5, null], [2.5, undefined]],
    );
    const lines = csv.trim().split('\n');
    check('curveToCsv: 表头 time_iso,t_ms,序列键', lines[0] === 'time_iso,t_ms,mpos,dpos');
    check(
        'curveToCsv: 缺失值写空单元格（不补 0）',
        lines[1].endsWith('1.5,2.5') && lines[2].endsWith(',,') && lines[2].split(',').length === 4,
        lines[2],
    );
    check('curveToCsv: 毫秒列取整', lines[1].split(',')[1] === '1000');
}

// commandsPure 无 vscode 依赖：FR-2.5 命令名补全的候选生成可直接单测（数据源 data/commands.json）
function checkCommandsPure() {
    const fs = require('node:fs');
    const {
        parseCommandTable,
        buildCommandCompletions,
        commandGroupOf,
        commandGroupLabel,
        COMMAND_GROUP_ORDER,
    } = require('../out/commandsPure.js');

    console.log('[smoke] 命令名补全（FR-2.5，数据源 data/commands.json）');

    const {
        hoverMarkdown,
        signatureHelpFor,
        snippetFor,
        splitTopLevel,
        activeParamIndex,
        signatureOf,
    } = require('../out/commandsPure.js');

    const dataFile = path.join(__dirname, '..', 'data', 'commands.json');
    check('data/commands.json 存在（T-04 产出）', fs.existsSync(dataFile));
    const rawJson = JSON.parse(fs.readFileSync(dataFile, 'utf8'));
    const table = parseCommandTable(rawJson);

    check(
        '命令表：条目与分组齐备（names/groupList/docs）',
        table.names.length >= 70 && table.groupList.length >= 6 &&
            Object.keys(table.docs).length === table.names.length,
        `names=${table.names.length} groups=${table.groupList.length} docs=${Object.keys(table.docs).length}`,
    );
    check('分组顺序来自 command_table.h 分节', table.groupList[0].label.includes('运动'));

    // T-04 纪律：data/commands.json 必须与控制器 command_table.h 同步（改表后忘记重跑生成器 → 失败）
    {
        const headerPath = path.join(__dirname, '..', '..', 'src', 'script', 'command_table.h');
        const header = fs.readFileSync(headerPath, 'utf8');
        const namesBlock = header.match(/kNames\[\]\s*=\s*\{([\s\S]*?)\};/);
        const docsBlock = header.match(/kDocs\[\]\s*=\s*\{([\s\S]*?)\};/);
        const hNames = [...namesBlock[1].matchAll(/"([^"]+)"/g)].map((m) => m[1]);
        const hDocs = [...docsBlock[1].matchAll(/\{\s*"([A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"/g)];
        check(
            '生成器一致性：命令名集合与 command_table.h 相同',
            hNames.length === table.names.length && hNames.every((n) => table.names.includes(n)),
            `header=${hNames.length} json=${table.names.length}`,
        );
        check(
            '生成器一致性：签名/说明与 kDocs 相同（需重跑 gen-commands.mjs）',
            hDocs.length === table.names.length &&
                hDocs.every((m) => table.docs[m[1]]?.sig === m[2] && table.docs[m[1]]?.brief === m[3]),
        );
        // 语法高亮的命令名表同样由生成器注入（TICKS 这类新命令不应漏）
        const grammar = fs.readFileSync(path.join(__dirname, '..', 'syntaxes', 'kx-lua.tmLanguage.json'), 'utf8');
        check('语法高亮：命令名表含新命令（TICKS）', grammar.includes('TICKS') && grammar.includes('MOVEABS'));
        check(
            '语法高亮：命令名表与命令表完全一致（无漏）',
            table.names.every((n) => new RegExp(`\\b${n}\\b`).test(grammar)),
        );
    }

    const basic = buildCommandCompletions(table, 'kx-basic');
    const lua = buildCommandCompletions(table, 'kx-lua');

    check('候选数与命令数一致（不漏命令）', basic.length === table.names.length, `${basic.length}/${table.names.length}`);
    check('候选无重复', new Set(basic.map((c) => c.label)).size === basic.length);
    check('候选全部来自命令表', basic.every((c) => table.names.includes(c.label)));
    check('BASIC：直插命令名（call=false）', basic.every((c) => c.call === false));
    check('Lua：函数调用形态（call=true）', lua.every((c) => c.call === true));
    check(
        '首候选 = 首个分组首命令（EN，detail 为签名）',
        basic[0].label === 'EN' && basic[0].detail === 'EN()',
        JSON.stringify(basic[0]),
    );
    check('常用度排序：组内 popular 命令排前（EN 在 POPULAR 第 3 位）', basic[0].sortText === '00_03_EN');
    check(
        '文档：MOVEABS 有签名与说明（补全/Hover 共用）',
        (() => {
            const c = basic.find((x) => x.label === 'MOVEABS');
            return Boolean(c && c.doc && c.doc.sig.includes('MOVEABS') && c.doc.brief.includes('绝对'));
        })(),
    );
    check('文档：大小写不敏感（moveabs 也能命中）', Boolean(signatureOf(table, 'moveabs')));
    check(
        '文档：SDO_WRITE 明示不可用（降级不伪装）',
        signatureOf(table, 'SDO_WRITE').brief.includes('不可用'),
    );
    check(
        'commandGroupOf: EN→运动分组 / MODBUS_REG→Modbus 分组',
        commandGroupOf(table, 'EN') === table.groupList[0].key &&
            commandGroupOf(table, 'MODBUS_REG').includes('Modbus'),
    );
    check('commandGroupLabel: 未知分组回落「其他」', commandGroupLabel('nope') === '其他' && commandGroupLabel(undefined) === '其他');

    // v0.8.0：Hover / 签名帮助 / 补全片段（纯逻辑可单测）
    const md = hoverMarkdown(table, 'MOVEABS');
    check('hoverMarkdown: 含签名与说明', Boolean(md && md.includes('MOVEABS') && md.includes('绝对') && md.includes('docs/planA')));
    check('hoverMarkdown: 未登记命令返回 undefined', hoverMarkdown(table, 'NOT_A_CMD') === undefined);
    const sh = signatureHelpFor(table, 'MOVEABS(100, ');
    check('signatureHelpFor: 识别命令与文档', Boolean(sh && sh.name === 'MOVEABS' && sh.doc.sig.includes('MOVEABS')));
    check('signatureHelpFor: 无括号调用返回 undefined', signatureHelpFor(table, 'MOVEABS 100') === undefined);
    check('activeParamIndex: 顶层逗号计数', activeParamIndex('MOVEABS(1, 2, ') === 2 && activeParamIndex('MOVEABS(') === 0);
    check(
        'snippetFor: 参数占位片段',
        snippetFor('MOVEABS', 'MOVEABS(p[, spd[, acc[, wait]]])') ===
            'MOVEABS(${1:p}, ${2:spd}, ${3:acc}, ${4:wait})$0',
    );
    check('snippetFor: 无参命令', snippetFor('EN', 'EN()') === 'EN()$0');
    check('splitTopLevel: 忽略方括号内逗号', JSON.stringify(splitTopLevel('p[, spd[, acc]]')) === '["p","spd","acc"]');

    // 健壮性：脏数据不抛异常；未登记分组的命令兜底补全（NFR-6）
    const t2 = parseCommandTable({ names: ['A', 'A', 1, ''], groups: { motion: ['A', 2] } });
    check(
        '解析（旧结构）：去重 / 剔除非字符串 / 派生分组标签',
        JSON.stringify(t2.names) === '["A"]' && JSON.stringify(t2.groups.motion) === '["A"]' &&
            t2.groupList[0].label === '运动控制',
        JSON.stringify(t2),
    );
    const c3 = buildCommandCompletions(parseCommandTable({ names: ['A', 'Z'], groups: { motion: ['A'] } }), 'kx-basic');
    check('未登记分组的命令兜底为「其他」', c3.length === 2 && c3[1].label === 'Z' && c3[1].detail === '其他');
    check('parseCommandTable: null → 空表（不抛）', parseCommandTable(null).names.length === 0);

    // 代码片段（v0.8.0）：两个语言各有一份，且为合法 JSON
    const snipDir = path.join(__dirname, '..', 'snippets');
    for (const f of ['kx-basic.code-snippets', 'kx-lua.code-snippets']) {
        const p = path.join(snipDir, f);
        let ok = fs.existsSync(p);
        if (ok) {
            try {
                const obj = JSON.parse(fs.readFileSync(p, 'utf8'));
                ok = Object.keys(obj).length > 0;
            } catch {
                ok = false;
            }
        }
        check(`代码片段：${f} 存在且合法`, ok);
    }
}

// T-22/T-24：离线安装脚本、Open VSX 发布脚本、用户手册（FR-8.1/8.2/8.5）。
function checkDistribution() {
    const fs = require('node:fs');
    const { spawnSync } = require('node:child_process');
    console.log('[smoke] 分发与文档（T-22 离线安装/发布 · T-24 手册）');

    const root = path.join(__dirname, '..');
    const installSh = path.join(root, 'tools', 'install-vsix.sh');
    const publishSh = path.join(root, 'tools', 'publish-ovsx.sh');
    const readme = path.join(root, 'README.md');

    check('T-22 install-vsix.sh 存在', fs.existsSync(installSh));
    check('T-22 publish-ovsx.sh 存在', fs.existsSync(publishSh));
    check('T-24 README.md 存在', fs.existsSync(readme));

    const installSrc = fs.existsSync(installSh) ? fs.readFileSync(installSh, 'utf8') : '';
    const publishSrc = fs.existsSync(publishSh) ? fs.readFileSync(publishSh, 'utf8') : '';
    const readmeSrc = fs.existsSync(readme) ? fs.readFileSync(readme, 'utf8') : '';

    // 安装脚本：自动探测双端 CLI + 安装/卸载入口；**离线**（不带 curl/wget）
    check(
        '安装脚本自动探测 codium/code',
        /codium/.test(installSrc) && /code/.test(installSrc) && /--install-extension/.test(installSrc),
    );
    check('安装脚本支持 --uninstall / --cli', /--uninstall/.test(installSrc) && /--cli/.test(installSrc));
    check('安装脚本离线（无 curl/wget）', !/\b(curl|wget)\b/.test(installSrc));

    // 发布脚本：走 ovsx 且缺 token 明确报错
    check('发布脚本走 ovsx 且依赖 OVSX_PAT', /ovsx/.test(publishSrc) && /OVSX_PAT/.test(publishSrc));

    // shell 语法检查（无 bash 环境则跳过，不算失败）
    const hasBash = spawnSync('bash', ['--version'], { encoding: 'utf8' }).status === 0;
    if (hasBash) {
        const a = spawnSync('bash', ['-n', installSh], { encoding: 'utf8' });
        check('install-vsix.sh 语法正确（bash -n）', a.status === 0, a.stderr);
        const b = spawnSync('bash', ['-n', publishSh], { encoding: 'utf8' });
        check('publish-ovsx.sh 语法正确（bash -n）', b.status === 0, b.stderr);
    } else {
        console.log('  - 跳过 shell 语法检查（未找到 bash）');
    }

    // 手册：四个必备章节 + 明示降级语义（「不支持」而非假象）
    check(
        'README 含 安装/连接/调试/FAQ 章节',
        ['安装', '连接', '调试', 'FAQ'].every((s) => readmeSrc.includes(s)),
    );
    check('README 明示降级语义（不支持）', /不支持/.test(readmeSrc));
    check('README 记录 D1~D5 能力位', ['d1', 'd2', 'd3', 'd4', 'd5'].every((s) => readmeSrc.includes(s)));
}

// T-23 定制 VSCodium 发行版（FR-8.4）：配方/脚本齐全、离线、语法与 JSON 合法。
function checkVscodiumRecipe() {
    const fs = require('node:fs');
    const { spawnSync } = require('node:child_process');
    console.log('[smoke] T-23 定制 VSCodium（配方/脚本）');

    const root = path.join(__dirname, '..');
    const vsDir = path.join(root, 'tools', 'vscodium');
    const script = path.join(vsDir, 'build-vscodium.sh');
    const overrides = path.join(vsDir, 'product.overrides.json');
    const wsSettings = path.join(vsDir, 'workspace-template', 'settings.json');
    const wsLaunch = path.join(vsDir, 'workspace-template', 'launch.json');
    const recipeReadme = path.join(vsDir, 'README.md');

    for (const p of [script, overrides, wsSettings, wsLaunch, recipeReadme]) {
        check(`T-23 存在 ${path.relative(root, p)}`, fs.existsSync(p));
    }

    const src = fs.existsSync(script) ? fs.readFileSync(script, 'utf8') : '';
    check(
        'T-23 子命令齐全（fetch/brand/workspace/inject/build/all）',
        ['fetch', 'brand', 'workspace', 'inject', 'build', 'all'].every((c) =>
            new RegExp(`\\b${c}\\b`).test(src),
        ),
    );
    check('T-23 脚本离线（无 curl/wget）', !/\b(curl|wget)\b/.test(src));
    check(
        'T-23 inject 预置为内置插件（resources/app/extensions）',
        /resources\/app\/extensions/.test(src) && /zipfile/.test(src),
    );

    const hasBash = spawnSync('bash', ['--version'], { encoding: 'utf8' }).status === 0;
    if (hasBash) {
        const r = spawnSync('bash', ['-n', script], { encoding: 'utf8' });
        check('build-vscodium.sh 语法正确（bash -n）', r.status === 0, r.stderr);
    }

    const parse = (p) => {
        try {
            return JSON.parse(fs.readFileSync(p, 'utf8'));
        } catch {
            return null;
        }
    };
    const ov = parse(overrides);
    const st = parse(wsSettings);
    const lc = parse(wsLaunch);
    check(
        'T-23 product.overrides.json 合法且含品牌 + Open VSX + 预置设置',
        !!ov &&
            !!ov.applicationName &&
            /open-vsx\.org/.test(JSON.stringify(ov.extensionsGallery || {})) &&
            !!ov.configurationDefaults &&
            ov.configurationDefaults['kine-x.debugPort'] === 5000,
    );
    // v0.8.2：底部状态栏品牌项显示插件版本号（版本取自 package.json，不硬编码）
    {
        const { S } = require('../out/strings.js');
        const pkg = parse(path.join(root, 'package.json'));
        check(
            '状态栏：品牌项含插件版本号（Kine-X v<version>）',
            !!pkg &&
                typeof S.extension.statusBrand === 'function' &&
                S.extension.statusBrand(pkg.version).includes('v' + pkg.version) &&
                S.extension.statusBrandTooltip(pkg.version).includes(pkg.version),
            S.extension.statusBrand(pkg && pkg.version ? pkg.version : '?'),
        );
    }
    check(
        'T-23 工作区模板含 kine-x 设置与 kine-x debug 类型',
        !!st &&
            st['kine-x.debugPort'] === 5000 &&
            !!lc &&
            Array.isArray(lc.configurations) &&
            lc.configurations.length > 0 &&
            lc.configurations[0].type === 'kine-x',
    );
}

// T-16 热更新 / T-17 断点·单步·暂停：需控制器声明 d4/d5。
// 第一个 Mock（无 d4/d5）已验证「降级」路径；此处用能力齐全的 Mock 验证「有能力」路径 —— 两条路都要通（§4.4）。
async function checkHotswapAndBreakpoints() {
    const port = PORT + 1;
    console.log('[smoke] T-16/T-17（能力齐全 Mock：d1,d2,d3,d4,d5）');
    const mock = await startMock(port, 'd1,d2,d3,d4,d5');
    const events = [];
    const conn = new KxConnection({ host: '127.0.0.1', port, timeoutMs: 3000 });
    conn.on('event', (ev) => events.push(ev));
    try {
        await conn.connect();
        const info = await conn.request('sys.info');
        check('sys.info 声明 d4/d5', info.caps.includes('d4') && info.caps.includes('d5'), JSON.stringify(info.caps));

        // --- T-16 热更新：运行中带 swap 编译到新实例并原子替换 ---
        await conn.request('script.compile', { src: 'PRINT "v1"\nEND', engine: 'basic' });
        await conn.request('script.run');
        const swap = await conn.request('script.compile', { src: 'PRINT "v2"\nEND', engine: 'basic', swap: true });
        check('T-16 swap → swapped=true', swap.swapped === true, JSON.stringify(swap));
        const running = await conn.request('script.status');
        check('T-16 热更新不中断（仍 READY）', running.status === 'READY', JSON.stringify(running));
        check('T-16 热更新落日志（[hotswap]）', events.some((e) => e.e === 'log' && String(e.s).includes('[hotswap]')));
        await conn.request('script.stop');

        // --- T-17 断点登记与命中 ---
        await conn.request('breakpoint.del', {});
        await conn.request('breakpoint.add', { line: 3 });
        const bpList = await conn.request('breakpoint.list');
        check('T-17 breakpoint.list 含 line=3', bpList.breakpoints.some((b) => b.line === 3), JSON.stringify(bpList));

        await conn.request('script.run');
        await sleep(500);
        let st = await conn.request('script.status');
        check('T-17 命中断点 → PAUSED', st.status === 'PAUSED', JSON.stringify(st));
        check(
            'T-17 PAUSED 事件带 line=3（供 DAP stopped 定位）',
            events.some((e) => e.e === 'script' && e.status === 'PAUSED' && e.line === 3),
        );

        // --- T-17 单步（行级） ---
        await conn.request('script.step');
        await sleep(60);
        st = await conn.request('script.status');
        check('T-17 单步 → 行号 +1 且仍 PAUSED', st.status === 'PAUSED' && st.line === 4, JSON.stringify(st));

        // --- T-17 恢复 → 跑完 ---
        await conn.request('script.resume');
        await sleep(1200);
        st = await conn.request('script.status');
        check('T-17 恢复 → DONE', st.status === 'DONE', JSON.stringify(st));

        // --- T-17 运行中暂停 ---
        await conn.request('breakpoint.del', {});
        await conn.request('script.run');
        await sleep(50);
        await conn.request('script.pause');
        await sleep(60);
        st = await conn.request('script.status');
        check('T-17 运行中 pause → PAUSED', st.status === 'PAUSED', JSON.stringify(st));
        await conn.request('script.resume');
        await conn.request('script.stop');
    } finally {
        conn.removeAllListeners();
        conn.disconnect();
        mock.kill();
    }
}

// 语言绑定（控制器 2026-09-25 拍板：语言由脚本目录内现有脚本决定，不靠配置）——
// 对 Mock `--engine auto` 跑通「空目录两种都收 → 落盘绑定 → 越界拒绝 → 删除回退」全流程
async function checkAutoEngineBind() {
    const port = PORT + 80;   // 避开常用手测端口（5097/5098 与历史遗留的 5099 Mock）
    console.log('[smoke] 语言绑定（--engine auto：空目录两种都收 / 落盘绑定 / 删除回退）');
    const mock = await startMock(port, 'd1,d2,d3,d4,d6', 'auto');
    const conn = new KxConnection({ host: '127.0.0.1', port, timeoutMs: 3000 });
    try {
        await conn.connect();

        const i0 = await conn.request('sys.info');
        check('空目录 → engine=auto', i0.engine === 'auto', JSON.stringify(i0));

        await conn.request('script.compile', { src: 'x = 1', engine: 'lua', name: 'demo.lua' });
        const i1 = await conn.request('sys.info');
        check('编译 lua 并落盘 → engine=lua', i1.engine === 'lua', JSON.stringify(i1));
        await expectError('绑定 lua 后编译 basic → ENGINE_MISMATCH', 'ENGINE_MISMATCH', () =>
            conn.request('script.compile', { src: 'A = 1\nEND', engine: 'basic' }),
        );

        await conn.request('file.del', { name: 'demo.lua' });
        const i2 = await conn.request('sys.info');
        check('删除唯一脚本 → 回到 auto', i2.engine === 'auto', JSON.stringify(i2));

        await conn.request('script.compile', { src: 'A = 1\nEND', engine: 'basic', name: 'demo.bas' });
        const i3 = await conn.request('sys.info');
        check('编译 basic 并落盘 → engine=basic', i3.engine === 'basic', JSON.stringify(i3));
        await expectError('绑定 basic 后编译 lua → ENGINE_MISMATCH', 'ENGINE_MISMATCH', () =>
            conn.request('script.compile', { src: 'x=1', engine: 'lua' }),
        );

        // 绑定 basic 下仍可继续下载同语言脚本（多文件），删除后回到 auto
        await conn.request('script.compile', { src: 'B = 2\nEND', engine: 'basic', name: 'keep.bas' });
        const list = await conn.request('file.list');
        check('绑定后仍可下载同语言脚本（2 个文件）', list.files.length === 2, JSON.stringify(list.files));
        await conn.request('file.del', { name: 'demo.bas' });
        await conn.request('file.del', { name: 'keep.bas' });
        const i4 = await conn.request('sys.info');
        check('全部删除 → 回到 auto', i4.engine === 'auto', JSON.stringify(i4));
    } catch (e) {
        fail++;
        console.log(`  \u2717 语言绑定冒烟异常：${e && e.message ? e.message : e}`);
    } finally {
        conn.removeAllListeners();
        conn.disconnect();
        mock.kill();
    }
}

async function main() {
    let mock;
    let conn;
    try {
        checkEngineRule();
        checkUtf8ChunkDecode();
        checkDapPure();
        checkStoreAndBits();
        checkCurvePure();
        checkCommandsPure();
        checkPortMaxPure();
        checkCommPanelPure();
        checkToolbarPure();
        checkConsolePure();
        checkAxisPanelPure();
        checkModbusPanelPure();
        checkFilesPanelPure();
        checkConnectPanelPure();
        checkDistribution();
        checkVscodiumRecipe();
        await checkSubscribePure();
        console.log('[smoke] 调试通道（连接层 ↔ Mock DebugServer）');
        mock = await startMock(PORT, 'd1,d2,d3');
        console.log(`[smoke] Mock 已启动 127.0.0.1:${PORT}`);

        conn = new KxConnection({ host: '127.0.0.1', port: PORT, timeoutMs: 3000 });
        const events = [];
        conn.on('event', (ev) => events.push(ev));
        await conn.connect();
        check('连接成功', conn.connected === true);

        const info = await conn.request('sys.info');
        check('sys.info → engine/axis_count', info.engine === 'basic' && info.axis_count === 1, JSON.stringify(info));

        // 编译失败：err.line 供 Problems 定位（FR-2.4）
        await expectError(
            'COMPILE_ERROR 且 line=2',
            'COMPILE_ERROR',
            () => conn.request('script.compile', { src: 'EN\nMOVEABS\nEND', engine: 'basic' }),
            2,
        );

        const comp = await conn.request('script.compile', {
            src: 'PRINT "hi"\nEN\nMOVEABS 55\nEND',
            engine: 'basic',
        });
        check('编译成功 → labels', Array.isArray(comp.labels) && comp.labels[0] === 'main', JSON.stringify(comp));

        await expectError('ENGINE_MISMATCH', 'ENGINE_MISMATCH', () =>
            conn.request('script.compile', { src: 'x=1', engine: 'lua' }),
        );

        // 运行 → 事件推送 → 结束
        await conn.request('script.run');
        await conn.request('subscribe', { topics: ['log', 'axis', 'bus', 'mb'], hz: 20 });
        await sleep(1000);
        const st = await conn.request('script.status');
        check('运行结束 → DONE/steps', st.status === 'DONE' && st.steps === 8123, JSON.stringify(st));
        check('收到 script 事件', events.some((e) => e.e === 'script' && e.status === 'READY'));
        check('收到 log 事件', events.some((e) => e.e === 'log'));
        check(
            '收到 axis 事件（含 axis_status/err_code）',
            events.some((e) => e.e === 'axis' && typeof e.axis_status === 'number' && typeof e.err_code === 'number'),
        );
        check(
            '收到 bus 事件（node_count + nodes[]）',
            events.some((e) => e.e === 'bus' && e.node_count === 2 && Array.isArray(e.nodes)),
        );
        check(
            '收到 mb 事件（start + regs[]）',
            events.some((e) => e.e === 'mb' && typeof e.start === 'number' && Array.isArray(e.regs) && e.regs.length > 0),
        );

        // 订阅事件流 → 数据仓库（端到端，T-18；FR-6.1/6.2/6.3）
        const panelStore = new KxStatusStore();
        panelStore.onConnect('basic', 1);
        for (const ev of events) {
            panelStore.update(ev);
        }
        check(
            '事件流喂入仓库：轴/总线/寄存器齐备',
            panelStore.axes.size > 0 && Boolean(panelStore.bus) && panelStore.regsLoaded === true,
            `axes=${panelStore.axes.size} bus=${Boolean(panelStore.bus)} regsLoaded=${panelStore.regsLoaded}`,
        );
        check(
            '仓库：轴 0 样本含位置与状态字段',
            typeof panelStore.axes.get(0).mpos === 'number' && panelStore.axes.get(0).axis_status !== undefined,
        );
        check(
            '仓库：总线 node_count ≥ 1 且 nodes 非空',
            panelStore.bus.node_count >= 1 && panelStore.bus.nodes.length > 0,
        );
        check('仓库：寄存器 256 格全部装载', panelStore.regs.length === 256 && panelStore.regsLoaded === true);

        // 运行中停止 → ABORTED
        await conn.request('script.run');
        const stopped = await conn.request('script.stop');
        check('stop → ABORTED', stopped.status === 'ABORTED', JSON.stringify(stopped));

        // 变量读写
        const vars = await conn.request('var.list');
        check('var.list 含 NAME = 值 形式', vars.some((v) => v.startsWith('SPEED')), JSON.stringify(vars));
        const got = await conn.request('var.get', { name: 'SPEED' });
        check('var.get → type/value', got.type === 'num' && got.value === 100, JSON.stringify(got));
        await conn.request('var.set', { name: 'SPEED', v: 250 });
        const after = await conn.request('var.get', { name: 'SPEED' });
        check('var.set 生效', after.value === 250, JSON.stringify(after));

        // 轴越界绝不回读别的轴（16 §4.4）
        await expectError('axis 越界 → BAD_PARAM', 'BAD_PARAM', () =>
            conn.request('axis.snapshot', { axis: 3 }),
        );

        // D6 文件管理：下载带 name 落盘 → list/get → del（端到端）
        const compNamed = await conn.request('script.compile', {
            src: 'PRINT "v1"\nEND',
            engine: 'basic',
            name: 'smoke_demo.bas',
        });
        check('D6 下载带 name → saved=true', compNamed.saved === true, JSON.stringify(compNamed));
        const fl = await conn.request('file.list');
        check(
            'D6 file.list 含刚下载的文件',
            fl.files.some((f) => f.name === 'smoke_demo.bas' && f.size > 0),
            JSON.stringify(fl),
        );
        const fg = await conn.request('file.get', { name: 'smoke_demo.bas' });
        check('D6 file.get 内容一致', fg.src === 'PRINT "v1"\nEND', JSON.stringify(fg));
        await expectError('D6 file.get 不存在 → NOT_FOUND', 'NOT_FOUND', () =>
            conn.request('file.get', { name: 'nope.bas' }),
        );
        await expectError('D6 非法 name（路径穿越）→ BAD_PARAM', 'BAD_PARAM', () =>
            conn.request('file.get', { name: '../etc/passwd' }),
        );
        const fd = await conn.request('file.del', { name: 'smoke_demo.bas' });
        check('D6 file.del → deleted', fd.deleted === true, JSON.stringify(fd));
        const fl2 = await conn.request('file.list');
        check('D6 删除后 list 为空', fl2.files.length === 0, JSON.stringify(fl2));

        // D9 端口数量上限：get 默认 16 → set 24 → 回读 24 → 非法值 → 恢复 16（Mock 只改内存）
        const pm0 = await conn.request('port.max.get');
        check('D9 port.max.get 默认 16 / 容量 64', pm0.max === 16 && pm0.slots === 64, JSON.stringify(pm0));
        const pmSet = await conn.request('port.max.set', { max: 24 });
        check(
            'D9 port.max.set 24 → {max,prev,saved}',
            pmSet.max === 24 && pmSet.prev === 16 && pmSet.saved === true,
            JSON.stringify(pmSet),
        );
        const pm1 = await conn.request('port.max.get');
        check('D9 设置后回读 24', pm1.max === 24, JSON.stringify(pm1));
        await expectError('D9 max=0 → BAD_PARAM', 'BAD_PARAM', () => conn.request('port.max.set', { max: 0 }));
        await expectError('D9 max=65 → BAD_PARAM', 'BAD_PARAM', () => conn.request('port.max.set', { max: 65 }));
        const pmBack = await conn.request('port.max.set', { max: 16 });
        check('D9 恢复 16', pmBack.max === 16 && pmBack.prev === 24, JSON.stringify(pmBack));

        // D10 通讯状态：订阅 conn → 事件（4 连接 + 1 从站；标签与板端脚本一致）
        await conn.request('subscribe', { topics: ['conn'], hz: 20 });
        {
            const deadline = Date.now() + 2500;
            while (Date.now() < deadline && !events.some((e) => e.e === 'conn')) {
                await new Promise((r2) => setTimeout(r2, 50));
            }
            const cev = events.find((e) => e.e === 'conn');
            check(
                'D10 收到 conn 事件（conns[] + bus.slaves[]）',
                Boolean(cev) && Array.isArray(cev.conns) && cev.conns.length === 4 &&
                    Boolean(cev.bus) && cev.bus.slaves.length === 1,
            );
            check(
                'D10 事件行含用途标签/对端 IP/端口',
                Boolean(cev) && cev.conns.some((c) => c.tag === 'Modbus-TCP 从站(502)' && c.role === '从站' &&
                    c.peer === '192.168.1.3' && c.listen === 502),
            );
            const cs = new KxStatusStore();
            cs.onConnect('lua', 1);
            if (cev) { cs.update(cev); }
            check('D10 仓库：conns/bus 装载', cs.conns.length === 4 && cs.busSlaves.length === 1);
        }
        await conn.request('unsubscribe', { topics: ['conn'] });

        // D8 主文件（开机运行）：空 → 设 → 失效 → 清除（Mock 只改内存清单）
        const bg0 = await conn.request('boot.get');
        check('D8 boot.get 初始为空且合法', bg0.name === '' && bg0.valid === true, JSON.stringify(bg0));
        await conn.request('script.compile', { src: 'A = 1\nEND\n', engine: 'basic', name: 'boot_main.bas' });
        const bs = await conn.request('boot.set', { name: 'boot_main.bas' });
        check('D8 boot.set → {name,valid}', bs.name === 'boot_main.bas' && bs.valid === true, JSON.stringify(bs));
        const bg1 = await conn.request('boot.get');
        check('D8 boot.get 回读主文件', bg1.name === 'boot_main.bas' && bg1.valid === true, JSON.stringify(bg1));
        await expectError('D8 boot.set 不存在的文件 → NOT_FOUND', 'NOT_FOUND', () =>
            conn.request('boot.set', { name: 'ghost.bas' }),
        );
        await conn.request('file.del', { name: 'boot_main.bas' });
        const bg2 = await conn.request('boot.get');
        check(
            'D8 删除主文件 → boot.get 明确失效（不静默）',
            bg2.name === 'boot_main.bas' && bg2.valid === false && typeof bg2.reason === 'string',
            JSON.stringify(bg2),
        );
        const bc = await conn.request('boot.clear');
        check('D8 boot.clear → 空', bc.name === '');

        // D5 未就绪 → 明确降级，不伪装（15 §4.4）
        await expectError('script.step → NOT_SUPPORTED', 'NOT_SUPPORTED', () => conn.request('script.step'));

        // T-16 未声明 d4 → 运行中 swap 明确拒绝（插件据此提示「先停止再下载」）
        await expectError('script.compile(swap) 无 d4 → NOT_SUPPORTED', 'NOT_SUPPORTED', () =>
            conn.request('script.compile', { src: 'PRINT "x"\nEND', engine: 'basic', swap: true }),
        );

        const cmd = await conn.request('cmd', { line: 'POS' });
        check('cmd → ret/out', cmd.ret === 0 && Array.isArray(cmd.out) && cmd.out.length === 1, JSON.stringify(cmd));

        // D7 重启（v0.5.0）：Mock 只回应答不真重启（真机由 DEBUG_RESTART_CMD 执行）
        const rst = await conn.request('sys.restart');
        check('D7 sys.restart → {restarting:true}', rst.restarting === true, JSON.stringify(rst));

        await expectError('未知方法 → UNKNOWN_METHOD', 'UNKNOWN_METHOD', () => conn.request('bogus.method'));

        await conn.request('unsubscribe', { topics: ['log', 'axis', 'bus', 'mb', 'script'] });

        conn.disconnect();
        check('断开后 connected=false', conn.connected === false);

        // 能力齐全路径（T-16 热更新 / T-17 断点单步暂停）
        await checkHotswapAndBreakpoints();
        // 语言绑定（--engine auto，控制器 2026-09-25 语义）
        await checkAutoEngineBind();
    } catch (e) {
        fail++;
        console.log(`  \u2717 冒烟流程异常中断：${e && e.stack ? e.stack : e}`);
    } finally {
        if (conn) {
            conn.removeAllListeners();
        }
        if (mock) {
            mock.kill();
        }
    }

    console.log(`\n[smoke] 通过 ${pass} / 失败 ${fail}`);
    process.exit(fail === 0 ? 0 : 1);
}

main();
