# 设计文档索引

> 给实现会话的入口。先读这一页，再按顺序读三份基线。

## 阅读顺序

1. architecture.md —— 7 大类、域边界、故障等级、manifest、D0/D1/D2。
2. evolution.md —— ng 分支、E0–E5 阶段、工作包（WP）、门禁、第一周 checklist。
3. decisions.md —— ADR 集合（含 64 位相关决策），改架构前先看这里。
4. three-layer.md —— 历史迁移记录（已被取代，只看 P1–P4 的事实）。

## 当前状态

- main：冻结在 69ab006（旧 32 位，305/305）。
- 开发分支：ng（64 位内核）。
- 当前阶段：**E1 完成（WP1-1..WP1-8），待打 tag `ng-e1`**。
- 门禁：`tools/ci-ng.ps1` 一条命令跑 check-domains / check-deps / unit-ng /
  build-ng / run-ng / 旧 32 位 `build.ps1 -Test`，全绿输出
  `=== NG CI PASS ===`（`-Quick` 跳 QEMU/legacy）；CI 见
  `.github/workflows/ng.yml`。
- 出口证据：ring3 `ng: ring3 hello`；host 单测 **1085 checks, 0 failures**；
  旧 32 位 305/305；check-domains / check-deps 0 违规
  （`docs/claims/ng-WP1-8.md`）。
- 下一步：打 tag `ng-e1` → E2 / WP2-1（VFS/ramfs 移植）。

## 执行规则

- 一个 WP 一个会话；按路径所有权，不跨域改文件。
- 每个 WP 必须有出口测试 + docs/claims/ng-<WP>.md。
- 提交按路径限定，不用 git add -A。
- 范围变更先改 decisions.md，再改代码。
- D0 之前不加“顺手功能”；P5/P6 冻结。

## 文件清单

| 文件 | 作用 |
|---|---|
| architecture.md | 长期架构口径 |
| evolution.md | 迁移计划与工作包 |
| decisions.md | ADR（含 64 位相关决策） |
| three-layer.md | 历史（已取代） |
| memory-ng.md / paging-ng.md | WP1-2/1-4 内存与页表 |
| trap-syscall-ng.md | WP1-3 trap / syscall |
| sched-ng.md | WP1-5 线程与调度 |
| ipc-ng.md | WP1-6 句柄 / 端口 / IPC |
| elf-ng.md | WP1-7 ELF64 加载器 / 用户地址空间 / uaccess |
| domains/ | 每域一页 |
