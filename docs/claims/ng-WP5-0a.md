# ng-WP5-0a：ABI 1.1 号段 + ENOSYS + abi-check（PASS）

## 结论

**PASS** —— D1 / WP5-0a 出口达成：

- ABI **1.1 只追加**：`docs/abi/abi-1.1.h`（机器可读，`#include "abi-1.0.h"`）+
  `docs/abi/abi-1.1.md`（**DRAFT**）冻结新号段 **48–95（48 个）** 与分配规则；
  1.0 的 29 个调用号、28 个错误码、64 个常量、4 个结构体布局**一个都没改**。
- 内核未分配号 / 1.1 已分配未实现号统一返回 **`-ENOSYS(-38)`**（不再 `-1`）；
  0/1、10–14、20–27、30–35、40–47 的语义与错误码不变。
- `tools/abi-check.ps1` 同时校验 **1.0（冻结）** 与 **1.1（号段/版本/ENOSYS）**；
  任一不一致 exit 1，负向测试（改坏号）实测 exit 1。
- host 单测覆盖：未知号/未实现号 = `-ENOSYS`，1.0 号的坏参数仍返回原错误码；
  `unit-ng` **5357 checks, 0 failures**。
- `abi-check`：**PASS (29+48 syscalls, 28 errors, 64 constants, 4 struct sizes)**。

**范围纪律**：只改本 WP 允许路径；未实现 48–95 中任何调用，未做 per-process fd /
fault→信号 / 信号 / 网络（后续 WP），未动 `src/desktop/**`、`src/services/**`、
`src/drivers/**`、`sdk/**`、`build.ps1`、`tools/ci*.ps1`、`abi.h`。

## 基线

- 仓库 `D:\OS_Project`，分支 `ng`，HEAD `a48c29b`（`[docs] docs: D1 decisions ...`），
  D0 已冻结（tag `ng-d0-final`）。
- **8h 浸泡占用 QEMU 4924**：本 WP 全程**没有启动 QEMU**、没有跑 `build-ng` /
  `ci-ng` / `release-ng` 等长构建；只做源码编辑 + host 编译/单测 + `abi-check`
  解析。串口证据行（`ng: syscall ENOSYS ok (-38)`）已在 `syscall_selftest()` 里
  实现，留待浸泡结束后第一次 QEMU 门禁打印（见“未执行/边界”）。
- 工作区进入时已有与本 WP 无关的修改：`docs/design/architecture.md`、
  `docs/design/evolution.md`；执行期间还有其他 D1 会话并行改动
  `docs/design/d1/{d1-plan,wps,risks}.md`、`docs/design/decisions.md` 并新建
  `docs/claims/ng-WP5-0g.md`——**均未纳入本提交**（路径限定提交）。

## 交付物

| 路径 | 内容 |
|---|---|
| `docs/abi/abi-1.1.h` | **新**：1.1 机器可读头。`#include "abi-1.0.h"` 继承 1.0 全部编号/错误码/常量/结构体；版本覆盖为 **1.1.0（0x00010100）**；48–95 逐号 `ASTER_ABI_SYS_*`；`ASTER_ABI_UNALLOCATED_ERR=ASTER_ABI_ENOSYS(-38)`、`LAST_ASSIGNED=95`、`NEXT_FREE=96`；编译期钉住 ENOSYS=-38 |
| `docs/abi/abi-1.1.md` | **新**：DRAFT 规范。只追加规则、48–95 分配规则（7 条）、逐号表（含 WP/状态）、ENOSYS 行为、与 1.0 的兼容矩阵、校验输出、负向测试 |
| `docs/abi/abi-1.0.md` | **改**：顶部加“后继版本”链接（指向 `abi-1.1.md`）与“未分配号 -1→ENOSYS 不破坏 1.0”的说明；冻结正文/编号/错误码/结论表**未改** |
| `src/ng/syscall.h` | **改**：新增 `NG_SYS_ENOSYS (-38)`；注释说明 1.1 号段 48–95 与未分配号的返回约定 |
| `src/ng/syscall.c` | **改**：`default:` 改返回 `(u64)(i64)NG_SYS_ENOSYS`（原 `(u64)-1`）；新增 `syscall_enosys_test()` 并在 `syscall_selftest()` 调用（打印 `ng: syscall ENOSYS ok (-38)`） |
| `tests/unit-ng/test_syscall.c` | **改**：新增 `t_dispatch_unknown()`（1.0 未分配号 + 1.1 未实现号 + 96/99/0xFFFFFFFF 全 `-ENOSYS`；1.0 号坏参数仍原错误码）；ring3 未知号 = `-ENOSYS` 且进程继续；`run_ctx` 记录每次返回 |
| `tools/abi-check.ps1` | **改**：ABI 1.0 原有 6 步保持不变；新增 `NG_SYS_ENOSYS` 检查、ABI 1.1 第 7 步（inherited 1.0 值、77 号 = 29+48、48–95 连续无撞号、1.1.0、ENOSYS、内核 default 文本断言、1.1 文档 DRAFT）、1.0 文档后继链接检查；BOM 保持 |
| `docs/claims/ng-WP5-0a.md` | **新**：本文件 |
| `docs/design/d1/posix-profile.md` | **改**：§3 顶部补一行“48–95 已在 WP5-0a 冻结进 abi-1.1.h（86/87 预留）”的状态说明 |

