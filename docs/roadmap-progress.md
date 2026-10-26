# 路线图 1→4 进度

每个阶段完成门禁后追加一段：阶段、HEAD、检查项数、测试证据、遗留。
门禁定义见 `docs/ci.md`：`tools/ci.ps1 -Quick -StrictBom` +
连续 2 次 `build.ps1 -Test` 全 PASS + `check-deps` 0 违规。

## 阶段 0（起点，只记录）

- 起点 HEAD：`07e43d0`（v0.72.0 tag `646f81d`）
- 工作区干净，131 项检查，无 QEMU 运行时冲突
- 提交：无（只写本文档）

## 阶段 1：push + GitHub CI 实跑

- 状态：**blocked**（环境缺失，非代码问题）
- 事实：`git remote -v` 为空；`gh` 不在 PATH（`CommandNotFoundException`）；
  无任何 GitHub 凭据可验证。
- 处置：按任务要求不卡住，写 `docs/github-ci-blocked.md`（缺什么 + 用户需
  执行的两条命令），继续阶段 2。
- 未做：创建公开仓库、推送到未知地址、伪造 CI 结论。
- HEAD：`07e43d0` + 本文档所在提交
