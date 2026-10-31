/*
 * ng WP3-0: 进程抽象 + 每进程句柄表 + 进程表（纯状态机 + 内核胶水）。
 *
 * 分两层（与 WP1-5 的 sched、WP1-6 的 ipc 同一套路）：
 *
 *   1. `struct proc_table` + `proc_*_table()`：**纯状态机**。只依赖 types.h 与
 *      handle.h，不认识 PML4/线程/ELF/串口，host 单测直接构造进程表驱动
 *      （tests/unit-ng/test_proc.c）。这里定义 pid 单调递增、父子关系、
 *      退出/僵尸/回收、wait 的阻塞条件、kill 的状态迁移、孤儿重挂。
 *   2. `proc.c`：内核胶水。spawn 从 VFS 读 ELF -> elf64_load 进新 AS -> 建
 *      内核线程 -> ring3 入口；wait 用 thread_sleep/thread_wake 阻塞；kill
 *      摘线程、摘 IPC 等待者、回收 AS/句柄/栈。
 *
 * 进程模型（D0 的最小子集）：
 *   - 每个进程 = 1 个用户地址空间（独立 PML4）+ 1 个主线程 + 1 张句柄表；
 *   - 进程表静态 32 槽，pid 从 1 单调递增，永不复用（槽位可以复用，pid 不行）；
 *   - pid 1 是内核 init（kmain 所在的 boot 线程，没有 AS），所有孤儿重挂到它；
 *   - 退出状态：正常退出 = exit code（0..255）；被 kill = 0x100|sig。
 *
 * 句柄表（每进程 64 槽）：
 *   - PORT 句柄指向**全局端口池**里的端口对象；用户可以拿到的是端口的
 *     **ticket**（全局身份 (gen<<16)|slot），ticket 才能跨进程传递；
 *   - THREAD 句柄指向主线程（为后续 fork/多线程留位）；
 *   - FILE 句柄位保留（VFS 的 fd 表仍是全局一份，见 claims 遗留）；
 *   - 进程退出/被 kill 时只按**自己的**表回收自己的端口，不会误关别人的。
 */
#ifndef NG_PROC_H
#define NG_PROC_H

#include "types.h"

#include "handle.h"

/* ---- 上限 ---------------------------------------------------------------- */
#define PROC_MAX 32u
#define PROC_PATH_MAX 64u
#define PROC_CWD_MAX 128u
#define PROC_MAX_ARGS 8u
#define PROC_ARG_MAX 128u
/* spawn 读 ELF 的上限（ramfs 单文件 1 MiB；这里给 256 KiB 足够）。 */
#define PROC_ELF_MAX (256u * 1024u)
/* 用户栈与 WP1-7/2-6 一致：16 KiB，下面一页不映射。 */
#define PROC_STACK_PAGES 4u
/*
 * ng WP3-0：每个进程都映射一页"进程标记页"（RW+NX，U=1），VA 固定。
 * 同一 ELF 的两个进程在**同一个 VA** 上的物理页不同 —— 这是 AS 隔离的
 * 自检证据；用户程序也用它确认自己拿到的是私有页。
 */
#define PROC_MARKER_VA 0x0000000040010000ULL
/* 用户进程主线程的 SYSCALL 入口栈（与 C 栈分开，见 thread.h 的说明）。 */
#define PROC_SYSCALL_STACK 16384ULL

/* ---- 状态 ---------------------------------------------------------------- */
#define PROC_STATE_FREE 0u
#define PROC_STATE_READY 1u   /* 已创建，等第一次被调度 */
#define PROC_STATE_RUNNING 2u /* 正在 CPU 上（含在 syscall 里） */
#define PROC_STATE_BLOCKED 3u /* 阻塞在 syscall（wait/recv/...） */
#define PROC_STATE_ZOMBIE 4u  /* 已退出，等父进程 wait 回收 */

#define PROC_EXIT_NONE 0u
#define PROC_EXIT_NORMAL 1u /* syscall 1 正常退出 */
#define PROC_EXIT_KILLED 2u /* kill 终止 */

/* 退出状态编码（int）。正常退出只占低 8 位；killed 用 bit8 区分。 */
#define PROC_STATUS_KILL_FLAG 0x100
#define PROC_STATUS_EXIT(code) ((int)((code) & 0xFF))
#define PROC_STATUS_KILLED(sig) (PROC_STATUS_KILL_FLAG | (int)((sig) & 0xFF))
#define PROC_STATUS_IS_KILLED(s) (((s) & PROC_STATUS_KILL_FLAG) != 0)
#define PROC_STATUS_SIG(s) ((s) & 0xFF)

/* ---- 错误码（与 VFS/IPC 同一套负 errno）--------------------------------- */
#define PROC_OK 0
#define PROC_EPERM (-1)
#define PROC_ENOENT (-2)
#define PROC_ESRCH (-3)
#define PROC_EINTR (-4)
#define PROC_EIO (-5)
#define PROC_ENOMEM (-12)
#define PROC_EFAULT (-14)
#define PROC_EBUSY (-16)
#define PROC_EINVAL (-22)
#define PROC_ENOSPC (-28)
#define PROC_ECHILD (-10)

