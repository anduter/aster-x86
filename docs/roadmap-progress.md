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

## 阶段 2：组件模型 MVP（签名 .pkg + `_pkgd` + echosvc 组件化）

- 状态：**完成**
- 新增：`src/kernel/include/pkg.h`、`src/kernel/pkg/{pkg_verify,pkgd,pkg_selftest}.c`、
  `tools/mkpkg.ps1`、`tools/pkg-verify.ps1`、`packages/{echosvc,needsdep,broken,helloapp}/*.conf`；
  `build.ps1` 打包 + 内建包目录 + 15 项检查；`supervisor` 支持 `start:/stop:`；
  `prog_init` 注册 `pkgd`/`pkgtest`，`bootseq` 起 `_pkgd` 与回归线程。
- 设计文档：`docs/pkg.md`（容器布局、manifest、目录/DB、安装回滚、服务协议、限制）。
- 检查项：**131 → 146**。
- 测试证据：
  - 单次 `build.ps1 -Test`：`=== TEST PASS (146/146) ===`（`serial-pkg3.log`）
  - 8 条要求的用例全部覆盖并 PASS：合法安装、坏签名、缺依赖、失败回滚、
    卸载运行中服务、重复安装、普通用户越权、用户应用装 `~/Apps`；
    另有 7 项（服务注册/落盘/通知 supervisor/校验/list/协议/重装上线）。
- 遗留：
  1. `entry` 仍指向内核内建程序（echosvc 未拆 ring3 ELF）；
  2. 无用户态 `pkg` 命令（只有内核 API + `_pkgd` 协议）；
  3. 组件 DB/目录落盘依赖显式 `sync`（安装发生在 `disk_selftest` 之后）；
  4. 同名组件不支持多版本/升级回滚。
- 门禁：见下方"阶段 2 门禁"小节。

