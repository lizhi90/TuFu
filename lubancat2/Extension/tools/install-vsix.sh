#!/usr/bin/env bash
# install-vsix.sh —— 离线安装 / 卸载 Kine-X 调试插件（docs/planA/15 FR-8.5）
#
# 用途：现场常无外网，用本脚本把已打好的 .vsix 装进 VSCodium / VS Code。
#
# 用法：
#   tools/install-vsix.sh                    # 自动选最新的 .vsix 并安装
#   tools/install-vsix.sh path/to/xxx.vsix   # 安装指定 .vsix
#   tools/install-vsix.sh --uninstall        # 卸载
#   tools/install-vsix.sh --list             # 查看已安装版本
#   tools/install-vsix.sh --cli /usr/bin/codium
#   tools/install-vsix.sh --help
#
# 设计约束（docs/planA/15 §11.2 D-07「先离线 .vsix」）：
#   - **全离线**：不访问任何网络（不下载、不查询远端）；
#   - 自动探测命令行入口（codium / codium-insiders / code / code-insiders）；
#   - 找不到 CLI 或找不到 .vsix → **明确报错并给出下一步**，不静默失败。
set -u

EXT_ID="kine-x.kinex-debug"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CLI_BIN=""
MODE="install"
VSIX=""

usage() {
  cat <<'EOF'
install-vsix.sh —— 离线安装/卸载 Kine-X 调试插件

用法：
  tools/install-vsix.sh [选项] [.vsix 路径]

选项：
  --cli <exe>     指定编辑器命令行（默认自动探测 codium/code）
  --uninstall     卸载 kine-x.kinex-debug
  --list          列出已安装的 Kine-X 插件版本
  -h, --help      显示本帮助

示例：
  tools/install-vsix.sh
  tools/install-vsix.sh --cli /usr/bin/codium
  tools/install-vsix.sh --uninstall
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall) MODE="uninstall"; shift ;;
    --list)      MODE="list"; shift ;;
    --cli)
      [ $# -ge 2 ] || { echo "错误：--cli 需要一个可执行文件路径" >&2; exit 2; }
      CLI_BIN="$2"; shift 2 ;;
    -h|--help)   usage; exit 0 ;;
    -*)          echo "错误：未知参数 $1" >&2; usage >&2; exit 2 ;;
    *)           VSIX="$1"; shift ;;
  esac
done

detect_cli() {
  if [ -n "$CLI_BIN" ]; then echo "$CLI_BIN"; return 0; fi
  local c
  for c in codium codium-insiders code code-insiders; do
    if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
  done
  echo ""
}

CLI_BIN="$(detect_cli)"
if [ -z "$CLI_BIN" ]; then
  echo "错误：找不到编辑器命令行（codium / code）。" >&2
  echo "      请用 --cli 指定，例如： tools/install-vsix.sh --cli /usr/bin/codium" >&2
  exit 1
fi

case "$MODE" in
  list)
    echo "已安装（$CLI_BIN）："
    "$CLI_BIN" --list-extensions --show-versions 2>/dev/null | grep -i "^${EXT_ID}@" \
      || echo "  （未安装 $EXT_ID）"
    exit 0
    ;;

  uninstall)
    echo "使用命令行：$CLI_BIN"
    echo "卸载：$EXT_ID"
    "$CLI_BIN" --uninstall-extension "$EXT_ID" \
      || { echo "错误：卸载失败（可能未安装）。" >&2; exit 1; }
    echo "完成。"
    exit 0
    ;;

  install)
    if [ -z "$VSIX" ]; then
      # 取扩展目录下最新的 .vsix（按修改时间）
      for f in "$EXT_ROOT"/*.vsix; do
        [ -e "$f" ] || continue
        if [ -z "$VSIX" ] || [ "$f" -nt "$VSIX" ]; then VSIX="$f"; fi
      done
    fi
    if [ -z "$VSIX" ] || [ ! -f "$VSIX" ]; then
      echo "错误：找不到 .vsix 文件。" >&2
      echo "      请先在本目录执行： npm run package" >&2
      echo "      或显式传入路径：   $0 path/to/kinex-debug-x.y.z.vsix" >&2
      exit 1
    fi
    echo "使用命令行：$CLI_BIN"
    echo "安装文件：$VSIX"
    # --force：覆盖已装版本，避免交互式确认
    "$CLI_BIN" --install-extension "$VSIX" --force \
      || { echo "错误：安装失败。" >&2; exit 1; }
    echo "完成。重启编辑器后，在「命令面板」运行 “Kine-X 控制器: 连接控制器”。"
    exit 0
    ;;
esac
