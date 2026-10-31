# Aster native ABI 1.1（DRAFT，只追加）

> 状态：**草案（DRAFT）**，2026-10-31，D1 / WP5-0a。
> 版本：ABI **1.1.0**（major 1，minor 1，patch 0；`ASTER_ABI_VERSION=0x00010100`）。
> 适用：ng 分支的 64 位内核 + 静态 ELF64 用户程序。
>
> **规范源（按优先级）**：
> 1. `docs/abi/abi-1.1.h` —— 机器可读的 1.1 增量头（`#include "abi-1.0.h"`，
>    只追加 48–95 与未分配号语义）；
> 2. 本文 —— 1.1 的语义、分配规则、ENOSYS 行为、与 1.0 的兼容矩阵；
> 3. `docs/abi/abi-1.0.h` / `abi-1.0.md` —— 1.0 冻结契约（**不变**）；
> 4. `src/ng/syscall.h`、`sdk/include/aster/syscall.h` —— 实现/消费镜像。
>
> 前置：ABI 1.0 已冻结（tag `ng-d0-final`）。1.1 **只追加**：1.0 的 29 个调用号、
> 28 个错误码数值、4 个结构体布局、句柄编码、调用约定全部不变。
>
> 号段来源：`../design/d1/posix-profile.md` §3.1（WP5-0a 冻结提案）。
> 门禁：`tools/abi-check.ps1`（同时校验 1.0 不变与 1.1 增量）。

## 1. 范围与状态

ABI 1.1 在 1.0 之上定义：

- **新调用号 48–95**（48 个）：D1 的 POSIX 文件/进程、内存、时间/熵、线程/信号、
  socket、futex、共享内存等能力。**本 WP（WP5-0a）只冻结号段与参数/返回的
  形状，不实现任何新调用**；
- **未分配号的返回值**：`-ENOSYS(-38)`；
- 新调用的**参数寄存器扩展**：最多 6 个参数，顺序 `rdi, rsi, rdx, r10, r8, r9`
  （1.0 调用不传 `r8/r9`，内核忽略）。

**明确不在本文件冻结**（由各自 WP 追加，追加时同步本文件与 `abi-1.1.h`）：

- 1.1 新错误码的数值（草案见 `../design/d1/posix-profile.md` §3.2；1.0 的 28 个
  值已冻结、不改）；
- 1.1 新结构体布局（`ng_timespec`/`ng_sockaddr_in`/`ng_pollfd`/`ng_dirent`/
  `ng_sigaction` 等，草案见同文件 §3.3）；
- 1.1 新 flags/常量（`O_CLOEXEC`/`O_NONBLOCK`/`CLONE_*`/`CAP_*`/`PROT_*`/
  `POLL_*`/`AF_*`/`SOCK_*` 等，草案见同文件 §3.4）。

> 1.1 现在是 **DRAFT**：号段 48–95、1.1.0 版本、ENOSYS 语义**已经冻结**（后续 WP
> 只使用、不得重排/复用）；其余内容随 WP 追加。全部 D1 号段落地后由负责人把
> 状态改为 FROZEN。

## 2. 与 ABI 1.0 的关系

ABI 1.1 **不是**新 ABI：它是 1.0 的超集，机器可读头直接
`#include "abi-1.0.h"`。

| 维度 | ABI 1.0（冻结） | ABI 1.1（本文件） | 兼容性 |
|---|---|---|---|
| 调用号 `0/1`、`10–14`、`20–27`、`30–35`、`40–47` | **29 个** | 原样保留，参数/返回/错误不变 | 完全兼容 |
| 新增调用号 | 无 | **48–95（48 个）** | 只追加 |
| 未分配号行为 | 不属于 1.0 契约；D0 实现返回 `-1`（§10 第 1 项“延后”） | **`-ENOSYS(-38)`** | 不破坏合法调用 |
| 错误码 | 28 个冻结值 | 全部保留；`ENOSYS(-38)` 数值不变、语义生效 | 数值兼容 |
| 结构体 | `vfs_stat` 24 / `vfs_dirent` 80 / `ipc_msg_hdr` 16 / `ds_fb_info` 48 | 原样；新结构体只能尾部追加 | 布局兼容 |
| 调用约定 | `rax` 号；`rdi/rsi/rdx/r10`；返回值 `rax`（负 = `-errno`） | 1.0 调用不变；新调用可用 `r8/r9` | 向后兼容 |
| 句柄命名空间 | fd / handle / ticket / shm | 不变（1.1 的 fd 语义由 WP5-0b/1a 定义） | 兼容 |
| SDK | `sdk/include/aster/syscall.h`（1.0） | 本 WP **不动 SDK**；新号随对应 WP 只追加 | 1.0 SDK 继续可用 |
| 内核 | D0 | WP5-0a 只改 `default` 分支的返回值；1.0 路径零改动 | 1.0 路径不变 |

