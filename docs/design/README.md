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
- 当前阶段：**E3 进行中（WP3-0 完成）**。E2（WP2-1…WP2-7）已全部完成并打
  tag `ng-e2`（`d3a5671`）；E3 的前置 **WP3-0（多进程用户态 + 跨进程 IPC）**
  落地：进程表/spawn/wait/kill、per-process 句柄表、ticket 跨进程 IPC、
  shell 作为父进程。
- 门禁：`tools/ci-ng.ps1` 一条命令跑 check-domains / check-deps / unit-ng /
  build-ng / run-ng / parity-ng / 旧 32 位 `build.ps1 -Test`，全绿输出
  `=== NG CI PASS ===`（`-Quick` 跳 QEMU/legacy）；CI 见
  `.github/workflows/ng.yml`。
- 出口证据：ring3 `ng: ring3 hello` + VFS/FAT/disk/svc demo +
  `ng: ring3 input ok` + 用户态 shell（`ng: shell ready` / 各命令证据行 /
  `ng: WP2-6 selftest PASS`）+ **parity 100/100**
  （`ng: parity 100/100 PASS`，host 95/95）+ **WP3-0 多进程**
  （`ng: proc spawn/wait/ipc cross/kill/as isolated` /`ng: WP3-0 selftest PASS`）；
  host 单测 **4043 checks, 0 failures**；旧 32 位 305/305；
  check-domains / check-deps 0 违规（`docs/claims/ng-WP2-1.md` …
  `ng-WP2-7.md`、`ng-WP3-0.md`）。
- 下一步：E3 主体（WP3-1 display server / WP3-2 shell-WM / WP3-3 session
  manager / WP3-4 SDK alpha），见 evolution.md。

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
| vfs-ng.md | WP2-1 VFS / ramfs / syscall 20..27 |
| fat16-ng.md | WP2-2 块设备抽象 / FAT16 / 镜像与 RAM 盘 |
| display-input-ng.md | WP2-3 PCI/VBE/帧缓冲控制台 / VGA 文本兜底 / PS/2 / readkey |
| block-ng.md | WP2-4 ATA PIO / blk_env 适配 / 8MB 磁盘镜像 / /mnt/disk |
| auth-log-pkg-ng.md | WP2-5 auth（salt+SHA-256 迭代）/ 64 KiB log 环 / pkg 清单与校验 / syscall 31..35 |
| shell-ng.md | WP2-6 用户态 shell（L3）：命令/行编辑/重定向/cwd/证据行/run-ng 注入 |
| proc-ng.md | WP3-0 进程模型：pid/进程表、spawn/wait/kill、per-process 句柄、ticket 跨进程 IPC |
| `tests/parity/parity_cases.h` / `parity_body.inc` | WP2-7 parity 100 项的唯一 case 表与共享断言体（kernel/host 同源） |
| domains/ | 每域一页 |