/* ---- 进程槽 -------------------------------------------------------------- */
struct proc {
    u32 pid;  /* 0 = 空槽（pid 从 1 起） */
    u32 ppid;
    u32 state;
    u32 tid;  /* 主线程 tid（0 = 还没建线程） */
    u32 uid;
    u32 exit_kind;
    int exit_status; /* 见 PROC_STATUS_* */
    u32 killed_sig;
    u32 wait_tid;   /* 正在 wait 我的线程 tid（0 = 没人等） */
    u32 wait_count; /* 被 wait 的次数（证据） */
    u32 flags;      /* PROC_F_* */
    u64 entry;      /* ELF e_entry（用户 VA） */
    u64 user_rsp;   /* 初始用户栈指针 */
    u64 argv_va;    /* 初始 argv（用户栈上的 char* 数组） */
    u32 argc;
    u64 stack_lo;
    u64 stack_hi;
    u64 load_base;
    u64 load_end;
    u64 load_pages;
    u32 load_segs;
    u32 marker_phys; /* 标记页物理地址（证据/自检） */
    /*
     * ng WP3-1：共享内存 bump 分配器（0 = 还没用过，从 SHM_VA_BASE 开始）。
     * 同一个 shm 对象在 displayd 和客户端里可以落在不同 VA —— 这是正常的。
     */
    u64 shm_next_va;
    char path[PROC_PATH_MAX];
    char cwd[PROC_CWD_MAX];
    void *as;      /* struct user_as *（纯表把它当不透明指针） */
    u32 as_pml4;   /* PML4 物理地址低 32 位（证据） */
    struct handle_table handles;
    /* 退出时的 ring3 统计（wp26 的证据行要用） */
    u64 syscalls;
    u64 writes;
    u64 fs_calls;
    u64 exit_code;
    u32 user_rsp_ok;
    u32 bad_writes;
    u32 exited;
};

/* proc.flags */
#define PROC_F_INIT 0x1u  /* pid 1：内核 init，没有用户 AS */
#define PROC_F_WAITED 0x2u /* 被 wait 过 */

struct proc_table {
    struct proc procs[PROC_MAX];
    u32 next_pid;
    u32 live;
    u64 spawned;
    u64 exited;
    u64 killed;
    u64 reaped;
    u64 wait_calls;
    u32 full_rejects;
    u32 bad_pids; /* 找不到 pid / pid 非法 */
};

/* ---- 纯状态机 ------------------------------------------------------------ */
void proc_table_init(struct proc_table *pt);
/* 分配一个槽：pid 单调递增（跳过 0）；表满返回 NULL。 */
struct proc *proc_alloc(struct proc_table *pt, u32 ppid, u32 uid);
struct proc *proc_find(struct proc_table *pt, u32 pid);
u32 proc_live(const struct proc_table *pt);
u32 proc_child_count(struct proc_table *pt, u32 ppid);
/* 第 index 个子进程（0..count-1）；没有返回 NULL。 */
struct proc *proc_child_at(struct proc_table *pt, u32 ppid, u32 index);
int proc_is_child(struct proc_table *pt, u32 pid, u32 ppid);

/*
 * 标记退出（正常/被 kill）。成功返回 1，*out_parent 是应该被唤醒的父 pid
 * （0 = 没有父进程要唤醒）；pid 不存在/已经退出返回 PROC_ESRCH/PROC_EBUSY。
 */
int proc_mark_exit(struct proc_table *pt, u32 pid, u32 kind, int status,
                   u32 *out_parent);
/*
 * 回收一个僵尸槽位（父进程 wait 成功后调用）。不是僵尸返回 PROC_EBUSY，
 * 没有这个 pid 返回 PROC_ESRCH。回收后槽位可复用，pid 不复用。
 */
int proc_reap(struct proc_table *pt, u32 pid);
/* kill 的状态迁移：只标记 + 返回应该被唤醒的父 pid；线程回收由内核胶水做。 */
int proc_kill_mark(struct proc_table *pt, u32 pid, u32 sig, u32 *out_parent);
/* 父进程退出时把子进程重挂到 new_ppid；返回被重挂的个数。 */
u32 proc_reparent_children(struct proc_table *pt, u32 pid, u32 new_ppid);

/* ---- 内核胶水 ------------------------------------------------------------ */
void proc_init(void);
/* 当前进程（内核线程/init 之外的上下文返回 NULL）。 */
struct proc *proc_current(void);
/* spawn：从 VFS 读 ELF、建 AS/栈/argv、建线程；*out_pid 写 pid。 */
int proc_spawn(const char *path, const char *const *argv, u32 *out_pid);
/* 内核线程等子进程：成功返回 PROC_OK 并写 *status（可空）；阻塞。 */
int proc_wait_pid(u32 pid, int *status, struct proc **out_proc);
/* wait 成功后回收僵尸槽位（*out_proc 的快照仍可用到回收前）。 */
int proc_reap_pid(u32 pid);
/* kill：sig 为信号号（status 记 0x100|sig）。 */
int proc_kill(u32 pid, u32 sig);
u32 proc_getpid(void);
u32 proc_getppid(void);
u32 proc_self_pid(void);
/* 自检/诊断： */
u32 proc_table_live(void);
u64 proc_table_spawned(void);
const struct proc *proc_slot(u32 pid);

/* ---- 进程自己的句柄表（ipc.c 用）--------------------------------------- */
int proc_port_install(struct proc *p, u32 ticket, void *port);
int proc_port_release(struct proc *p, void *port);
u32 proc_handle_live(const struct proc *p);

/* ---- syscall 入口（40..44）--------------------------------------------- */
u64 proc_sys_spawn(u64 upath, u64 uargv, u64 upid);
u64 proc_sys_wait(u64 pid, u64 ustatus);
u64 proc_sys_getpid(void);
u64 proc_sys_kill(u64 pid, u64 sig);
u64 proc_sys_getppid(void);

#endif /* NG_PROC_H */
