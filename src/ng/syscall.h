/*
 * ng WP1-3: syscall（STAR/LSTAR/FMASK）与 ring0/ring3 往返测试。
 * ng WP1-6: 调用号表（0/1 已用；10..14 为 IPC/句柄）。
 * ng WP1-7: syscall 0 支持用户缓冲区 write；ring3 执行统一入口 + per-thread
 *           内核栈（TSS.RSP0 / syscall_kernel_rsp0 同步）。
 */
#ifndef NG_SYSCALL_H
#define NG_SYSCALL_H

#include "types.h"

struct thread; /* 前向声明：per-thread 内核栈接口用 */

/*
 * 调用号（ABI 1.0 草案，WP4-1 冻结）：
 *   rax=调用号，rdi/rsi/rdx/r10 = 参数，返回值放 rax（负 = 错误码）。
 *   0  write(buf, len)                     -> 写入的字节数 / -err
 *      rdi==0 时是 WP1-3 的诊断模式（只打印 "syscall write (ringN)"）
 *   1  exit(code)                          -> 不返回（ring3 退出到内核恢复点）
 *  10  port_create(depth)                  -> handle / -err
 *  11  port_send(handle, msg, len, tmo)    -> 0 / -err
 *  12  port_recv(handle, buf, max, tmo)    -> 消息总长 / -err
 *  13  port_reply(ticket, msg, len, tmo)   -> 0 / -err
 *  14  port_close(handle)                  -> 0 / -err
 *  20  open(path, flags, mode)             -> fd / -err          （WP2-1）
 *  21  read(fd, buf, len)                  -> 字节数 / -err     （WP2-1）
 *  22  write(fd, buf, len)                 -> 字节数 / -err     （WP2-1）
 *  23  close(fd)                           -> 0 / -err          （WP2-1）
 *  24  stat(path, struct vfs_stat*)        -> 0 / -err          （WP2-1）
 *  25  listdir(path, struct vfs_dirent*, max) -> 条数 / -err    （WP2-1）
 *  26  mkdir(path, mode)                   -> 0 / -err          （WP2-1）
 *  27  unlink(path)                        -> 0 / -err          （WP2-1）
 *  30  readkey()                           -> (flags<<24)|(ascii<<16)|scancode
 *                                             / -EAGAIN          （WP2-3）
 *  31  login(name, pass)                   -> uid / -err        （WP2-5）
 *  32  dmesg(buf, max)                     -> 字节数 / -err     （WP2-5）
 *  33  log_write(tag, msg)                 -> 0 / -err          （WP2-5）
 *  34  pkg_list(buf, max)                  -> 包数 / -err       （WP2-5）
 *  35  pkg_verify(name)                    -> 0 / -err          （WP2-5）
 *  40  spawn(path, argv, &pid)              -> 0 / -err          （WP3-0）
 *  41  wait(pid, &status)                   -> 0 / -err          （WP3-0，阻塞）
 *  42  getpid()                             -> pid               （WP3-0）
 *  43  kill(pid, sig)                       -> 0 / -err          （WP3-0）
 *  44  getppid()                            -> ppid              （WP3-0）
 *  45  shm_alloc(size_pages, &va, &handle)  -> 0 / -err          （WP3-1）
 *  46  shm_map(handle, &va)                 -> 0 / -err          （WP3-1）
 *  47  fb_map(&va, &info)                   -> 0 / -err          （WP3-1，root）
 * 其中 msg/buf/path/stat 是用户指针（内核做 U/S 页校验 + SMAP copyin/copyout）；
 * timeout 单位是 TSC tick（0 = 非阻塞，~0 = 永久）。
 *
 * WP3-0 起 10..14 的参数是 **ticket**（端口在全局端口池里的身份，(gen<<16)|slot），
 * 不再是"本进程句柄"：ticket 可以交给别的进程；recv/close 要求调用者是端口
 * 的属主进程。进程自己的句柄表（proc_t.handles）记录"我拥有哪些端口"，
 * 退出/被 kill 时按表回收。
 *
 * ABI 1.1（WP5-0a，docs/abi/abi-1.1.h）：
 *   - 48..95 是 1.1 新号段（已冻结分配，尚未实现）；未分配号（2..9、15..19、
 *     28..29、36..39、96..）也不属于任何 ABI；
 *   - 两者在 syscall_dispatch() 的 default 统一返回 -ENOSYS(-38)，不再是过去的
 *     -1(EPERM)。1.0 §10 第 1 项已声明未分配号行为不属于 1.0 契约，故不破坏
 *     1.0；已分配 0/1、10..14、20..27、30..35、40..47 的语义/错误码不变。
 */
