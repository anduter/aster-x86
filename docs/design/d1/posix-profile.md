# POSIX Profile 1.0：syscall / errno / 结构体映射（设计输入）

> 状态：**草案（DRAFT）**，2026-10-31；WP5-1a 的设计输入，**不是冻结规范**。
> 冻结方式：`WP5-0a` 把本文 §3 的号段/错误码/结构体落进 `docs/abi/abi-1.1.h`
> 与 `docs/abi/abi-1.1.md`，之后只追加、不重排。
> 上位：`../architecture.md` §8（POSIX 用户态）、`../decisions.md` ADR-005、
> `d1-plan.md` D1-A/D1-G/D1-H、`wps.md` WP5-1a…1g。
> 原则：**内核只讲 native ABI；POSIX 语义全部在 libc 里翻译**；源码兼容，
> 不做二进制兼容；静态链接；不依赖 OpenSSL。

## 1. 分层与调用路径

```text
应用源码（POSIX / C）
   │  #include <stdio.h> <pthread.h> <sys/socket.h> ...
   ▼
libc/  （newlib 移植 + libc/aster 包装 + libpthread + libtls/libhttp）
   │  errno 设置、结构体转换、flags 映射、循环/重试
   ▼
native ABI 1.1（syscall 指令；rax=号，rdi/rsi/rdx/r10；新调用可带 r8/r9；负值=-errno）
   │
   ▼
内核机制（fd/pipe/exec/线程/信号/时间/socket；无 POSIX 名称与策略）
```

- libc 负责：`errno = -ret`、`EINTR` 重试、短读/短写循环、`FILE*` 缓冲、
  `struct stat`/`dirent`/`sockaddr` 转换、`select`→`poll`、`system`→
  `posix_spawn`、`getenv`→`environ`。
- 内核负责：fd 生命周期、pipe 缓冲、exec 换 AS、clone/futex、信号 pending、
  brk/mmap、clock/getrandom、socket 状态机；**不解析 POSIX 头/结构体**。
- 进程入口沿用 D0：`rdi=argc`、`rsi=argv`（用户栈 `char*[]`，NULL 结尾）；
  crt0 再解析 `envp`（1b 定义布局）。

## 2. 范围分层

| 层 | 内容 | D1 出口 |
|---|---|---|
| **Tier 1（必须）** | `open/read/write/close/stat/fstat/lseek/dup/dup2/pipe/chdir/getcwd/getdents/unlink/mkdir/rename/rmdir/fcntl`；`execve/waitpid/getpid/getppid/getuid/kill/exit`；`brk/mmap/munmap`；`clock_gettime/nanosleep`；`signal/sigaction/sigprocmask/sigreturn`；`pthread_*` 核心；`socket/connect/send/recv/poll/close`；`malloc/stdio/string/stdlib/time` | 是（D1-E1/E2） |
| **Tier 2（尽力）** | `select`（poll 包装）、`gettimeofday`、`realpath`、`glob`、`fnmatch`、`wordexp`、`sem_*`、`rwlock`、`getaddrinfo`（A/IN）、`gethostbyname`、`inet_*`、`getopt`、`regex` | 不影响 D1 出口；库需要就补 |
| **Tier 3（明确不做）** | `fork/vfork`、`dlopen/dlsym`、`setuid/setgid/chroot`、作业控制、`epoll`、`aio`、`io_uring`、POSIX MQ、文件锁、`inotify`、`mmap` 文件映射、`ptrace`、`timer_create`、`sigqueue` 实时信号 | 否（D2） |

`fork()` 返回 `-ENOSYS`；`system()` 用 `posix_spawn` 实现；`posix_spawn`
用 native `spawn`(40) + `waitpid`，**不支持** file_actions/attrp 的完整语义
（D1 只支持 `POSIX_SPAWN_SETPGROUP` 忽略、`POSIX_SPAWN_SETSIGDEF` 忽略）。

## 3. ABI 1.1 新增（提案，WP5-0a 冻结）

> **状态（2026-10-31，WP5-0a）**：48–95 号段已冻结进 `docs/abi/abi-1.1.h` /
> `docs/abi/abi-1.1.md`（86/87 `getpeername/getsockname` 标为“预留 Tier 2”）；
> 未分配号与“已分配未实现号”统一返回 `-ENOSYS(-38)`。下面的 **3.2 错误码 /
> 3.3 结构体 / 3.4 flags 仍是提案**，由各自 WP 追加进 1.1 头，本 WP 未冻结。

### 3.1 调用号（从 48 起）

