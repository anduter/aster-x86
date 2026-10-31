# 设计文档索引

> 给实现会话的入口。先读这一页，再按顺序读三份基线。

## 阅读顺序

1. architecture.md —— 7 大类、域边界、故障等级、manifest、D0/D1/D2。
2. evolution.md —— ng 分支、E0–E5 阶段、工作包（WP）、门禁、第一周 checklist。
3. decisions.md —— 12 条 ADR，改架构前先看这里。
4. three-layer.md —— 历史迁移记录（已被取代，只看 P1–P4 的事实）。

## 当前状态

- 基线 HEAD：69ab006（305/305，32 位）。
- 策略：演进；main 冻结，ng 分支开发。
- 当前阶段：**E0 准备**。
- 下一步：WP0-1（tag/branch）、WP0-2（文档/所有权）、WP0-3（check-domains）、WP0-4（64 位工具链）。

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
| decisions.md | ADR |
| three-layer.md | 历史（已取代） |
| domains/ | 每域一页（待 WP0-2 创建） |
