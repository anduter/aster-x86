# CI 与提交守卫

把"提交卫生"从口头约定变成机制：本地一条命令自检，`git commit` 有钩子把关，
CI 分两层跑（托管 runner 只能跑静态检查，完整构建需要自托管）。

相关工具：

| 文件 | 作用 |
|---|---|
| `tools/ci.ps1` | 统一入口：`-Quick` 静态检查（<10s）/ `-Full` 构建 + N 次回归 |
| `tools/pre-commit.ps1` | 提交守卫本体：BOM、禁用文件、`src/kernel/**` 模块边界 |
| `tools/githooks/pre-commit` | bash 钩子入口，转调上面的 `.ps1` |
| `tools/install-hooks.ps1` | 安装钩子（`core.hooksPath`），幂等 |
| `tools/session-commit.ps1` | 按会话限定路径提交，防止扫入别人 staged 的文件 |
| `.github/workflows/static.yml` | 托管 runner：只跑静态子集（BOM / 禁用文件 / check-deps） |
| `.github/workflows/full.yml` | 自托管 Windows runner：`ci.ps1 -Full`（构建 + 2×`-Test`） |

---

## 1. 本地用法

```powershell
# 提交前常规自检（静态，约 1s）
powershell -NoProfile -File tools/ci.ps1 -Quick

# 完整验证：build.ps1 + 连续 2 次 build.ps1 -Test（各 ~14s）
powershell -NoProfile -File tools/ci.ps1 -Full

# 工作区有别的会话改动时（静态检查仍然有效）
powershell -NoProfile -File tools/ci.ps1 -Quick -AllowDirty

# 加跑 flake-hunt（只报告，不作门禁；已知偶发 #PF 见 docs/parallel-sessions.md）
powershell -NoProfile -File tools/ci.ps1 -Full -FlakeRuns 5

# 强制"所有被跟踪脚本都必须有 BOM"（含历史豁免文件）
powershell -NoProfile -File tools/ci.ps1 -Quick -StrictBom

# 批量给历史脚本补 BOM（会自动列出改了哪些文件；请单独提交）
powershell -NoProfile -File tools/ci.ps1 -Quick -FixBom
```

输出与退出码（`0` = PASS，`1` = FAIL）：

```
=== CI (quick) ===
  repo  : D:\OS_Project
  head  : 3f79e0c [net] UDP echo selftest: ...
--- static 1/4: worktree ---
  [PASS] worktree clean  (0 项改动)
--- static 2/4: BOM + parse ---
  [PASS] BOM + parse (20 files)  (missing BOM: 0, parse errors: 0)
--- static 3/4: forbidden files ---
  [PASS] no build artifacts (226 tracked)  (bad: 0)
--- static 4/4: module boundaries (check-deps) ---
  [PASS] check-deps 0 violations  (exit=0, violations=0)
elapsed: 0.9s
=== CI PASS ===
```

检查项（任何一项失败 → exit 1）：

| # | 检查 | 说明 |
|---|---|---|
| 1 | `git status --porcelain` 干净 | `-AllowDirty` 降级为警告 |
| 2 | `*.ps1`/`*.psm1` 以 `EF BB BF` 开头 + `Parser::ParseFile` 0 错误 | 见第 6 节的"历史豁免" |
| 3 | 被跟踪文件里没有 `.obj-*` `.dist-*` `*.img` `*.o` `*.bin` `*.elf` `*.map` `serial-*.log` `logs-*.log` | 构建产物/日志 |
| 4 | `tools/check-deps.ps1` 0 违规 | 模块边界，规则见 `docs/modules.md` |
| 5 | `-Full`：`build.ps1` 成功 | 产物用隔离目录 |
| 6 | `-Full`：连续 2 次 `build.ps1 -Test` 全 PASS 且项数一致 | 当前 112/112（98 + 账号系统 P1 的 14 项） |

`-Full` 的隔离参数（结束自动清理，`-KeepArtifacts` 保留现场）：

```
-ObjDir .obj-ci  -DistDir .dist-ci  -QmpPort 4691  -SerialLog serial-ci.log
```

构建/回归日志在 `.session/ci/build.log`、`.session/ci/test-1.log`、`test-2.log`（`/session*/` 之外，`.session/` 已被忽略）。

