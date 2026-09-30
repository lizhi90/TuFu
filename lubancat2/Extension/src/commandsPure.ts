// commandsPure.ts —— 设备命令数据与「命令名补全 / Hover / 签名帮助」纯逻辑
//                  （不依赖 vscode，可 Node 单测）
//
// 对应 docs/planA/15：
//   FR-2.5 命令名补全（P1）：`.bas` / `.lua` 内补全设备命令名，且「与两引擎名称一致（同名同义）」；
//   NFR-7 单一来源：命令名与**签名/说明**只来自 `data/commands.json`
//          （由 `tools/gen-commands.mjs` 从控制器 `src/script/command_table.h` 的
//          `kNames[]` / `kDocs[]` 生成）。本文件只做解析 / 分组 / 排序 / 拼装，绝不新增改写命令。
//
// 设计：本模块保持纯函数（无 vscode、无 fs）；文件读取与 Provider 组装在 completion.ts。

import { S } from './strings';

/** 未登记到任何分组的命令一律归入此组（顺序排在已知分组之后） */
export const UNKNOWN_GROUP_LABEL: string = S.commandGroup.other;

/** 旧数据（仅有 groups 映射）的兜底标签：key → 中文 */
export const COMMAND_GROUP_LABELS: Readonly<Record<string, string>> = S.commandGroup;

/** 旧数据的分组展示顺序兜底；新数据按 command_table.h 分节顺序展示 */
export const COMMAND_GROUP_ORDER: readonly string[] = [
    'motion',
    'axis',
    'status',
    'bus',
    'port',
    'misc',
];

/** 命令文档（签名 + 一句话说明；来自控制器 kDocs） */
export interface CommandDoc {
    sig: string;
    brief: string;
}

/** 命令分组（label 为展示名，key 对数据驱动分组与 label 相同） */
export interface CommandGroupEntry {
    key: string;
    label: string;
    names: string[];
}

/** 解析后的命令表（结构见 data/commands.json） */
export interface CommandTable {
    /** 全部命令名（去重，保持数据源顺序） */
    names: string[];
    /** 分组列表（保持 command_table.h 分节顺序） */
    groupList: CommandGroupEntry[];
    /** 命令名（大写）→ 文档 */
    docs: Record<string, CommandDoc>;
    /** 兼容派生：分组 key → 命令名（旧代码/旧数据可用） */
    groups: Record<string, string[]>;
}

/**
 * 补全候选（与 vscode 类型解耦，便于 Node 单测）。
 *
 * `call` 区分两引擎的调用形态：BASIC 直接写 `NAME`（参数以空格分隔），
 * Lua 则是函数调用 `NAME(...)`——调用侧据此决定是否插入带光标占位的片段。
 */
export interface CommandCompletion {
    /** 补全列表显示名 = 命令名 */
    label: string;
    /** 纯命令名（无参数、无括号） */
    name: string;
    /** Lua 为 true：调用侧应插入 `NAME(${1:…})` 形态的可编辑片段 */
    call: boolean;
    /** 分组中文名（有文档时改为签名），作为 CompletionItem.detail 兜底 */
    detail: string;
    /** 排序键：`<组序>_<常用度>_<命令名>`，同组内常用命令排前 */
    sortText: string;
    /** 命令文档（有则补全/Hover/签名帮助共用） */
    doc?: CommandDoc;
}

/** 常用命令（补全排序靠前；仅影响排序，不影响数据来源） */
const POPULAR: readonly string[] = [
    'MOVEABS', 'MOVE', 'EN', 'STOP', 'POS', 'MPOS', 'WAIT', 'DIS', 'BASE', 'JOG', 'WAITIDLE', 'SPEED',
];

function popularRank(name: string): string {
    const i = POPULAR.indexOf(name.toUpperCase());
    return i < 0 ? '9' : String(i + 1).padStart(2, '0');
}

