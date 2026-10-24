/* =============================================================================
 * security/security_syscall.c - 身份/权限相关系统调用（M7）
 *
 * 从 proc/trap/syscall.c 拆出的 security 组：getuid / getgid / setuid。
 * 只搬代码不改逻辑（capability 的 cap_drop/cap_send 按拆分计划留在 svc 组）。
 * ============================================================================= */
#include "../types.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "security_syscall.h"

bool security_syscall_owns(uint32_t nr)
{
    return nr == SYS_GETUID || nr == SYS_GETGID || nr == SYS_SETUID;
}

int security_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_GETUID:
        ret = (proc_current() != 0) ? (int)proc_current()->uid : 0;
        break;
    case SYS_GETGID:
        ret = (proc_current() != 0) ? (int)proc_current()->gid : 0;
        break;
    case SYS_SETUID: {
        /* 只有 root 能改身份；降权后不能回升（单向） */
        proc_t *p = proc_current();

        if (p == 0 || p->uid != 0U) { ret = -1; break; }
        p->uid = f->ebx;
        p->gid = (f->ecx != 0U) ? f->ecx : f->ebx;
        ret = 0;
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
