/* =============================================================================
 * proc/sched_selftest.c - 调度器优先级 + aging 自检
 *
 * 在 sched_start() 选第一个进程之前注册测试程序 "schedprio"，并创建 3 个
 * 忙等线程（全部通过 cpu_pin/sched_enqueue 固定在 BSP，共享同一 runqueue）：
 *
 *   prio 0（高）  忙等 30 tick
 *   prio 1（中）  忙等 4 tick
 *   prio 2（低）  忙等 4 tick
 *
 * 三个优先级都高于系统默认的 4，因此自检会在 shell 等默认优先级进程之前
 * 跑完，串口输出不会和启动日志交错；同时低优先级在等待期间仍会被 aging
 * 提升（默认窗口 20 tick），证明不会饿死。
 *
 * 期望：
 *   1. 第一次获得 CPU 的顺序严格是 0,1,2 —— 高优先级先运行；
 *   2. 中/低优先级在等待期间被 aging 提升过（aged > 0），低优先级最终也能
 *      运行，不会被高优先级饿死；
 *   3. 被调度上 CPU 时 eff_prio 已回落到 base_prio。
 *
 * 打印（回归脚本按这些行加检查项）：
 *   sched: priority ok (order=0,1,2)
 *   sched: aging ok (base=2 aged=1 eff=2)
 *
 * 顺序可判定的前提：三个线程都固定在 BSP，且高优先级线程在自己的时间片
 * 到期时会重新被选中（sched_tick 先把 cur 放回 READY 再选）。若 future 的
 * 负载/时间片改动让严格顺序不稳定，这里只依赖“高优先级先、低优先级最终
 * 运行”，不依赖中间线程的绝对 tick。
 * ============================================================================= */
#include "proc.h"
#include "proc_internal.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/userland.h"

#define SELFTEST_PROG   "schedprio"
#define SELFTEST_THREADS 3U

#define SELFTEST_PRIO_HIGH  0U
#define SELFTEST_PRIO_MID   1U
#define SELFTEST_PRIO_LOW   2U

#define SELFTEST_HIGH_TICKS 30U   /* 高优先级跑得够久，低优先级能触发 aging */
#define SELFTEST_LOW_TICKS  4U

/* 首次运行顺序（存 base_prio） */
static volatile uint32_t selftest_order[SELFTEST_THREADS];
static volatile uint32_t selftest_order_n;
static spinlock_t        selftest_lock = SPINLOCK_INIT;

static void busy_ticks(uint32_t ticks)
{
    uint32_t start = timer_ticks();

    while ((timer_ticks() - start) < ticks) {
        __asm__ volatile ("pause");
    }
}

/* 3 个测试线程共用的入口：用 base_prio 区分角色，避免注册多个程序 */
static void schedprio_entry(void)
{
    proc_t *me = proc_current();
    uint32_t flags;
    uint32_t slot;
    uint32_t n;
    uint32_t busy;

    if (me == 0) {
        proc_exit(0);
    }

    flags = spin_lock_irqsave(&selftest_lock);
    slot = selftest_order_n;
    if (slot < SELFTEST_THREADS) selftest_order[slot] = me->base_prio;
    selftest_order_n = slot + 1U;
    n = selftest_order_n;
    spin_unlock_irqrestore(&selftest_lock, flags);

    kprintf("sched: prio pid %u base=%u first run (aged=%u eff=%u)\n",
            me->pid, (unsigned int)me->base_prio,
            me->aged, (unsigned int)me->eff_prio);

    /* 被调度时 aging 提升应已回落 */
    if (me->eff_prio != me->base_prio) {
        kprintf("sched: priority FAIL (pid %u eff=%u base=%u not restored)\n",
                me->pid, (unsigned int)me->eff_prio, (unsigned int)me->base_prio);
    }

    /* 第三个进入的线程负责判定顺序：高 -> 中 -> 低 */
    if (n == SELFTEST_THREADS) {
        uint32_t a = selftest_order[0];
        uint32_t b = selftest_order[1];
        uint32_t c = selftest_order[2];

        if (a == SELFTEST_PRIO_HIGH && b == SELFTEST_PRIO_MID &&
            c == SELFTEST_PRIO_LOW) {
            kprintf("sched: priority ok (order=%u,%u,%u)\n", a, b, c);
        } else {
            kprintf("sched: priority FAIL (order=%u,%u,%u)\n", a, b, c);
        }
    }

    /* 低优先级线程：验证 aging 确实在等待期提升过 */
    if (me->base_prio == SELFTEST_PRIO_LOW) {
        if (me->aged > 0U && me->eff_prio == me->base_prio) {
            kprintf("sched: aging ok (base=%u aged=%u eff=%u)\n",
                    (unsigned int)me->base_prio, me->aged,
                    (unsigned int)me->eff_prio);
        } else {
            kprintf("sched: aging FAIL (base=%u aged=%u eff=%u)\n",
                    (unsigned int)me->base_prio, me->aged,
                    (unsigned int)me->eff_prio);
        }
    }

    busy = (me->base_prio == SELFTEST_PRIO_HIGH) ? SELFTEST_HIGH_TICKS
                                                 : SELFTEST_LOW_TICKS;
    busy_ticks(busy);
    proc_exit(0);
}

static proc_t *spawn_prio(uint8_t prio)
{
    proc_t *p = proc_spawn(SELFTEST_PROG);

    if (p == 0) return 0;

    p->cpu_pin = 0;                 /* 只在 BSP 上跑：顺序可判定 */
    (void)sched_set_priority(p, prio);
    sched_enqueue(p, 0U);           /* 从共享 runqueue 移到 BSP 的优先级桶 */
    return p;
}

void sched_selftest_start(void)
{
    selftest_order_n = 0;

    if (prog_lookup(SELFTEST_PROG) == 0) {
        if (prog_register(SELFTEST_PROG, 1, "scheduler priority/aging selftest",
                          schedprio_entry) != E_OK) {
            kprintln("sched: priority FAIL (program register)");
            return;
        }
    }

    if (spawn_prio(SELFTEST_PRIO_HIGH) == 0 ||
        spawn_prio(SELFTEST_PRIO_MID) == 0 ||
        spawn_prio(SELFTEST_PRIO_LOW) == 0) {
        kprintln("sched: priority FAIL (spawn)");
        return;
    }
}