> 现有 0/1、10–14、20–27、30–35、40–47 不变。以下是**提案**；未用号保持
> 未分配、不复用。`timeout` 单位：IPC 仍是 TSC tick（1.0 不动）；新调用
> 一律用**毫秒**或 `~0` 表示永久，并在 ABI 1.1 写清楚。

| rax | 名称 | 参数（rdi, rsi, rdx, r10, [r8, r9]） | 返回 | 说明 / WP |
|---|---|---|---|---|
| 48 | `fstat` | `fd, struct vfs_stat*` | 0 | 24 B 输出；`EBADF/EFAULT`；1a |
| 49 | `lseek` | `fd, off, whence` | 新偏移 | `SEEK_SET/CUR/END`；`EINVAL/ESPIPE`；1a |
| 50 | `dup` | `fd` | 新 fd | 共享 `struct file`（偏移/标志/refcount）；1a |
| 51 | `dup2` | `oldfd, newfd` | newfd | 目标已开则原子关闭；1a |
| 52 | `pipe` | `int fds[2]` | 0 | 两个 fd（读/写），64 KiB 环形缓冲；1a |
| 53 | `chdir` | `path` | 0 | 修改当前进程 cwd；1a |
| 54 | `getcwd` | `buf, size` | 0 | 含 NUL；不够 `ERANGE(-34)`；1a |
| 55 | `getuid` | — | uid | D1 全 0；`geteuid` 同值；1a |
| 56 | `waitpid` | `pid, &status, options` | pid | `pid>0` 直接子进程；`-1` 任一子；`WNOHANG=1`；1a |
| 57 | `execve` | `path, argv, envp` | 不返回/负值 | 替换当前进程镜像；保留 pid/fd/cwd；1a |
| 58 | `readlink` | `path, buf, size` | 目标长度 | 不跟随；1a |
| 59 | `rename` | `old, new` | 0 | VFS 已支持；1a |
| 60 | `rmdir` | `path` | 0 | 空目录；`ENOTEMPTY`；1a |
| 61 | `fcntl` | `fd, cmd, arg` | 依 cmd | `F_GETFL/F_SETFL/F_GETFD/F_SETFD/F_DUPFD`；1a |
| 62 | `getdents` | `fd, buf, len` | 字节数 | 目录枚举；记录见 §3.3；1a |
| 63 | `brk` | `addr` | 新 break / 负值 | `addr=0` 查询；按页映射/回收；1b |
| 64 | `mmap` | `addr, len, prot, flags, fd, off`（`r8/r9`） | VA / 负值 | 仅匿名 `MAP_PRIVATE + MAP_ANONYMOUS`；`fd=-1, off=0`；1c |
| 65 | `munmap` | `addr, len` | 0 | 释放匿名映射；1c |
| 66 | `mprotect` | `addr, len, prot` | 0 | W^X 拒绝 RWX；1c |
| 67 | `clock_gettime` | `clockid, struct timespec*` | 0 | `REALTIME=0`/`MONOTONIC=1`；1ms；0f |
| 68 | `nanosleep` | `req, rem` | 0 / `EINTR` | ms 粒度；0f |
| 69 | `getrandom` | `buf, len, flags` | 字节数 | `flags=0`；0f |
| 70 | `sched_yield` | — | 0 | 协作式让出；1d |
| 71 | `rt_sigaction` | `sig, act, oldact` | 0 | 见 §3.3；1e |
| 72 | `rt_sigprocmask` | `how, set, oldset` | 0 | 每线程 mask；1e |
| 73 | `rt_sigreturn` | — | 不返回 | handler 返回；1e |
| 74 | `signal_send` | `pid, sig` | 0 | POSIX `kill()` 用；与 native 43 分离；1e |
| 75 | `poll` | `fds, nfds, timeout_ms` | 就绪数 | `timeout=-1` 永久；1a/2b |
| 76 | `socket` | `domain, type, protocol` | fd | `AF_INET=2`、`SOCK_STREAM=1`/`DGRAM=2`；2b |
| 77 | `bind` | `fd, addr, len` | 0 | `sockaddr_in`；2b |
| 78 | `connect` | `fd, addr, len` | 0/`EINPROGRESS` | 非阻塞；2b |
| 79 | `listen` | `fd, backlog` | 0 | 2b |
| 80 | `accept` | `fd, addr, len` | 新 fd | 2b |
| 81 | `sendto` | `fd, buf, len, flags, addr, addrlen`（`r8/r9`） | 字节数 | `send()` 用 NULL addr；D1 只要求 connected；2b |
| 82 | `recvfrom` | `fd, buf, len, flags, addr, addrlen`（`r8/r9`） | 字节数 | `recv()` 用 NULL addr；2b |
| 83 | `setsockopt` | `fd, level, opt, val, len` | 0 | 最小集；2b |
| 84 | `getsockopt` | `fd, level, opt, val, len` | 0 | 2b |
| 85 | `shutdown` | `fd, how` | 0 | 2b |
| 86 | `getpeername` | `fd, addr, len` | 0 | Tier 2；2b |
| 87 | `getsockname` | `fd, addr, len` | 0 | Tier 2；2b |
| 88 | `clone` | `flags, stack, ptid, ctid, tls`（5 参数需 `r8`） | tid | pthread 用；1c |
| 89 | `futex` | `uaddr, op, val, timeout_ms` | 0/`EAGAIN` | WAIT/WAKE/REQUEUE；1c |
| 90 | `set_tid_address` | `tidptr` | tid | 线程退出清零/唤醒；1c |
| 91 | `arch_prctl` | `code, addr` | 0 | `SET_FS=0x1002`/`GET_FS=0x1003`；1c |
| 92 | `exit_thread` | `code` | 不返回 | 单线程退出；1c |
| 93 | `gettid` | — | tid | 1c |
| 94 | `shm_free` | `handle` | 0 | 0d；refcount 归零才释放 |
| 95 | `spawn_ex` | `path, argv, caps, &pid` | 0 | sessiond 授 cap；0d |

