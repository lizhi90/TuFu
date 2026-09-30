# GitHub 提交指南（本项目 → `lizhi90/TuFu`）

> 建立日期：2026-09-30　适用：把 `/home/ubuntu20/Kine-X/` 工作区提交/增量同步到 GitHub 仓库 `lizhi90/TuFu`。
> 本文记录**实测可用的提交方式、四种方式的对比结论**，以及后续提交的快捷命令。

---

## 0. 前置事实（2026-09-30 实测）

| 项 | 值 |
|----|----|
| 目标仓库 | `https://github.com/lizhi90/TuFu` |
| 仓库可见性 | **private（私有）** |
| 默认分支 | `main`（初始只有一个 6 字节 `README.md`） |
| Token 身份 | `lizhi90`（= 仓库所有者，权限 `admin/maintain/push/triage/pull` 全开） |
| Token 存放位置 | `/home/ubuntu20/.codebuddy/mcp.json` → `mcpServers.GitHub.env.GITHUB_PERSONAL_ACCESS_TOKEN` |
| MCP 服务 | `npx -y @modelcontextprotocol/server-github`（stdio，`github-mcp-server 0.6.2`，暴露 **26 个工具**） |
| 本机 git | `git version 2.25.1`（**可用**，注意：AGENTS.md 里"git 命令不可用"的说法已过时） |
| 工作区体积 | 244 MB（其中 `SV630N系列伺服用户手册-CN-D00.PDF` 单文件 **133.9 MB**，超过 GitHub 100MB 硬限制） |

---

## 1. 四种提交方式对比（结论先行）

| 维度 | A. IDE 内置 MCP 工具 | B. MCP 命令行 stdio 调用 | C. 原生 git CLI ★ 推荐 | D. REST API + curl |
|------|---------------------|--------------------------|------------------------|--------------------|
| 调用形式 | `mcp__GitHub__push_files` 等 | 手写 JSON-RPC 喂给 stdio 服务 | `git add/commit/push` | `curl -X PUT /contents/...` |
| 适合场景 | 单文件读写、Issue/PR、建仓建分支 | 同 A，无 IDE 时的替代 | **整仓首次提交 / 批量增量同步** | 单文件、CI 脚本 |
| 能否一次提交整仓 | ✗ 需把**全部文件内容内联进 JSON**，244MB 不可行 | ✗ 同 A | ✅ 天然支持（增量对象传输） | ✗ |
| 大文件（>1MB） | ✗ contents API 上限 1MB/文件 | ✗ | ✅（单文件 ≤100MB） | ✗ |
| 认证 | MCP 配置里的 PAT | 环境变量 `GITHUB_PERSONAL_ACCESS_TOKEN` | `https://x-access-token:$TOKEN@github.com/...` | `Authorization: Bearer $TOKEN` |
| 是否留下本地 `.git` | 否 | 否 | **是**（利于后续增量） | 否 |
| 增量提交成本 | 每次重传全量文件 | 同左 | **只传 diff**（最优） | 每文件一次请求 |
| 历史/回退 | 由 MCP 逐次 commit | 同左 | 完整本地历史，可 diff/revert | 无 |
| 本次实测 | 会话未暴露该工具集 | ✅ 可用（已验证 26 工具） | ✅ **本次实际使用** | 未用（仅用于查询仓库元信息） |

**结论**：
- **整仓/批量提交 → 用 C（git CLI）**；MCP 的 `push_files`、`create_or_update_file` 只适合**少量小文件**（内容 API 单文件 1MB 上限）。
- **仓库管理类操作 → 用 A/B（MCP）**：建仓、建分支、读文件、列 commit、建 Issue/PR、合并 PR。
- 两者**互补**：本次提交用 git CLI 完成主体上传，再用 MCP `list_commits`/`get_file_contents` 做**推送结果校验**。
- 组合原则：**能用 MCP 表达的管理动作优先用 MCP**（符合"降级不伪装"：MCP 做不到批量时，明确说明并换 git，而不是假装用 MCP 完成）。

---

## 2. MCP GitHub Server 的 26 个工具（能力清单）

