/* =============================================================================
 * sched.c - 轮转调度
 *
 * 策略：在进程表里做环形扫描，挑下一个 READY。
 *   - sched_tick()：时钟中断里递减时间片，到期切走（抢占）；
 *   - proc_yield() 走 sched_next()：主动让出；
 *   - sched_start()：切到第一个可运行进程，正常不返回。
 *
 * 现在没有优先级和阻塞队列，只有 READY/RUNNING/ZOMBIE 三态，
 * 等 P2 有进程间等待时再加阻塞队列和优先级。
 * ============================================================================= */
#include "proc.h"
#include "proc_internal.h"
#include "../core/percpu.h"
#include "../core/spinlock.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

/* 全局调度锁：BSP/AP 不能同时选同一个 READY 进程。 */
static spinlock_t sched_lock = SPINLOCK_INIT;
static volatile uint32_t smp_sched_enabled;

static int ap_safe(const proc_t *p)
{
    if (p == 0 || p->name == 0) return 0;
    return strcmp(p->name, "heartbeat") == 0 ||
           strcmp(p->name, "hello") == 0 ||
           strcmp(p->name, "userdemo") == 0;
}

void sched_enqueue(proc_t *p, uint32_t cpu)
{
    uint32_t flags;

    if (p == 0 || cpu >= SMP_MAX_CPUS) return;
    flags = spin_lock_irqsave(&sched_lock);
    per_cpu[cpu].runq_mask |= (1U << (uint32_t)(p - proc_table));
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* 从本 CPU runqueue 的 cur 下一个槽位开始环形找 READY */
proc_t *sched_next(proc_t *cur)
{
    uint32_t start = 0;
    uint32_t flags;
    uint32_t cpu = smp_cpu_id();
    uint32_t mask;
    proc_t *found = 0;

    if (cur != 0) {
        start = (uint32_t)(cur - proc_table) + 1U;
    }

    flags = spin_lock_irqsave(&sched_lock);
    mask = (cpu < SMP_MAX_CPUS) ? per_cpu[cpu].runq_mask : 0U;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        uint32_t idx = (start + i) % PROC_MAX;
        proc_t *p = &proc_table[idx];
        if ((mask & (1U << idx)) == 0U) continue;
        if (p->state != PROC_READY) continue;
        if (cpu != 0U && !ap_safe(p)) continue;
        p->state = PROC_RUNNING;
        found = p;
        break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return found;
}

void sched_tick(void)
{
    proc_t *cur = proc_current();
    proc_t *next;

    if (cur == 0) return;

    /* 时间片没用完就继续跑 */
    if (cur->timeslice > 0U) {
        cur->timeslice--;
        return;
    }

    cur->timeslice = PROC_TIMESLICE;
    next = sched_next(cur);
    if (next != 0) {
        proc_switch_to(next);
    }
}

void sched_start(void)
{
    proc_t *first = sched_next(0);

    if (first == 0) {
        kprintln("sched: nothing to run");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    smp_sched_enabled = 1;
    kprintf("sched: starting pid %u (%s)\n", first->pid, first->name);
    proc_switch_to(first);

    /* 正常情况下永远不会回到这里 */
    kprintln("sched: start returned unexpectedly");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void sched_ap_tick(void)
{
    proc_t *cur;

    if (!smp_sched_enabled) return;

    cur = proc_current();
    if (cur == 0) {
        proc_t *next = sched_next(0);
        if (next != 0) proc_switch_to(next);
        return;
    }

    sched_tick();
}