> **ABI 1.1 的调用约定扩展**：新调用最多 6 个参数，顺序为
> `rdi, rsi, rdx, r10, r8, r9`（1.0 的 `rdi/rsi/rdx/r10` 不变；旧调用不传
> `r8/r9`，内核忽略）。`syscall` 入口/返回已经保存全部 15 个 GPR，`r8/r9`
> 在 `struct syscall_frame` 里可用，不需要改汇编。1.1 冻结时把这条写进 ABI
> 文档；`clone` 的 5 个参数正好用到 `r8`，`mmap/sendto/recvfrom` 用到 `r8/r9`。
> 如果负责人否决 6 参数扩展，备选是拆调用（如 `mmap` 先 `mmap_anon` 再
> `mmap_file`），但**不混用**两种 ABI。

### 3.2 新增错误码（只追加，不改旧值）

| 值 | 名称 | 用途 |
|---|---|---|
| -8 | `ENOEXEC` | `execve` 非 ELF/不可执行 |
| -32 | `EPIPE` | 写无读者 pipe/socket（默认 SIGPIPE） |
| -34 | `ERANGE` | `getcwd` 缓冲不足 |
| -88 | `ENOTSOCK` | fd 不是 socket |
| -89 | `EDESTADDRREQ` | `sendto` 缺地址 |
| -91 | `EPROTOTYPE` | socket 类型/协议不匹配 |
| -92 | `ENOPROTOOPT` | `setsockopt` 未知选项 |
| -93 | `EPROTONOSUPPORT` | 协议不支持 |
| -94 | `ESOCKTNOSUPPORT` | socket 类型不支持 |
| -95 | `EOPNOTSUPP` | 操作不支持 |
| -97 | `EAFNOSUPPORT` | 地址族不支持 |
| -98 | `EADDRINUSE` | bind 冲突 |
| -99 | `EADDRNOTAVAIL` | 地址不可用 |
| -100 | `ENETDOWN` | 链路 down |
| -101 | `ENETUNREACH` | 不可达 |
| -102 | `ENETRESET` | 连接被重置 |
| -103 | `ECONNABORTED` | 连接中止 |
| -104 | `ECONNRESET` | 连接重置 |
| -105 | `ENOBUFS` | 缓冲不足 |
| -106 | `EISCONN` | 已连接 |
| -107 | `ENOTCONN` | 未连接 |
| -108 | `ESHUTDOWN` | 已 shutdown |
| -109 | `ETOOMANYREFS` | 引用过多（保留） |
| -111 | `ECONNREFUSED` | 拒绝 |
| -112 | `EHOSTDOWN` | 主机 down |
| -113 | `EHOSTUNREACH` | 主机不可达 |
| -114 | `EALREADY` | 进行中 |
| -115 | `EINPROGRESS` | 非阻塞 connect 进行中 |

已有 `-90 EMSGSIZE`、`-110 ETIMEDOUT`、`-125 ECANCELED` 复用；`EAGAIN(-11)`
即 `EWOULDBLOCK`。ABI 1.0 的 28 个值**一个都不改**；`EPERM(-1)` 的歧义
（权限 vs 未分配号）继续按 1.0 处理。

