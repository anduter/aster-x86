# ng-WP4-1：ABI 1.0 冻结（PASS）

## 结论

**PASS** —— E4 / WP4-1 出口达成：native ABI **1.0.0 冻结**。

- `docs/abi/abi-1.0.h`：机器可读冻结头。逐号镜像 **29** 个调用号
  （0/1、10–14、20–27、30–35、40–47）、**28** 个错误码、**64** 个关键常量、
  **4** 个结构体大小；句柄/ticket 编码与四类命名空间写在注释里。
- `docs/abi/abi-1.0.md`：定稿并标注 **FROZEN**。内容包含调用约定、四类
  命名空间（fd/handle/ticket/shm）、完整调用号表、错误码、指针/长度/对齐
  上限、只追加的版本策略、D0 兼容矩阵，以及冻结前 **13 项 + 审计新增 2 项**
  待决项的“保留/延后/已解决”结论。
- `tools/abi-check.ps1`：三方门禁。解析 ABI 头 / `sdk/include/aster/syscall.h` /
  `src/ng/syscall.h` 的调用号，比较 SDK 错误码、内核各模块错误码别名、
  64 个内核宏、4 个结构体的自然对齐布局，并检查冻结文档标记；任何不一致
  以非零退出。脚本带 UTF-8 BOM（Windows PowerShell 5.1 中文输出前提）。
- `docs/abi/abi-1.0-draft.md`：标记为历史草案，已被冻结版取代（保留推理记录）。

冻结后没有改任何 `src/**`、`sdk/include/**`、`build.ps1`、`tools/ci*.ps1`、
`abi.h`；没有新增 syscall/功能。

## 基线

- 仓库 `D:\OS_Project`，分支 `ng`，HEAD `9a6ab48`（`docs: README E3 status
  complete (ng-e3)`），tag `ng-e3`。
- 既有证据：`docs/claims/ng-WP1-8.md`、`ng-WP2-1.md` … `ng-WP2-7.md`、
  `ng-WP3-0.md` … `ng-WP3-5.md`。
- 冻结前草案：`docs/abi/abi-1.0-draft.md`（v0.1，基线 `c4efabc`）。

## 交付物

| 路径 | 内容 |
|---|---|
| `docs/abi/abi-1.0.h` | **新**：机器可读 ABI 1.0 冻结头（版本、调用号、错误码、句柄编码、IPC/VFS/进程/SHM/FB/auth/log/pkg 常量、结构体大小） |
| `docs/abi/abi-1.0.md` | **新**：冻结规范（29 调用号 / 四类命名空间 / 错误码 / 上限 / 版本策略 / 15 项冻结结论 / 兼容矩阵 / 校验） |
| `docs/abi/abi-1.0-draft.md` | **改**：顶部标注“历史草案，已被冻结版取代”；正文保留 |
| `tools/abi-check.ps1` | **新**：ABI 1.0 三方门禁（BOM；失败 exit 1） |
| `docs/claims/ng-WP4-1.md` | **新**：本文件 |

未修改 `src/**`、`sdk/include/**`、`build.ps1`、`tools/ci.ps1`、
`tools/ci-ng.ps1`、`abi.h`。

## 冻结结论（§10 的 15 项，摘要）