`create_or_update_file`、`search_repositories`、`create_repository`、`get_file_contents`、`push_files`、
`create_issue`、`create_pull_request`、`fork_repository`、`create_branch`、`list_commits`、
`list_issues`、`update_issue`、`add_issue_comment`、`search_code`、`search_issues`、`search_users`、
`get_issue`、`get_pull_request`、`list_pull_requests`、`create_pull_request_review`、`merge_pull_request`、
`get_pull_request_files`、`get_pull_request_status`、`update_pull_request_branch`、`get_pull_request_comments`、
`get_pull_request_reviews`

**注意**：该包 `@modelcontextprotocol/server-github@2025.4.8` 已被 npm 标记弃用（"Package no longer supported"）；
官方替代为 `github/github-mcp-server`（Docker）或官方远程 MCP 端点。迁移时工具名会变，其余流程不变。

---

## 3. 方式 B：MCP 的命令行调用（无 IDE 插件时的通用写法）

```bash
# 1) 从 mcp.json 取出 token（避免明文写进命令/日志）
TOKEN=$(sed -n 's/.*GITHUB_PERSONAL_ACCESS_TOKEN": "\([^"]*\)".*/\1/p' ~/.codebuddy/mcp.json)

# 2) 以 stdio 启动 MCP 服务，握手后调用任意工具
call_mcp () {  # $1=tool  $2=arguments(JSON)
  printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"kx","version":"1.0.0"}}}' \
    '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}" \
  | GITHUB_PERSONAL_ACCESS_TOKEN="$TOKEN" timeout 120 npx -y @modelcontextprotocol/server-github 2>/dev/null
}

# 示例：
call_mcp list_commits     '{"owner":"lizhi90","repo":"TuFu","sha":"main","perPage":5}'
call_mcp get_file_contents '{"owner":"lizhi90","repo":"TuFu","path":"README.md"}'
```

- 返回为 JSON-RPC 信封：`{"result":{"content":[{"type":"text","text":"<真正的 JSON>"}]}}`。
- **IDE 内置工具未暴露时**（本机 2026-09-30 会话即如此，工具表里没有 `mcp__GitHub__*`），
  要么按上面走命令行，要么在 MCP 面板确认 server 为 Connected / Reload Window 后重试。
- 单次调用只发一个工具请求（`id:2`）；如需串联多次调用，需要常驻进程而不是每次重开。

---

## 4. 方式 C：git CLI（★ 本次实际使用的提交方式）

### 4.1 首次全量提交（2026-09-30 实测通过）

```bash
cd /home/ubuntu20/Kine-X
TOKEN=$(sed -n 's/.*GITHUB_PERSONAL_ACCESS_TOKEN": "\([^"]*\)".*/\1/p' ~/.codebuddy/mcp.json)

# ① 先写 .gitignore（见 §5），避免把 build/ 与 node_modules/ 灌进仓库
# ② 初始化并绑定远端（token 只临时挂在 URL 上，随后立刻改回无 token 形式）
git init
git remote add origin "https://x-access-token:${TOKEN}@github.com/lizhi90/TuFu.git"
GIT_TERMINAL_PROMPT=0 git fetch origin

# ③ 基于远端 main 建本地分支（保留仓库里已有的 README.md）
git checkout -b main origin/main

# ④ 提交（用 -c 传身份，避免改动任何 git config）
git add -A
git -c user.name="lizhi90" -c user.email="lizhi90@users.noreply.github.com" \
    commit -m "initial commit: Kine-X（鲁班猫2 + IgH EtherCAT 替换正运动 ZMC 控制器）"

# ⑤ 推送
GIT_TERMINAL_PROMPT=0 git push -u origin main

# ⑥ 把远端 URL 改回不带 token 的形式（token 不留在 .git/config）
git remote set-url origin https://github.com/lizhi90/TuFu.git
```

### 4.2 后续增量提交（日常就用这三条）

```bash
cd /home/ubuntu20/Kine-X
git add -A
git -c user.name="lizhi90" -c user.email="lizhi90@users.noreply.github.com" commit -m "说明本次改动"
TOKEN=$(sed -n 's/.*GITHUB_PERSONAL_ACCESS_TOKEN": "\([^"]*\)".*/\1/p' ~/.codebuddy/mcp.json)
GIT_TERMINAL_PROMPT=0 git push "https://x-access-token:${TOKEN}@github.com/lizhi90/TuFu.git" main
```

