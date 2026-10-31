# 当前状态（唯一状态源）

> 更新：2026-10-31 21:2x；HEAD `cc3f231`（分支 ng）。
> **其他文档不再重复“当前阶段/当前状态”，只链接本文。**

## 1. 当前计划

**D1 / M1（内核地基 + 预检）**。已完成 WP5-0a（ABI 1.1 + ENOSYS）。
按代码审查顺序：**0c（用户 fault→信号）→ 8h 证据 → 0b（per-process uid/fd/cwd）→ 0d（cap/peer/token/spawn 授权）→ 0f（tick/时间）→ 0g（ring3 抢占）→ 0e（FPU/SIMD）**。
计划见 [`design/d1/d1-plan.md`](design/d1/d1-plan.md)、[`design/d1/wps.md`](design/d1/wps.md)、[`design/d1/risks.md`](design/d1/risks.md)。

**WP5-0g 裁决：接受**（+ADR-021）：总数 31→**32**，M1 由 0a…0f 扩为 **0a…0g**（6→7）；口径为“**内核协作式 + ring3 tick 抢占**”，不做内核抢占/SMP 调度。

## 2. 当前 WIP

- **非监工所有，禁止提交/覆盖**：`src/ng/main.c`、`src/ng/sched.c`、`src/ng/syscall.{c,h}`、`src/ng/wp23*`、`src/drivers/input/ps2.c`、`src/desktop/display/displayd.c`（WP5-0g 抢占与输入改造的未提交改动）。
- 活跃会话：8h 浸泡（只读，QEMU PID 3420）；输入/鼠标会话已暂停，恢复前先登记 WP。
- **WIP 上限：最多 2 个活跃 WP（1 个内核 + 1 个用户态/文档）**。

## 3. 当前门禁

- **8h：证据未闭合**。`ng-d0-final`（a44e896）上的 8h 于 21:04 起跑（QEMU 3420），尚无 8h 报告/claim；完成前 D0 只能写“**功能关闭、验收未闭合**”。
- `ci-ng -Quick`：**干净 HEAD `cc3f231` 上 PASS**（4 passed / 3 skipped，20.5s，临时 worktree 已验证）；当前工作树含他人 WIP（`main.c`/`sched.c`/`syscall.*`/`ps2.c`/`displayd.c`），WIP 未提交前工作树门禁归 WIP 所有者。
- tag：`ng-e1`…`ng-e4`、`ng-d0-final`。

## 4. 禁止事项

- **M1 未过，不开 M2**（POSIX 库/网络/TLS/浏览器）。
- **RT 是独立产品线**，不进 D1；**32 位 legacy 树不改**；**浏览器 B0 属 D2**。
- 不启动新功能、不并行跑 QEMU、不改 ABI/内核语义、不 `reset`、不删 legacy。
- 不把审计报告 P0/P1 当“本周全做”；按 §1 顺序。

## 5. 变更规则

- 新 WP：**先改 `docs/design/d1/wps.md` + `docs/design/decisions.md`（ADR）**，再写代码。
- 每个 WP：出口断言 + `docs/claims/ng-<WP>.md` + 路径限定提交；不 `git add -A`。
- 同一时间最多 2 个活跃 WP；文档/规划可与内核并行。
- 状态只写本文；README/status/d1-plan 只链接。

## 6. 最近提交

| 提交 | 内容 |
|---|---|
| `cc3f231` | 代码审查 2026-10-31 + WP5-0g gate + D1 对齐 |
| `f9345b4` | WP5-0a：ABI 1.1（48–95）+ ENOSYS + abi-check |
| `a48c29b` | D1 决策（newlib/BearSSL/socket/协作+ring3 抢占） |
| `4e38954` | D1 规划（32 WP / M1–M6） |
| `a44e896` | **D0 冻结** tag `ng-d0-final` |
