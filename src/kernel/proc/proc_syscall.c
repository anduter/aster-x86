/* =============================================================================
 * proc/proc_syscall.c - 进程相关系统调用（M1）
 *
 * 从 proc/trap/syscall.c 拆出的 proc 组：getpid/uptime/yield/exit/kill/sleep/
 * fork/exec/wait/sigaction/sigreturn。只搬代码不改逻辑。
 * ============================================================================= */
#include "../types.h"
#include "include/kernel.h"
#include "proc_syscall.h"
#include "proc.h"
#include "trap/interrupt.h"
#include "trap/syscall.h"
#include "trap/syscall_internal.h"
#include "trap/uaccess.h"
#include "include/userland.h"

static int sys_getpid(void)
{
    proc_t *p = proc_current();
    return (p != 0) ? (int)p->pid : 0;
}

static int sys_uptime(void)
{
    return (int)(timer_ticks() / timer_hz());
}

static int sys_yield(void)
{
    proc_yield();
    return 0;
}

bool proc_syscall_owns(uint32_t nr)
{
    /* 2..5   : GETPID/UPTIME/YIELD/EXIT
     * 18..20 : FORK/EXEC/WAIT
     * 23..24 : KILL/SLEEP
     * 30..31 : SIGACTION/SIGRETURN
     * 60..61 : WAITPID/GETPPID（ABI v1.1 追加） */
    return (nr >= SYS_GETPID && nr <= SYS_EXIT) ||
           (nr >= SYS_FORK && nr <= SYS_WAIT) ||
           (nr >= SYS_KILL && nr <= SYS_SLEEP) ||
           (nr >= SYS_SIGACTION && nr <= SYS_SIGRETURN) ||
           nr == SYS_WAITPID || nr == SYS_GETPPID;
}

int proc_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_GETPPID:
        ret = proc_getppid();
        break;
    case SYS_UPTIME:
        ret = sys_uptime();
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_EXIT:
        /* 不返回：直接让当前进程退出 */
        proc_exit((int)f->ebx);
        ret = 0;
        break;
    case SYS_KILL: {
        proc_t *t = 0;

        for (uint32_t i = 0; i < PROC_MAX; i++) {
            const proc_t *q = proc_at(i);
            if (q != 0 && q->pid == f->ebx && q->state != PROC_FREE) { t = (proc_t *)q; break; }
        }
        if (t == 0) {
            ret = -1;
        } else {
            proc_send_signal(t, (int)f->ecx);
            ret = 0;
        }
        break;
    }
    case SYS_SLEEP: {
        uint32_t ms = f->ebx;
        uint32_t ticks = ms / 10U;
        if (ms > 0U && ticks == 0U) ticks = 1U;
        proc_sleep_ticks(ticks);
        ret = 0;
        break;
    }
    case SYS_SIGACTION:
        ret = proc_signal_set((int)f->ebx, (uint32_t)f->ecx);
        break;
    case SYS_SIGRETURN:
        ret = proc_sigreturn((struct interrupt_frame *)f);
        break;
    case SYS_FORK: {
        proc_t *child = proc_fork((struct interrupt_frame *)f);
        ret = (child != 0) ? (int)child->pid : -1;
        break;
    }
    case SYS_EXEC: {
        char rel[256];
        char args[96];
        char path[256];

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) {
            ret = -1;
            break;
        }
        if (f->ecx != 0U) {
            if (copy_user_string(args, sizeof(args), f->ecx) != E_OK) {
                ret = -1;
                break;
            }
        } else {
            args[0] = '\0';
        }
        make_abs_path(path, sizeof(path), rel);
        ret = user_exec_replace(path, args, (struct interrupt_frame *)f);
        break;
    }
    case SYS_WAIT: {
        int status = 0;
        int child = proc_wait((int)f->ebx, &status);

        if (child > 0 && f->ecx != 0U) {
            if (copy_to_user((void *)(uintptr_t)f->ecx, &status, sizeof(status)) != E_OK) {
                child = -1;
            }
        }
        ret = child;
        break;
    }
    case SYS_WAITPID: {
        /* options 在 edx：0 = 阻塞等待（同 SYS_WAIT），WNOHANG = 不阻塞。
         * WNOHANG 返回 0 时不写 status，也不碰用户缓冲。 */
        int status = 0;
        int child = proc_wait_options((int)f->ebx, &status, (int)f->edx);

        if (child > 0 && f->ecx != 0U) {
            if (copy_to_user((void *)(uintptr_t)f->ecx, &status, sizeof(status)) != E_OK) {
                child = -1;
            }
        }
        ret = child;
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
