#!/usr/bin/env bash
#
# build-vscodium.sh —— T-23「定制 VSCodium 发行版」（可选·配方，docs/planA/15 §8.6）
#
# 为什么必须是源码构建：14 §6.2 的合规红线是**不得再分发微软官方 VS Code 二进制**，
# 只能基于 VSCodium / Code-OSS 源码构建。本脚本把 §8.6 的配方固化为可执行步骤。
#
# 本仓库**默认不构建**（D-07：先把离线 `.vsix` 跑通），因此此脚本不进入 `npm run package`
# 的产物，仅在具备工具链的机器上按需执行；`inject` 子命令可在**无构建环境**下离线使用。
#
# 子命令：
#   fetch                       拉取 VSCodium + Code-OSS 源码（需外网 + git）
#   brand   <code-oss-dir>      合并 product.overrides.json → <dir>/product.json
#   workspace <target-dir>      写入 .vscode/settings.json + launch.json 模板
#   inject  <vscodium-install> [vsix]   把 .vsix 预置为「内置插件」（离线，无需构建环境）
#   build   [vscodium-dir]      执行 VSCodium 构建脚本（产出 tar.gz / .deb；需完整工具链）
#   all                         fetch → brand → build → inject（全流程）
#   --help
#
# 环境变量：
#   KINEX_VSCODIUM_WORK  工作目录（默认 <本脚本目录>/.work）
#   KINEX_VSCODIUM_REPO  VSCodium 仓库地址（默认 GitHub 上游）
#   KINEX_VSCODIUM_REF   分支/标签（默认上游 default）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="${KINEX_VSCODIUM_WORK:-$SCRIPT_DIR/.work}"
VSCODIUM_REPO="${KINEX_VSCODIUM_REPO:-https://github.com/VSCodium/vscodium.git}"
VSCODIUM_REF="${KINEX_VSCODIUM_REF:-}"
BUILTIN_EXT_ID="kinex-debug"

log() { printf '%s\n' "$*"; }
err() { printf '错误：%s\n' "$*" >&2; }
need() {
    command -v "$1" >/dev/null 2>&1 || {
        err "缺少命令 $1${2:+ $2}"
        exit 1
    }
}

usage() {
    cat <<'EOF'
T-23 定制 VSCodium 发行版（可选·配方，docs/planA/15 §8.6）

用法：build-vscodium.sh <子命令> [参数]

  fetch                       拉取 VSCodium + Code-OSS 源码（需外网 + git）
  brand   <code-oss-dir>      合并 product.overrides.json → <dir>/product.json
  workspace <target-dir>      写入 .vscode/settings.json + launch.json 模板
  inject  <vscodium-install> [vsix]   把 .vsix 预置为「内置插件」（离线，无需构建环境）
  build   [vscodium-dir]      执行 VSCodium 构建脚本（产出 tar.gz / .deb；需完整工具链）
  all                         fetch → brand → build → inject（全流程）
  -h | --help                 显示本帮助

环境变量：
  KINEX_VSCODIUM_WORK  工作目录（默认 <本脚本目录>/.work）
  KINEX_VSCODIUM_REPO  VSCodium 仓库地址（默认 GitHub 上游）
  KINEX_VSCODIUM_REF   分支/标签（默认上游 default）

提示：本仓库默认不构建（D-07 先离线 .vsix）；无构建环境时可用
      `inject` 把已打包的 .vsix 预置进现有 VSCodium 安装目录。
EOF
}

# 合并 overrides → product.json（深度合并：dict 递归，其余整体替换；跳过 `_` 前缀注释键）
merge_product() {
    local target="$1" overrides="$2"
    python3 - "$target" "$overrides" <<'PY'
import json, sys

target, overrides = sys.argv[1], sys.argv[2]
with open(target, encoding='utf-8') as f:
    base = json.load(f)
with open(overrides, encoding='utf-8') as f:
    ov = json.load(f)

def merge(a, b):
    for k, v in b.items():
        if k.startswith('_'):
            continue
        if isinstance(v, dict) and isinstance(a.get(k), dict):
            merge(a[k], v)
        else:
            a[k] = v
    return a

merge(base, ov)
with open(target, 'w', encoding='utf-8') as f:
    json.dump(base, f, indent=2, ensure_ascii=False)
    f.write('\n')
print(f'已合并 {overrides} → {target}')
PY
}

cmd_fetch() {
    need git "（fetch 需要 git）"
    mkdir -p "$WORK_DIR"
    if [ -d "$WORK_DIR/vscodium/.git" ]; then
        log "已存在源码：$WORK_DIR/vscodium（跳过 clone，如需更新请手动 git pull）"
    else
        log "clone VSCodium → $WORK_DIR/vscodium"
        # shellcheck disable=SC2086
        git clone --depth 1 ${VSCODIUM_REF:+--branch "$VSCODIUM_REF"} "$VSCODIUM_REPO" "$WORK_DIR/vscodium"
    fi
    log "拉取 Code-OSS 源码（VSCodium/get_repo.sh，含其补丁）…"
    (cd "$WORK_DIR/vscodium" && ./get_repo.sh)
    log "完成。Code-OSS 源码目录：$WORK_DIR/vscodium/vscode"
}

