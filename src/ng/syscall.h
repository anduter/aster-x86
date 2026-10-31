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
 * 其中 msg/buf/path/stat 是用户指针（内核做 U/S 页校验 + SMAP copyin/copyout）；
 * timeout 单位是 TSC tick（0 = 非阻塞，~0 = 永久）。
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
 * WP1-7：per-thread 内核栈。syscall_set_thread_stack() 在每次上下文切换前
 * 被 sched_arch.c 调用；syscall_kernel_stack_sync() 把"当前线程"的栈顶同步
 * 到 syscall 入口与 TSS.RSP0，并返回该栈顶。
 */
void syscall_set_kernel_stack(u64 top);
void syscall_set_thread_stack(struct thread *t);
u64 syscall_kernel_stack_sync(void);

#endif /* NG_SYSCALL_H */