**1.0 应用在 1.1 内核上**：行为不变（1.0 路径逐字节保留；未分配号不在任何
合法调用的路径上）。

**1.1 应用在 1.0 内核上**：48–95 会落到 1.0 的 `default`，返回 `-1(EPERM)`
（D0 的实现细节，不是契约）。1.1 应用必须：

1. 用 `ASTER_ABI_VERSION >= 0x00010100` 做编译期检查；或
2. 把“未实现”判据定为 `-ENOSYS(-38)`，**不要**把 `-1` 当 ENOSYS
   （`-1` 是 `EPERM`，可能来自真实权限错误）。

## 3. 调用号：1.0 的 29 个（不变）

逐号语义/错误条件见 `abi-1.0.md` §5。这里只列编号（机器可读定义仍在
`abi-1.0.h`，1.1 头原样继承）：

```text
0 write                 1 exit
10 port_create         11 port_send         12 port_recv
13 port_reply          14 port_close
20 open                21 read              22 write_fd        23 close
24 stat                25 listdir           26 mkdir           27 unlink
30 readkey             31 login             32 dmesg           33 log_write
34 pkg_list            35 pkg_verify
40 spawn               41 wait              42 getpid          43 kill
44 getppid             45 shm_alloc         46 shm_map         47 fb_map
```

1.1 不改这些号的参数个数/含义/返回值/错误码；内核分发代码的 `case` 分支
保持原样。

## 4. 调用号：1.1 新增 48–95（冻结号段）

> 全部 48 个号在 WP5-0a 冻结：**名字、参数顺序、返回形状、归属 WP** 不得
> 重排/复用。实现落地前，内核统一返回 `-ENOSYS(-38)`（§5）。
> 状态：`已分配` = 号与名已冻结，由归属 WP 实现；`预留` = 号/名保留，
> 实现可延后（Tier 2）。

### 4.1 POSIX 文件 / 进程（48–62，WP5-1a）

| rax | 名字 | 参数（rdi, rsi, rdx, r10） | 返回 | 状态 |
|---|---|---|---|---|
| 48 | `fstat` | `fd, struct vfs_stat*` | 0 | 已分配 |
| 49 | `lseek` | `fd, off, whence` | 新偏移 | 已分配 |
| 50 | `dup` | `fd` | 新 fd | 已分配 |
| 51 | `dup2` | `oldfd, newfd` | newfd | 已分配 |
| 52 | `pipe` | `int fds[2]` | 0 | 已分配 |
| 53 | `chdir` | `path` | 0 | 已分配 |
| 54 | `getcwd` | `buf, size` | 0 | 已分配 |
| 55 | `getuid` | — | uid（D1 全 0） | 已分配 |
| 56 | `waitpid` | `pid, &status, options` | pid | 已分配 |
| 57 | `execve` | `path, argv, envp` | 不返回 / 负值 | 已分配 |
| 58 | `readlink` | `path, buf, size` | 目标长度 | 已分配 |
| 59 | `rename` | `old, new` | 0 | 已分配 |
| 60 | `rmdir` | `path` | 0 | 已分配 |
| 61 | `fcntl` | `fd, cmd, arg` | 依 cmd | 已分配 |
| 62 | `getdents` | `fd, buf, len` | 字节数 | 已分配 |

### 4.2 内存（63–66）

| rax | 名字 | 参数 | 返回 | WP | 状态 |
|---|---|---|---|---|---|
| 63 | `brk` | `addr` | 新 break / 负值 | 1b | 已分配 |
| 64 | `mmap` | `addr, len, prot, flags, fd, off`（`r8/r9`） | VA / 负值 | 1c | 已分配 |
| 65 | `munmap` | `addr, len` | 0 | 1c | 已分配 |
| 66 | `mprotect` | `addr, len, prot` | 0（W^X 拒绝 RWX） | 1c | 已分配 |

### 4.3 时间 / 熵（67–69，WP5-0f）

| rax | 名字 | 参数 | 返回 | 状态 |
|---|---|---|---|---|
| 67 | `clock_gettime` | `clockid, struct ng_timespec*` | 0 | 已分配 |
| 68 | `nanosleep` | `req, rem` | 0 / `-EINTR` | 已分配 |
| 69 | `getrandom` | `buf, len, flags` | 字节数 | 已分配 |

### 4.4 线程 / 信号（70–74）