**未改**：`sdk/include/aster/syscall.h`（SDK 早已有 `ASTER_ENOSYS (-38)`，本 WP 无需
改 SDK；新号随各自 WP 追加）、`src/desktop/**`、`src/services/**`、`src/drivers/**`、
`build.ps1`、`tools/ci*.ps1`、`abi.h`。

## ABI 1.1 号段表摘要（48–95，48 个）

| 范围 | 个数 | 内容 | 归属 WP |
|---|---|---|---|
| 48–62 | 15 | `fstat lseek dup dup2 pipe chdir getcwd getuid waitpid execve readlink rename rmdir fcntl getdents` | WP5-1a |
| 63–66 | 4 | `brk`；`mmap munmap mprotect` | 1b；1c |
| 67–69 | 3 | `clock_gettime nanosleep getrandom` | WP5-0f |
| 70–74 | 5 | `sched_yield`；`rt_sigaction rt_sigprocmask rt_sigreturn signal_send` | 1d；1e |
| 75 | 1 | `poll` | 1a / 2b |
| 76–87 | 12 | `socket bind connect listen accept sendto recvfrom setsockopt getsockopt shutdown`（86/87 `getpeername/getsockname` **预留 Tier 2**） | WP5-2b |
| 88–93 | 6 | `clone futex set_tid_address arch_prctl exit_thread gettid` | WP5-1c |
| 94–95 | 2 | `shm_free spawn_ex` | WP5-0d |

规则（`abi-1.1.md` §5）：从 48 起单调追加、禁止重排/复用；96 起留给 D2；
`2–9/15–19/28–29/36–39` 永远未分配；一个号一个 owner；名字冻结；参数只追加
（最多 6 个：`rdi,rsi,rdx,r10,r8,r9`）；未实现号返回 `ENOSYS`；每落地一个号
必须同步头/文本/`abi-check` 表/claim。

**1.0 部分（不变）**：29 个号 `0/1、10–14、20–27、30–35、40–47`；28 个错误码；
`vfs_stat=24 / vfs_dirent=80 / ipc_msg_hdr=16 / ds_fb_info=48`；由
`abi-1.1.h` 的 `#include "abi-1.0.h"` 原样继承，`abi-check` 逐项复核。

## ENOSYS 证据

### 代码

```c
/* src/ng/syscall.h */
#define NG_SYS_ENOSYS (-38)

/* src/ng/syscall.c: syscall_dispatch() */
default:
    /*
     * ABI 1.1（WP5-0a）：未分配号（2..9、15..19、28..29、36..39、96..）与
     * 1.1 已分配但内核尚未实现的号（48..95）统一 -ENOSYS(-38)。
     * 1.0 §10 第 1 项已声明未分配号行为不属于 1.0 契约，故不破坏 1.0。
     */
    f->rax = (u64)(i64)NG_SYS_ENOSYS;
    break;
```

