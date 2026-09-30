# T-23 定制 VSCodium 发行版（可选·配方）

> 需求：`docs/planA/15-VSCodium插件需求与开发任务.md` §8.6（FR-8.4）。
> 合规红线（`14` §6.2）：**不得再分发微软官方 VS Code 二进制**，只能基于 VSCodium / Code-OSS **源码**构建。

本目录把 §8.6 的配方固化为可执行脚本与配置模板。**本仓库默认不构建**（决策 D-07：先把离线 `.vsix` 跑通），
因此这些文件不进入 `npm run package` 的 VSIX 产物（`.vscodeignore` 已排除 `tools/**`）。

## 文件

| 文件 | 作用 |
|------|------|
| `build-vscodium.sh` | 主驱动脚本：`fetch` / `brand` / `workspace` / `inject` / `build` / `all` |
| `product.overrides.json` | 合并进 Code-OSS `product.json` 的覆盖片段：品牌 + Open VSX 扩展市场 + 预置设置 |
| `workspace-template/settings.json` | 终端工作区模板（锁连接参数、语言关联、订阅频率） |
| `workspace-template/launch.json` | 一键调试配置（`type: kine-x`） |

## 三步走

### 1) 预置插件（离线，无需构建环境）

把已打包的 `.vsix` 预置成 VSCodium 的**内置插件**，打开即用、无需再安装：

```bash
npm run package                                   # 产物 kinex-debug-<ver>.vsix
bash tools/vscodium/build-vscodium.sh inject /usr/share/codium   # 或免安装目录
```

### 2) 源码构建 + 品牌（需完整工具链 + 外网）

```bash
bash tools/vscodium/build-vscodium.sh fetch       # 拉取 VSCodium + Code-OSS 源码
bash tools/vscodium/build-vscodium.sh brand  .work/vscodium/vscode
bash tools/vscodium/build-vscodium.sh build  .work/vscodium      # 产出 tar.gz / .deb
bash tools/vscodium/build-vscodium.sh inject .work/vscodium/VSCodium/<ver>/  # 预置进免安装包
```

或一步到位：`bash tools/vscodium/build-vscodium.sh all`。

### 3) 终端工作区模板

```bash
bash tools/vscodium/build-vscodium.sh workspace /path/to/kinex-project
# → /path/to/kinex-project/.vscode/{settings.json,launch.json}
```

## 品牌与预置项说明

`product.overrides.json` 会**深度合并**进 `vscode/product.json`（`_` 前缀键为注释，不写入）：

- 品牌：`nameShort` / `nameLong` / `applicationName` / `dataFolderName` / `linuxIconName` 等。
- 扩展市场：指向 **Open VSX**（`extensionsGallery`），与 T-22 的 `publish-ovsx.sh` 对齐。
- `configurationDefaults`：`kine-x.host` / `kine-x.debugPort` / `kine-x.engine` / `kine-x.showRawProtocol`
  作为**所有用户**的默认值（仍可被用户设置覆盖）。

> 图标需自行准备 `resources/linux/<linuxIconName>.png`（`.deb` 打包用）；本配方只设置名称，不内置二进制资产。

## 验证清单（构建后）

1. 启动定制版，确认窗口/关于页显示 `Kine-X Studio`。
2. 打开 `.bas` 文件，`Cmd/Ctrl+Shift+P` → 命令列表可搜到 `Kine-X:` 命令。
3. 用工作区模板打开工程，`F5` 走 `launch.json`（`type: kine-x`）连上控制器。
4. 输出面板有 `[sys.info] … 能力 d1,d2,d3,…`，命令名补全（FR-2.5）可用。
5. 能力缺失时（如无 `d4`/`d5`）提示**明示降级**，不得伪装成功。

## 回归

冒烟 `npm run test:smoke` 的 `checkVscodiumRecipe()` 会校验：文件存在、脚本离线（无 `curl`/`wget`）、
`bash -n` 语法通过、`product.overrides.json` 与工作区模板为合法 JSON 且含关键字段。
