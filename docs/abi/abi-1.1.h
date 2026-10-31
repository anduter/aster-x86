/*
 * Aster native ABI 1.1 — 机器可读头（DRAFT / 只追加）
 *
 * 状态：DRAFT（2026-10-31，D1 / WP5-0a）。语义说明见同目录 abi-1.1.md。
 * 版本：ABI 1.1.0（ASTER_ABI_VERSION=0x00010100）。
 *
 * 1.1 = 1.0 的**严格超集**：
 *   - 本头 `#include "abi-1.0.h"` 取得 1.0 的 29 个调用号、28 个错误码、
 *     全部常量与 4 个结构体大小；1.0 的数值/布局**一个都不改**；
 *   - 1.1 只追加**新调用号 48–95**（本 WP5-0a 冻结号段）与新的未分配号语义；
 *   - 版本宏在 include 之后覆盖为 1.1.0（1.0 头里的 1.0.0 只代表 1.0 契约）。
 *
 * 三重编号约定：docs/abi/abi-1.0.h（1.0 冻结）、本文件（1.1 增量）、
 * src/ng/syscall.h（内核实现）。tools/abi-check.ps1 同时校验 1.0 不变与
 * 1.1 增量；任何不一致 exit 1。
 *
 * 调用约定（1.1）：
 *   rax = 调用号；参数顺序 rdi, rsi, rdx, r10, [r8, r9]；返回值 = rax
 *   （有符号 64 位；负值 = -errno）。1.0 的调用最多用 4 个参数（不传 r8/r9）；
 *   1.1 新调用最多 6 个参数，按 rdi/rsi/rdx/r10/r8/r9 顺序取。
 *
 * 未分配号 / 未实现号：
 *   - 未分配号（2–9、15–19、28–29、36–39、96 起）不属于任何 ABI 版本；
 *   - 1.1 号段内已分配但内核尚未实现的号（各 WP 落地前）同样返回
 *     ASTER_ABI_ENOSYS(-38)；
 *   - 因此“调用返回 -38”= 该号在**当前内核**上不可用，应用应把它当
 *     ENOSYS 处理，不得把 -1(EPERM) 当作“未实现”的判据。
 *
 * 冻结规则：
 *   - 48–95 的号/名冻结后不得重排、不得复用；新增号只能从 96 起追加；
 *   - 1.0 的编号、错误码数值、结构体布局不变（见 abi-1.0.md §9）；
 *   - 新错误码/新结构体/新 flags 由各自 WP 追加进本头，追加时同步
 *     abi-1.1.md 与 tools/abi-check.ps1；1.1 定稿时状态改为 FROZEN。
 */
#ifndef ASTER_ABI_1_1_H
#define ASTER_ABI_1_1_H

/* 1.0 的全部编号/错误码/常量/结构体大小：逐字继承，不复制、不改值。 */
#include "abi-1.0.h"

/* ------------------------------------------------------------------------- */
/* 版本（覆盖 abi-1.0.h 的 1.0.0；1.0 文件本身保持冻结不变）                   */
/* ------------------------------------------------------------------------- */

#undef ASTER_ABI_VERSION_MAJOR
#undef ASTER_ABI_VERSION_MINOR
#undef ASTER_ABI_VERSION_PATCH
#undef ASTER_ABI_VERSION
#undef ASTER_ABI_VERSION_STRING

#define ASTER_ABI_VERSION_MAJOR 1
#define ASTER_ABI_VERSION_MINOR 1
#define ASTER_ABI_VERSION_PATCH 0
#define ASTER_ABI_VERSION 0x00010100
#define ASTER_ABI_VERSION_STRING "1.1.0"

/*
 * 1.1 的未分配号语义：ENOSYS(-38) 的数值沿用 1.0（abi-1.0.h 已冻结 -38，
 * 1.0 只是“保留不用”）。编译期钉住，防止 1.0 头被误改后 1.1 静默漂移。
 */
