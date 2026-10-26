# 并行批次集成记录（2026-10-27）

## 结果

- HEAD: `b79ca12`；回归 **225 项**（原 182，+43 条并行批次断言）。
- `ci.ps1 -Full -StrictBom`：**225/225 ×2 PASS**，静态/check-deps/单测（10 套 520 checks）全过。

## 并入的会话

| 会话 | 提交 | 内容 |
|---|---|---|
| sec | `22e799a` / `5a27b09` | shadow PBKDF2-SHA512（salt/10000 轮/迁移）、审计环 128 追加、getty 重启、会话槽复用 |
| fs | `8767724` / `4c90fb2` | XFS1 v6：inode_id/nlink 落盘、v5 兼容挂载、fsck 修复 |
| mm | `27ae52e` | per-process RSS（扫页表）、文件 MAP_PRIVATE mmap |
| net | `400500e` | TCP listen/accept、backlog、TIME_WAIT 复用、12 条自检 |
| pkg | `15d15b0` / `0186819` | 组件 ELF entry（哈希+验签+ring3 exec+回滚）、SYS_PKG(77) 用户 CLI |
| smp | `647f8a6` / `aab37ef` / `ed11e64` | per-CPU runqueue 锁、4 核验证、IOAPIC 全量路由 |
| eng | `a3144cc` / `887ccb0` | 全局测试锁、host 单测 path/PMM/FAT16（10 套 520 checks）、文档同步 |

## 集成期修的两个问题

1. **进程/程序表容量**：组件服务 + tcptest + pkgtest 把 `PROC_MAX=24` 占满，导致 shell `fork` 失败、sectest 连锁红。修复：`PROC_MAX 32`、`PROG_MAX 32`（`45e295e`）。
   - 注意：不可超过 32（`runq_mask` 是 32 位，48 会触发 `sched: dirty runq idx=32` panic）。
2. **TCP 自检时序**：负载下 `tcptest` 的最后两行晚于固定等待。修复：`build.ps1` 有界等待 60s（`1a6998e`/`b79ca12`）。

## 仍未做

- GitHub 远端 / Actions 实跑（`docs/github-ci-blocked.md`）；
- 0.73.0 的 8h 浸泡仍在旧 HEAD 上进行（约 08:31 结束）；
- 225 项这个 HEAD 还没有完整 8h 浸泡；
- 审计会话（攻击/bug 扫描）待启动。