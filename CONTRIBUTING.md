# 贡献指南（Contributing）

感谢你对 Aster / x86 OS 的兴趣。这个项目是一个从引导扇区写起的 32 位教学操作系统，
目标是"能读懂、能验证、能复现"。提交请优先保证**可复现**与**回归不退化**。

## 目录

- [开发环境](#开发环境)
- [构建与测试](#构建与测试)
- [提交规范](#提交规范)
- [DCO（签名）](#dco签名)
- [代码风格](#代码风格)
- [Pull Request 清单](#pull-request-清单)
- [安全](#安全)

## 开发环境

| 依赖 | 说明 |
|---|---|
| Windows + PowerShell 5.1 | `build.ps1` / `tools/*.ps1` 的主要验证环境 |
| `nasm` | 引导扇区、loader、内核汇编 |
| `i686-elf` 交叉工具链 | 放在 `tools/i686-elf/`（约 3.5GB，**不随仓库分发**），或用 PATH 上的 |
| `qemu-system-i386` | 跑 `build.ps1 -Test` |
| OpenSSL 1.1+ | 镜像签名（Windows 可用 Git for Windows 自带的一份） |
| gcc（host） | 可选，仅 `tools/unit.ps1` 跑 host 单元测试；优先用 `tools/w64devkit/bin/gcc.exe` |

获取工具链与首次构建见 `README.md` 的"快速开始 / 自带交叉工具链"。

**签名私钥不在仓库里。** 首次构建前先生成密钥对：

```powershell
powershell -NoProfile -File tools/gen-key.ps1
```

私钥写入 `keys/shell_private.pem`（被 `.gitignore` 忽略），公钥 `keys/shell_public.pem` 可提交。
细节与轮换流程见 `keys/README.md`。

## 构建与测试

```powershell
# 构建 + 启动 QEMU 自检（当前 112 项，全部 PASS 才算过）
powershell -NoProfile -File build.ps1
powershell -NoProfile -File build.ps1 -Test

# 提交卫生（秒级）：工作区干净、BOM/解析、无构建产物、模块边界、单元测试
powershell -NoProfile -File tools/ci.ps1 -Quick -StrictBom

# 完整验证：构建 + 连续 2 次 -Test（慢，需要 QEMU 与工具链）
powershell -NoProfile -File tools/ci.ps1 -Full

# 只跑 host 单元测试（不启动 QEMU）
powershell -NoProfile -File tools/unit.ps1
```

**测试要求**

1. 新功能/行为变化：在 `build.ps1` 的检查列表里加一条 `-Test` 检查项（或在
   `tests/unit/` 加 host 单元测试），并在 PR 描述里说明检查项数从 N 变成 N+1。
2. Bug 修复：先加一个能复现的检查（红），再修（绿），不要只改代码不加检查。
3. 不允许减少现有检查项；`-Test` 必须连续 2 次稳定 PASS（并行会话抢 QEMU/端口时先看
   `docs/parallel-sessions.md`）。
4. 改到 `src/kernel/**` 的 include 关系时，`tools/check-deps.ps1` 必须 0 违规。

**PowerShell 脚本必须有 UTF-8 BOM**（`EF BB BF`）。PS5.1 会把无 BOM 的 UTF-8 当 ANSI 解码，
中文注释会变乱码甚至语法错误。`pre-commit` 会拦截；批量补齐用：

```powershell
powershell -NoProfile -File tools/ci.ps1 -Quick -FixBom
```

安装提交钩子（每个克隆一次）：

```powershell
powershell -NoProfile -File tools/install-hooks.ps1
```

## 提交规范

- 提交信息格式：`[模块] 简短祈使句描述`
  - 模块示例：`[kernel]` `[fs]` `[net]` `[proc]` `[abi]` `[accounts]` `[docs]` `[eng]` `[test]`
  - 描述用现在时、说"做什么"而不是"改了什么文件"；正文里写动机、影响面、验证命令。
- **一个提交一件事**：不要把 BOM 修复、格式化、重命名混进功能提交。
- 提交信息里引用验证结果，例如：
  `-Test 112/112 PASS x2；ci.ps1 -Quick -StrictBom PASS`。
- 并行开发时用 `tools/session-commit.ps1 -Name <模块> -Message "..." -Paths a,b,c`，
  它只提交指定路径，避免把别人 staged 的文件扫进来。
- 不要 `git add -A`；不要提交 `dist/` `obj/` `.obj-*` `.dist-*` 日志或镜像。
- 不要提交任何私钥、token、口令（`tests/unit/fixtures/` 里只允许公开数据，如签名/模数）。

## DCO（签名）

本项目采用 [Developer Certificate of Origin 1.1](https://developercertificate.org/)。
每个提交必须带 `Signed-off-by`，表示你有权按本仓库许可证（Apache-2.0）提交该贡献：

```
Signed-off-by: Your Name <you@example.com>
```

用 `git commit -s` 自动附加（名字/邮箱需与 git 配置一致）。
没有 `Signed-off-by` 的 PR 会被要求补签（`git rebase --signoff`）。

## 代码风格

- C：C99 + GNU 扩展，内核侧 freestanding，**不依赖 libc**；错误码用 `core/error.h` 的 `E_*`。
- 4 空格缩进；`if`/`for` 一律带花括号；避免超过 ~100 列的代码行。
- 内核与用户态共享的结构体/常量只能加在 `src/kernel/include/abi.h`，且必须同步
  `_Static_assert` 与 `docs/api.md` / `docs/native-abi.md` 的版本说明。
- 注释写"为什么"，不要复述代码；中文注释可以，但 `.ps1` 记得 BOM。
- 新增文件若属于内核，保持在对应模块目录内，不要跨模块 include 内部头文件
  （见 `docs/modules.md` 与 `tools/check-deps.ps1`）。
- 文档与代码同一次 PR 更新；用户可见行为变化必须改 `README.md` / `docs/**`。

## Pull Request 清单

- [ ] `tools/ci.ps1 -Quick -StrictBom` PASS
- [ ] `build.ps1` 通过；`build.ps1 -Test` 连续 2 次全 PASS（项数不回退）
- [ ] 新增/修改行为有对应检查项或 `tests/unit/` 用例
- [ ] 每个提交有 `Signed-off-by`（DCO），一个提交一件事
- [ ] 没有私钥/口令/日志/构建产物；`git status` 干净
- [ ] 文档（`README.md`、`docs/**`）已同步；`docs/ci.md` 的参数表如变动已更新
- [ ] PR 描述写清：动机、影响面、验证命令与输出、未完成项

## 安全

发现安全问题**不要开公开 issue**，流程见 `SECURITY.md`。
涉及签名密钥的改动（`sign_image.ps1`、`keys/**`、`trust.c`）请在 PR 里说明轮换与兼容性影响。