#if !defined(ASTER_ABI_ENOSYS) || (ASTER_ABI_ENOSYS != -38)
#error "ABI 1.1 requires ASTER_ABI_ENOSYS == -38 from abi-1.0.h"
#endif

#define ASTER_ABI_UNALLOCATED_ERR ASTER_ABI_ENOSYS /* -38 */
#define ASTER_ABI_LAST_ASSIGNED_NR 95u             /* 1.1 号段上界（含） */
#define ASTER_ABI_NEXT_FREE_NR 96u                 /* 下一个可追加号 */

/* ------------------------------------------------------------------------- */
/* 1.1 号段概览（机器可读的逐号定义在下面；这里只放“范围 → 归属”摘要）        */
/* ------------------------------------------------------------------------- */
/*
 * 1.0（保持不变，逐号见 abi-1.0.h）——29 个：
 *   0 write            1 exit
 *   10 port_create    11 port_send     12 port_recv     13 port_reply    14 port_close
 *   20 open           21 read          22 write_fd      23 close
 *   24 stat           25 listdir       26 mkdir         27 unlink
 *   30 readkey        31 login         32 dmesg         33 log_write
 *   34 pkg_list       35 pkg_verify
 *   40 spawn          41 wait          42 getpid        43 kill
 *   44 getppid        45 shm_alloc     46 shm_map       47 fb_map
 *
 * 1.1 新增（48–95，全部 48 个号在 WP5-0a 冻结；实现前返回 ENOSYS）：
 *   48–62  POSIX 文件/进程（WP5-1a）      63–66  内存（WP5-1b/1c）
 *   67–69  时间/熵（WP5-0f）              70–74  线程/信号（WP5-1d/1e）
 *   75     poll（WP5-1a/2b）              76–87  网络 socket（WP5-2b）
 *   88–93  线程/futex/TLS（WP5-1c）       94–95  共享内存/cap（WP5-0d）
 */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：POSIX 文件/进程（48–62，WP5-1a）                               */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_FSTAT 48u        /* fstat(fd, struct vfs_stat*)          */
