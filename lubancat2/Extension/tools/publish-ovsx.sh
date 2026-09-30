#!/usr/bin/env bash
# publish-ovsx.sh —— 发布到 Open VSX（docs/planA/15 FR-8.2；T-22）
#
# 背景：VSCodium 默认从 **Open VSX** 拉插件，而非微软 Marketplace（14 §6.1）。
#      发布是**可选项**（D-07：先离线 .vsix）；需要外网与 Open VSX 账号。
#
# 前置：
#   1) 环境变量 OVSX_PAT（Open VSX personal access token，https://open-vsx.org 生成）；
#   2) 外网可访问目标 registry。
#
# 用法：
#   OVSX_PAT=xxxx tools/publish-ovsx.sh
#   tools/publish-ovsx.sh --registry https://open-vsx.org
#   tools/publish-ovsx.sh --no-build        # 跳过编译（用现有 out/）
#
# 说明：脚本会先 compile + package，再 publish；任一步失败即中止（set -e）。
set -euo pipefail

REGISTRY="https://open-vsx.org"
DO_BUILD=1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

while [ $# -gt 0 ]; do
  case "$1" in
    --registry)
      [ $# -ge 2 ] || { echo "错误：--registry 需要一个 URL" >&2; exit 2; }
      REGISTRY="$2"; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "错误：未知参数 $1" >&2; exit 2 ;;
  esac
done

if [ -z "${OVSX_PAT:-}" ]; then
  echo "错误：未设置 OVSX_PAT（Open VSX 访问令牌）。" >&2
  echo "      在 https://open-vsx.org 生成后： OVSX_PAT=xxxx $0" >&2
  exit 1
fi

cd "$EXT_ROOT"

if [ "$DO_BUILD" -eq 1 ]; then
  echo "== 编译（tsc） =="
  npm run compile
fi

echo "== 打包（vsce） =="
npx @vscode/vsce package --no-dependencies

VSIX="$(ls -1t "$EXT_ROOT"/*.vsix | head -n1)"
[ -n "$VSIX" ] || { echo "错误：未生成 .vsix" >&2; exit 1; }

echo "== 发布到 Open VSX：$REGISTRY =="
echo "   文件：$VSIX"
npx --yes ovsx publish "$VSIX" --pat "$OVSX_PAT" --registry "$REGISTRY"

echo "完成。VSCodium 内可在扩展市场搜索 “Kine-X 调试” 安装/更新。"
