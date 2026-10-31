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
- 开发分支：ng（64 位内核）。阶段 tag：`ng-e1`、`ng-e2`、`ng-e3`。
- 当前阶段：**E4 收口（D0）**。
  - **WP4-1 完成**：native ABI **1.0 冻结**（v1.0.0）——
    `docs/abi/abi-1.0.md` 定稿 + `docs/abi/abi-1.0.h` 机器可读头；
    调用号/错误码/四类命名空间/上限/版本策略冻结，15 项冻结前待决项给出
    “保留/延后/已解决”结论；`tools/abi-check.ps1` 对 ABI 头、SDK 头、
    `src/ng/syscall.h` 及内核常量做三方比对。
  - **WP4-2 完成**：外部开发者测试（另一会话提交 `3328b8e`）——
    `sdk/examples/hello_d0`（480×300 GUI、计数/交互/8s 空闲退出）+
    `tools/d0-demo.ps1` QEMU/QMP 证据；受限口径：不改仓库能写/能跑，
    但**没有产品部署路径**（sessiond/LAUNCH 拉起）仍需安装器/包启动。
  - **WP4-3 完成**：D0 本地发布包 —— `tools/release-ng.ps1` 一条命令跑
    `abi-check` + `build-ng`，生成 `dist-ng/release/d0/`（ELF、disk.img、
    SDK 头、示例、ABI 1.0、文档、`RELEASE-NOTES.md`、`SHA256SUMS`）并逐文件
    校验；只本地，不 push、不签名、不打 tag。说明见 `../release/d0.md`。
  - **E4 未整体关闭**：WP4-4（1 小时 smoke）尚无证据；D0 总出口以
    `architecture.md` §10 为准。
- E1/E2/E3 出口（已完成）：
  - E1：64 位 long mode、页表、trap/syscall、PMM/heap、调度、IPC/句柄、
    ELF64 + ring3 hello；host 单测；`tools/ci-ng.ps1` 一条命令门禁。
  - E2：VFS/ramfs、FAT16、display/input、ATA、auth/log/pkg、64 位 shell；
    parity **100/100**（host 95/95）；旧 32 位 305/305。
  - E3：L2a displayd、L2b WM、L2c sessiond、SDK alpha + GUI demo；S1
    故障注入 3 PASS + 1 KNOWN（WM 死循环）；QMP 截图像素证据。
- 当前门禁：
  - `tools/ci-ng.ps1`：check-domains / check-deps / unit-ng / build-ng /
    run-ng / parity-ng / 旧 32 位 `build.ps1 -Test`，全绿输出
    `=== NG CI PASS ===`；CI 见 `.github/workflows/ng.yml`。
  - `tools/abi-check.ps1`：ABI 1.0 冻结门禁（29 syscalls / 28 errors /
    64 constants / 4 struct sizes）。
  - `tools/sdk-check.ps1`：SDK 头/示例语法 + 237 host checks。
  - `tools/release-ng.ps1`：D0 发布包 + SHA256 自检。
- 下一步：**WP4-4 1h smoke**（无 panic/停滞），然后 E4 出口评审；
  D1 见 `evolution.md`。
- ABI 规范：冻结版 `../abi/abi-1.0.md`；机器头 `../abi/abi-1.0.h`；
  历史草案 `../abi/abi-1.0-draft.md`（已被取代，仅供追溯）。
- D0 发布：`../release/d0.md`；本地产物 `dist-ng/release/d0/`（gitignore）。

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
| ../abi/abi-1.0.md | **native ABI 1.0 冻结规范**（v1.0.0；调用约定/四类命名空间/错误码/上限/版本策略/冻结结论） |
| ../abi/abi-1.0.h | ABI 1.0 机器可读冻结头（`tools/abi-check.ps1` 三方比对） |
| ../abi/abi-1.0-draft.md | 历史草案（已被冻结版取代，仅供追溯） |
| ../release/d0.md | D0 本地发布说明（范围/出口/已知限制/安装运行/校验） |
