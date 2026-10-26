/* =============================================================================
 * proc/audit_priv_selftest.c - 权限边界自检（audit-smp 会话接收 audit-syscall P0）
 *
 * 三条纯函数回归，在 sched_start() 之前（还没有任何进程在跑）执行：
 *   1) SYS_SIGRETURN 信号帧校验：用户不能伪造 cs/ss 进 ring0，不能带
 *      IOPL/NT/VM86，eip 必须在用户地址空间；
 *   2) SYS_KILL 的 uid 策略：root / 自己 / 同 uid 允许，跨用户拒绝；
 *   3) copy_*_user 的全局地址窗口：内核低地址、>=3GB、回绕全部拒绝，
 *      内核自检（非用户上下文）保留低地址例外。
 *
 * 期望串口行：
 *   audit: sigreturn frame validation ok (user cs/ss, user eip, no iopl/nt/vm)
 *   audit: kill uid policy ok (root/self/same-uid/other)
 *   audit: uaccess user window ok (kernel-low/3gb/wrap rejected)
 * ============================================================================= */
#include "proc.h"
#include "proc_internal.h"
#include "proc_sec.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/userland.h"

static void priv_frame_init(interrupt_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->cs = 0x1BU;
    f->ss = 0x23U;
    f->eip = USER_LAYOUT_CODE_BASE + 0x1000U;
    f->useresp = USER_LAYOUT_STACK_TOP - 0x100U;
    f->eflags = 0x202U;                 /* IF + 保留位 */
}

static uint32_t priv_sig_fail;

static bool priv_sigframe_test(void)
{
    interrupt_frame_t f;

    priv_frame_init(&f);
    if (!proc_sigframe_valid(&f)) priv_sig_fail |= 0x1U;   /* 合法帧 */

    priv_frame_init(&f);
    f.cs = 0x08U;                                          /* ring0 代码段 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x2U;

    priv_frame_init(&f);
    f.ss = 0x10U;                                          /* ring0 数据段 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x4U;

    priv_frame_init(&f);
    f.cs = 0x3BU;                                          /* 用户段但 RPL=3 的 LDT/其它 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x8U;

    priv_frame_init(&f);
    f.eip = 0xC0001000U;                                   /* 内核高地址 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x10U;

    priv_frame_init(&f);
    f.eflags = 0x202U | (3U << 12);                        /* IOPL=3 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x20U;

    priv_frame_init(&f);
    f.eflags = 0x202U | (1U << 14);                        /* NT */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x40U;

    priv_frame_init(&f);
    f.eflags = 0x202U | (1U << 17);                        /* VM86 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x80U;

    priv_frame_init(&f);
    f.eflags = 0x200U;                                     /* 缺 bit1 */
    if (proc_sigframe_valid(&f)) priv_sig_fail |= 0x100U;

    return (priv_sig_fail == 0U);
}

static bool priv_kill_test(void)
{
    proc_t root;
    proc_t alice;
    proc_t bob;
    bool ok = true;

    memset(&root, 0, sizeof(root));
    memset(&alice, 0, sizeof(alice));
    memset(&bob, 0, sizeof(bob));

    root.uid = root.euid = root.suid = 0U;
    alice.uid = alice.euid = alice.suid = 1000U;
    bob.uid = bob.euid = bob.suid = 1001U;

    ok = ok && proc_can_signal(&root, &alice);             /* root -> 任意 */
    ok = ok && proc_can_signal(&alice, &alice);            /* 自己 */
    ok = ok && proc_can_signal(&alice, &bob) == false;     /* 跨用户拒绝 */
    ok = ok && proc_can_signal(&bob, &root) == false;      /* 普通用户 -> root */
    alice.suid = 1001U;                                    /* 保存 uid 匹配允许 */
    ok = ok && proc_can_signal(&bob, &alice);
    ok = ok && proc_can_signal(0, &alice);                 /* 内核上下文 */
    return ok;
}

static bool priv_uaccess_test(void)
{
    const bool U = true;                                  /* 用户上下文 */
    const bool K = false;                                 /* 内核自检例外 */
    bool ok = true;

    ok = ok && !uaccess_addr_ok(0x00100000U, 4U, U);      /* 内核恒等映射低地址 */
    ok = ok && uaccess_addr_ok(0x40000000U, 4U, U);       /* 用户基址 */
    ok = ok && !uaccess_addr_ok(0xBFFFFFFCU, 8U, U);      /* 越过 3GB */
    ok = ok && !uaccess_addr_ok(0xFFFFFFFCU, 8U, U);      /* 回绕 */
    ok = ok && !uaccess_addr_ok(0xC0000000U, 1U, U);
    ok = ok && uaccess_addr_ok(0x00100000U, 4U, K);       /* boot syscall_selftest */
    return ok;
}

void audit_priv_selftest(void)
{
    bool sig_ok = priv_sigframe_test();
    bool kill_ok = priv_kill_test();
    bool ua_ok = priv_uaccess_test();

    if (sig_ok) {
        kprintln("audit: sigreturn frame validation ok (user cs/ss, user eip, no iopl/nt/vm)");
    } else {
        kprintf("audit: sigreturn frame validation FAIL (mask=0x%x)\n", (unsigned int)priv_sig_fail);
    }
    if (kill_ok) {
        kprintln("audit: kill uid policy ok (root/self/same-uid/other)");
    } else {
        kprintln("audit: kill uid policy FAIL");
    }
    if (ua_ok) {
        kprintln("audit: uaccess user window ok (kernel-low/3gb/wrap rejected)");
    } else {
        kprintln("audit: uaccess user window FAIL");
    }
}
