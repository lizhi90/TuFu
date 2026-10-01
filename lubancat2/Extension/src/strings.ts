// strings.ts —— UI 文案集中（docs/planA/15 NFR-8：首版中文；文案集中，预留 i18n）
//
// 约定：
//   * **所有面向用户的文案集中在此**，按模块分命名空间，避免散落在逻辑里。
//   * 带变量的文案用**函数**承载：参数与语序同处一地，便于翻译时调整。
//   * 新增语言：在 `CATALOGS` 注册一份 `Catalog`，在 `activate()` 里调用 `setLocale(id)`。
//     首版仅 `zh-CN`，故默认不调用 `setLocale`。
//   * 本模块**纯函数、无 vscode 依赖**，故纯逻辑模块（statusBits/engineRule/…）也能引用，
//     其文案同样可在 `test/connection.smoke.cjs` 中被断言。
//
// 边界（有意为之）：与控制器**协议逐字对齐**的常量（如状态位名、EtherCAT AL 状态名）留在
// 各自的数据模块（statusBits.ts），不搬进文案目录——避免「文案」与「协议数值」分家导致失同步。

export const zh = {
    /** 命令分组显示名（FR-2.5 补全 detail）；数据源见 data/commands.json */
    commandGroup: {
        motion: '运动控制',
        axis: '轴参数',
        status: '状态查询',
        bus: '总线与节点',
        port: '端口与打印',
        misc: '任务与 Modbus',
        other: '其他',
    },

    /** 通用占位（各模块共用） */
    common: {
        unknown: '未知',
        none: '无',
        noData: '—（未收到数据）',
        noDataShort: '未收到数据',
        listSep: '、',
    },

    /** engineRule.ts */
    engine: {
        mismatch: (engine: string, file: string): string =>
            `脚本文件扩展名(.lua/.bas)与脚本引擎(${engine})不一致：${file}`,
        /** sys.info.engine = auto：控制器脚本目录为空，两种语言都可编译（首次落盘绑定） */
        autoLabel: '自动',
        /** sys.info.engine = mixed：控制器目录同时存在 .bas 与 .lua，需先删除一种 */
        mixedLabel: '混合',
        /**
         * 控制器已确定的引擎（`sys.info.engine` ← 板端 `SCRIPT_ENGINE`）与本地文件引擎冲突。
         * 明确给出两条出路，避免用户误以为「控制器里没有文件就不该有语言」。
         */
        controllerConflict: (controllerEngine: string, fileEngine: string): string =>
            `控制器当前脚本语言为 ${controllerEngine}（板端 config/app.conf 的 SCRIPT_ENGINE，` +
            `与控制器里有无脚本文件无关）：本文件是 ${fileEngine} 脚本。` +
            `处理：① 改用 ${controllerEngine === 'basic' ? '.bas' : '.lua'} 脚本；` +
            `② 或把板端 SCRIPT_ENGINE 改为 ${fileEngine} 并重启 kine-x.service` +
            `（重启会中断正在运行的脚本/电机任务）。同一时刻只加载一种脚本语言（engine_rule.h）。`,
    },

    /** engineResolve.ts（D-03 裁决） */
    engineResolve: {
        settingConflict: (cfgEngine: string, inferred: string, fsPath: string): string =>
            `引擎设置(kine-x.engine=${cfgEngine}) 与文件扩展名推断(${inferred}) 冲突：` +
            `${fsPath}（D-03：以扩展名推断为主，请对齐设置或改用对应语言文件）`,
    },

    /** dapPure.ts */
    dapPure: {
        d5Notice: (command: string): string =>
            `[NOT_SUPPORTED] ${command} 需控制器引擎 pause 钩子（docs/planA/13 D5），当前未就绪。` +
            '降级说明：可看变量/改值、看输出与终止原因；断点、单步、暂停/恢复不可用。',
    },

    /** curve.ts */
    curve: {
        title: 'Kine-X 实时曲线',
        targetCmd: '指令位置 DPOS',
        targetFb: '反馈位置 MPOS',
        followerr: '跟随误差 DPOS-MPOS',
        speed: '速度(推算)',
        opened: '实时曲线已打开：已订阅 axis（事件驱动，不轮询）。',
        exported: (path: string): string => `曲线已导出 CSV：${path}`,
        exportEmpty: '暂无可导出的数据（先让曲线收到样点）。',
        exportFailed: (msg: string): string => `导出 CSV 失败：${msg}`,
        cleared: '曲线已清空（继续按订阅事件采样）。',
    },

        /** axisPanel.ts —— 右侧「轴状态」面板（v0.4.2 自控制台迁出；v0.4.3 寄存器拆出） */
    axisPanel: {
        title: 'Kine-X 轴状态',
        /** v0.7.0 在线调试动作的执行记录与提示 */
        ctlEnable: '轴使能',
        ctlDisable: '轴去使能',
        ctlStop: '急停（STOP）',
        ctlJog: (dir: number): string => `点动 ${dir < 0 ? '负向' : '正向'}`,
        ctlJogStop: '停止点动',
        ctlMove: (mode: string, target: number, speed: number): string =>
            `${mode}移动 ${target.toFixed(3)} mm @ ${speed} mm/s（异步下发）`,
        ctlTargetRequired: '请先填写目标位置（mm）再执行。',
        ctlScale: (v: number): string => `设置脉冲当量 ${v} inc/mm（UNITS）`,
        ctlScaleRequired: '脉冲当量必须 > 0（inc/mm）。',
        jogWatchdog: '[axisCtl] 点动面板心跳中断（面板关闭/卡死），已自动停止点动（CANCEL 2）。',
        abs: '绝对',
        rel: '相对',
    },

    /** filesPanelPure.ts / filesTree.ts —— 「控制器文件」原生 TreeView（v0.4.9 起） */
    filesPanel: {
        title: 'Kine-X 控制器文件',
        viewName: '控制器文件',
        langBasic: 'Basic',
        langLua: 'Lua',
        langOther: '其他',
        notConnected: '连接控制器后显示已保存的脚本（.bas / .lua）',
        noD6: '控制器未声明 d6（文件管理）：板端程序升级后才会保存/列出下载过的脚本',
        listError: (err: string): string => `列表获取失败：${err}`,
        empty: '还没有文件：打开本地脚本点「下载」，成功后自动保存到这里',
        /** 一致性标识（v0.8.4：控制器 file.list.hash ↔ 本地 controller-sync/ 副本） */
        syncSame: '一致',
        syncDiff: '不一致',
        syncMissing: '本地无副本',
        syncUnknown: '未比较',
        syncHint: (state: string): string => {
            switch (state) {
                case 'same':
                    return '一致性：本地 controller-sync/ 副本与控制器内容相同（FNV-1a 64 哈希一致）。';
                case 'diff':
                    return '一致性：本地 controller-sync/ 副本与控制器内容不同——重新拉取可覆盖本地副本（本地改动会丢失）。';
                case 'missing':
                    return '一致性：本地 controller-sync/ 下没有该文件——点击本行或「同步」拉取后可比对。';
                default:
                    return '一致性：未比较（无工作区、本地读取失败，或控制器固件未提供 file.list.hash）。';
            }
        },
        fileTooltip: (name: string, lang: string, size: string): string =>
            `${name}（${lang}，${size}）\n点击拉取到工作区 controller-sync/；行内按钮可拉取、设为主文件或删除`,
        /** D8 主文件（开机运行） */
        mainTag: '主文件',
        mainTooltip: (name: string, lang: string, size: string): string =>
            `${name}（${lang}，${size}）\n★ 主文件：控制器开机自动运行它；其它脚本由它在 INCLUDE 中作为子程序调用。` +
            `\n点击拉取到工作区 controller-sync/`,
        bootInvalid: (name: string, reason: string): string =>
            `主文件不可用（${name}）：${reason}`,
        setMainTitle: '设为主文件（开机运行）',
        setMainTip: '把该脚本设为控制器开机自动运行的主文件（下次启动生效，可用「重启控制器」立即应用）',
        clearMainTitle: '取消主文件',
        clearMainTip: '取消开机运行的主文件（之后回退到控制器配置的 SCRIPT_FILE，若也未配置则不自动运行）',
        setMainDone: (name: string): string => `已把 ${name} 设为主文件（开机运行），下次启动生效。`,
        setMainAskRestart: '现在重启控制器立即应用吗？',
        setMainFail: (name: string, err: string): string => `设置主文件 ${name} 失败：${err}`,
        clearMainDone: '已取消主文件（开机将回退到 SCRIPT_FILE 配置；未配置则不会自动运行脚本）。',
        clearMainFail: (err: string): string => `取消主文件失败：${err}`,
        noD8: '控制器未声明 d8（主文件/开机运行）：板端程序升级后才能设主文件。',
        pullTitle: '拉取到工作区',
        pullAllTitle: '全部拉取到工作区',
        refreshTitle: '刷新控制器文件',
        deleteTitle: '删除控制器文件',
        deleteConfirm: (name: string): string =>
            `确定删除控制器上的 ${name}？\n删除后若目录清空，脚本语言绑定会回到「自动」（.bas/.lua 都可下载）。`,
        deleteConfirmMain: (name: string): string =>
            `确定删除控制器上的主文件 ${name}？\n删除后**开机将没有可运行的主文件**（需重新指定），` +
            `且若目录清空，脚本语言绑定会回到「自动」。`,
        deleteBtn: '删除',
        deleteDone: (name: string): string => `已删除控制器文件 ${name}`,
        deleteFail: (name: string, err: string): string => `删除 ${name} 失败：${err}`,
        pulled: (name: string): string => `已拉取 ${name} → controller-sync/`,
    },

    /** modbusPanel.ts —— 右侧「Modbus 寄存器」面板（v0.4.3 自轴状态面板拆出） */
    modbusPanel: {
        title: 'Kine-X Modbus 寄存器',
    },

    /** curvePure.ts */
    curvePure: {
        axisPrefix: '轴 ',
        samplePrefix: '样点 ',
        waiting: '等待订阅数据…',
        exportCsv: '导出 CSV',
        clear: '清空',
        capError: (cap: string): string => `CurveRing.cap 必须是正整数，收到 ${cap}`,
    },

    /** connection.ts（调试通道客户端） */
    connection: {
        notConnected: '未连接',
        closed: '连接已断开',
        closedByPeer: '连接已关闭',
        unknownError: '未知错误',
        timeout: (method: string, timeout: number): string => `${method} 超时(${timeout}ms)`,
        warnNoAuth: '[warn] 服务端未实现 auth，按未启用 token 处理',
        warnBadJson: (line: string): string => `[warn] 非法 JSON 行：${line}`,
        warnUnknownMessage: (line: string): string => `[warn] 未知报文：${line}`,
        warnUnmatchedId: (id: string): string => `[warn] 收到无匹配 id 的应答：${id}`,
    },

    /** toolbarPure.ts / toolbar.ts —— 侧边栏顶部横向菜单条 */
    toolbar: {
        viewName: '菜单栏',
        /** 按钮上的可见短文字（与图标并排；保证任何环境都能看懂） */
        connectText: '连接',
        disconnectText: '断开',
        downloadText: '下载',
        downloadAndRunText: '运行',
        stopText: '停止',
        runText: '运行脚本',
        syncLocalText: '同步',
        restartText: '重启控制器',
        cmdText: '设备命令',
        axisPanelText: '轴状态',
        modbusText: 'Modbus',
        curveText: '曲线',
        refreshText: '刷新',
        sysInfoText: '信息',
        setIpText: '修改IP地址',
        /** 顶层菜单名（普通软件菜单栏样式） */
        menuController: '控制器',
        menuScript: '脚本',
        menuScriptLang: '脚本语言',
        menuTools: '工具',
        engineBasicText: 'Basic',
        engineLuaText: 'Lua',
        engineBasicTip: '使用 Basic 语言（写入本项目设置 kine-x.engine）',
        engineLuaTip: '使用 Lua 语言（写入本项目设置 kine-x.engine）',
        /** 语言切换结果提示 */
        engineSetDone: (lang: string, scope: string): string => `已切换为 ${lang} 语言环境（写入${scope}设置 kine-x.engine）。`,
        engineSetMismatch: (ctrl: string): string => `注意：控制器当前运行 ${ctrl} 引擎，语言不一致时连接/编译将被拒（ENGINE_MISMATCH）。`,
        engineSetScopeWorkspace: '工作区',
        engineSetScopeGlobal: '全局',
        /** 悬停完整说明 */
        connectTip: '连接控制器',
        disconnectTip: '断开连接',
        downloadTip: '下载到控制器（当前打开的 .bas / .lua）',
        downloadAndRunTip: '下载并运行',
        stopTip: '停止脚本',
        runTip: '运行已下载的脚本',
        syncLocalTip: '同步：把控制器中保存的脚本文件拉取到工作区 controller-sync/ 目录（需控制器支持 d6）',
        restartTip: '重启控制器（sys.restart，需控制器支持 d7；会中断运行的脚本/电机任务）',
        portMaxText: '修改端口数量',
        portMaxTip: '修改控制器端口数量上限（port.max.set，需控制器支持 d9；重启后保留）',
        commText: '通讯状态',
        commTip: '查看控制器对外连接与 EtherCAT 主/从状态（需控制器支持 d10）',
        mbmapText: 'Modbus 从站',
        mbmapTip: '控制器作 Modbus 从站：组态对外暴露的 4x 寄存器表（变量名/地址/类型/读写/掉电保持/默认值；触摸屏/机器人按地址用，脚本按名用；需 d12 或 d11）',
        mbdevText: 'Modbus 主站',
        mbdevTip: '配置控制器作为 Modbus 主站读写的设备与点位（读→落 4x/变量；写→变化即下发；需控制器支持 d13）',
        cmdTip: '执行一条设备命令',
        axisPanelTip: '打开轴状态面板（轴 / 总线，右侧窗口）',
        modbusTip: '打开 Modbus 寄存器面板（4x 寄存器，右侧窗口）',
        curveTip: '打开实时曲线',
        refreshTip: '刷新状态面板',
        sysInfoTip: '显示控制器信息',
        setIpTip: '修改IP地址（同一表单里也可改端口，写入 kine-x.host / kine-x.debugPort）',
        statusOffline: '未连接',
        statusIdle: '空闲',
        statusItemText: '$(list-unordered) Kine-X',
        statusItemTooltip: 'Kine-X 菜单：连接 / 下载 / 运行 / 停止 / 曲线 / 刷新 / 信息（点击展开）',
        statusItemPlaceholder: '选择 Kine-X 操作',
        /** 状态栏文字动作条（v0.2.0：底部横排，点击直接执行） */
        tbConnect: '$(plug) 连接',
        tbDisconnect: '$(debug-disconnect) 断开',
        tbDownload: '$(cloud-upload) 下载',
        tbRun: '$(debug-start) 运行',
        tbStop: '$(debug-stop) 停止',
        tbCurve: '$(graph) 曲线',
        tbRefresh: '$(refresh) 刷新',
        /** 需要调试口可独占脚本引擎（caps 含 d2）时的置灰原因 */
        disabledNeedEngine: 'controller 未提供 d2（调试口不能独占脚本引擎）',
        /** 未连接时的置灰原因 */
        disabledOffline: '未连接控制器',
    },

    /** portMaxPure.ts / extension.ts —— 「控制器 → 修改端口数量」（D9，v0.8.5） */
    portMax: {
        noD9: '控制器未声明 d9（端口数量可调）：板端固件升级后可用',
        prompt: (cur: number, max: number): string => `端口数量上限（当前 ${cur}，可设 1..${max}）`,
        invalid: (min: number, max: number): string => `请输入 ${min}..${max} 的整数`,
        summary: (max: number, slots: number, used: string): string =>
            `端口数量上限 ${max}（静态容量 ${slots}；占用：${used}）`,
        busy: (err: string): string => `无法收缩：${err}`,
        done: (n: number, prev: number): string => `端口数量上限已从 ${prev} 改为 ${n}（已保存，重启后保留）`,
        unchanged: (n: number): string => `端口数量上限已是 ${n}，未改动`,
        logDone: (n: number, file: string): string => `[portMax] 上限=${n}，已写入 ${file}`,
    },

    /** commPanelPure.ts —— 「工具 → 通讯状态」（D10，v0.8.7） */
    commPanel: {
        title: '通讯状态',
        headConns: '对外连接（TCP）',
        headBus: 'EtherCAT 总线',
        colKind: '连接方式',
        colRole: '控制器角色',
        colMaster: '主/从',
        colPeer: '对方 IP',
        colPort: '端口',
        colState: '状态',
        colSlave: '从站',
        colIdentity: '身份（Vendor:Product rev）',
        colName: '名称',
        colAxis: '轴',
        colAl: 'AL',
        colOnline: '在线',
        kindServer: 'TCP 服务端',
        kindClient: 'TCP 客户端',
        roleServer: '服务端',
        roleClient: '客户端',
        masterMaster: '主站',
        masterSlave: '从站',
        masterNone: '—',
        stateConnected: '已连接',
        stateListening: '监听中（等待客户端）',
        stateConnecting: '连接中',
        stateDisconnected: '断开',
        busMasterRow: 'EtherCAT 主站',
        busSummary: (link: string, op: string, nodes: number): string =>
            `link=${link} 从站=${nodes} AL聚合=${op}`,
        waitData: '等待连接数据（控制器运行后出现）',
        noneYet: '—',
        noD10: '控制器未声明 d10（通讯状态）：板端固件升级后可用',
        offline: '未连接控制器',
        updated: (hhmmss: string): string => `最后更新 ${hhmmss}`,
    },

    /** mbmapPanelPure.ts —— 「工具 → Modbus 配置」（D11，v0.9.0） */
    mbmapPanel: {
        title: 'Modbus 从站',
        sourceController: '来源：控制器（.mbmap）',
        sourceLocal: '来源：本地副本（控制器未连接 / 不支持 d11 / 读取失败）',
        sourceNone: '来源：无数据',
        sourceLoading: '加载中…',
        errEmpty: '寄存器表为空',
        errAccessD12: '读写权限仅支持：读(r) / 写(w) / 读写(rw)',
        errPersistRange: '掉电保持仅支持 4x0~1023（NVRAM 1024 字上限）',
        errJson: (e: string): string => `寄存器表 JSON 解析失败：${e}`,
        errShape: '寄存器表结构不正确（缺少 registers/entries 数组）',
        accessR: '读',
        accessW: '写',
        accessRW: '读写',
        accessNone: '—',
        persistMark: '★',
        zoneDefault: '未分组',
        colAddr: '地址',
        colName: '名称',
        colType: '类型',
        colAccess: '读写',
        colKeep: '保持',
        colDesc: '说明',
        searchPlaceholder: '搜索：地址 / 名称 / 类型 / 说明…',
        refreshBtn: '刷新',
        totalText: (n: number): string => `共 ${n} 条寄存器（★ = 掉电保持）`,
        empty: '（无寄存器条目）',
        note: '说明：本页展示已配置的寄存器（当前为项目已实现清单）。用户自定义寄存器编辑将在后续版本开放。',
        noD11: '控制器未声明 d11/d12（Modbus 配置）：板端固件升级后可用',
        /** 用户寄存器编辑（v0.10.0；4x300~999 真正生效） */
        userTitle: '用户寄存器（可编辑 · 真正生效）',
        userHint: '地址 4x300~999；类型 u16/i16 占 1 个寄存器，u32/f32/f32hi 占 2 个；“掉电保持”写入即存，重启后自动恢复。保存后控制器热加载（约 1 秒生效），固定区寄存器不受影响。',
        addBtn: '新增',
        saveBtn: '保存到控制器',
        reloadBtn: '重新加载',
        delBtn: '删除',
        colPersist: '保持',
        colUserCol: '操作',
        colAddrPh: '4x300',
        colNamePh: '名称',
        colDescPh: '说明',
        typeLocked: '（固定）',
        saveOk: '用户寄存器已保存并下发（控制器约 1 秒内热加载）',
        saveFail: (m: string): string => `保存失败：${m}`,
        errAddr: '地址格式应为 4xNNN（如 4x300）',
        errRange: (lo: number, hi: number): string => `地址需在 4x${lo}~4x${hi} 之间（含 32 位类型占位）`,
        errAccess: '读写权限仅支持：读 / 读写',
        errOverlap: (a: string): string => `地址与已有条目重叠：${a}`,
        errNoD11: '控制器未声明 d11：无法保存（只读展示）',
        /** v0.11.0：D12 固件组态（productized；planA/20） */
        protoD12: '固件组态（D12）',
        protoD11: '过渡模式（D11 用户寄存器）',
        colDefault: '默认值',
        defPh: '可选',
        d12Title: '寄存器组态（可编辑 · 固件生效）',
        d12Hint: '固件 Modbus 组态：全部条目可直接编辑（名称/地址/类型/读写/掉电保持/默认值/说明）。保存后固件即时热加载生效；脚本按 MB_READ/MB_WRITE 名字使用，触摸屏/机器人按地址使用。',
        d11Hint: '当前固件为过渡版（D11）：固定区只读、仅“用户寄存器”可编辑。升级固件（含 D12）后本页将变为全表可编辑。',
        stationText: (n: number): string => `从站站号 ${n}`,

        userEmpty: '（暂无用户寄存器：点「新增」添加，例如 4x300 温度设定 f32 读写 掉电保持）',
    },

    /** mbdevPanelPure.ts —— 「工具 → Modbus 主站」（D13，v0.12.0；planA/21） */
    mbdevPanel: {
        title: 'Modbus 主站',
        sourceController: '来源：控制器（config/modbus_master.json）',
        sourceNone: '未连接控制器或控制器未声明 d13（固件升级后可用）',
        sourceLoading: '加载中…',
        errEmpty: '主站组态文本为空',
        errJson: (e: string): string => `主站组态 JSON 解析失败：${e}`,
        errShape: '主站组态结构不正确（缺少 devices 数组）',
        errDevTimeout: '超时需在 50~5000 ms',
        errDevRetries: '重试次数需在 0~5',
        errDevPoll: '轮询周期需在 20~60000 ms',
        errDevNoPoints: '设备至少需要一个点位',
        errPointBitType: '线圈类点位类型仅支持 u16/i16',
        errPointFc6Span: 'fc6（写单寄存器）不支持 2 字类型，请改用 fc16',
        hint: '设备填写链路（tcp / rtu-tcp 透传网关）与轮询周期；点位方向/功能码/地址/类型，映射选「4x 地址」（屏/机器人直接用）或「变量」（脚本 MB_READ("设备.点位") 用）。保存后固件即时热加载；设备在线状态在下方显示。',
        devTitle: '设备',
        pointTitle: '点位',
        addDev: '新增设备',
        delDev: '删除设备',
        addPoint: '新增点位',
        saveBtn: '保存到控制器',
        reloadBtn: '重新加载',
        delBtn: '删除',
        namePh: '名称',
        hostPh: 'IP / 主机',
        portPh: '端口',
        unitPh: '站号',
        timeoutPh: '超时ms',
        lblKind: '链路',
        lblName: '名称',
        lblHost: '主机/IP',
        lblPort: '端口',
        lblUnit: '站号',
        lblTimeout: '超时(ms)',
        lblRetries: '重试',
        lblPoll: '轮询(ms)',
        retriesPh: '重试',
        pollPh: '轮询ms',
        addrPh: '地址',
        mapAddrPh: '4x600',
        colName: '名称',
        colDir: '方向',
        colFc: '功能码',
        colAddr: '地址',
        colCount: '数量',
        colType: '类型',
        colMap: '映射',
        colOnChange: '变化即发',
        colOp: '操作',
        dirRead: '读',
        dirWrite: '写',
        mapReg: '4x地址',
        mapVar: '变量',
        kindTcp: 'TCP',
        kindRtu: 'RTU透传',
        empty: '（暂无：点「新增设备」开始组态）',
        emptyPoints: '（该设备暂无点位：点「新增点位」）',
        statusTitle: '设备状态',
        statusOnline: '在线',
        statusOffline: '离线',
        statusText: (n: number): string => `共 ${n} 台设备（状态根据控制器返回）`,
        saveOk: '主站组态已保存并下发（固件即时热加载）',
        saveFail: (m: string): string => `保存失败：${m}`,
        errDevName: '设备名不能为空且需唯一',
        errDevHost: '主机地址不能为空',
        errDevPort: '端口需 1~65535',
        errDevUnit: '站号需 1~247',
        errPointName: '点位名不能为空且设备内唯一',
        errPointAddr: '地址需 0~65535 且与数量不越界',
        errPointFc: '功能码与方向不匹配（读 1/2/3/4；写 5/6/15/16）',
        errPointCount: '数量必须等于类型的寄存器字数（u16/i16=1，u32/f32/f32hi=2；线圈=1）',
        errPointMap: '映射为 4x 地址时需填写形如 4x600 的地址',
        errWriteDup: (a: string): string => `主站写点的 4x 映射重叠：${a}`,
    },

    /** connectPanelPure.ts —— 「连接控制器」表单弹窗（v0.3.2 替代两步 InputBox） */
    connectPanel: {
        title: '连接控制器',
        hostLabel: 'IP 地址',
        portLabel: '端口',
        connectBtn: '连接',
        cancelBtn: '取消',
        connectingBtn: '连接中…',
        hint: '连接后轴状态 / 总线 / 寄存器由事件推送自动刷新。上次使用的地址会自动带出。',
        errHostEmpty: 'IP 地址不能为空',
        errHostFormat: 'IP 地址格式不正确（应为 192.168.1.11 这样的 IPv4 地址）',
        errPortEmpty: '端口不能为空',
        errPortRange: '端口应为 1–65535 的整数',
        /** 设置模式（修改IP地址 + 端口，v0.5.1）：同一表单，按钮为「保存」 */
        settingsTitle: '修改IP地址',
        saveBtn: '保存',
        settingsHint:
            '保存后作为「连接控制器」与调试器的默认地址（写入 kine-x.host / kine-x.debugPort），不会立刻连接。',
    },
    connectPanelTitle: '连接控制器',
    connectPanelSettingsTitle: '修改IP地址',

    /** extension.ts —— 扩展装配（命令 / 状态栏 / 输出通道 / 诊断） */
    extension: {
        outputDebug: 'Kine-X 调试',
        outputRaw: 'Kine-X 原始报文',
        activated:
            '[kine-x] 扩展已激活。菜单：控制器（连接 / 断开连接 / 本地同步 / 下载到控制器 / 修改 IP 地址）、' +
            '调试（下载并运行 / 运行 / 停止 / 实时曲线 / 控制器信息 / 设备命令）。' +
            '（「调试」下为临时集合，待定稿。）',
        engineChanged: (lang: string, scope: string): string =>
            `[kine-x] 脚本语言环境已切换为 ${lang}（写入${scope}设置 kine-x.engine）。`,
        toolbarRegistered:
            '[kine-x] 侧边栏横条「菜单栏」已注册（view: kine-x.toolbar）；' +
            '若侧边栏看不到它，可用状态栏左侧的「Kine-X」按钮作为兜底入口。',

        sshForward: (hint: string): string => `[ssh] 远程地址建议先做端口转发：${hint}`,
        sshForwardSuffix: (hint: string): string => `\n可尝试 SSH 端口转发：${hint}`,
        alreadyConnectedAt: (host: string, port: number): string => `已连接控制器 ${host}:${port}。`,

        /** 连接弹窗（FR-1.1：先输 IP，再输端口；分两步，校验不过不进下一步） */
        connectHostTitle: '连接控制器（1/2）· 控制器 IP 地址',
        connectHostPrompt: '控制器 IP 地址（默认 192.168.1.11；本机 Mock 用 127.0.0.1）',
        connectCancelled: '[连接] 已取消（未输入 IP / 端口）。',
        targetLine: (host: string, port: number): string => `${host}:${port}`,

        /** 修改IP地址 + 端口（kine-x.ip.set；v0.5.1 起含端口，表单复用 connectPanel 的设置模式） */
        setIpDone: (host: string, port: number): string => `控制器地址已更新为 ${host}:${port}。`,
        setIpFailed: (msg: string): string => `写入控制器地址失败：${msg}`,
        setIpCancelled: '[地址] 已取消（未修改 IP / 端口）。',

        /** 同步（kine-x.sync，v0.4.5 起方向 = 控制器 → 工作区） */
        syncPickTitle: '本地同步 · 选择要同步到控制器的脚本（可多选）',
        syncNoScripts: (pattern: string, engine: string): string =>
            `工作区内没有匹配 ${pattern} 的脚本文件（当前引擎 ${engine}，本地同步只上传该语言的脚本）。`,
        syncNoEngine:
            '本地同步已中止：控制器引擎未知（未取到 sys.info）且 kine-x.engine 未显式配置。' +
            '无法判断该同步 .bas 还是 .lua，请先连接控制器或显式配置 kine-x.engine。',
        syncNoD4:
            '[本地同步] 脚本运行中且控制器未声明 d4（运行中原子替换）：批量同步会中断脚本，已中止。',
        syncNoD4Toast: '脚本正在运行，且控制器未声明 d4（热更新）：请先停止脚本，再执行本地同步。',
        syncFileOk: (rel: string, labels: string): string => `[本地同步] ${rel} ✓（labels: ${labels}）`,
        syncFileFail: (rel: string, err: string): string => `[同步] ${rel} ✗ ${err}`,
        syncSummary: (ok: number, fail: number): string =>
            `[同步] 完成：成功 ${ok} 个，失败 ${fail} 个。`,
        syncSummaryClean: (ok: number): string => `[同步] 完成：成功 ${ok} 个，全部通过。`,
        syncSummaryToast: (ok: number, fail: number): string =>
            fail > 0
                ? `同步完成：成功 ${ok} 个，失败 ${fail} 个（详见「Kine-X 调试」输出）。`
                : `同步完成：成功 ${ok} 个。`,
        syncNoD6Cap: '控制器未声明 d6（文件管理）：板端程序需升级后才能列出/拉取控制器文件（下载时也不会保存）。',
        restartNoD7: '控制器未声明 d7（重启能力）：板端程序升级后才支持远程重启（DEBUG_RESTART_CMD 非空）。',
        restartConfirm: (host: string, port: number): string =>
            `确定重启控制器 ${host}:${port} 吗？\n` +
            '重启会中断正在运行的脚本、调试会话与电机任务（驱动器会先安全去使能），' +
            '并断开当前连接；重启完成后插件会自动重连。',
        restartBtn: '重启控制器',
        restartSent: '[restart] 已请求控制器重启（sys.restart），等待服务恢复…',
        restartSentToast: '控制器正在重启，等待服务恢复…',
        restartBack: (host: string, port: number): string => `控制器已重启并重新连接（${host}:${port}）。`,
        restartBackToast: (host: string, port: number): string =>
            `控制器已重启并重新连接（${host}:${port}）。`,
        restartTimeout: '控制器重启后 20 秒内未能重连：请稍后手动连接，或检查 kine-x.service 状态。',
        /** v0.7.0：调试会话交接（单客户端）与断线自动重连 */
        debugHandover: '[debug] 调试会话开始：主连接已让出（控制器单客户端），面板暂停更新；会话结束后自动连回。',
        debugBusy: '调试会话正在占用控制器连接，请先停止调试再连接（控制器单客户端，见 docs/planA/13）。',
        debugRestore: '[debug] 调试会话结束，正在连回控制器…',
        debugRestoreFail: '调试会话结束，但 15 秒内未能自动连回：请手动连接，或检查控制器状态。',
        autoReconnecting: (attempt: number, seconds: number): string =>
            `重连中(第${attempt}次，${seconds}s)`,
        autoReconnected: (host: string, port: number): string =>
            `断线后已自动重连（${host}:${port}）。`,
        connDebugOccupied: '调试占用',
        connDebugOccupiedTooltip:
            '调试会话正在使用控制器连接（控制器单客户端）；结束调试后插件会自动连回。',
        syncNoWorkspace: '当前没有打开的工作区：请先打开一个文件夹，同步的文件将写入其 controller-sync/ 目录。',
        syncEmpty: '控制器中还没有文件：在本地打开 .bas / .lua 脚本点「下载」，成功后即自动保存到控制器。',

        connecting: (host: string, port: number): string => `[连接] ${host}:${port} …`,
        connectFailedLog: (msg: string): string => `[连接失败] ${msg}`,
        connectFailed: (msg: string, extra: string): string => `连接控制器失败：${msg}${extra}`,
        notConnected: '当前未连接。',
        disconnecting: '[断开] 关闭调试通道连接。',
        linkState: (connected: boolean): string => `[状态] ${connected ? '已连接' : '已断开'}`,
        transportError: (msg: string): string => `[传输错误] ${msg}`,
        subscribeUnavailable: (reason: string): string => `[订阅] 不可用：${reason}`,
        subscribeDegrade: (reason: string): string => `[订阅] 不可用（${reason}），日志仅显示调试器消息。`,
        subscribeRequest: (hz: number): string => `[订阅] 请求 log @ ${hz}Hz（输出面板，FR-7.1）`,
        noD3: 'sys.info.caps 未声明 d3（13 D3 订阅未就绪）',
        noSubscription: '控制器未提供事件订阅',

        sysInfoLine: (ver: string, engine: string, axis: string): string =>
            `版本 ${ver} / 引擎 ${engine} / 轴数 ${axis}`,
        sysInfoCaps: (caps: string): string => ` / 能力 ${caps}`,
        capsNone: '(无)',
        controllerLine: (line: string): string => `控制器：${line}`,

        cmdTitle: '执行设备命令（经调试通道 cmd）',
        cmdPrompt: '单条设备命令，如 STA / POS / MOVEABS 55',
        cmdNoOutput: (ret: number): string => `  (ret=${ret}，无输出)`,

        openScriptFirst: '请先打开一个 .bas / .lua 脚本文件。',
        notScriptFile: '当前文件不是 Kine-X 脚本（.bas / .lua）。',
        notConnectedWithHint: '未连接控制器，请先执行「Kine-X 控制器: 连接控制器」。',

        engineCheck: (reason: string): string => `[引擎校验] ${reason}`,
        engineMismatchFallback: '引擎与文件不一致',
        engineSettingMismatch: (engine: string, cfgEngine: string): string =>
            `[引擎校验] 控制器引擎(${engine}) 与显式设置 kine-x.engine(${cfgEngine}) 不一致，请对齐后重连。`,
        engineWorkspaceMismatch: (engineLabel: string, names: string): string =>
            `[引擎校验] 控制器当前引擎为 ${engineLabel}，但工作区存在不同语言的脚本：${names}`,
        engineWorkspaceHint: '（依 engine_rule.h，同会话只允许一种引擎，请拆分为独立工程）。',

        hotswapNoD4: '[热更新] 控制器未声明 d4（运行中原子替换），无法热更新。',
        hotswapNoD4Toast: '脚本正在运行，且控制器未声明 d4（热更新）：请先停止脚本，再下载。',
        hotswapOk: (rel: string, labels: string): string =>
            `[热更新] ${rel} 已原子替换运行中脚本（未中断总线；labels: ${labels}）`,
        hotswapOkToast: '已热更新运行中脚本（未中断总线）。',
        downloadOk: (rel: string, labels: string): string => `[下载] ${rel} 编译通过（labels: ${labels}）`,

        runStatus: (status: string): string => `[运行] → ${status}`,
        stopStatus: (status: string): string => `[停止] → ${status}`,

        compileFailedLog: (rel: string, line: number, msg: string): string =>
            `[编译失败] ${rel}:${line} ${msg}`,
        compileFailedToast: (line: number, msg: string): string => `编译失败（第 ${line} 行）：${msg}`,

        scriptEvent: (status: string, steps: string): string => `[script] ${status}${steps}`,
        rawEvent: (e: string, json: string): string => `[事件 ${e}] ${json}`,
        curveNotice: (m: string): string => `[曲线] ${m}`,
        snapshotFailed: (index: number, err: string): string => `[面板] axis.snapshot(${index}) 失败：${err}`,
        panelsRefreshed: '状态面板已刷新。',

        // —— 状态栏 · 左区（品牌 + 运行概要；VS Code 状态栏只有左/右两区，无中间区）——
        /** 品牌项文案带插件版本号（v0.8.2；版本取 package.json，不硬编码） */
        statusBrand: (version: string): string => `$(chip) Kine-X v${version}`,
        statusBrandTooltip: (version: string): string =>
            `Kine-X 控制器调试上位机 v${version}（点击查看控制器信息）`,
        statusRuntimeIdle: '$(circle-outline) 无控制器信息',
        statusRuntimeEngine: (engine: string, axis: number): string =>
            `$(circuit-board) ${engine} · ${axis} 轴`,
        statusRuntimeScript: (status: string, steps: number): string =>
            steps > 0 ? `$(pulse) ${status} (${steps})` : `$(pulse) ${status}`,
        statusRuntimeTooltipIdle: '连接后取 sys.info 得到引擎与轴数（事件驱动，不轮询）。',
        statusRuntimeTooltip: (engine: string, axis: number, status: string, steps: number): string =>
            `引擎：${engine}\n轴数：${axis}\n脚本状态：${status}${steps > 0 ? `（步数 ${steps}）` : ''}`,

        // —— 状态栏 · 右区（连接状态，固定最右）——
        connDisconnected: '$(plug) 未连接',
        connConnecting: (target: string): string => `$(sync~spin) 正在连接 ${target}`,
        connConnected: (target: string): string => `$(vm-active) 已连接 ${target}`,
        connTooltipDisconnected: '点击输入 IP 与端口，连接控制器（Kine-X 控制器: 连接控制器）',
        connTooltipConnecting: (target: string): string => `正在连接调试通道 ${target} …`,
        connTooltipConnected: (target: string, scriptStatus: string): string =>
            `已连接调试通道 ${target}` +
            (scriptStatus ? `\n脚本状态：${scriptStatus}` : '') +
            '\n点击查看控制器信息',

        errorLog: (method: string, msg: string): string => `[错误] ${method} 失败：${msg}`,
        errorToast: (method: string, msg: string): string => `${method} 失败：${msg}`,
    },

    /** completion.ts —— FR-2.5「命令名补全」装配（诊断 + 候选文档） */
    completion: {
        dataUnavailable: '[kine-x] 命令补全数据不可用，已跳过命令名补全（FR-2.5）。',
        loadFailed: (msg: string): string => `[kine-x] 读取命令表失败：${msg}`,
        docSuffix: 'Kine-X 设备命令（`.bas` / `.lua` 同名同义）。',
        /** Lua 专属 API（不在设备命令表内，仅 kx-lua 补全） */
        wwCallDetail: 'Lua API · 通用转调',
        wwCallDoc: '**ww.call(name, …)** — 按字符串调用任意设备命令（支持动态命令名）；返回命令返回值。\n\n等价 BASIC：直接写命令名。',
        wwRetDetail: 'Lua API · 最近命令返回值',
        wwRetDoc: '**ww.ret()** — 读最近一次命令的返回值（与 BASIC 的 `RETURN` 系统变量同源同义）。',
    },

    /** debugAdapter.ts（DAP：会话骨架 / 变量 / 终止映射 / 断点） */
    debugAdapter: {
        notSupported: (command: string): string =>
            `[NOT_SUPPORTED] 暂不支持 ${command}（docs/planA/15 §4.4 降级）`,
        transportError: (msg: string): string => `[传输错误] ${msg}`,
        channelClosed: '调试通道已断开。',
        capsUndeclared: '未声明',
        connectedLine: (
            host: string,
            port: number,
            ver: string,
            engine: string,
            axis: string,
            caps: string,
        ): string =>
            `已连接 ${host}:${port}｜版本 ${ver}｜引擎 ${engine}｜轴数 ${axis}｜能力 ${caps}`,
        noD5: '控制器未声明 D5（引擎 pause 钩子）：断点/单步/暂停将明确报「不支持」，不伪装。',
        noD4: '控制器未声明 D4（运行中原子替换）：脚本运行中下载会提示「先停止」，不做热更新。',
        programUnresolved: '(未指定，且无活动脚本编辑器)',
        engineInferred: '(按扩展名推断)',
        launchArgs: (
            program: string,
            engine: string,
            stopOnEntry: string,
            waitBus: string,
            maxSteps: string,
            trace: string,
        ): string =>
            `launch 参数：program=${program} engine=${engine} stopOnEntry=${stopOnEntry} ` +
            `waitBus=${waitBus} maxSteps=${maxSteps} trace=${trace}` +
            '（后三项对应 SCRIPT_WAIT_BUS / SCRIPT_MAX_STEPS / SCRIPT_TRACE）',
        connectFailed: (msg: string): string => `连接调试通道失败：${msg}`,

        entryPauseDetail: '入口暂停（脚本尚未运行）',
        entryPauseLabel: '入口暂停',
        entryPauseOutput: (d5: boolean): string =>
            '入口暂停：可查看/修改变量（变量视图），再按「继续」启动脚本。' +
            (d5
                ? '已登记的行断点将在命中原生挂起（D5 已就绪）。'
                : '断点/单步需控制器 D5，当前不可用。'),

        channelNotConnected: '调试通道未连接。',
        channelNotConnectedBare: '调试通道未连接',
        resumeFailed: (msg: string): string => `恢复失败：${msg}`,
        endedCannotResume: (status: string, d5: boolean): string =>
            `[NOT_SUPPORTED] 脚本已结束（${status}），无法恢复；` +
            (d5
                ? '请重新启动调试会话（重启后断点仍有效）。'
                : '请重新启动调试会话（断点/单步/恢复需控制器 D5）。'),
        stepOnlyPaused: '[NOT_SUPPORTED] 仅能在「挂起」状态单步；当前脚本未挂起。',
        stepFailed: (msg: string): string => `单步失败：${msg}`,
        pauseOnlyRunning: '[NOT_SUPPORTED] 仅能在脚本运行时暂停；当前未运行。',
        pauseFailed: (msg: string): string => `暂停失败：${msg}`,

        noProgram:
            '未指定 program，且当前编辑器不是 .bas/.lua 脚本；请在 launch.json 配置 program 或打开脚本文件',
        engineArgConflict: (explicit: string, inferred: string): string =>
            `launch.engine=${explicit} 与扩展名推断(${inferred}) 冲突（D-03：扩展名为主）`,
        engineControllerMismatch: (controller: string, script: string): string =>
            `控制器引擎(${controller}) 与脚本(${script}) 不一致；` +
            '同一时刻只允许一种引擎（engine_rule.h），请换用对应语言的脚本文件',
        readScriptFailed: (msg: string): string => `读取脚本失败：${msg}`,

        stackUnavailable: '[NOT_SUPPORTED] 脚本运行中，调用栈不可用（需控制器 D5 的 pause 钩子）。',
        scriptFallback: '脚本',
        scopeLocals: '脚本变量（引擎 get_var / list_vars）',
        listVarsFailed: (msg: string): string => `读取变量列表失败：${msg}`,
        setVarScopeOnly: '仅支持写入「脚本变量」作用域中的具名变量。',
        setVarFailed: (msg: string): string => `写入变量失败：${msg}`,
        varAssigned: (name: string, value: string): string => `[变量] ${name} = ${value}`,
        emptyExpression: '表达式为空。',
        getVarFailed: (name: string, msg: string): string => `读取变量 ${name} 失败：${msg}`,
        exprNotSupported:
            '当前调试口只支持求值「变量名」或「变量名 = 值」；复合表达式需控制器侧实现（docs/planA/13 D5）。',
        cmdFailed: (msg: string): string => `执行命令失败：${msg}`,

        bpUnverifiedReason: '需控制器引擎 pause 钩子（docs/planA/13 D5），当前仅登记不生效',
        bpRegistered: (count: number, reason: string): string =>
            `[断点] 已登记 ${count} 个断点，但${reason}。`,
        bpSetFailed: (msg: string): string => `设置断点失败：${msg}`,
        bpRegisteredReady: (count: number): string =>
            `[断点] 已在控制器登记 ${count} 个行断点（D5 已就绪）。`,

        unknownEvent: (e: string): string => `[事件 ${e}]`,
        scriptStatus: (status: string, steps: string, line: string): string =>
            `[script] ${status}${steps}${line}`,
        bpHit: (line: number): string => `断点命中（第 ${line} 行）`,
        pausedAt: (line: number): string => `已暂停（第 ${line} 行）`,
        debuggerMsg: (detail: string): string => `[调试器] ${detail}`,
        debuggerMsgLine: (detail: string, line: number): string => `[调试器] ${detail}（第 ${line} 行）`,
        atLine: (line: number): string => `第 ${line} 行`,
        stopFailed: (msg: string): string => `[停止] 失败：${msg}`,
        threadName: (engine: string): string => `脚本引擎（${engine}）`,
        threadNameFallback: '脚本引擎',
    },

    /**
     * config.ts —— 连接目标（IP / 端口）就地校验文案。
     * 校验逻辑是纯函数，但**文案必须集中在此**（NFR-8：除本文件外源码不得出现界面文案）。
     */
    configCheck: {
        hostEmpty: '地址不能为空',
        hostIpv4Range: 'IPv4 每段必须是 0~255，且不允许前导零',
        hostNotHost: '既不是 IPv4 字面量，也不是合法主机名',
        portNotNumber: '端口必须是纯数字',
        portRange: (min: number, max: number): string => `端口必须在 ${min}~${max}`,
        hostReject: (reason: string): string => `IP 地址非法：${reason}`,
        portReject: (reason: string): string => `端口非法：${reason}`,
    },
} as const;

export type Catalog = typeof zh;

export type LocaleId = 'zh-CN';

export const DEFAULT_LOCALE: LocaleId = 'zh-CN';

/** 已登记的语言目录（预留：新增语言在此追加） */
export const CATALOGS: Record<LocaleId, Catalog> = {
    'zh-CN': zh,
};

/**
 * 当前文案目录。以 `let` 导出 → 编译为按属性访问的实时绑定，
 * 故 `setLocale()` 切换后，已 `import { S }` 的模块会立即取到新目录。
 */
export let S: Catalog = CATALOGS[DEFAULT_LOCALE];

/** 预留：切换界面语言（首版仅 zh-CN；新增语言后在 activate() 中调用） */
export function setLocale(id: LocaleId): void {
    S = CATALOGS[id];
}