### 3.3 新结构体（固定布局、自然对齐、小端）

| 结构 | 大小 | 布局 | 用途 |
|---|---|---|---|
| `struct ng_timespec` | 16 | `{i64 tv_sec; i64 tv_nsec;}` | `clock_gettime/nanosleep` |
| `struct ng_timeval` | 16 | `{i64 tv_sec; i64 tv_usec;}` | newlib `_gettimeofday` |
| `struct ng_sockaddr_in` | 16 | `{u16 sin_family; u16 sin_port; u32 sin_addr; u8 sin_zero[8];}` | 网络字节序；与 POSIX `sockaddr_in` 同布局 |
| `struct ng_pollfd` | 8 | `{i32 fd; i16 events; i16 revents;}` | `poll`；与 Linux `pollfd` 同布局 |
| `struct ng_dirent` | 88 | `{u64 ino; u64 off; u32 type; u32 name_len; char name[64];}` | `getdents` 记录；`name_len` 1..63，NUL 结尾 |
| `struct ng_sigaction` | 32 | `{u64 handler; u64 flags; u64 mask; u64 restorer;}` | handler 0=默认、1=忽略；其余为用户函数 |
| `struct ng_futex_wait` | — | 用参数传递，不新增结构 | 1c |

已有 `vfs_stat`(24)、`vfs_dirent`(80)、`ipc_msg_hdr`(16)、`ds_fb_info`(48)
**不变**。`getdents` 用新 88 B 记录，避免改 `vfs_dirent`。

### 3.4 flags 映射

**open（native = POSIX 低位，D0 已如此）**：

| POSIX | native | 备注 |
|---|---|---|
| `O_RDONLY` 0 / `O_WRONLY` 1 / `O_RDWR` 2 | 同值 | `O_ACCMODE=3` |
| `O_CREAT` 0x40 / `O_EXCL` 0x80 / `O_TRUNC` 0x200 / `O_APPEND` 0x400 / `O_DIRECTORY` 0x800 | 同值 | 1.0 已冻结 |
| `O_CLOEXEC` | **0x1000**（新） | `execve` 时关闭 |
| `O_NONBLOCK` | **0x2000**（新） | 仅 pipe/socket 有意义；普通文件忽略 |

**lseek**：`SEEK_SET=0`、`SEEK_CUR=1`、`SEEK_END=2`。

**mmap**：`PROT_READ=1`、`PROT_WRITE=2`、`PROT_EXEC=4`；`MAP_PRIVATE=0x02`、
`MAP_ANONYMOUS=0x20`、`MAP_FIXED=0x10`（D1 只接受匿名；`fd=-1`、`off=0`）。
`mprotect` 拒绝 `PROT_WRITE|PROT_EXEC` 同时设置（W^X）。

**poll**：`POLLIN=0x1`、`POLLOUT=0x4`、`POLLERR=0x8`、`POLLHUP=0x10`、
`POLLNVAL=0x20`。

**socket**：`AF_INET=2`、`SOCK_STREAM=1`、`SOCK_DGRAM=2`；`SOCK_NONBLOCK=0x800`、
`SOCK_CLOEXEC=0x80000` 在 libc 侧转成 `fcntl`（D1 内核只认 type 1/2）。
`SOL_SOCKET=1`、`SO_REUSEADDR=2`、`SO_RCVTIMEO=20`、`SO_SNDTIMEO=21`；
`IPPROTO_TCP=6`、`TCP_NODELAY=1`。

**信号号**：采用 Linux 编号（`SIGHUP=1`、`SIGINT=2`、`SIGQUIT=3`、`SIGILL=4`、
`SIGTRAP=5`、`SIGABRT=6`、`SIGBUS=7`、`SIGFPE=8`、`SIGKILL=9`、`SIGUSR1=10`、
`SIGSEGV=11`、`SIGUSR2=12`、`SIGPIPE=13`、`SIGALRM=14`、`SIGTERM=15`、
`SIGCHLD=17`、`SIGCONT=18`、`SIGSTOP=19`）。native 退出状态 `0x100|sig`
与 POSIX `wait` 状态的转换在 libc 做。

**线程 / futex / cap 常量**（ABI 1.1 冻结）：

