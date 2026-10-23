/* =============================================================================
 * apps/spin.c - 调度器压力自检（多核负载均衡回归用）
 *
 * 启动时创建若干个"忙等 2 秒"的内核线程：
 *   - 它们的放置完全由 sched_place() 按负载决定，不看程序名；
 *   - 只要有一个被放到 AP，就证明负载放置/迁移路径真的工作；
 *   - 运行结束后自动 proc_exit，不影响后续交互。
 * ============================================================================= */
#include "spin.h"
#include "../proc/proc.h"
#include "../core/percpu.h"
#include "../intr/interrupt.h"
#include "../lib/kprintf.h"

#define SPIN_TICKS 200U     /* 100Hz 下约 2 秒 */

void spin_entry(void)
{
    proc_t *me = proc_current();
    uint32_t start = timer_ticks();

    /* 纯忙等：不主动让出，靠时间片轮转打断，制造持续负载 */
    while ((timer_ticks() - start) < SPIN_TICKS) {
        __asm__ volatile ("pause");
    }

    kprintf("spin: pid %u done on cpu %u\n",
            (me != 0) ? me->pid : 0U, smp_cpu_id());
    proc_exit(0);
}
