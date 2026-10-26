# GitHub CI：本机无法完成（blocked）

日期：2026-10-26
执行者：路线图 1→4 自动推进会话
HEAD：`07e43d0`（v0.72.0 tag `646f81d`）

## 结论

**blocked —— 本机没有远端仓库，也没有 GitHub CLI / 凭据。** 因此
`git push origin main`、`git push origin v0.72.0`、`gh run list`、
`gh workflow run static.yml` 都无法真实执行，也无法证明任何一次
GitHub Actions 运行成功。

这是环境缺失，不是代码问题：`static.yml` / `full.yml` 与 `tools/ci.ps1`
本身已就绪（`-Quick -StrictBom` 本机可跑通，见 `docs/ci.md`）。

## 缺什么（逐项核对）

| 检查 | 结果 | 说明 |
| --- | --- | --- |
| `git remote -v` | 空 | 没有 `origin`，也没有任何其它远端 |
| `gh` 命令 | 不存在 | `gh : 无法将“gh”项识别为 cmdlet...`，PATH 上没有 GitHub CLI |
| `gh auth status` | 未执行（无 gh） | 没有可用的 GitHub token/凭据 |
| SSH 远端凭据 | 未知 | 没有远端可连，无法验证 |

仓库本身是干净的、可交付的：

- 工作区 `git status --porcelain` 为空；
- 唯一 tag：`v0.44.0`、`v0.72.0`（后者指向 `646f81d`）；
- `.github/workflows/static.yml`（ubuntu-latest 静态层）与
  `full.yml`（自托管 Windows，需 i686-elf + QEMU）都已入库。

## 用户需要执行的两条命令

先把仓库地址换成你自己的（**不要**用未知地址；建议先建**私有**仓库）：

```powershell
# 1) 绑定远端（https 或 ssh，二选一）
git remote add origin https://github.com/<你的账号>/<仓库名>.git

# 2) 推 main 与 v0.72.0 tag
git push -u origin main; git push origin v0.72.0
```

推送后想确认 CI 实跑：

```powershell
gh auth login                       # 若尚未安装：winget install GitHub.cli
gh run list --workflow static.yml --limit 5
gh run watch                        # 或 gh run view <run-id> --log
```

## 需要注意（不要做的事）

1. **不要创建公开仓库**：本仓库带教学/演示代码与默认口令（`ACCT_*_PW`），
   公开前需先按 `SECURITY.md` 处理。
2. **不要把代码推到未知地址**：远端必须由用户显式提供。
3. **不要用 `gh repo create` 代替上面两步**：仓库可见性与归属应由用户决定。
4. 推送前确保工作区干净（`tools/ci.ps1 -Quick -StrictBom` 会检查）；
   `.ps1` 必须带 UTF-8 BOM，构建产物不得入库（pre-commit 已强制）。

## 已尝试与未尝试

已尝试（真实执行）：

- `git remote -v` → 无输出（无远端）；
- `git config user.name` → `Aster Dev`，`user.email` → `dev@aster.local`；
- `Get-Command gh` → `CommandNotFoundException`，`gh auth status` 无法运行。

未尝试（刻意不做，避免伪造结论或把代码推到未知地址）：

- 创建 GitHub 仓库、生成/使用 token、`git push` 到任何地址。

## 恢复条件

用户完成上面两条命令（并提供仓库 URL / 让 `gh auth status` 通过）后，
本项即可解除 blocked：届时重跑 `gh run list --workflow static.yml`
记录真实 run id 与结论，写入 `docs/roadmap-progress.md` 的阶段 1 段落即可。