实测（`8f95dbf`，Windows 10 + PowerShell 5.1）：`-Quick` **0.9s**；
`-Full` **47.7s**（构建 12.7s + `-Test` 19.7s/14.4s，各 **98/98**），结束后 `.obj-ci`/`.dist-ci`/`serial-ci.log` 已清理、`git status` 仍干净。

参数一览：

| 参数 | 默认 | 作用 |
|---|---|---|
| `-Quick` | （不给模式时默认） | 只跑静态检查 |
| `-Full` | — | 静态 + `build.ps1` + `-Test` ×N |
| `-TestRepeat N` | 2 | `-Full` 里 `-Test` 的次数 |
| `-ExpectChecks N` | 0（自动取第一次的项数） | 要求每次 `-Test` 都是 N/N |
| `-MinChecks N` | 90 | 低于这个项数视为没跑全 |
| `-FlakeRuns N` | 0 | 追加 `tools/flake-hunt.ps1 -Runs N`（**只报告，不作门禁**） |
| `-AllowDirty` | — | 工作区不干净时降级为警告（CI/多会话） |
| `-StrictBom` | — | 不再豁免历史无 BOM 文件 |
| `-FixBom` | — | 给所有被跟踪的无 BOM 脚本补 BOM（改文件，需单独提交） |
| `-KeepArtifacts` | — | `-Full` 结束不删 `.obj-ci`/`.dist-ci`/`serial-ci.log` |
| `-ObjDir`/`-DistDir`/`-QmpPort`/`-SerialLog` | `.obj-ci`/`.dist-ci`/`4691`/`serial-ci.log` | 隔离产物 |
| `-PowerShellExe` | `powershell` | 跑 `build.ps1` 用的解释器（保持 5.1 与本地一致） |

`-Full` 前置检查（任一不满足直接失败，不会跑出"假失败"）：

- 平台是 Windows（Linux/macOS 上没有 QEMU `qemu-system-i386`，且 `tools/i686-elf` 是 Windows 版）；
- `tools/i686-elf/bin` 或 PATH 上有 `i686-elf-gcc`；
- PATH 上有 `nasm`、`qemu-system-i386`；
- 宿主 `12345`（UDP+TCP）与 QMP `4691` 空闲 —— 同一时刻只能跑一个 `-Test`。

`-Full` 结束后还会再查一次 `git status`：产物如果漏进被跟踪路径，直接 FAIL。

---

## 2. CI 分两层

| 层 | 文件 | runner | 跑什么 | 为什么 |
|---|---|---|---|---|
| 静态 | `static.yml` | `ubuntu-latest`（托管） | BOM、禁用文件、`check-deps`、`ci.ps1 -Quick -AllowDirty` | 只需 git + PowerShell，秒级 |
| 完整 | `full.yml` | `[self-hosted, windows, x64]`（自托管） | `tools/ci.ps1 -Full`（构建 + 全 PASS ×2，当前 112 项） | 需要 `tools/i686-elf`（约 3.5GB，gitignored）与 QEMU |

**托管 runner 上不要试图构建**：那里没有 vendored 工具链、没有 QEMU，
`tools/i686-elf` 也不会随仓库分发（见 README 的"自带交叉工具链"一节）。
`static.yml` 只跑静态子集；想跑完整回归就注册一个自托管 Windows runner。

### 自托管 runner 要求

- Windows（x64）+ PowerShell 5.1（`build.ps1` 就是用 5.1 验证的）；
- 仓库工作区内有 `tools/i686-elf/bin/i686-elf-gcc.exe`（3.5GB，`git clone` 之后按 README 自备/软链）；
- PATH 上有 `nasm`（或 w64devkit 里的那份）与 `qemu-system-i386`；
- 宿主 `12345` 端口不能被别的进程长期占用（`full.yml` 用 `concurrency` 串行，
  但**同一台机器上别人的本地 `-Test` 仍会抢端口**）。

`full.yml` 是 `workflow_dispatch`（手动触发），可选参数 `test_repeat`（默认 2）、`flake_runs`（默认 0）。
失败时会尝试上传 `.session/ci/` 日志作为 artifact。

---

## 3. pre-commit 守卫

安装（每个克隆/每台新机器一次，本地配置，不会提交）：

```powershell
powershell -NoProfile -File tools/install-hooks.ps1
# install-hooks: core.hooksPath 已设为 tools/githooks
# install-hooks: 验证 git config --get core.hooksPath -> [tools/githooks]
# pre-commit: ok (nothing staged)
# install-hooks: 冒烟测试通过（暂存区为空 → ok）
```