| 名称 | 值 | 说明 |
|---|---|---|
| `CLONE_VM` | 0x00000100 | 共享地址空间 |
| `CLONE_FS` | 0x00000200 | 共享 cwd |
| `CLONE_FILES` | 0x00000400 | 共享 fd 表 |
| `CLONE_SIGHAND` | 0x00000800 | 共享信号处置 |
| `CLONE_THREAD` | 0x00010000 | 同进程线程 |
| `CLONE_SETTLS` | 0x00080000 | 设置 FS 基 |
| `CLONE_CHILD_CLEARTID` | 0x00200000 | 退出清零 `ctid` 并唤醒 futex |
| `FUTEX_WAIT` | 0 | 等值/超时 |
| `FUTEX_WAKE` | 1 | 唤醒 N 个 |
| `FUTEX_REQUEUE` | 3 | cond 用 |
| `CAP_NONE` | 0 | 默认 |
| `CAP_FB` | 1<<0 | `fb_map`/displayd |
| `CAP_NET` | 1<<1 | socket |
| `CAP_SPAWN` | 1<<2 | 允许 `spawn_ex` 授 cap（sessiond） |
| `ARCH_SET_FS` | 0x1002 | `arch_prctl` |
| `ARCH_GET_FS` | 0x1003 | `arch_prctl` |

`VFS_O_FLAGS_MASK` 从 `0xEC3` 扩到 `0x3EC3`（新增 `O_CLOEXEC/O_NONBLOCK`）；
ABI 1.0 的取值不变。

## 4. POSIX → native 映射

### 4.1 文件 / fd

| POSIX | native | 说明 |
|---|---|---|
| `open(path, flags, mode)` | 20 | flags 见 §3.4；路径 256 B |
| `read(fd, buf, n)` | 21 | 单次 ≤64 KiB；短读合法；libc 循环 |
| `write(fd, buf, n)` | 22 | 单次 ≤64 KiB；短写合法；`EINTR` 由 libc 处理 |
| `close(fd)` | 23 | 只认 fd；不再误关 ticket（0b） |
| `stat(path, st)` | 24 | 输出 `vfs_stat`，libc 填 `struct stat` |
| `fstat(fd, st)` | 48 | 同上 |
| `lseek` | 49 | 普通文件；pipe/socket `ESPIPE` |
| `dup/dup2` | 50/51 | 共享 `struct file`，refcount |
| `pipe` | 52 | 64 KiB；读端/写端两个 fd |
| `chdir/getcwd` | 53/54 | 每进程 cwd |
| `getdents` | 62 | 目录 fd；libc 转 `struct dirent` |
| `unlink/mkdir` | 27/26 | 已有 |
| `rename/rmdir` | 59/60 | VFS 后端已支持 rename；rmdir 新增 |
| `readlink` | 58 | 不跟随符号链接 |
| `fcntl` | 61 | `F_GETFL/F_SETFL/F_GETFD/F_SETFD/F_DUPFD` |
| `access` | 24 | libc 用 `stat` 判断；uid=0 实际全允许 |
| `fsync` | — | libc 返回 0（D1 无写缓存） |
| `ftruncate` | — | Tier 3；`ENOSYS` |
| `opendir/readdir/closedir` | `open(20)+getdents(62)+close(23)` | libc 自己实现，不依赖 newlib 的 syscall 假设 |
| `realpath` | — | libc 用 `getcwd` + 路径规范化（或 native 24） |

### 4.2 进程