#define NG_SYS_WRITE 0u
#define NG_SYS_EXIT 1u
#define NG_SYS_PORT_CREATE 10u
#define NG_SYS_PORT_SEND 11u
#define NG_SYS_PORT_RECV 12u
#define NG_SYS_PORT_REPLY 13u
#define NG_SYS_PORT_CLOSE 14u
#define NG_SYS_OPEN 20u
#define NG_SYS_READ 21u
#define NG_SYS_WRITE_FD 22u
#define NG_SYS_CLOSE 23u
#define NG_SYS_STAT 24u
#define NG_SYS_LISTDIR 25u
#define NG_SYS_MKDIR 26u
#define NG_SYS_UNLINK 27u
#define NG_SYS_READKEY 30u
#define NG_SYS_LOGIN 31u
#define NG_SYS_DMESG 32u
#define NG_SYS_LOG_WRITE 33u
#define NG_SYS_PKG_LIST 34u
#define NG_SYS_PKG_VERIFY 35u
#define NG_SYS_SPAWN 40u
#define NG_SYS_WAIT 41u
#define NG_SYS_GETPID 42u
#define NG_SYS_KILL 43u
#define NG_SYS_GETPPID 44u
#define NG_SYS_SHM_ALLOC 45u
#define NG_SYS_SHM_MAP 46u
#define NG_SYS_FB_MAP 47u
/*
 * ABI 1.1（WP5-0a）：未分配号与"1.1 已分配但未实现号"的统一返回值，数值沿用
 * ABI 1.0 保留的 ENOSYS(-38)。默认分支只返回它，不返回 -1。
 */
#define NG_SYS_ENOSYS (-38)

/* ring3 一次执行的统计（syscall_run_user 填）。 */
struct ring3_result {
    u64 syscalls;   /* 从 CPL3 进来的 syscall 次数 */
    u64 writes;     /* 成功执行的 write(buf,len) 次数 */
    u64 bytes;      /* write 写出的总字节数 */
    u64 fs_calls;   /* 20..27 的调用次数（WP2-1 演示的证据） */
    u64 exit_code;  /* syscall 1 的 rdi */
    int exited;     /* 命中 syscall 1（回到内核恢复点） */
    int user_rsp_ok;/* 每次进入内核时的 RSP 都在用户栈区间内 */
    int bad_writes; /* 坏用户指针的 write 次数（应为 0） */
};

/* 置 EFER.SCE 与 STAR/LSTAR/FMASK，打印读回值；同步引导线程内核栈。 */
void syscall_init(void);

/* ring0 syscall 往返 + ring3 syscall 往返；返回失败项数。 */
int syscall_selftest(void);

/*
 * WP1-7：进 CPL3 执行 entry（用户 RSP = user_rsp，栈区间 [stack_lo,stack_hi)
 * 用于校验 syscall 入口 RSP）。返回 0 = syscall 1 正常退出；-1 = 参数非法或
 * 没有退出（异常会先 panic）。统计写进 *out（可为 NULL）。
 */
int syscall_run_user(u64 entry, u64 user_rsp, u64 stack_lo, u64 stack_hi,
                     struct ring3_result *out);

/*
 * ng WP3-0：带初始 argc/argv 的版本（进程入口 ABI：用户态 rdi=argc、
 * rsi=argv）。argc=0/argv=0 等价于 syscall_run_user()。
 */
int syscall_run_user_args(u64 entry, u64 user_rsp, u64 stack_lo, u64 stack_hi,
                          u64 argc, u64 argv, struct ring3_result *out);

/*
 * WP1-7：per-thread 内核栈。syscall_set_thread_stack() 在每次上下文切换前
 * 被 sched_arch.c 调用；syscall_kernel_stack_sync() 把"当前线程"的栈顶同步
 * 到 syscall 入口与 TSS.RSP0，并返回该栈顶。
 */
void syscall_set_kernel_stack(u64 top);
void syscall_set_thread_stack(struct thread *t);
u64 syscall_kernel_stack_sync(void);

/* WP3-0：ring3_enter 在 iretq 前调用，把恢复点存进当前线程（isr.S 调用）。 */
void ring3_save_resume(u64 rsp, u64 rip);

#endif /* NG_SYSCALL_H */