卸载：`tools/install-hooks.ps1 -Uninstall`。

> `.gitignore` 里 `/tools/*` 会把 `tools/` 下非 `*.ps1` 的东西整体忽略，
> 所以 `tools/githooks/` 需要显式放行（`.gitignore` 里已有 `!/tools/githooks/`）。
> 否则 hook 文件 `git add` 不进去，`install-hooks.ps1` 装了也没有东西可跑。

装好之后 `git commit` 会自动跑 `tools/githooks/pre-commit`
→ `powershell -NoProfile -ExecutionPolicy Bypass -File <repo>/tools/pre-commit.ps1`。

守卫只看**暂存区**（`git diff --cached --diff-filter=ACMR`）：

1. **禁用路径** `.obj-*` `.dist-*` `*.img` `*.o` `*.bin` `*.elf` `*.map`
   `serial-*.log` `logs-*.log` `.session/` → 拒绝；
2. **`*.ps1`/`*.psm1`**：暂存内容（blob）前 3 字节必须是 `EF BB BF`，
   且 `Parser::ParseFile` 0 错误 → 否则拒绝并打印文件名；
3. 暂存区含 `src/kernel/**` 时跑 `tools/check-deps.ps1`，>0 违规 → 拒绝。

失败示例（真实输出）：

```
pre-commit: blocked: 1 issue(s)
  - tools/zz-neg-bom.ps1: 缺少 UTF-8 BOM（前 3 字节不是 EF BB BF）

fix:
  $ powershell -NoProfile -File tools/ci.ps1 -FixBom   # 给被跟踪的脚本补 BOM，然后 git add 再次提交
  $ 或手工：$b=[IO.File]::ReadAllBytes('tools/zz-neg-bom.ps1'); ...

紧急绕过（仅限救急，之后必须修复并补跑 tools/ci.ps1 -Quick）：
  git commit --no-verify ...
```

```
pre-commit: blocked: 1 issue(s)
  - serial-x.log: 禁用文件（serial-*.log（串口日志））

fix:
  $ git rm --cached -- 'serial-x.log'   # 保留工作区文件，只把它从提交里去掉
```

### 绕过策略

`git commit --no-verify` **只限救急**（例如 hook 本身坏了、正在修 hook）。
绕过之后必须补：`tools/ci.ps1 -Quick`，把问题修掉再提交一次。
CI 的 `static.yml` 会在 PR 上重跑同样的规则，绕过只影响本地这一次。

---

## 4. 按会话限定路径提交

事故复盘：`git commit` 把别的会话 staged 的文件一起提交（已发生 3 次，例如 `ed33a6a` 扫入 `ipc→svc` rename）。
`tools/session-commit.ps1` 把"只提交自己的文件"变成机制：

```powershell
powershell -NoProfile -File tools/session-commit.ps1 `
    -Name ci `
    -Message "add tools/ci.ps1 + pre-commit guard" `
    -Paths tools/ci.ps1,tools/pre-commit.ps1,tools/githooks/pre-commit,docs/ci.md
```

它按顺序做：

1. `-Name` / `-Message` / `-Paths` 任一为空 → 拒绝；
2. 暂存区里已有**不在 `-Paths` 中**的文件 → 直接失败并列出（这是扫入事故的根源）；
3. 只 `git add -- <Paths>`，然后确认暂存区集合 ⊆ `-Paths`；
4. 跑 `tools/pre-commit.ps1`（和 hook 同一份检查）；
5. `git commit -m "[<Name>] <Message>"`，打印哈希。

拒绝示例：

```
session-commit: 暂存区里已有 1 个不属于本次提交的文件：
  - serial-x.log

这通常意味着另一个会话先 git add 过。请先决定归属，再重试：
  git restore --staged -- <file>     # 从暂存区撤出（工作区不动）
  （或把属于你的路径加进 -Paths）

session-commit: aborted: 暂存区有其它来源的文件（拒绝扫入）
```

`-Paths` 可以给目录（如 `docs`），该目录下所有改动都算你的。

---

## 5. 负向验证（守卫真的有效）

改动守卫本身时，至少手工验一次这三条（都可以在同一次会话里做完再撤销）：

```powershell
# 1) 无 BOM 的 .ps1 必须被拒
[IO.File]::WriteAllText("$PWD\tools\zz-neg-bom.ps1", "Write-Host 'x'`n", (New-Object Text.UTF8Encoding($false)))
git add -- tools/zz-neg-bom.ps1
git commit -m "should be blocked"      # 期望 exit 1，且 HEAD 不变
git restore --staged -- tools/zz-neg-bom.ps1; Remove-Item tools/zz-neg-bom.ps1

