# 设计文档索引

> 给实现会话的入口。先读这一页，再按顺序读基线。

## 阅读顺序

1. architecture.md —— 7 大类、域边界、故障等级、manifest、D0/D1/D2。
2. evolution.md —— ng 分支、E0–E5 阶段、工作包（WP）、门禁、第一周 checklist。
3. decisions.md —— ADR 集合（含 64 位相关决策），改架构前先看这里。
4. three-layer.md —— 历史迁移记录（已被取代，只看 P1–P4 的事实）。
5. domains/_index.md —— 7 个域的职责/接口/失败/manifest/状态，以及**已知漂移**；
   实现会话按域读对应页面，再读接口的 `../abi/abi-1.0.md`。
6. d1/d1-plan.md —— D1（可用桌面）的目标拆解、依赖/顺序、M1–M6、范围围栏；
   再读 d1/wps.md（32 个 WP5 子包）、d1/risks.md（风险/回退）、
   d1/posix-profile.md（POSIX→native 映射，WP5-1a 的设计输入）。

## 当前状态

见 [../CURRENT.md](../CURRENT.md)（唯一状态源）。

## 执行规则

- 一个 WP 一个会话；按路径所有权，不跨域改文件。
- 每个 WP 必须有出口测试 + docs/claims/ng-<WP>.md。
- 提交按路径限定，不用 git add -A。
- 范围变更先改 decisions.md，再改代码。
- D0 之前不加“顺手功能”；P5/P6 冻结。
- D1 范围以 `d1/d1-plan.md` §6 为准：不做 fork/COW、动态链接、抢占式
  时间片/SMP 调度、安装器/更新（后者建议 D1.1，待负责人确认）。

## 文件清单

| 文件 | 作用 |
|---|---|
| architecture.md | 长期架构口径 |
| evolution.md | 迁移计划与工作包 |
| decisions.md | ADR（含 64 位相关决策） |
| d1/d1-plan.md | D1 计划：目标拆解/依赖顺序/M1–M6/范围围栏/待确认决策 |
| d1/wps.md | D1 工作包：WP5-0…WP5-6，32 个子包（范围/允许路径/出口/依赖） |
| d1/risks.md | D1 风险、未知数、回退方案与“先验证什么” |
| d1/posix-profile.md | POSIX Profile 1.0 设计输入：syscall/errno/结构体/flags/上限 |
| three-layer.md | 历史（已取代） |
| memory-ng.md / paging-ng.md | WP1-2/1-4 内存与页表 |
| trap-syscall-ng.md | WP1-3 trap / syscall |
| sched-ng.md | WP1-5 线程与调度 |
| ipc-ng.md | WP1-6 句柄 / 端口 / IPC |
| elf-ng.md | WP1-7 ELF64 加载器 / 用户地址空间 / uaccess |
| vfs-ng.md | WP2-1 VFS / ramfs / syscall 20..27 |
| fat16-ng.md | WP2-2 块设备抽象 / FAT16 / 镜像与 RAM 盘 |
| display-input-ng.md | WP2-3 PCI/VBE/帧缓冲控制台 / VGA 文本兜底 / PS/2 / readkey |
| block-ng.md | WP2-4 ATA PIO / blk_env 适配 / 8MB 磁盘镜像 / /mnt/disk |
| auth-log-pkg-ng.md | WP2-5 auth（salt+SHA-256 迭代）/ 64 KiB log 环 / pkg 清单与校验 / syscall 31..35 |
| shell-ng.md | WP2-6 用户态 shell（L3）：命令/行编辑/重定向/cwd/证据行/run-ng 注入 |
| proc-ng.md | WP3-0 进程模型：pid/进程表、spawn/wait/kill、per-process 句柄、ticket 跨进程 IPC |
| display-server-ng.md | WP3-1 display server（L2a）：syscall 45/46/47、共享内存 refcount、协议、surface/z-order/合成、输入路由、QMP 像素断言；WP3-2 追加协议 v2 与 L2b WM（第 13 节）；WP3-3 追加协议 v3 与 L2c 指针（第 14 节） |
| session-ng.md | WP3-3 session manager（L2c）：启动/端口发现、会话表与角色令牌、会话协议、LAUNCH/EXIT/KILL/心跳回收、注册表与 displayd LIST 同步、WM 重启退避、状态汇总、`.bigdata` 布局 |
| `tests/parity/parity_cases.h` / `parity_body.inc` | WP2-7 parity 100 项的唯一 case 表与共享断言体（kernel/host 同源） |
| domains/ | 每域一页：`l0-kernel.md`、`l1-services.md`、`l1d-drivers.md`、`l2-desktop.md`、`l3-apps.md`、`a-platform.md`、`b-lifecycle.md`；`_index.md` 定义 manifest 五元组与**已知漂移** |
| ../abi/abi-1.0.md | **native ABI 1.0 冻结规范**（v1.0.0；调用约定/四类命名空间/错误码/上限/版本策略/冻结结论） |
| ../abi/abi-1.0.h | ABI 1.0 机器可读冻结头（`tools/abi-check.ps1` 三方比对） |
| ../audit/ng-code-review-2026-10-31.md | 代码审查：P0/P1/P2 缺陷、证据、修复顺序（静态审查，基线 a48c29b） |
| ../abi/abi-1.0-draft.md | 历史草案（已被冻结版取代，仅供追溯） |
| ../release/d0.md | D0 本地发布说明（范围/出口/已知限制/安装运行/校验） |