`syscall_enosys_test()`（`syscall_selftest()` 第 2 项）对
`2/9/15/19/28/29/36/39/48/95/96` 逐个 dispatch，全部 `-38` 且断言
`NG_SYS_ENOSYS == 0xFFFFFFFFFFFFFFDA` 后打印：

```text
ng: syscall ENOSYS ok (-38)
```

### host 单测（已执行）

```text
tests/unit-ng/test_syscall.c
  t_dispatch_basic   : 99 -> -ENOSYS（并断言 0xFFFFFFFFFFFFFFDA）
  t_dispatch_unknown : 2/9/15/19/28/29/36/39/48/49/75/95/96/99/127/0xFFFFFFFF
                       16 个号 × (== -38, != -1) = 32 checks
  t_dispatch_unknown : write(0x1000,1) -> EFAULT；port_close -> IPC_EBADF
                       （ENOSYS 只属于 default，不覆盖 1.0 已分配号的错误码）
  t_run_user_bad     : ring3 号 48 -> rax=-ENOSYS，随后 exit 仍生效（进程继续）
```

### abi-check（已执行）

```text
[ok] kernel NG_SYS_ENOSYS == -38 (ABI 1.1 unallocated/unimplemented)
[ok] 1.1: unallocated/unimplemented -> ENOSYS(-38); kernel default returns NG_SYS_ENOSYS
```

### 兼容性说明

1. 1.0 的 §3/§8/§10 第 1 项明确“未分配号行为不属于 ABI 1.0”“ENOSYS(-38) 数值
   保留，1.1 改返回不构成破坏”——因此 `-1 → -38` 不违反冻结承诺。
2. 1.0 已分配号的错误码**不变**：坏参数仍是 `EINVAL/EFAULT/...`，不会被
   `ENOSYS` 掩盖（单测显式验证 `EFAULT`/`IPC_EBADF`）。
3. `-1` 在 1.1 只表示 `EPERM`（权限/属主）；应用不得再用 `-1` 判断“未实现”。
4. 1.1 应用跑在 1.0 内核上时 48–95 会拿到 `-1`（D0 实现细节）；应用应做
   `ASTER_ABI_VERSION >= 0x00010100` 编译期检查，或同时把 `-38` 视为未实现。
5. 48–95 在**本 WP 之后**仍全部返回 `ENOSYS`（没有实现任何新调用）；各 WP 落地
   对应号后，该号不再返回 `ENOSYS`，这是 1.1 的预期演进，不是破坏。

## abi-check 输出（2026-10-31，实际）

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi-check.ps1
=== abi-check (WP4-1: ABI 1.0 freeze + WP5-0a: ABI 1.1 additive) ===
repo: D:\OS_Project
abi:  D:\OS_Project\docs/abi/abi-1.0.h
abi11:D:\OS_Project\docs/abi/abi-1.1.h
sdk:  D:\OS_Project\sdk/include/aster/syscall.h
src:  D:\OS_Project\src/ng/syscall.h
[ok] 29 syscalls agree (ABI/SDK/kernel): 0/1, 10-14, 20-27, 30-35, 40-47
[ok] kernel NG_SYS_ENOSYS == -38 (ABI 1.1 unallocated/unimplemented)
[ok] 28 error codes frozen; SDK mirrors 25; 73 kernel aliases agree
[ok] 64 ABI constants match 64 kernel-source checks
[ok] struct layouts: vfs_stat=24, vfs_dirent=80, ipc_msg_hdr=16, ds_fb_info=48
[ok] docs/abi/abi-1.0.md is marked frozen and links abi-1.1.md
[ok] ABI version 1.0.0 (0x00010000)
[ok] 1.1: 77 syscalls (29 legacy + 48 new 48-95), number segment frozen
[ok] 1.1: version 1.1.0 (0x00010100)
[ok] 1.1: unallocated/unimplemented -> ENOSYS(-38); kernel default returns NG_SYS_ENOSYS
[ok] docs/abi/abi-1.1.md is marked DRAFT (48-95 + ENOSYS)
abi-check: PASS (29+48 syscalls, 28 errors, 64 constants, 4 struct sizes)
```

### 负向测试（临时副本，验收后删除）

| 改坏项 | 实际输出 | 退出码 |
|---|---|---|
| `abi-1.1.h`：`ASTER_ABI_SYS_FSTAT 48` → `49` | `abi-check: FAIL (ABI 1.1 syscall FSTAT=49, frozen table says 48)` | **1** |
| `abi-1.0.h`：`ASTER_ABI_SYS_FB_MAP 47` → `46` | `abi-check: FAIL (ABI syscall FB_MAP=46, frozen table says 47)` | **1** |

做法：临时目录里复制 `docs/abi`，用 junction 指向真实 `src`/`sdk`，只改副本；
`-Repo <tmp>` 运行，未污染仓库。

### 头文件编译检查（附加，未提交）

`clang -std=c11 -Wall -Wextra -Werror -I docs/abi`：
`abi-1.1.h` 单独包含、`abi-1.0.h`→`abi-1.1.h`、`abi-1.1.h`→`abi-1.0.h` 三种顺序
均编译通过；`_Static_assert` 复核 1.1.0、ENOSYS=-38、FSTAT=48、SPAWN_EX=95、
`LAST/NEXT=95/96`、继承的 `VFS_STAT=24`/`S_IFDIR=0x4000`。

## unit-ng 结果

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/unit-ng.ps1
...
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
5357 checks, 0 failures
unit-ng: PASS (5357 checks, 0 failures)
```