| # | 项 | 结论 |
|---|---|---|
| 1 | 未分配调用号返回 `-1` 而非 `ENOSYS` | **延后**：1.0 不定义未分配号行为；`ENOSYS(-38)` 数值保留 |
| 2 | IPC timeout 是未校准 TSC tick | **保留**：不透明 tick；只保证 0 与 `~0` |
| 3 | ticket 是 bearer token、无 cap | **保留**：`send/reply` 不校验调用者；cap 留 1.1 |
| 4 | 所有进程 uid=0 | **保留**：1.0 权限模型 = root-only |
| 5 | 没有 ABI 版本查询 | **延后**：版本在编译期宏；不分配调用号 |
| 6 | 没有机器可读 ABI 头 | **已解决**：`abi-1.0.h` + `abi-check` |
| 7 | ring3 #PF/#GP 仍 panic | **延后（已知限制）**：ABI 只承诺 uaccess 的 `EFAULT` |
| 8 | `write` 命名冲突 | **保留**：0 = 控制台，22 = fd；机器头已区分 |
| 9 | 结构体对齐未强制 | **保留（调用方责任）**：未对齐是未定义行为 |
| 10 | 无 mmap/mprotect/lseek/dup/pipe/socket/sleep | **延后**：D1 POSIX 用户态 shim |
| 11 | `shm` 无 `SHM_FREE` | **保留**：最后映射解除/进程退出回收；无显式 free |
| 12 | fd 表全局一份 | **保留**：1.0 全局 VFS ctx；每进程 fd 留 1.1 |
| 13 | `spawn` path/argv 静默截断 | **已解决**：超长统一 `ENAMETOOLONG` |
| 14 | `shm_alloc` 对 `&handle` 按 8 B 校验（语义 4 B） | **保留（记录实现细节）**：1.1 修正校验长度，不改参数含义 |
| 15 | SDK 未导出 `ESRCH`/`EINTR`/`ECHILD` | **保留**：ABI 头冻结 `-3/-4/-10`；SDK 只追加宏 |

完整表格与理由见 `docs/abi/abi-1.0.md` §10。

## 证据（2026-10-31）

### abi-check

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi-check.ps1
=== abi-check (WP4-1: ABI 1.0 freeze) ===
repo: D:\OS_Project
abi:  D:\OS_Project\docs/abi/abi-1.0.h
sdk:  D:\OS_Project\sdk/include/aster/syscall.h
src:  D:\OS_Project\src/ng/syscall.h
[ok] 29 syscalls agree (ABI/SDK/kernel): 0/1, 10-14, 20-27, 30-35, 40-47
[ok] 28 error codes frozen; SDK mirrors 25; 73 kernel aliases agree
[ok] 64 ABI constants match 64 kernel-source checks
[ok] struct layouts: vfs_stat=24, vfs_dirent=80, ipc_msg_hdr=16, ds_fb_info=48
[ok] docs/abi/abi-1.0.md is marked frozen
[ok] ABI version 1.0.0 (0x00010000)
abi-check: PASS (29 syscalls, 28 errors, 64 constants, 4 struct sizes)
```

### 文件/编码

```text
tools/abi-check.ps1 首 3 字节：ef bb bf（UTF-8 BOM）
docs/abi/abi-1.0.h / abi-1.0.md：UTF-8 无 BOM（与仓库 docs 一致）
```

### 未执行/边界

- 没有跑 QEMU、`run-ng`、`unit-ng`、`ci-ng`、`build.ps1 -Test`：本 WP 只改
  文档 + 新门禁，不触碰运行时；构建/发布在 WP4-3 里由 `release-ng.ps1` 跑。
- 没有改 SDK 头：`abi-1.0.h` 是规范镜像，不是 SDK 的一部分；SDK 仍只发
  `sdk/include/aster/syscall.h`。
- 没有新增 syscall、没有修 ABI 1.0 的延后项（明确不在本 WP）。

## 遗留（按 WP 边界）

- 15 项冻结结论中“延后/保留”的语义仍是 D0 已知限制；尤其：未分配号、
  用户态 fault 隔离、cap、每进程 uid/fd、`SHM_FREE`、运行时版本查询。
- `docs/abi/abi-1.0-draft.md` 只做历史标记，未逐条回填冻结结论（避免出现
  两份互相竞争的规范）。
- SDK 的 `ESRCH/EINTR/ECHILD` 宏、`shm_alloc` 的 8 B 校验属后续 SDK/内核
  小修，不在本 WP（不改 `sdk/**`/`src/**`）。

## 出口

```text
powershell -File tools/abi-check.ps1     # PASS (29 syscalls, 28 errors, 64 constants, 4 structs)
powershell -File tools/release-ng.ps1    # WP4-3：PASS（含 abi-check）
```
