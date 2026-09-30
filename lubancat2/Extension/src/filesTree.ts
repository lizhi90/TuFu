// filesTree.ts —— 「控制器文件」原生 TreeView 适配层（v0.4.9）
//
// 形态：侧边栏 `kine-x` 容器内的独立视图 `kine-x.files`（原生 List/TreeView）。
// 为什么用原生 TreeView 而不是 Webview：原生列表自带键盘导航/无障碍/主题、
// 行内按钮（view/item/context 的 inline）与右键菜单，不必自绘；行模型与文案在
// filesPanelPure.ts（纯逻辑，可 Node 单测）。
//
// 数据：低频——连接成功 / 手动刷新 / 下载成功 / 删除 / 设主文件后各拉一次（extension.ts
// 的 refreshControllerFiles 更新 file.list + boot.get 缓存后调 provider.refresh()）。
// 纪律：未连接、缺 d6、拉取失败、空目录、主文件失效都以**占位行**明示原因（降级不伪装）。

import * as vscode from 'vscode';

import { FilesTreeRow, FilesTreeState, fileIconSpec, buildFilesTreeRows } from './filesPanelPure';
import { S } from './strings';

/** 文件行/占位行 → TreeItem（点击文件行 = 拉取到工作区） */
export class KxFilesTreeItem extends vscode.TreeItem {
    constructor(readonly row: FilesTreeRow) {
        super(row.label, vscode.TreeItemCollapsibleState.None);
        this.id = row.id;

        if (row.kind === 'placeholder') {
            this.contextValue = 'kxPlaceholder';
            this.tooltip = row.label;
            return;
        }

        // 主文件（开机运行，D8）用星标 + 独立 contextValue（行内菜单：取消主文件/删除）
        this.contextValue = row.main ? 'kxMainFile' : 'kxFile';
        this.description = row.description;
        this.tooltip = row.tooltip;
        if (row.main) {
            this.iconPath = new vscode.ThemeIcon('star-full', new vscode.ThemeColor('charts.yellow'));
        } else {
            const icon = fileIconSpec(row.lang ?? 'other');
            this.iconPath = new vscode.ThemeIcon(
                icon.id,
                icon.colorId ? new vscode.ThemeColor(icon.colorId) : undefined,
            );
        }
        this.command = {
            command: 'kine-x.files.pull',
            title: S.filesPanel.pullTitle,
            arguments: [this],
        };
    }

    /** kind=file 时的文件名（命令参数） */
    get fileName(): string | undefined {
        return this.row.kind === 'file' ? this.row.name : undefined;
    }
}

export class KxFilesTreeProvider implements vscode.TreeDataProvider<KxFilesTreeItem> {
    static readonly viewType = 'kine-x.files';

    private readonly emitter = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.emitter.event;

    constructor(private readonly state: () => FilesTreeState) {}

    /** 缓存变化后调用：整树重取（文件数少，不做增量） */
    refresh(): void {
        this.emitter.fire();
    }

    getTreeItem(element: KxFilesTreeItem): vscode.TreeItem {
        return element;
    }

    getChildren(): KxFilesTreeItem[] {
        return buildFilesTreeRows(this.state()).map((row) => new KxFilesTreeItem(row));
    }
}
