# ng-WP0-2：域文档补全 + native ABI 1.0 草案（PASS，仅文档）

## 结论

**PASS（仅文档）** —— E0/WP0-2 的“域目录 + 文档 + 所有权”补齐，并把 native ABI
1.0 从 `src/ng/syscall.h` 的实际实现提取成可评审草案：

- `docs/design/domains/` 新增 **7 篇域文档 + 1 篇索引**：每页固定结构
  （职责 / 不做什么 / 接口 / 失败等级与恢复 / manifest 五元组 / 当前实现状态 /
  待办），覆盖 L0、L1、L1D、L2、L3、A、B；
- `docs/abi/abi-1.0-draft.md`：syscall 表 **0/1、10–14、20–27、30–35、40–47**
  （按 `src/ng/syscall.h` 逐号核对）、句柄/ticket/fd 语义、错误码表、用户指针/
  长度/对齐限制、只追加的版本策略；显式标注“草案，E4 冻结”，并与旧 32 位
  `abi.h`（int 0x81 / 0x80）区分；
- `docs/design/README.md`：文件清单补 `domains/`、`../abi/`；当前阶段改为
  **E3 进行中（WP3-0…WP3-2 完成，WP3-3 进行中）**；main 描述改为“代码冻结
  69ab006、顶端 d62bccb 为设计文档提交”；
- `docs/design/domains/_index.md` 记录 **D01–D18 已知漂移**（架构快照过期、
  tag 状态、manifest 五元组未实现、L1/L3 故障隔离不成立、watchdog/鼠标缺位、
  hwcap 只在旧树、log errno、新旧编号冲突、ABI 头缺失、WP3-3 未提交、
  uid 未生效、SHM_FREE 缺失、S1 未验收、未分配调用号返回 -1、spawn 路径截断）；
- 本次**没有修改任何代码/工具/构建/`abi.h`/`src/**`**；没有跑 QEMU、
  `build.ps1 -Test`、`unit-ng`、`ci-ng`。

## 基线

- 分支 `ng`，HEAD `c4efabc`（`[ng][WP3-2]`）；tag `ng-e1=69d84ca`、
  `ng-e2=d3a5671` 已在本地存在。
- 上位文档：`docs/design/architecture.md`、`evolution.md`、`decisions.md`、
  `README.md`、`docs/design/*-ng.md`。
- 证据：`docs/claims/ng-WP1-1.md` … `ng-WP3-2.md`（共 19 份，WP0-3/WP0-4 另读）。
- 工作区另有 WP3-3 会话的未提交 `src/desktop/**` 改动；本文档一律按
  **已提交的 v2 协议**写，不引用工作区 v3/session_*。

## 交付物

| 路径 | 内容 |
|---|---|
| `docs/design/domains/_index.md` | 域/目录/WP 映射；manifest 五元组词汇；工作包证据表；已知漂移；维护规则 |
| `docs/design/domains/l0-kernel.md` | boot/arch/mm/sched/IPC/进程机制；syscall 0/1/10–14/40–47；fatal |
| `docs/design/domains/l1-services.md` | VFS/ramfs/FAT16/auth/log/pkg；syscall 20–27/31–35；core→实际 fatal |
| `docs/design/domains/l1d-drivers.md` | PCI/VBE/fb/PS2/ATA；ring0 现状；readkey/fb_map；core/optional |
| `docs/design/domains/l2-desktop.md` | L2a displayd + L2b wm；协议 v2；L2c 进行中；S1 未验收 |
| `docs/design/domains/l3-apps.md` | shell/demo/proc/hello；进程入口 ABI；isolated 的边界 |
| `docs/design/domains/a-platform.md` | ABI 1.0 草案/SDK/POSIX/包格式/版本策略 |
| `docs/design/domains/b-lifecycle.md` | 身份/沙箱/签名更新/审计/CI/发布/硬件矩阵 |
| `docs/abi/abi-1.0-draft.md` | native ABI 1.0 草案 v0.1（E4 冻结） |
| `docs/design/README.md`（改） | 阅读顺序、当前阶段、文件清单 |
| `docs/claims/ng-WP0-2.md`（新） | 本 claim |

## 核对方式（只读）

1. 逐号读 `src/ng/syscall.h`（0/1、10–14、20–27、30–35、40–47），并与
   `syscall_dispatch()` 的 case 对照；未分配号按“保留、不复用”写。
2. 读 `src/ng/{ipc.h,proc.h,shm.h,fbmap.h,uaccess.h}`、`src/services/fs/include/vfs.h`、
   `src/services/{auth,log,pkg}` 与 `src/ng/*_syscall.c`，核对长度/错误码；
   `stat/listdir` 结构体按 `_Static_assert`/字段顺序写。
3. 读 `docs/claims/ng-WP1-1.md` … `ng-WP3-2.md` 的结论与“未做/遗留”段；
   域文档里的“已实现”全部指到对应 WP；没有 claim 的能力写“设计/待办”。
4. 与 `architecture.md` §3/§4 对表：失败等级、manifest 字段、不变量；
   不一致或未实现的条目进 `_index.md` §5“已知漂移”，不在域页里美化。
5. 旧 `abi.h` 只用于“新旧区分”对照；不修改、不复制其编号。

## 出口 / 验证

- 文档结构：7 页均有固定 7 节；`_index.md` 含“已知漂移”。
- 路径/链接：域页相对链接指向 `../architecture.md`、`../../claims/ng-WP*.md`、
  `../../abi/abi-1.0-draft.md`；README 指向 `domains/` 与 `../abi/`。
- 文本检查：`git diff --check`（提交前执行；只检查空白/冲突标记）。
- **未执行**：QEMU、`run-ng`、`build-ng`、`unit-ng`、`ci-ng`、`build.ps1 -Test`
  （按 WP0-2 约束：只写文档，不跑运行时门禁）。
- 未修改 `src/**`、`tools/**`、`build.ps1`、`abi.h`、`.github/**`。

## 未做 / 遗留（按 WP 边界）

- **ABI 1.0 未冻结**：草案仍留 13 个冻结前决策（未分配号 `-1`、timeout 单位、
  cap/ticket、uid、版本查询、ABI 头、用户 fault、`write` 命名、对齐、
  `mmap` 等、`SHM_FREE`、每进程 fd、spawn 超长截断）。
- **manifest 五元组未实现**：`pkg_manifest_parse()` 只认 5 个 key；D04 未解决，
  域页里的五元组明确标为“设计值”。
- **D01–D18 未修代码**：本次只记录，不改 `architecture.md`/`evolution.md`/
  代码；F01/F02 是本次已修正的 README 文本。
- **WP3-3/WP3-4/WP3-5**：session manager、SDK alpha、S1 故障注入不在本 WP；
  WP3-3 工作区改动未提交，本文不引用。
- **旧 32 位**：仍冻结在 main `69ab006`，305/305；ng 不合并旧 ABI。
