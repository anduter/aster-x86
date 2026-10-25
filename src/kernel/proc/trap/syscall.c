/* =============================================================================
 * syscall.c - 旧原型系统调用分发（迁移期 int 0x81）
 *
 * 入口是 isr.asm 的 isr129（压栈布局与异常一致），isr_dispatch 看到 129
 * 就转到 syscall_dispatch。返回值写回现场的 eax 槽位，iret 后就在 eax 里。
 * 新程序请使用 int 0x80 的 Native ABI（intr/native.c）。
 *
 * 本文件现在只剩：入口 + 沙箱检查 + 按模块委派。
 * 各组实现：net/net_syscall.c（M5）、fs/fs_syscall.c（M4）、
 * proc/proc_syscall.c（M1）、mm/mm_syscall.c（M2）、svc/svc_syscall.c（M6）、
 * security/security_syscall.c（M7）、core/misc_syscall.c（杂项/调试）。
 * 新增 legacy 调用请加到对应模块的 *_syscall.c，不要再往这里堆 case。
 *
 * 所有委派都必须走 `ret = xxx_syscall(f); goto delegate_out;`：尾部统一的
 * proc_check_signals(f) 负责返回用户态前投递待处理信号，早退会漏掉它。
 * ============================================================================= */
#include "syscall.h"
#include "interrupt.h"
#include "../proc.h"
#include "../../net/net_syscall.h"
#include "../../fs/fs_syscall.h"
#include "../proc_syscall.h"
#include "../../mm/mm_syscall.h"
#include "../../svc/svc_syscall.h"
#include "../../security/security_syscall.h"
#include "../../core/misc_syscall.h"

static unsigned int calls;

void syscall_dispatch(interrupt_frame_t *f)
{
    int ret;
    proc_t *sp = proc_current();

    calls++;

    /* 最小沙箱：进程只允许 syscall_mask 中列出的 legacy 调用。
     * 位图只有低 64 位（1..63）；ABI v1.3 起的 64+ 调用默认对沙箱进程关闭，
     * 否则任何 >= 64 的新调用都能绕过 allowlist。 */
    if (sp != 0 && sp->sandboxed) {
        if (f->eax >= 64U ||
            (f->eax > 0U && (sp->syscall_mask & (1ULL << f->eax)) == 0ULL)) {
            f->eax = (uint32_t)-1;
            return;
        }
    }

    /* 网络组（UDP/socket） */
    if (net_syscall_owns(f->eax)) {
        ret = net_syscall(f);
        goto delegate_out;
    }

    /* 文件系统组 */
    if (fs_syscall_owns(f->eax)) {
        ret = fs_syscall(f);
        goto delegate_out;
    }

    /* 进程组 */
    if (proc_syscall_owns(f->eax)) {
        ret = proc_syscall(f);
        goto delegate_out;
    }

    /* 内存组 */
    if (mm_syscall_owns(f->eax)) {
        ret = mm_syscall(f);
        goto delegate_out;
    }

    /* 服务/能力组 */
    if (svc_syscall_owns(f->eax)) {
        ret = svc_syscall(f);
        goto delegate_out;
    }

    /* 身份/权限组 */
    if (security_syscall_owns(f->eax)) {
        ret = security_syscall(f);
        goto delegate_out;
    }

    /* 杂项/调试组 */
    if (misc_syscall_owns(f->eax)) {
        ret = misc_syscall(f);
        goto delegate_out;
    }

    /* 未登记的调用号 */
    ret = -1;

delegate_out:
    /* pushad 保存的 eax 就在现场里，改这里等价于设置返回值 */
    f->eax = (unsigned int)ret;

    /* 返回用户态前处理待处理信号（可能直接终止或跳到用户 handler） */
    proc_check_signals(f);
}

unsigned int syscall_count(void)
{
    return calls;
}
