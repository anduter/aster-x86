# 当前状态（唯一状态源）

> 更新：2026-10-31 22:55；HEAD `321b722`（分支 ng）。
> **其他文档不再重复“当前阶段/当前状态”，只链接本文。**

## 1. 当前计划

**D1 / M1（内核地基 + 预检）**。已完成 WP5-0a（ABI 1.1 + ENOSYS）、**WP5-0h（输入服务化 + PS/2 鼠标）**。
按代码审查顺序：**0c（用户 fault→信号）→ 8h 证据 → 0b（per-process uid/fd/cwd）→ 0d（cap/peer/token/spawn 授权）→ 0f（tick/时间）→ 0g（ring3 抢占）→ 0e（FPU/SIMD）**。
计划见 [`design/d1/d1-plan.md`](design/d1/d1-plan.md)、[`design/d1/wps.md`](design/d1/wps.md)、[`design/d1/risks.md`](design/d1/risks.md)。

**裁决**：WP5-0g 接受（ADR-021，内核协作 + ring3 tick 抢占）；WP5-0h 接受（ADR-022，displayd 输入认领 + 单一轮询点 + 鼠标）。总数 **33 WP**，M1 为 **0a…0h（8 项）**。

## 2. 当前 WIP

- **代码 WIP：无**（输入/鼠标修复已提交 `321b722`；预览用的 keep-alive/跳过 shell 补丁已从主树撤回，只存在于临时 worktree `D:\ng-preview4`）。
- 活跃会话：8h 浸泡（只读，QEMU PID 3420，`ng-d0-final` 镜像）。
- **WIP 上限：最多 2 个活跃 WP（1 个内核 + 1 个用户态/文档）**。

## 3. 当前门禁

- **8h：证据未闭合**。`ng-d0-final`（a44e896）上的 8h 于 21:04 起跑（QEMU 3420），尚无 8h 报告/claim；完成前 D0 只能写“**功能关闭、验收未闭合**”。
- **WP5-0h 门禁（已过）**：`unit-ng` **5594 / 0**、`build-ng` PASS、`run-ng` **161/161 行 + 76 正则**（含 mouse/claim 新证据）。
- `ci-ng -Quick`：干净 HEAD `7db7ac8` PASS；本次输入提交后**待复跑**（下一个动作）。
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
| `321b722` | **WP5-0h**：输入服务化（96/97）+ PS/2 鼠标 + 单一轮询点 + 清屏/光标 + WM 寿命/去抖 |
| `1d3d559` | tools：反馈录制（ffmpeg 录屏 + 键盘时间线） |
| `4e3c51e` | CURRENT 门禁证据 |
| `7db7ac8` | D1-D 裁决行统一（协作 + ring3 抢占） |
| `e0767fb` | 监工收口：CURRENT、32 WP、status 归档、.gitignore |
| `f9345b4` | WP5-0a：ABI 1.1（48–95）+ ENOSYS + abi-check |
| `a44e896` | **D0 冻结** tag `ng-d0-final` |