- 新增 40 个 check（`t_dispatch_unknown` 36 + ring3 3 + basic 1），0 失败；
- `syscall.c` 在 host 下重新编译（`-Wall -Wextra -Werror`）通过，`syscall_enosys_test()`
  不依赖串口/页表替身之外的设施。

## 预检状态

| 项 | 状态 | 说明 |
|---|---|---|
| abi-check（1.0 不变） | **PASS** | 29 号 / 28 错误码 / 64 常量 / 4 结构体逐项一致 |
| abi-check（1.1 新增） | **PASS** | 77 号、48–95 连续、1.1.0、ENOSYS、default 文本断言 |
| 负向测试 | **PASS** | 改坏 1.1 号 / 1.0 号均 exit 1 |
| host 单测 | **PASS** | 5357 checks / 0 failures |
| SDK 头 | **未改** | `ASTER_ENOSYS(-38)` 已存在；新号由各自 WP 追加 |
| 1.0 冻结内容 | **未改** | 只加“后继版本”链接；编号/错误码/结构体/结论表未动 |
| QEMU / `run-ng` / `ci-ng` / `build-ng` | **未跑** | 8h 浸泡（QEMU 4924）期间禁止第二实例/长构建；串口证据行待浸泡结束后的门禁 |
| 预检 S1–S4（d1-plan §5） | **不在本 WP** | 各自独立会话；本 WP 只负责 ABI 1.1/ENOSYS/abi-check 地基 |

## 未执行 / 边界

- **没有 QEMU 证据**：`ng: syscall ENOSYS ok (-38)` 已写进 `syscall_selftest()`，
  但 8h 浸泡期间不启动 QEMU，故本 claim 只给 host 断言与代码；浸泡结束后跑一次
  `run-ng` 即可看到该行（不影响本 WP 的代码/门禁出口）。
- 没有实现 48–95 中任何调用（全部仍走 `default → ENOSYS`）；这是 WP5-0a 的
  “号段冻结”语义，不是遗漏。
- 没有冻结 1.1 的新 errno 数值/结构体/flags（设计提案在
  `docs/design/d1/posix-profile.md` §3.2–§3.4），留给各自的实现 WP 追加；
  `abi-1.1.md` §7 已写清追加义务。
- 没有改 SDK；1.1 的 SDK 镜像（新号 wrapper、宏别名）属于后续 WP。

## 复现

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi-check.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/unit-ng.ps1
git log -1 --grep "WP5-0a"
```

## 出口

```text
abi-check: PASS (29+48 syscalls, 28 errors, 64 constants, 4 struct sizes)
unit-ng:   PASS (5357 checks, 0 failures)
ABI 1.1:   48-95 号段冻结；未分配/未实现号 = -ENOSYS(-38)
```
