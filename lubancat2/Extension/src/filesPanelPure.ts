// filesPanelPure.ts —— 「控制器文件」列表的纯逻辑（v0.4.9 起为侧边栏原生 TreeView）
//
// 用户拍板（v0.4.9）：控制器文件用**原生 List/TreeView** 展示（侧边栏独立视图 `kine-x.files`），
// 不再画在控制台 Webview 里——原生列表自带键盘导航、主题、行内按钮与右键菜单。
// 数据来源：调试通道 D6（file.list / file.get / file.del，见 docs/planA/13）——
// 控制器把「下载时带 name 的脚本」落盘到 DEBUG_SCRIPT_DIR，目录内容同时决定脚本语言绑定。
//
// 本文件不 import vscode：语言判定、行模型、图标规格、占位文案都是纯函数，可 Node 单测；
// vscode 适配层在 filesTree.ts。
//
// 纪律不变：缺 D6 → 明示需升级板端，不伪装出空列表冒充「控制器没有文件」。

import { S } from './strings';

/** 文件语言（按扩展名判定） */
export type FileLang = 'basic' | 'lua' | 'other';

/**
 * 控制器文件 ↔ 本地 `controller-sync/` 副本的一致性（v0.8.4）：
 * same=哈希相同 / diff=哈希不同 / missing=本地无副本 / unknown=无法比较（无工作区、读失败或旧固件无 hash）。
 */
export type FileSyncState = 'same' | 'diff' | 'missing' | 'unknown';

/** 文件行（size 字节；lang/langLabel 由扩展名判定；hash/sync = 一致性标识用，可缺省） */
export interface ControllerFileRow {
    name: string;
    size: number;
    lang: FileLang;
    langLabel: string;
    hash?: string;
    sync?: FileSyncState;
}

/** file.list 拉取后的缓存（extension.ts 持有；本模块只做纯转换） */
export interface ControllerFilesCache {
    dir: string;
    files: Array<{ name: string; size: number; hash?: string; sync?: FileSyncState }>;
    /** 列表拉取失败原因，空串 = 正常 */
    error: string;
}

/** 主文件（开机运行）清单状态（D8 boot.get；未连接/无 d8 时 name 为空） */
export interface FilesBootState {
    name: string;
    valid: boolean;
    reason: string;
}

/** TreeView 渲染输入 */
export interface FilesTreeState {
    connected: boolean;
    hasD6: boolean;
    /** D8：主文件（开机运行）能力（旧固件无 → 不显示主文件交互，降级不伪装） */
    hasD8: boolean;
    boot: FilesBootState;
    cache: ControllerFilesCache;
}

/** 列表行：占位提示 或 文件行 */
export interface FilesTreeRow {
    kind: 'placeholder' | 'file';
    id: string;
    label: string;
    description?: string;
    tooltip?: string;
    /** kind=file：文件名（命令参数用） */
    name?: string;
    lang?: FileLang;
    size?: number;
    /** kind=file：当前主文件（开机运行） */
    main?: boolean;
    /** kind=file：控制器与本地副本一致性（v0.8.4；undefined=未比对，不显示标识） */
    sync?: FileSyncState;
}

/** 扩展名 → 语言（.bas=basic / .lua=lua / 其他） */
export function langOfName(name: string): FileLang {
    const n = name.toLowerCase();
    if (n.endsWith('.bas')) return 'basic';
    if (n.endsWith('.lua')) return 'lua';
    return 'other';
}

/** file.list 条目 → 渲染行（纯函数，可单测）；文案取自 strings.ts（NFR-8） */
export function toControllerFileRow(
    name: string,
    size: number,
    hash?: string,
    sync?: FileSyncState,
): ControllerFileRow {
    const lang = langOfName(name);
    const langLabel =
        lang === 'basic' ? S.filesPanel.langBasic : lang === 'lua' ? S.filesPanel.langLua : S.filesPanel.langOther;
    return { name, size, lang, langLabel, hash, sync };
}

/** 字节数 → 列表右对齐描述（B / KB；非法/负数按 0） */
export function formatSize(bytes: number): string {
    const n = Number.isFinite(bytes) && bytes > 0 ? Math.trunc(bytes) : 0;
    return n < 1024 ? `${n} B` : `${(n / 1024).toFixed(1)} KB`;
}