| rax | 名字 | 参数 | 返回 | WP | 状态 |
|---|---|---|---|---|---|
| 70 | `sched_yield` | — | 0 | 1d | 已分配 |
| 71 | `rt_sigaction` | `sig, act, oldact` | 0 | 1e | 已分配 |
| 72 | `rt_sigprocmask` | `how, set, oldset` | 0 | 1e | 已分配 |
| 73 | `rt_sigreturn` | — | 不返回 | 1e | 已分配 |
| 74 | `signal_send` | `pid, sig` | 0 | 1e | 已分配 |

### 4.5 poll / socket（75–87，WP5-1a + WP5-2b）

| rax | 名字 | 参数 | 返回 | 状态 |
|---|---|---|---|---|
| 75 | `poll` | `fds, nfds, timeout_ms` | 就绪数 | 已分配 |
| 76 | `socket` | `domain, type, protocol` | fd | 已分配 |
| 77 | `bind` | `fd, addr, len` | 0 | 已分配 |
| 78 | `connect` | `fd, addr, len` | 0 / `-EINPROGRESS` | 已分配 |
| 79 | `listen` | `fd, backlog` | 0 | 已分配 |
| 80 | `accept` | `fd, addr, len` | 新 fd | 已分配 |
| 81 | `sendto` | `fd, buf, len, flags, addr, addrlen`（`r8/r9`） | 字节数 | 已分配 |
| 82 | `recvfrom` | `fd, buf, len, flags, addr, addrlen`（`r8/r9`） | 字节数 | 已分配 |
| 83 | `setsockopt` | `fd, level, opt, val, len` | 0 | 已分配 |
| 84 | `getsockopt` | `fd, level, opt, val, len` | 0 | 已分配 |
| 85 | `shutdown` | `fd, how` | 0 | 已分配 |
| 86 | `getpeername` | `fd, addr, len` | 0 | **预留（Tier 2）** |
| 87 | `getsockname` | `fd, addr, len` | 0 | **预留（Tier 2）** |

### 4.6 线程 / futex / TLS（88–93，WP5-1c）

| rax | 名字 | 参数 | 返回 | 状态 |
|---|---|---|---|---|
| 88 | `clone` | `flags, stack, ptid, ctid, tls`（`r8`） | tid | 已分配 |
| 89 | `futex` | `uaddr, op, val, timeout_ms` | 0 / `-EAGAIN` | 已分配 |
| 90 | `set_tid_address` | `tidptr` | tid | 已分配 |
| 91 | `arch_prctl` | `code, addr` | 0 | 已分配 |
| 92 | `exit_thread` | `code` | 不返回 | 已分配 |
| 93 | `gettid` | — | tid | 已分配 |

### 4.7 共享内存 / cap（94–95，WP5-0d）

| rax | 名字 | 参数 | 返回 | 状态 |
|---|---|---|---|---|
| 94 | `shm_free` | `handle` | 0 | 已分配 |
| 95 | `spawn_ex` | `path, argv, caps, &pid` | 0 | 已分配 |

## 5. 48–95 分配规则（冻结）

1. **从 48 起、单调追加**：新号只能取当前最大已分配号 +1；**禁止重排、禁止
   复用**已删除/保留的号。
2. **号段归属**：`48–95` 是 D1 号段（本文件冻结）；`96` 起留给 D2；`2–9`、
   `15–19`、`28–29`、`36–39` 永远未分配。1.0 的 `0–47` 保持原样。
3. **一个号一个 owner**：每个号的实现与语义修改只由表中“归属 WP”负责；其他
   WP 只读。跨 WP 需要改号/改参数时先改本文件并拿到负责人确认，不能“顺手改”。
4. **名字冻结**：`ASTER_ABI_SYS_*` 名字冻结后不改名；需要别名时只追加新宏，
   不复用旧号。
5. **参数只追加**：新调用最多 6 个参数，顺序 `rdi, rsi, rdx, r10, r8, r9`；
   已有调用不得增加参数（1.0 的 4 参数约定不变）。结构体/输出字段只允许尾部
   追加。
6. **未实现即 `ENOSYS`**：号已分配但内核还没实现时，`syscall_dispatch()` 落到
   `default` 返回 `-ENOSYS(-38)`；应用不能用“返回 -1”判断未实现。
7. **同步义务**：每落地一个 1.1 号，必须同时更新 `abi-1.1.h`（如需新常量/
   结构体）、本文 §4 状态、`tools/abi-check.ps1` 的 1.1 表、以及对应
   `docs/claims/ng-WP5-*.md`。`abi-check` 是门禁，不是文档。

## 6. 未分配号 / ENOSYS（1.1 的行为变化）

### 6.1 规则

