// completion.ts —— FR-2.5「命令名补全」的 vscode 侧装配
//
// 职责：读取 data/commands.json → 交给 commandsPure 生成候选 → 注册 CompletionItemProvider。
// 数据缺失/损坏时**不阻塞扩展**（NFR-6），只写一条告警。

import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import {
    activeParamIndex,
    buildCommandCompletions,
    CommandCompletion,
    CommandTable,
    hoverMarkdown,
    parseCommandTable,
    signatureHelpFor,
    signatureOf,
    snippetFor,
    splitTopLevel,
} from './commandsPure';
import { S } from './strings';

/** 参与补全的脚本语言 id（与 package.json contributes.languages 一致） */
export const SCRIPT_LANGUAGE_IDS: readonly string[] = ['kx-basic', 'kx-lua'];

/** 命令数据在扩展目录下的相对路径（随 VSIX 分发；NFR-7 单一来源） */
const COMMAND_DATA_RELPATH = ['data', 'commands.json'];

/**
 * 注册命令名补全。返回 Disposable（已并入 context.subscriptions）；
 * 数据不可用时返回 undefined（补全缺席，其余功能照常）。
 */
export function registerCompletion(
    context: vscode.ExtensionContext
): vscode.Disposable | undefined {
    const table = loadCommandTable(context.extensionPath);
    if (!table || table.names.length === 0) {
        console.warn(S.completion.dataUnavailable);
        return undefined;
    }

    const provider: vscode.CompletionItemProvider = {
        provideCompletionItems(document) {
            const specs = buildCommandCompletions(table, document.languageId);
            const items = specs.map(toCompletionItem);
            // Lua 专属 API（docs/planA/11）：ww.call / ww.ret 不在设备命令表内，这里手工补齐
            if (document.languageId === 'kx-lua') {
                items.push(wwCallItem(), wwRetItem());
            }
            return items;
        },
    };

    /** `ww.call(name, …)`：按字符串调用任意设备命令（Lua 逃生舱 / 动态命令名） */
    function wwCallItem(): vscode.CompletionItem {
        const item = new vscode.CompletionItem('ww.call', vscode.CompletionItemKind.Function);
        item.detail = S.completion.wwCallDetail;
        item.insertText = new vscode.SnippetString('ww.call("${1:POS}"$0)');
        item.documentation = new vscode.MarkdownString(
            S.completion.wwCallDoc + '\n\n示例：`local p = ww.call("POS", 0)`',
        );
        return item;
    }

    /** `ww.ret()`：读最近一次命令返回值（与 BASIC `RETURN` 同源） */
    function wwRetItem(): vscode.CompletionItem {
        const item = new vscode.CompletionItem('ww.ret', vscode.CompletionItemKind.Function);
        item.detail = S.completion.wwRetDetail;
        item.insertText = new vscode.SnippetString('ww.ret()');
        item.documentation = new vscode.MarkdownString(
            S.completion.wwRetDoc + '\n\n示例：`if ww.ret() ~= 0 then … end`',
        );
        return item;
    }

    /** ww.call / ww.ret 的悬停文档（非控制器命令，插件侧补齐，见 docs/planA/11） */
    function wwDoc(name: 'ww.call' | 'ww.ret'): vscode.MarkdownString {
        const md = new vscode.MarkdownString();
        if (name === 'ww.call') {
            md.appendCodeblock('ww.call(命令名 [, 参数…])');
            md.appendText(S.completion.wwCallDetail + '。');
            md.appendMarkdown('\n\n' + S.completion.wwCallDoc);
        } else {
            md.appendCodeblock('ww.ret()');
            md.appendText(S.completion.wwRetDetail + '。');
            md.appendMarkdown('\n\n' + S.completion.wwRetDoc);
        }
        return md;
    }

    const disposables: vscode.Disposable[] = [
        vscode.languages.registerCompletionItemProvider(SCRIPT_LANGUAGE_IDS, provider),
        // 悬停：光标停在命令名上 → 签名 + 一句话说明（数据来自 kDocs；ww.call/ww.ret 特判）
        vscode.languages.registerHoverProvider(SCRIPT_LANGUAGE_IDS, {
            provideHover(document, position) {
                const range = document.getWordRangeAtPosition(position, /[A-Za-z_]\w*/);
                if (!range) {
                    return undefined;
                }
                let name = document.getText(range);
                if (/^(call|ret)$/i.test(name)) {
                    const prefix = document.getText(
                        new vscode.Range(
                            position.with({ character: Math.max(0, range.start.character - 3) }),
                            position,
                        ),
                    );
                    if (/ww\.$/i.test(prefix)) {
                        return new vscode.Hover(wwDoc(name.toLowerCase() === 'call' ? 'ww.call' : 'ww.ret'), range);
                    }
                }
                const md = hoverMarkdown(table, name);
                return md ? new vscode.Hover(new vscode.MarkdownString(md), range) : undefined;
            },
        }),
        // 签名帮助：光标在命令括号内（输入 `(`、`,` 触发）→ 签名 + 参数占位 + 当前列高亮
        vscode.languages.registerSignatureHelpProvider(
            SCRIPT_LANGUAGE_IDS,
            {
                provideSignatureHelp(document, position) {
                    const before = document.getText(
                        new vscode.Range(new vscode.Position(0, 0), position),
                    );
                    // 先试 Lua 转调形态 `ww.call("NAME`，再试普通 `NAME(`
                    const ww = /ww\.call\(\s*"?([A-Za-z_]\w*)"?\s*,?[^()]*$/.exec(before);
                    let doc = ww?.[1] ? signatureOf(table, ww[1]) : undefined;
                    if (!doc) {
                        doc = signatureHelpFor(table, before)?.doc;
                    }
                    if (!doc || !doc.sig) {
                        return undefined;
                    }
                    const help = new vscode.SignatureHelp();
                    const si = new vscode.SignatureInformation(doc.sig, new vscode.MarkdownString(doc.brief));
                    const params = splitTopLevel(doc.sig.slice(doc.sig.indexOf('(') + 1, doc.sig.lastIndexOf(')')));
                    si.parameters = params.map((p) => new vscode.ParameterInformation(p));
                    help.signatures = [si];
                    help.activeSignature = 0;
                    const n = params.length;
                    const idx = activeParamIndex(before);
                    help.activeParameter = n === 0 ? 0 : Math.min(idx, n - 1);
                    return help;
                },
            },
            '(',
            ',',
        ),
    ];
    return vscode.Disposable.from(...disposables);
}

/** 从磁盘加载命令表；任何异常都吞掉并返回 undefined（NFR-6） */
function loadCommandTable(extensionPath: string): CommandTable | undefined {
    try {
        const file = path.join(extensionPath, ...COMMAND_DATA_RELPATH);
        const raw = JSON.parse(fs.readFileSync(file, 'utf8')) as unknown;
        return parseCommandTable(raw);
    } catch (err) {
        console.warn(S.completion.loadFailed((err as Error).message));
        return undefined;
    }
}

/** 候选 → CompletionItem：BASIC 直插命令名，Lua 按签名插入带参数占位的可编辑片段 */
function toCompletionItem(spec: CommandCompletion): vscode.CompletionItem {
    const item = new vscode.CompletionItem(spec.label, vscode.CompletionItemKind.Function);
    // 有文档的命令：detail 显示签名（比分组名更实用）；文档带说明
    item.detail = spec.doc?.sig ? spec.doc.sig : spec.detail;
    item.filterText = spec.label;
    item.sortText = spec.sortText;
    const snippet = spec.call ? snippetFor(spec.name, spec.doc?.sig) : undefined;
    item.insertText = spec.call
        ? new vscode.SnippetString(snippet ?? `${spec.name}($0)`)
        : spec.name;
    const md = new vscode.MarkdownString();
    if (spec.doc) {
        if (spec.doc.sig) {
            md.appendCodeblock(spec.doc.sig);
        }
        md.appendText((spec.doc.brief || spec.detail) + '。');
        md.appendMarkdown('\n\n' + S.completion.docSuffix);
    } else {
        md.appendMarkdown(`**${spec.label}** · ${spec.detail}\n\n` + S.completion.docSuffix);
    }
    item.documentation = md;
    return item;
}