# 2) 禁用文件必须被拒（.gitignore 被 -f 绕过时）
[IO.File]::WriteAllText("$PWD\serial-x.log", "x`n")
git add -f -- serial-x.log
git commit -m "should be blocked"      # 期望 exit 1
git restore --staged -- serial-x.log; Remove-Item serial-x.log

# 3) 暂存区有别人的文件时 session-commit 必须拒绝
git add -- <some-other-file>
tools/session-commit.ps1 -Name ci -Message "must abort" -Paths tools/ci.ps1   # 期望 exit 1 并列出对方文件
git restore --staged -- <some-other-file>

# 4) src/kernel 里的跨模块 include 必须被拒（用 .h 建临时探针，不会被编译）
'#include "fs/fs.h"' | Set-Content src/kernel/core/zz_neg.h
git add -- src/kernel/core/zz_neg.h
tools/pre-commit.ps1                    # 期望 exit 1，并打印 module include violations: 1
git restore --staged -- src/kernel/core/zz_neg.h; Remove-Item src/kernel/core/zz_neg.h
```

第 4 条的真实输出：

```
pre-commit: blocked: 1 issue(s)
  - src/kernel/** 有改动，但 tools/check-deps.ps1 报违规（exit=1）

    checked: 165 files, 499 quoted includes
    module include violations: 1

    --- violations (first 20) ---

    From To Header  File                              Line
    ---- -- ------  ----                              ----
    core fs fs/fs.h src/kernel/core/zz_neg_ci_probe.h    3
```

---

## 6. BOM 历史豁免（迁移中）

PS5.1 对**没有 BOM 的 UTF-8** 按系统 ANSI 码页解码，中文注释/字符串会变乱码，
严重时直接语法错误 —— 这就是"`build.ps1` 丢 BOM 导致解析失败"事故的成因（已发生 3 次）。

现状：仓库里有一批**历史上就没有 BOM 的脚本**，其中 16 个是纯 ASCII（PS5.1 能解析）。
`tools/ci.ps1` 把这份名单写死在 `$BomLegacy` 里（豁免，但会打印警告），因为
"补 BOM" 会动到不属于本次改动的文件；`tools/pre-commit.ps1` 对**暂存文件是严格模式**，
所以任何被改到的脚本都会在提交时被迫补上 BOM（棘轮）。

两个需要优先处理的文件（含非 ASCII 字节，有乱码风险）：

- `sign_image.ps1`（每次构建都会跑！）
- `tools/mkfat16.ps1`

迁移建议（单独一个 `[chore]` 提交，不要混进功能提交）：

```powershell
powershell -NoProfile -File tools/ci.ps1 -Quick -FixBom
git add -- <列出的文件>
# 之后可以把 tools/ci.ps1 里的 $BomLegacy 删空，并启用 -StrictBom
```

补 BOM 不会改变脚本行为（PS5.1 与 pwsh 都认 BOM）。

---

## 7. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `宿主 12345 端口已被占用` | 另一个会话的 `-Test` 在跑；等它结束，或看 `tools/session-status.ps1` |
| `QMP 端口 4691 已被占用` | 有残留 QEMU：`Get-Process qemu-system-i386 \| Stop-Process -Force` |
| `-Full` 报 `i686-elf toolchain` FAIL | `tools/i686-elf` 不存在（gitignored，需按 README 自备） |
| commit 没被拦住 | `git config --get core.hooksPath` 是否等于 `tools/githooks`；`git commit --no-verify` 会跳过 hook |
| hook 报 `bad interpreter: /bin/sh^M` | `tools/githooks/pre-commit` 被改成了 CRLF，且该环境是直接 exec（不是用 sh 跑）。改回 LF 即可；Git for Windows 用自己的 sh 执行 hook，实测 CRLF 也能跑（本仓库 `core.autocrlf=true`，checkout 出来会是 CRLF），但建议保持 LF |
| `ci.ps1 -Quick` 报某个 `.ps1` 无 BOM | `tools/ci.ps1 -FixBom`（或被改过的文件，见上节） |
| 想只查暂存区 | `powershell -NoProfile -File tools/pre-commit.ps1` |