```text
号不属于任何 ABI（2–9、15–19、28–29、36–39、96 起）  -> -ENOSYS(-38)
号属于 1.1 号段 48–95 但内核尚未实现                  -> -ENOSYS(-38)
号属于 1.0（0/1、10–14、20–27、30–35、40–47）          -> 原语义/原错误码（不变）
```

`ENOSYS = -38`（`0xFFFFFFFFFFFFFFDA`），数值沿用 1.0 的保留值，**不改值**。
错误码表见 `abi-1.0.md` §8；1.1 不新增数值（新 errno 由各 WP 追加）。

`-1(EPERM)` 在 1.1 **只表示权限/属主错误**，不再表示“未分配号”。这是 1.1
相对 1.0 唯一的返回值行为变化，且 1.0 §10 第 1 项已声明“未分配号行为不属于
ABI 1.0”，所以不构成对合法调用的破坏。

### 6.2 内核落地（WP5-0a）

`src/ng/syscall.c` 的 `default` 分支：

```c
default:
    f->rax = (u64)(i64)NG_SYS_ENOSYS; /* -38：ABI 1.1 未分配/未实现号 */
    break;
```

`src/ng/syscall.h` 增加 `NG_SYS_ENOSYS (-38)`。已分配 0/1、10–14、20–27、
30–35、40–47 的 `case` 分支一行未改。

### 6.3 证据

- host 单测 `tests/unit-ng/test_syscall.c`：`2/9/15/19/28/29/36/39/48/95/96/99/
  0xFFFFFFFF` 全部 `-ENOSYS`，且 `NG_SYS_ENOSYS == -38`；1.0 已分配号坏参数
  仍返回原错误码；
- 内核自检（QEMU，WP5-0a 未跑）：`ng: syscall ENOSYS ok (-38)`（由
  `syscall_selftest()` 在 `syscall_ring0_test()` 之后打印；8h 浸泡期间不启动
  第二个 QEMU，串口证据留待浸泡结束后的门禁）；
- `tools/abi-check.ps1`：`[ok] 1.1: ... ENOSYS(-38)`。

## 7. 1.1 之后的追加（不在本 WP）

| 项 | 来源 | 冻结时机 |
|---|---|---|
| 新 errno 数值（`ENOEXEC`/`EPIPE`/`ERANGE`/`ENOTSOCK`/`ECONN*`…） | `../design/d1/posix-profile.md` §3.2 | 对应 WP 落地时追加进 `abi-1.1.h`；不得改 1.0 的 28 个值 |
| 新结构体（`ng_timespec`/`ng_timeval`/`ng_sockaddr_in`/`ng_pollfd`/`ng_dirent`/`ng_sigaction`） | 同文件 §3.3 | 同上；只追加、固定大小 |
| 新 flags/常量（`O_CLOEXEC/O_NONBLOCK`、`CLONE_*`、`CAP_*`、`PROT_*`、`POLL_*`、`AF_*`、`SOCK_*`、`SEEK_*`、`ARCH_*`） | 同文件 §3.4 | 同上 |
| 48–95 的实现/语义细节 | 各 WP | 实现落地时更新本文 §4 的“状态/错误条件” |

1.1 **定稿（FROZEN）条件**：D1 号段 48–95 的实现全部落地或明确降级；新 errno/
结构体/flags 已齐；`abi-check` 同时钉住 1.0 与 1.1；由负责人把本文状态改为
FROZEN 并冻结 `abi-1.1.h`。此后任何新增号只能开 1.2。

## 8. 校验

```text
powershell -File tools/abi-check.ps1
=== abi-check (WP4-1: ABI 1.0 freeze + WP5-0a: ABI 1.1 additive) ===
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

负向测试（在临时副本上改坏号，`abi-check` 必须 exit 1）：

| 改坏项 | 实际输出 | 退出码 |
|---|---|---|
| `abi-1.1.h`：`ASTER_ABI_SYS_FSTAT 48` → `49` | `abi-check: FAIL (ABI 1.1 syscall FSTAT=49, frozen table says 48)` | 1 |
| `abi-1.0.h`：`ASTER_ABI_SYS_FB_MAP 47` → `46` | `abi-check: FAIL (ABI syscall FB_MAP=46, frozen table says 47)` | 1 |

证据文件：`../claims/ng-WP5-0a.md`。

## 9. 变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-31 | **v1.1.0-draft** | WP5-0a：建立 1.1 头/文本；冻结 48–95 号段（48 个）；未分配/未实现号改返回 `-ENOSYS(-38)`；`abi-check` 扩展为同时校验 1.0（不变）与 1.1（号段/ENOSYS）；新 errno/结构体/flags 留各 WP 追加 |
