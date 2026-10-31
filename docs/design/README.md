# 设计文档索引

> 给实现会话的入口。先读这一页，再按顺序读基线。

## 阅读顺序

1. architecture.md —— 7 大类、域边界、故障等级、manifest、D0/D1/D2。
2. evolution.md —— ng 分支、E0–E5 阶段、工作包（WP）、门禁、第一周 checklist。
3. decisions.md —— ADR 集合（含 64 位相关决策），改架构前先看这里。
4. three-layer.md —— 历史迁移记录（已被取代，只看 P1–P4 的事实）。
5. domains/_index.md —— 7 个域的职责/接口/失败/manifest/状态，以及**已知漂移**；
   实现会话按域读对应页面，再读接口的 `../abi/abi-1.0-draft.md`。

## 当前状态

- main：**代码**冻结在 69ab006（旧 32 位，305/305）；main 顶端 `d62bccb` 是
  7 域架构基线的设计文档提交，不含功能改动。
- 开发分支：ng（64 位内核）。
- 当前阶段：**E3 完成（WP3-0…3-5，tag ng-e3；S1 卡死为已知限制）**。E2
  （WP2-1…WP2-7）已全部完成并打 tag `ng-e2`（`d3a5671`）；E3 的前置
  **WP3-0（多进程用户态 + 跨进程 IPC）**、**WP3-1（display server，L2a）**、
  **WP3-2（shell/WM，L2b + 协议 v2）** 与 **WP3-3（session manager，L2c +
  协议 v3）** 落地：进程表/spawn/wait/kill、per-process 句柄表、ticket 跨进程
  IPC、shell 作为父进程；syscall 45/46/47（共享内存 + LFB 用户映射）、ring3
  `/bin/displayd`（surface/合成/焦点/输入转发）、L3 `/bin/demo`、QMP
  screendump 像素断言；ring3 `/bin/wm`（1280×32 面板/启动器/任务切换/全局
  热键）、协议 v2（LIST/LIST_REPLY/REGISTER_HOTKEY/WM_STATUS、CREATE title）；
  ring3 `/bin/sessiond`（应用生命周期/窗口注册表/角色令牌）、协议 v3
  （CREATE token/LIST token/FOCUS token/SESSION_STATUS）、WM 崩溃重启。
- 门禁：`tools/ci-ng.ps1` 一条命令跑 check-domains / check-deps / unit-ng /
  build-ng / run-ng / parity-ng / 旧 32 位 `build.ps1 -Test`，全绿输出
  `=== NG CI PASS ===`（`-Quick` 跳 QEMU/legacy）；CI 见
  `.github/workflows/ng.yml`。
- 出口证据：ring3 `ng: ring3 hello` + VFS/FAT/disk/svc demo +
  `ng: ring3 input ok` + 用户态 shell（`ng: shell ready` / 各命令证据行 /
  `ng: WP2-6 selftest PASS`）+ **parity 100/100**
  （`ng: parity 100/100 PASS`，host 95/95）+ **WP3-0 多进程**
  （`ng: proc spawn/wait/ipc cross/kill/as isolated` /`ng: WP3-0 selftest PASS`）
  + **WP3-1 display server**（`ng: displayd up` / `ng: composite ok (px=...)` /
  `ng: input route ok` / `ng: WP3-1 selftest PASS` + QMP 截图像素）
  + **WP3-2 shell/WM**（`ng: wm up` / `ng: wm panel ok (px=0x202838 at=0,0)` /
  `ng: wm launcher ok` / `ng: wm task switch ok` / `ng: wm launch ok` /
  `ng: WP3-2 selftest PASS` + 第二张 QMP 截图断言面板与 Demo 窗口）
  + **WP3-3 session manager**（`ng: sessiond up` /
  `ng: session app launch ok (pid=..)` / `ng: session registry ok (apps=..)` /
  `ng: role token ok` / `ng: session survives wm ok (app=.. alive=1 ...)` /
  `ng: session wm restart seen ok` / `ng: WP3-3 selftest PASS` + 第三张 QMP
  截图断言 WM 重启后 面板 + Demo 窗口）；
  host 单测 **5025 checks, 0 failures**；旧 32 位 305/305；
  check-domains / check-deps 0 违规（`docs/claims/ng-WP2-1.md` …
  `ng-WP2-7.md`、`ng-WP3-0.md`、`ng-WP3-1.md`、`ng-WP3-2.md`、
  `ng-WP3-3.md`）。
- 下一步：**WP3-4 SDK alpha + demo app**（外部可编译 GUI app）、**WP3-5 S1
  故障注入**（杀 shell/输入、应用存活、面板回来）待开始，见 evolution.md。
  域文档与已知漂移见 `domains/_index.md`；ABI 草案见 `../abi/abi-1.0-draft.md`。

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
| display-server-ng.md | WP3-1 display server（L2a）：syscall 45/46/47、共享内存 refcount、协议、surface/z-order/合成、输入路由、QMP 像素断言；WP3-2 追加协议 v2 与 L2b WM（第 13 节）；WP3-3 追加协议 v3 与 L2c 指针（第 14 节） |
| session-ng.md | WP3-3 session manager（L2c）：启动/端口发现、会话表与角色令牌、会话协议、LAUNCH/EXIT/KILL/心跳回收、注册表与 displayd LIST 同步、WM 重启退避、状态汇总、`.bigdata` 布局 |
| `tests/parity/parity_cases.h` / `parity_body.inc` | WP2-7 parity 100 项的唯一 case 表与共享断言体（kernel/host 同源） |
| domains/ | 每域一页：`l0-kernel.md`、`l1-services.md`、`l1d-drivers.md`、`l2-desktop.md`、`l3-apps.md`、`a-platform.md`、`b-lifecycle.md`；`_index.md` 定义 manifest 五元组与**已知漂移** |
| ../abi/abi-1.0-draft.md | native ABI 1.0 草案（syscall 表/句柄/错误码/指针与长度/版本策略；E4 冻结） |