cmd_brand() {
    local dir="${1:-}"
    [ -n "$dir" ] || {
        err "用法：brand <code-oss-dir>"
        exit 2
    }
    local pj="$dir/product.json"
    [ -f "$pj" ] || {
        err "找不到 $pj（先执行 fetch）"
        exit 1
    }
    need python3 "（brand 需要 python3 做 JSON 合并）"
    merge_product "$pj" "$SCRIPT_DIR/product.overrides.json"
}

cmd_workspace() {
    local target="${1:-}"
    [ -n "$target" ] || {
        err "用法：workspace <target-dir>"
        exit 2
    }
    mkdir -p "$target/.vscode"
    cp "$SCRIPT_DIR/workspace-template/settings.json" "$target/.vscode/settings.json"
    cp "$SCRIPT_DIR/workspace-template/launch.json" "$target/.vscode/launch.json"
    log "已写入工作区模板：$target/.vscode/{settings.json,launch.json}"
}

# 找到最新的本地 .vsix（npm run package 产物）
latest_vsix() {
    local f newest=""
    for f in "$EXT_ROOT"/*.vsix; do
        [ -e "$f" ] || continue
        if [ -z "$newest" ] || [ "$f" -nt "$newest" ]; then
            newest="$f"
        fi
    done
    printf '%s' "$newest"
}

cmd_inject() {
    local install="${1:-}" vsix="${2:-}"
    [ -n "$install" ] || {
        err "用法：inject <vscodium-install> [vsix]"
        exit 2
    }
    local ext_dir="$install/resources/app/extensions"
    [ -d "$ext_dir" ] || {
        err "找不到内置扩展目录：$ext_dir（应为 VSCodium 安装目录）"
        exit 1
    }
    if [ -z "$vsix" ]; then
        vsix="$(latest_vsix)"
    fi
    [ -n "$vsix" ] && [ -f "$vsix" ] || {
        err "找不到 .vsix：先执行 npm run package，或显式传入路径"
        exit 1
    }
    need python3 "（inject 用 python3 解包 zip）"
    local dest="$ext_dir/$BUILTIN_EXT_ID"
    rm -rf "$dest"
    mkdir -p "$dest"
    python3 -m zipfile -e "$vsix" "$dest"
    [ -f "$dest/package.json" ] || {
        err "解包后缺少 package.json：$dest"
        exit 1
    }
    log "已预置内置插件：$dest（来源 $(basename "$vsix")）"
    log "注意：目录名仅作标识，Code-OSS 以内层 package.json 的 publisher/name/version 识别插件。"
    if [ ! -w "$ext_dir" ]; then
        log "提示：$ext_dir 不可写，可能需 sudo 或改用免安装（tar.gz）目录。"
    fi
}

cmd_build() {
    local dir="${1:-$WORK_DIR/vscodium}"
    [ -d "$dir" ] || {
        err "找不到 VSCodium 源码：$dir（先执行 fetch）"
        exit 1
    }
    [ -x "$dir/build.sh" ] || {
        err "$dir/build.sh 不存在或不可执行"
        exit 1
    }
    log "开始构建（耗时较长；产物见 $dir/VSCodium/）…"
    (cd "$dir" && ./build.sh)
    log "构建完成。产出目录：$dir/VSCodium/"
}

cmd_all() {
    cmd_fetch
    cmd_brand "$WORK_DIR/vscodium/vscode"
    cmd_build "$WORK_DIR/vscodium"
    local portable=""
    portable="$(ls -1d "$WORK_DIR"/vscodium/VSCodium/*/ 2>/dev/null | head -n 1 || true)"
    if [ -n "$portable" ]; then
        cmd_inject "$portable"
    else
        log "提示：未自动找到免安装目录，跳过 inject。"
        log "      可在安装（.deb）后手动执行：$0 inject /usr/share/kinex-studio"
    fi
    log "全流程结束。请用「Kine-X 控制器: 连接控制器」验证 D1~D5 与插件加载。"
}

case "${1:-}" in
fetch) cmd_fetch ;;
brand) shift && cmd_brand "$@" ;;
workspace) shift && cmd_workspace "$@" ;;
inject) shift && cmd_inject "$@" ;;
build) shift && cmd_build "$@" ;;
all) cmd_all ;;
-h | --help | help | "") usage ;;
*)
    err "未知子命令：$1"
    usage
    exit 2
    ;;
esac