#define ASTER_ABI_SYS_LSEEK 49u        /* lseek(fd, off, whence)               */
#define ASTER_ABI_SYS_DUP 50u          /* dup(fd)                              */
#define ASTER_ABI_SYS_DUP2 51u         /* dup2(oldfd, newfd)                   */
#define ASTER_ABI_SYS_PIPE 52u         /* pipe(int fds[2])                     */
#define ASTER_ABI_SYS_CHDIR 53u        /* chdir(path)                          */
#define ASTER_ABI_SYS_GETCWD 54u       /* getcwd(buf, size)                    */
#define ASTER_ABI_SYS_GETUID 55u       /* getuid()                             */
#define ASTER_ABI_SYS_WAITPID 56u      /* waitpid(pid, &status, options)       */
#define ASTER_ABI_SYS_EXECVE 57u       /* execve(path, argv, envp)             */
#define ASTER_ABI_SYS_READLINK 58u     /* readlink(path, buf, size)            */
#define ASTER_ABI_SYS_RENAME 59u       /* rename(old, new)                     */
#define ASTER_ABI_SYS_RMDIR 60u        /* rmdir(path)                          */
#define ASTER_ABI_SYS_FCNTL 61u        /* fcntl(fd, cmd, arg)                  */
#define ASTER_ABI_SYS_GETDENTS 62u     /* getdents(fd, buf, len)               */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：内存（63–66；brk=WP5-1b，mmap/munmap/mprotect=WP5-1c）          */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_BRK 63u          /* brk(addr)                            */
#define ASTER_ABI_SYS_MMAP 64u         /* mmap(addr,len,prot,flags,fd,off)     */
#define ASTER_ABI_SYS_MUNMAP 65u       /* munmap(addr, len)                    */
#define ASTER_ABI_SYS_MPROTECT 66u     /* mprotect(addr, len, prot)            */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：时间 / 熵（67–69，WP5-0f）                                     */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_CLOCK_GETTIME 67u /* clock_gettime(clockid, struct ng_timespec*) */
#define ASTER_ABI_SYS_NANOSLEEP 68u     /* nanosleep(req, rem)                 */
#define ASTER_ABI_SYS_GETRANDOM 69u     /* getrandom(buf, len, flags)          */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：线程 / 信号（70–74；sched_yield=1d，信号=1e）                   */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_SCHED_YIELD 70u     /* sched_yield()                     */
#define ASTER_ABI_SYS_RT_SIGACTION 71u    /* rt_sigaction(sig, act, oldact)    */
#define ASTER_ABI_SYS_RT_SIGPROCMASK 72u  /* rt_sigprocmask(how, set, oldset)  */
#define ASTER_ABI_SYS_RT_SIGRETURN 73u    /* rt_sigreturn()                    */
#define ASTER_ABI_SYS_SIGNAL_SEND 74u     /* signal_send(pid, sig)             */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：poll / socket（75–87；poll=1a/2b，socket=2b）                   */
/* 86/87 为 Tier 2：号与名字保留，实现可延后（未实现前 ENOSYS）。              */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_POLL 75u         /* poll(fds, nfds, timeout_ms)          */
#define ASTER_ABI_SYS_SOCKET 76u       /* socket(domain, type, protocol)       */
#define ASTER_ABI_SYS_BIND 77u         /* bind(fd, addr, len)                  */
#define ASTER_ABI_SYS_CONNECT 78u      /* connect(fd, addr, len)               */
#define ASTER_ABI_SYS_LISTEN 79u       /* listen(fd, backlog)                  */
#define ASTER_ABI_SYS_ACCEPT 80u       /* accept(fd, addr, len)                */
#define ASTER_ABI_SYS_SENDTO 81u       /* sendto(fd,buf,len,flags,addr,alen)   */
#define ASTER_ABI_SYS_RECVFROM 82u     /* recvfrom(fd,buf,len,flags,addr,alen) */
#define ASTER_ABI_SYS_SETSOCKOPT 83u   /* setsockopt(fd, level, opt, val, len) */
#define ASTER_ABI_SYS_GETSOCKOPT 84u   /* getsockopt(fd, level, opt, val, len) */
#define ASTER_ABI_SYS_SHUTDOWN 85u     /* shutdown(fd, how)                    */
#define ASTER_ABI_SYS_GETPEERNAME 86u  /* getpeername(fd, addr, len) [Tier 2]  */
#define ASTER_ABI_SYS_GETSOCKNAME 87u  /* getsockname(fd, addr, len) [Tier 2]  */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：线程 / futex / TLS（88–93，WP5-1c）                             */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_CLONE 88u            /* clone(flags,stack,ptid,ctid,tls) */
#define ASTER_ABI_SYS_FUTEX 89u            /* futex(uaddr, op, val, timeout)   */
#define ASTER_ABI_SYS_SET_TID_ADDRESS 90u  /* set_tid_address(tidptr)          */
#define ASTER_ABI_SYS_ARCH_PRCTL 91u       /* arch_prctl(code, addr)           */
#define ASTER_ABI_SYS_EXIT_THREAD 92u      /* exit_thread(code)                */
#define ASTER_ABI_SYS_GETTID 93u           /* gettid()                         */

/* ------------------------------------------------------------------------- */
/* 1.1 调用号：共享内存 / cap（94–95，WP5-0d）                                */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_SYS_SHM_FREE 94u     /* shm_free(handle)                     */
#define ASTER_ABI_SYS_SPAWN_EX 95u     /* spawn_ex(path, argv, caps, &pid)     */

/*
 * 1.1 号段到此为止（48–95 共 48 个）。96 起留给 D2；2–9、15–19、28–29、
 * 36–39 永远未分配、不复用。未分配号与“已分配未实现号”都返回 ENOSYS。
 */

#endif /* ASTER_ABI_1_1_H */