### 4.3 只提交/撤销某几个文件

```bash
git status                     # 看改动
git add lubancat2/src/xxx.cpp  # 只加指定文件
git commit -m "..."
git restore --staged <file>    # 撤出暂存
git checkout -- <file>         # 丢弃工作区改动（危险：不可恢复）
```

---

## 5. `.gitignore` 要点（否则提交必失败或仓库炸掉）

已在仓库根写入 `/home/ubuntu20/Kine-X/.gitignore`，关键条目：

| 条目 | 原因 |
|------|------|
| `build/`、`lubancat2/build/`、`CMakeFiles/`、`*.o`、`*.make` | 构建产物（本机 cmake + 板端 g++ 直编），约 14MB 且无意义 |
| `lubancat2/Extension/node_modules/` | Node 依赖，约 30MB，`package.json` 可还原 |
| `SV630N系列伺服用户手册-CN-D00.PDF` | **133.9MB，超过 GitHub 单文件 100MB 硬限制，必然被拒** |
| `*.log`、`*.tmp`、`.DS_Store` | 本机临时文件 |

其他厂商手册（`xCore…_A.pdf` 20.9MB、`ZBasic编程手册V3.3.0.pdf` 9.5MB、
`TAS-LAN-869&869F_产品资料/` 21MB 等）均在 100MB 以下，**当前保留提交**；如需瘦身可自行加入忽略。

---

## 6. 注意事项与坑

1. **单文件 100MB 是硬限制**（GitHub 直接拒绝 push）；>50MB 会有警告。提交前先 `find . -type f -size +50M`。
2. **PAT 明文存放在 `~/.codebuddy/mcp.json`**：不要把它复制进工作区，也不要把 `.codebuddy` 之外的内容连同 token 一起提交。
   本仓使用 `x-access-token:${TOKEN}@` 的临时 URL + `git remote set-url` 还原，**token 不留在 `.git/config`**。
3. **仓库当前为 private**，因此 `lubancat2/deploy/*.sh` 与 `planA/07` 中出现的板端默认口令 `temppwd`
   （`docs/planA/07`、`.codebuddy/memory/2026-09-24.md` 亦有记录）风险可控；
   **若将来改为公开仓库，必须先清理这些口令**，或改用环境变量/`.env` 方案。
4. **不要 force push / 不要 `--amend` 已推送的提交**（项目纪律：非用户明确要求禁止）。
5. `.codebuddy/` 属于项目数据（非临时缓存），当前一并提交；它是 AI 工作记忆，含内部操作细节，公开仓库前请复核。
6. 首次提交前 `git init` 会在工作区生成 `.git/`；本项目此前"非 git 仓库"，从此**有本地版本历史**，
   后续请勿删除 `.git/`，否则增量提交会退化成全量。
7. 一个仓库里 `README.md` 只有 6 字节，被本项目的根 `README.md` 覆盖（正常修改，非冲突）。

---

## 7. MCP 与 git 的分工（推荐日常用法）

| 需求 | 用什么 |
|------|--------|
| 建仓库 / 建分支 / 建 PR / 建 Issue / 合并 PR | **MCP**（`create_repository`/`create_branch`/`create_pull_request`/`create_issue`/`merge_pull_request`） |
| 读远端文件、看提交历史、查 PR 状态 | **MCP**（`get_file_contents`/`list_commits`/`get_pull_request_status`） |
| 改单个小文件（≤1MB，如配置、文档、版本号） | **MCP**（`create_or_update_file`） |
| 提交整个项目 / 批量增量同步 | **git CLI**（见 §4） |
| 推送后自动校验 | **MCP**（`list_commits` 确认 commit 落地 + `get_file_contents` 抽查关键文件） |

---

## 8. 变更记录

| 日期 | 内容 |
|------|------|
| 2026-09-30 | 建立本文；完成项目首次全量提交到 `lizhi90/TuFu`（git CLI + MCP 校验），并记录四种提交方式对比 |