/**
 * FNV-1a 64 位哈希（小写十六进制 16 字符）——**必须与控制器 `file.list` 的 `hash` 同算法**
 * （`src/script/debug_server.cpp` 的 `fnv1a64_hex`：初值 0xcbf29ce484222325、质数 0x100000001b3）。
 * 已知向量：'' → cbf29ce484222325，'a' → af63dc4c8601ec8c（smoke 锁定，防两边漂移）。
 */
export function fnv1a64Hex(bytes: Uint8Array): string {
    const PRIME = 0x100000001b3n;
    const MASK = 0xffffffffffffffffn;
    let h = 0xcbf29ce484222325n;
    for (let i = 0; i < bytes.length; i++) {
        h ^= BigInt(bytes[i]);
        h = (h * PRIME) & MASK;
    }
    return h.toString(16).padStart(16, '0');
}

/**
 * 远端/本地哈希 → 一致性状态（纯函数）：
 * 远端无哈希（旧固件）或本地读取失败 → unknown；本地文件不存在（null）→ missing；相等 → same。
 */
export function compareFileHash(remote?: string, local?: string | null): FileSyncState {
    if (!remote) return 'unknown';
    if (local === undefined) return 'unknown';
    if (local === null) return 'missing';
    return remote === local ? 'same' : 'diff';
}

/** 一致性状态 → 行内标识文案（集中在 strings.ts，NFR-8） */
export function syncLabel(state: FileSyncState): string {
    switch (state) {
        case 'same':
            return S.filesPanel.syncSame;
        case 'diff':
            return S.filesPanel.syncDiff;
        case 'missing':
            return S.filesPanel.syncMissing;
        default:
            return S.filesPanel.syncUnknown;
    }
}

/** 语言 → 原生 ThemeIcon 规格（无 vscode 依赖，便于单测） */
export interface FileIconSpec {
    id: string;
    colorId?: string;
}

export function fileIconSpec(lang: FileLang): FileIconSpec {
    switch (lang) {
        case 'basic':
            return { id: 'file-code', colorId: 'charts.green' };
        case 'lua':
            return { id: 'file-code', colorId: 'charts.blue' };
        default:
            return { id: 'file' };
    }
}

/**
 * 状态 → 列表行（纯函数）：
 *   未连接 / 缺 d6 / 拉取失败 / 空目录 → 单行占位（明示原因，不伪装空列表）；
 *   主文件清单失效（引用已删/语言冲突）→ 顶部一条占位明确原因（不静默）；
 *   有文件 → 每个文件一行（语言 + 大小在 description；主文件带 star 标记与指定说明）。
 */
export function buildFilesTreeRows(state: FilesTreeState): FilesTreeRow[] {
    if (!state.connected) {
        return [{ kind: 'placeholder', id: 'ph:offline', label: S.filesPanel.notConnected }];
    }
    if (!state.hasD6) {
        return [{ kind: 'placeholder', id: 'ph:nod6', label: S.filesPanel.noD6 }];
    }
    if (state.cache.error) {
        return [{ kind: 'placeholder', id: 'ph:error', label: S.filesPanel.listError(state.cache.error) }];
    }

    const rows = state.cache.files.map((f) => toControllerFileRow(f.name, f.size, f.hash, f.sync));
    if (rows.length === 0) {
        return [{ kind: 'placeholder', id: 'ph:empty', label: S.filesPanel.empty }];
    }

    const out: FilesTreeRow[] = [];
    if (state.hasD8 && state.boot.name.length > 0 && !state.boot.valid) {
        out.push({
            kind: 'placeholder',
            id: 'ph:boot',
            label: S.filesPanel.bootInvalid(state.boot.name, state.boot.reason),
        });
    }

    for (const r of rows) {
        const sizeText = formatSize(r.size);
        const main = state.hasD8 && state.boot.valid && state.boot.name === r.name;
        const sync = r.sync;
        const syncSuffix = sync ? ` · ${syncLabel(sync)}` : '';
        out.push({
            kind: 'file',
            id: `file:${r.name}`,
            label: r.name,
            name: r.name,
            lang: r.lang,
            size: r.size,
            main,
            sync,
            description:
                (main
                    ? `${S.filesPanel.mainTag} · ${r.langLabel} · ${sizeText}`
                    : `${r.langLabel} · ${sizeText}`) + syncSuffix,
            tooltip:
                (main
                    ? S.filesPanel.mainTooltip(r.name, r.langLabel, sizeText)
                    : S.filesPanel.fileTooltip(r.name, r.langLabel, sizeText)) +
                (sync ? `\n${S.filesPanel.syncHint(sync)}` : ''),
        });
    }
    return out;
}