/** 宽松解析 data/commands.json：容忍缺字段/脏数据，绝不抛异常（NFR-6 不阻塞扩展） */
export function parseCommandTable(raw: unknown): CommandTable {
    const obj = (typeof raw === 'object' && raw !== null ? raw : {}) as {
        names?: unknown;
        groupList?: unknown;
        groups?: unknown;
        docs?: unknown;
    };

    const names: string[] = [];
    if (Array.isArray(obj.names)) {
        for (const n of obj.names) {
            if (typeof n === 'string' && n.length > 0 && !names.includes(n)) {
                names.push(n);
            }
        }
    }

    // 文档
    const docs: Record<string, CommandDoc> = {};
    if (typeof obj.docs === 'object' && obj.docs !== null) {
        for (const [k, v] of Object.entries(obj.docs as Record<string, unknown>)) {
            const d = v as { sig?: unknown; brief?: unknown };
            const sig = typeof d?.sig === 'string' ? d.sig : '';
            const brief = typeof d?.brief === 'string' ? d.brief : '';
            if (sig || brief) {
                docs[k.toUpperCase()] = { sig, brief };
            }
        }
    }

    // 分组：优先新结构 groupList，其次旧 groups 映射
    const groupList: CommandGroupEntry[] = [];
    if (Array.isArray(obj.groupList)) {
        for (const g of obj.groupList as Array<{ key?: unknown; label?: unknown; names?: unknown }>) {
            const label = typeof g?.label === 'string' ? g.label : typeof g?.key === 'string' ? g.key : '';
            const list = Array.isArray(g?.names)
                ? (g.names as unknown[]).filter((x): x is string => typeof x === 'string' && x.length > 0)
                : [];
            if (label && list.length > 0) {
                groupList.push({ key: label, label, names: list });
            }
        }
    } else if (typeof obj.groups === 'object' && obj.groups !== null) {
        for (const [g, list] of Object.entries(obj.groups as Record<string, unknown>)) {
            if (!Array.isArray(list)) {
                continue;
            }
            const cleaned = list.filter((x): x is string => typeof x === 'string' && x.length > 0);
            if (cleaned.length > 0) {
                groupList.push({ key: g, label: COMMAND_GROUP_LABELS[g] ?? g, names: cleaned });
            }
        }
    }

    const groups: Record<string, string[]> = {};
    for (const g of groupList) {
        groups[g.key] = g.names.slice();
    }

    return { names, groupList, docs, groups };
}

/** 命令所属分组 key（未登记返回 undefined） */
export function commandGroupOf(table: CommandTable, name: string): string | undefined {
    for (const g of table.groupList) {
        if (g.names.includes(name)) {
            return g.key;
        }
    }
    return undefined;
}

/**
 * 分组展示名：
 *   * 数据驱动分组的 key 即展示标签（中文含空格）→ 原样返回；
 *   * 旧数据 key（motion/axis/…）→ strings.ts 映射；
 *   * 未知/空 → 「其他」。
 */
export function commandGroupLabel(group: string | undefined): string {
    if (!group) {
        return UNKNOWN_GROUP_LABEL;
    }
    const mapped = COMMAND_GROUP_LABELS[group];
    if (mapped) {
        return mapped;
    }
    return /^[a-z_]+$/.test(group) ? UNKNOWN_GROUP_LABEL : group;
}

/** 查命令文档：大小写不敏感；未登记返回 undefined */
export function signatureOf(table: CommandTable, name: string): CommandDoc | undefined {
    return table.docs[name.toUpperCase()];
}

/**
 * 由签名生成补全片段：`MOVEABS(p[, spd[, acc[, wait]]])` →
 * `MOVEABS(${1:p}, ${2:spd}, ${3:acc}, ${4:wait})`（无参数则返回 `NAME()`）。
 * 解析失败返回 undefined（调用侧退回纯命令名插入）。
 */
export function snippetFor(name: string, sig: string | undefined): string | undefined {
    if (!sig) {
        return undefined;
    }
    const open = sig.indexOf('(');
    const close = sig.lastIndexOf(')');
    if (open < 0 || close < open) {
        return undefined;
    }
    const inner = sig.slice(open + 1, close).trim();
    if (inner === '' || inner === '…' || inner.startsWith('…') || inner === '...') {
        return `${name}()$0`;
    }
    const parts = splitTopLevel(inner);
    const holes = parts.map((p, i) => `\${${i + 1}:${p}}`);
    return `${name}(${holes.join(', ')})$0`;
}

