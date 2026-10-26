# 并行会话分工与规矩（2026-10-27）

目标：v0.73.0 之后并行推进 P0–P3（真实硬件除外），共享同一个工作树但**文件所有权互斥**。

## 所有权（只改自己的路径）

| 会话 | 路径 | 任务 |
|---|---|---|
| sec | `security/**`、`include/security.h`、`apps/getty.*`、`core/bootseq.c` | P0：shadow PBKDF2-SHA512（salt+迭代+迁移）、审计环扩容/追加语义、getty 重启 |
| fs | `fs/**`、`include/fs.h` | P0：XFS1 v6 硬链接持久化（nlink 落盘、fsck/兼容） |
| pkg | `pkg/**`、`user/**`、`include/pkg.h` | P0：组件 ELF entry（加载/验签/exec）、用户态 `pkg` CLI |
| net | `net/**`、`include/net.h`、`include/abi.h` | P0：TCP listen/accept（新 syscall 从 **77** 起）、TIME_WAIT 复用 |
| smp | `proc/**`、`include/proc.h`、`dev/intc/**` | P1：per-CPU 调度锁、>2 核验证、IOAPIC 全量路由 |
| mm | `mm/**`、`include/mm.h` | P1：per-process RSS、文件映射 mmap 基础 |
| eng | `tests/**`、`tools/**`、`docs/**`、`README*`、`include/kernel.h` | P2/P3：单测覆盖（路径/PMM/FAT16）、docs/README 同步、dmesg 环 |

## 硬规矩

1. **不改 `build.ps1`**（检查项由后续集成会话统一加）；新增行为用 `*_selftest` 打印行，并在 `docs/claims/<会话>.md` 写明期望断言行。
2. **`include/abi.h` 只有 net 可改**；其他会话需要新 syscall 时先在 `docs/claims` 提请求，不要动 abi.h。
3. **`include/kernel.h` 只有 eng 可改**。
4. 测试必须用 `tools\session-run.ps1 -Name <会话> -Test`（自动串行化 12345 端口与锁）；不要直接并发跑 `build.ps1 -Test`。
5. 共享树里别人可能正在编译：**编译失败若指向别人的文件，等 60s 重试**（最多 5 次），不要去修别人的模块。
6. 提交只用 `tools\session-commit.ps1 -Name <会话>`，路径限定；不 `git add -A`。
7. 每完成一项：`ci.ps1 -Quick` + `session-run -Test` 现有 182 项必须全 PASS；新增 selftest 行写入 `docs/claims/<会话>.md`。
8. 失败自行定位修复、重跑；连续 3 次修不好或需改别人文件时，发消息给父会话。
9. 真实硬件不做；GUI/Qt 冻结；不改 `tools/soak*.ps1`、`flake-hunt`。

## 集成

所有会话完成后由父会话统一：
- 把各 `docs/claims/*` 的期望行加进 `build.ps1` 检查项（182 → N）；
- 跑 `ci.ps1 -Full` 与完整 8h 浸泡；
- 合并冲突（若有）按模块归属处理。