| POSIX | native | 说明 |
|---|---|---|
| `getpid/getppid` | 42/44 | 已有 |
| `getuid/geteuid` | 55 | D1 全 0 |
| `kill(pid, sig)` | **74** | POSIX 信号投递；native 43 仍是“强制终止 + 0x100 OR sig” |
| `raise(sig)` | 74 | `signal_send(getpid(), sig)` |
| `wait/waitpid` | 41/**56** | 41 阻塞等直接子；56 支持 `-1/WNOHANG` |
| `execve` | **57** | 保留 pid/fd/cwd；失败保留旧镜像 |
| `exit/_exit` | 1 | 进程（所有线程）退出；`exit` 先 flush stdio |
| `atexit` | — | libc 数组，`exit` 调用 |
| `fork/vfork` | — | `-ENOSYS`（D1 明确不做） |
| `posix_spawn` | 40 + 56 | libc 用 `spawn` + `waitpid`；env 忽略 |
| `system` | 40 + 56 | `posix_spawn("/bin/sh", "-c", cmd)`；D1 无 `/bin/sh` 则 `ENOENT` |
| `getenv/setenv` | — | libc `environ`；`setenv` 仅进程内 |

### 4.3 内存

| POSIX | native | 说明 |
|---|---|---|
| `malloc/free/realloc/calloc` | newlib + `brk`(63) | `_sbrk` 按页扩堆；`free` 合并 |
| `mmap`（匿名） | 64 | `MAP_PRIVATE + MAP_ANONYMOUS`，`fd=-1`；页对齐；0 填充 |
| `munmap` | 65 | 释放；线程栈用 |
| `mprotect` | 66 | 拒绝 RWX；D1 主要用于栈/guard |
| `sbrk` | 63 | libc `_sbrk` |

### 4.4 时间 / 随机

| POSIX | native | 说明 |
|---|---|---|
| `time` | 67 + 换算 | `CLOCK_REALTIME` 秒 |
| `gettimeofday` | 67 | newlib `_gettimeofday` 填 `ng_timeval` |
| `clock_gettime` | 67 | 1ms 分辨率；`CLOCK_REALTIME=0`/`MONOTONIC=1` |
| `nanosleep` | 68 | ms 粒度；`EINTR` + `rem` |
| `getrandom` | 69 | 0..256 B/次；TLS 熵 |
| `alarm/setitimer` | — | Tier 3；libc 用 timer 线程或 `ENOSYS` |
| `sleep` | 68 | libc 循环处理 `EINTR` |

### 4.5 信号

| POSIX | native | 说明 |
|---|---|---|
| `signal(sig, handler)` | 71 | 简化 `sigaction`（`SA_RESTART` 可选） |
| `sigaction` | 71 | `ng_sigaction`；不支持 `SA_SIGINFO`/altstack |
| `sigprocmask` | 72 | 每线程 mask |
| `sigreturn` | 73 | 内核在 handler 返回时用（trampoline） |
| `kill/raise` | 74 | 见 4.2 |
| `pause` | 68 | `nanosleep(~0)` + 信号唤醒；或 `ENOSYS` |
| `sigemptyset/sigfillset/...` | — | libc 位操作 |
| `SIGCHLD` | 内核在子进程退出时投递 | 默认忽略；`waitpid` 仍能收 |
| `SIGPIPE` | 写无读者 pipe/socket | 默认终止；libc 可忽略 |
| `SIGINT` | 键盘 Ctrl-C（display/console） | D1 只做串口/键盘最小路径 |

### 4.6 线程

| POSIX | native | 说明 |
|---|---|---|
| `pthread_create` | `mmap` + **88** `clone` | 共享 AS/fd/cwd/信号；栈 64 KiB |
| `pthread_join` | **89** `futex` | 等 `clear_child_tid` 清零/唤醒 |
| `pthread_exit` | **92** `exit_thread` | 单线程退出 |
| `pthread_self` | — | TLS 指针 |
| `pthread_detach` | **90** `set_tid_address` + 标志 | 线程资源自动回收 |
| `mutex/cond/rwlock/sem` | **89** `futex` | fast path 原子 CAS，慢路径 wait/wake |
| `pthread_key_*` | — | libc TLS 数组（`arch_prctl(91)` 设 FS） |
| `sched_yield` | **70** | 协作式让出 |
| `pthread_cancel` | — | Tier 3；`ENOSYS` |

### 4.7 网络

| POSIX | native | 说明 |
|---|---|---|
| `socket` | 76 | `AF_INET`；`SOCK_STREAM/DGRAM` |
| `bind/connect` | 77/78 | `ng_sockaddr_in`；非阻塞 connect 返回 `EINPROGRESS` |
| `listen/accept` | 79/80 | 测试/本地服务用 |
| `send/recv` | 81/82 | 用 `connect` 后的地址（NULL）；短读/短写合法 |
| `sendto/recvfrom` | 81/82 | D1 只支持无 addr 的 connected socket；带 addr 的 UDP 可后续 |
| `setsockopt/getsockopt` | 83/84 | 最小集 |
| `shutdown` | 85 | `SHUT_RD/WR/RDWR` |
| `poll/select` | 75 | `select` 由 libc 用 `poll` 实现 |
| `getaddrinfo` | 2c | 只 A/IN、同步；`freeaddrinfo` 释放 |
| `gethostbyname` | 2c | 兼容旧库；内部同 DNS |
| `inet_pton/ntop` | — | libc 纯函数 |

## 5. `struct stat` / `dirent` 的填充规则

D1 的 VFS **没有** uid/gid/时间戳/硬链接计数；libc 必须填确定值，不能让
未初始化字段漏出去。

### 5.1 `stat`

| 字段 | 来源 | 值 |
|---|---|---|
| `st_dev` | 固定 0 | 单设备号空间 |
| `st_ino` | `vfs_stat.ino` | VFS 单调值；FAT16 由 VFS 生成 |
| `st_mode` | `vfs_stat.mode` | `VFS_S_IFREG/DIR/LNK` → POSIX `S_IFREG/DIR/LNK`；权限位照抄（uid=0 绕过） |
| `st_nlink` | 固定 1 | 无硬链接 |
| `st_uid/st_gid` | 固定 0 | D1 单用户 |
| `st_rdev` | 0 | 无设备文件 |
| `st_size` | `vfs_stat.size` | 字节 |
| `st_blksize` | 4096 | 逻辑块 |
| `st_blocks` | `(size+511)/512` | 512 B 块 |
| `st_atim/mtim/ctim` | 0（或 boot 时间） | **VFS 无时间戳**；文档写明 |

### 5.2 `dirent`

`getdents` 的 88 B 记录 → libc `struct dirent`：

- `d_ino`：`ng_dirent.ino`（FAT16 可为 0/哈希）；
- `d_off`：`ng_dirent.off`（下一次 `getdents` 的索引）；
- `d_reclen`：libc 计算；
- `d_type`：`DT_REG/DT_DIR/DT_LNK/DT_UNKNOWN` 由 `ng_dirent.type` 映射；
- `d_name`：`name`（NUL 结尾）。

目录 fd 的 `offset` 由内核按记录推进；`telldir/seekdir` 用 `d_off`。

## 6. newlib syscall stubs 对照（1b 实现）

| newlib stub | native | 备注 |
|---|---|---|
| `_open` | 20 | 路径缓冲 256 |
| `_read` | 21 | 循环到 64 KiB |
| `_write` | 22 | 循环；stdout/stderr 走 fd 1/2 |
| `_close` | 23 | |
| `_lseek` | 49 | |
| `_fstat` | 48 | 填 newlib `struct stat` |
| `_stat` | 24 | |
| `_isatty` | — | fd 0/1/2 返回 1；其余 0 |
| `_sbrk` | 63 | 页对齐；失败 `ENOMEM` |
| `_gettimeofday` | 67 | 填 `ng_timeval` |
| `_getpid` | 42 | |
| `_kill` | 74 | `kill(pid,sig)` |
| `_exit` | 1 | |
| `_wait` | 56 | `waitpid(-1,&st,0)` |
| `_execve` | 57 | |
| `_fork` | — | `ENOSYS` |
| `_system` | 40+56 | 自实现，不用 newlib |
| `_signal` | 71 | 自实现，不用 newlib 的 sigtramp |
| `_raise` | 74 | |
| `_pipe` | 52 | |
| `_dup/_dup2` | 50/51 | |
| `_chdir/_getcwd` | 53/54 | |
| `_unlink/_mkdir/_rename/_rmdir` | 27/26/59/60 | |
| `_link/_symlink` | — | Tier 3（VFS 有 symlink 但无 syscall；可后续） |
| `_readlink` | 58 | |
| `_fcntl` | 61 | |
| `_getdents` | 62 | libc 自己实现 opendir/readdir，可不接 newlib |

## 7. 上限（D1）

| 项 | 值 | 说明 |
|---|---|---|
| `PATH_MAX` | 256（有效 ≤254） | 与 ABI 1.0 一致；组件 ≤63 |
| `NAME_MAX` | 63 | |
| 每进程 fd | 32 | 0b 从 16 提高；含 socket/pipe |
| 全局 fd/句柄 | 按进程表/端口池 | 不新增全局限制 |
| `read/write` 单次 | 64 KiB | 1a 从 512 B 提高；超出 `EINVAL` |
| `pipe` 容量 | 64 KiB | `PIPE_BUF=4096` 原子写 |
| `ARG_MAX` | 4 KiB（argv+envp） | `execve`；旧 `spawn` 1 KiB 不变 |
| `execve` ELF | 256 KiB | 与 spawn 一致 |
| 线程 | 16/进程，64 全局 | 1c |
| pthread 栈 | 64 KiB | 1d；guard page 可选 |
| `mmap` 匿名 | ≤64 MiB/进程 | PMM 窗口限制 |
| socket 缓冲 | 16 KiB 收/发 | 2b |
| `getrandom` | ≤256 B/次 | 0f |
| 时钟分辨率 | 1 ms | 0f |
| 信号 | 64（Linux 编号） | 1e |

## 8. 错误处理约定（libc）

1. 所有 syscall 返回 `<0` → `errno = -ret`，函数返回 `-1`（指针函数返回
   `NULL`/`MAP_FAILED`）。
2. `EINTR`：`read/write/accept/connect/poll/nanosleep/waitpid` 按 POSIX 返回
   `EINTR`；`SA_RESTART` 的调用由 libc 重试（D1 只对 `read/write` 做）。
3. 短读/短写：libc 负责循环，直到满足或遇到 EOF/错误；`write` 返回部分
   字节数时应用可见。
4. `errno` 是每线程（1c 后 TLS）；主线程也要初始化。
5. `ENOSYS`：`fork/dlopen/setuid/...` 明确返回，不静默成功。
6. `EPIPE`：写无读者 pipe/socket 时先发 `SIGPIPE`，再返回 `-EPIPE`；应用
   忽略 `SIGPIPE` 时能拿到错误。

## 9. WP5-1a 的验收（本文对应的第一个 WP）

**host 单测**（`tests/unit-ng/test_posix_map.c` 或 `tests/posix/`）：

- flags 映射（open/fcntl/poll/socket）逐位；
- `vfs_stat` → `struct stat` 字段/大小/对齐；
- `getdents` 记录 → `dirent` 转换；
- errno 表数值与 ABI 1.1 逐项一致；
- `waitpid` 状态转换（正常/被信号/`WNOHANG`）；
- `execve` 失败保留旧镜像（内核侧用 fake loader 单测）。

**QEMU 证据行**（`posix-check`）：

```text
posix: fstat/lseek/dup/dup2/pipe/chdir/getcwd/getuid ok
posix: getdents 128 entries ok
posix: pipe 64KiB ok
posix: execve keeps fd/cwd ok
posix: execve failure keeps image ok
posix: waitpid wnohang ok
posix: read/write 64KiB ok
posix: errno map ok
posix: abi 1.1 numbers ok
```

## 10. 开放问题（WP5-0a/1a 冻结前必须回答）

1. **libc 底座**：newlib vs picolibc（S1 结果决定）；`setjmp/longjmp` 是否
   需要自己补。
2. **`fork` 延后**是否被负责人接受；`system()` 无 `/bin/sh` 时的行为。
3. **`getdents` 记录**：88 B 是否固定；FAT16 的 `ino` 是否稳定（可 0）。
4. **`execve` 的 envp 布局/上限**：4 KiB 是否够；shebang `#!` 是否支持
   （D1 建议只支持 ELF，`ENOEXEC`）。
5. **`mmap` 是否需要文件映射**：FFmpeg/SDL2 若必须，提前到 1c；D1 先匿名。
6. **`spawn_ex` 的 cap 继承**：sessiond 授 `CAP_NET`/`CAP_FB` 的具体策略；
   D1 明确“非安全边界”。
7. **`O_CLOEXEC/O_NONBLOCK` 的新 flag 值**：0x1000/0x2000 是否与未来冲突；
   `O_FLAGS_MASK` 更新为 ABI 1.1。
8. **信号与线程**：进程级 pending + 线程级 mask 的投递目标（当前线程 vs
   主线程）；`SIGCHLD` 在 `waitpid` 阻塞时的唤醒顺序。
9. **`select`**：libc 用 `poll` 实现是否够（`fd_set` 上限 32）；FFmpeg/SDL2
   是否真的用 `select`。
10. **时间戳**：VFS 无 mtime，`make`/缓存类库会退化；D1 接受，D2 补。
11. **`fstat` 的 `st_ino`**：FAT16 无稳定 inode；用 VFS 临时值还是路径哈希。
12. **`EPERM` 与 `EACCES`**：VFS 权限用 `EPERM`，auth 用 `EACCES`；POSIX
    libc 原样透传，文档写明，避免应用误判。

## 11. 与 ABI 1.0 的关系

- ABI 1.0 的 29 个调用/28 个错误码/4 个结构体**不变**；本文新增内容全部走
  ABI 1.1（只追加）。
- ABI 1.0 的未分配号当前返回 `-EPERM`，WP5-0a 改为 `-ENOSYS`；这是 1.0
  §10 第 1 项允许的行为，不构成对合法调用的破坏。
- `kill`(43) 的“强制终止 + 0x100|sig”保留；POSIX `kill()` 用新 74，二者
  并存，文档必须在 SDK/ABI 1.1 里写清楚，避免应用混用。
- `write` 的命名冲突（0=控制台，22=fd）继续按 1.0；POSIX `write(1)` 走 22，
  libc 不碰 0（0 只用于内核诊断/串口控制台）。