/**
 * 按逗号切分签名参数：只把括号 `()` 视为分组（`[]` 只是「可选参数」记号，透明处理），
 * 这样 `p[, spd[, acc[, wait]]]` → `["p","spd","acc","wait"]`（4 个参数）。
 * 同时剥掉参数文本里的方括号。
 */
export function splitTopLevel(text: string): string[] {
    const out: string[] = [];
    let depth = 0;
    let cur = '';
    for (const ch of text) {
        if (ch === ',' && depth === 0) {
            out.push(cur);
            cur = '';
            continue;
        }
        if (ch === '(') depth += 1;
        if (ch === ')') depth = Math.max(0, depth - 1);
        cur += ch;
    }
    out.push(cur);
    return out
        .map((p) => p.replace(/[[\]]/g, '').trim())
        .filter((p) => p.length > 0);
}

/** 生成 Hover 内容（Markdown）；命令未登记返回 undefined */
export function hoverMarkdown(table: CommandTable, name: string): string | undefined {
    const upper = name.toUpperCase();
    const doc = table.docs[upper];
    if (!doc) {
        return undefined;
    }
    const group = commandGroupLabel(commandGroupOf(table, upper));
    const lines = [`**${upper}** — ${doc.brief || '（无说明）'}`, ''];
    if (doc.sig) {
        lines.push('```', doc.sig, '```', '');
    }
    lines.push(`分组：${group}｜控制器命令（BASIC 见 docs/planA/10，Lua 见 docs/planA/11）`);
    return lines.join('\n');
}

/** 光标前文本 → 当前调用中的命令与文档（签名帮助用） */
export function signatureHelpFor(
    table: CommandTable,
    textBefore: string,
): { name: string; doc: CommandDoc } | undefined {
    let depth = 0;
    for (let i = textBefore.length - 1; i >= 0; i--) {
        const ch = textBefore[i];
        if (ch === ')') {
            depth += 1;
        } else if (ch === '(') {
            if (depth === 0) {
                let j = i - 1;
                while (j >= 0 && /[A-Za-z0-9_.]/.test(textBefore[j])) {
                    j -= 1;
                }
                const raw = textBefore.slice(j + 1, i);
                const upper = raw.toUpperCase();
                const doc = table.docs[upper];
                return doc ? { name: upper, doc } : undefined;
            }
            depth -= 1;
        }
    }
    return undefined;
}

/** 当前实参序号（顶层逗号计数，从 0 开始；超过参数个数则夹到最后一个） */
export function activeParamIndex(textBefore: string): number {
    let depth = 0;
    let commas = 0;
    for (let i = textBefore.length - 1; i >= 0; i--) {
        const ch = textBefore[i];
        if (ch === ')') {
            depth += 1;
        } else if (ch === '(') {
            if (depth === 0) {
                break;
            }
            depth -= 1;
        } else if (ch === ',' && depth === 0) {
            commas += 1;
        }
    }
    return commas;
}

/**
 * 生成补全候选：按分组顺序展开，同组内保持数据源顺序；
 * 未登记分组的命令追加到末尾，确保**表内每条命令都能补全**（不漏）。
 *
 * @param languageId 脚本语言 id（`kx-lua` 生成函数调用片段，其余按 BASIC 直写命令名）
 */
export function buildCommandCompletions(table: CommandTable, languageId: string): CommandCompletion[] {
    const isLua = languageId === 'kx-lua';
    const out: CommandCompletion[] = [];
    const seen = new Set<string>();
    const push = (name: string, group: string | undefined, gi: number): void => {
        if (seen.has(name)) {
            return;
        }
        seen.add(name);
        const doc = signatureOf(table, name);
        out.push({
            label: name,
            name,
            call: isLua,
            detail: doc?.sig ? doc.sig : commandGroupLabel(group),
            sortText: `${String(gi).padStart(2, '0')}_${popularRank(name)}_${name}`,
            doc,
        });
    };

    for (let gi = 0; gi < table.groupList.length; gi++) {
        const g = table.groupList[gi];
        for (const name of g.names) {
            push(name, g.key, gi);
        }
    }
    // 兜底：数据源 names 里存在、但未登记进任何分组的命令也要能补全
    for (const name of table.names) {
        push(name, undefined, table.groupList.length);
    }

    return out;
}
