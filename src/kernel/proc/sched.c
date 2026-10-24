/* =============================================================================
 * sched.c - 轮转调度
 *
 * 策略：每个 CPU 一个 runqueue 位图（per_cpu[cpu].runq_mask），从 cur
 *       之后环形扫描本 CPU 的位图，挑下一个 READY。
 *   - sched_tick()：BSP 的 PIT 中断里递减时间片，到期切走（抢占）；
 *   - sched_ap_tick()：AP 的 LAPIC timer / 唤醒 IPI 里取活或轮转；
 *   - proc_yield() 走 sched_next()：主动让出；
 *   - sched_start()：切到第一个可运行进程，正常不返回。
 *
 * 现在没有优先级和阻塞队列，只有 READY/RUNNING/ZOMBIE 三态，
 * 等 P2 有进程间等待时再加阻塞队列和优先级。
 * ============================================================================= */
#include "proc.h"
#include "proc_internal.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/kernel.h"
#include "include/kernel.h"

/* 全局调度锁：BSP/AP 不能同时改同一份 runqueue / 进程状态。 */
static spinlock_t sched_lock = SPINLOCK_INIT;
static volatile uint32_t smp_sched_enabled;

/* 每 N 个 BSP tick 做一次负载均衡检查 */
#define SCHED_BALANCE_INTERVAL 20U

static uint32_t balance_ticks;

static inline bool runnable_state(const proc_t *p)
{
    return p->state == PROC_READY || p->state == PROC_RUNNING;
}

/* 本 CPU runqueue 里真正可运行的进程数（位图可能残留已退出进程的位，用状态过滤） */
static uint32_t load_locked(uint32_t cpu)
{
    uint32_t mask = per_cpu[cpu].runq_mask;
    uint32_t n = 0;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if ((mask & (1U << i)) != 0U && runnable_state(&proc_table[i])) n++;
    }
    return n;
}

/* 选一个负载最小的在线 CPU；负载相同时优先 BSP（局部性与启动顺序） */
uint32_t sched_place(const proc_t *p)
{
    uint32_t flags;
    uint32_t best = 0;
    uint32_t best_load = 0xFFFFFFFFU;

    /* 显式亲和：进程自己被钉在某颗 CPU 上（如 AP 探针） */
    if (p != 0 && p->cpu_pin >= 0 && (uint32_t)p->cpu_pin < SMP_MAX_CPUS) {
        uint32_t pinned = (uint32_t)p->cpu_pin;
        if (per_cpu[pinned].online && (pinned == 0U || per_cpu[pinned].ready)) {
            return pinned;
        }
    }

    flags = spin_lock_irqsave(&sched_lock);
    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
        uint32_t load;

        if (!per_cpu[c].online) continue;
        if (c != 0U && !per_cpu[c].ready) continue;   /* AP 还没装好 GDT/IDT */
        load = load_locked(c);
        if (load < best_load) {
            best_load = load;
            best = c;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return best;
}

/* 给指定 CPU 发"重新调度"IPI（0x31 固定投递，AP 的 ipi handler 会去 runqueue 取活） */
void sched_kick(uint32_t cpu)
{
    if (cpu >= SMP_MAX_CPUS) return;
    if (!per_cpu[cpu].online || !per_cpu[cpu].ready) return;
    (void)apic_send_ipi(per_cpu[cpu].apic_id, 0x00000031U);
}

/* 把进程挂到指定 CPU 的 runqueue：先从所有位图摘掉，保证一个进程只属于一个 CPU */
void sched_enqueue(proc_t *p, uint32_t cpu)
{
    uint32_t flags;
    uint32_t bit;

    if (p == 0 || cpu >= SMP_MAX_CPUS) return;
    bit = 1U << (uint32_t)(p - proc_table);

    flags = spin_lock_irqsave(&sched_lock);
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        per_cpu[i].runq_mask &= ~bit;
    }
    per_cpu[cpu].runq_mask |= bit;
    spin_unlock_irqrestore(&sched_lock, flags);

    /* 目标 CPU 正在空转就踢一下，不用等它下一个 timer tick */
    if (cpu != smp_cpu_id()) sched_kick(cpu);
}

/* 唤醒一个阻塞进程：状态迁移与入队在同一把锁里完成，
 * 避免与另一 CPU 的 sched_next/sched_balance 竞争。 */
void sched_wake(proc_t *p)
{
    uint32_t flags;
    uint32_t cpu;
    uint32_t bit;

    if (p == 0) return;

    /* 调度器还没启动（内核初始化阶段）：只迁移状态，不入队 */
    if (!smp_sched_enabled) {
        if (p->state == PROC_BLOCKED) p->state = PROC_READY;
        return;
    }

    cpu = sched_place(p);                 /* 内部自己拿锁 */
    bit = 1U << (uint32_t)(p - proc_table);

    flags = spin_lock_irqsave(&sched_lock);
    if (p->state != PROC_ZOMBIE && p->state != PROC_RUNNING) {
        p->state = PROC_READY;
        for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
            per_cpu[i].runq_mask &= ~bit;
        }
        per_cpu[cpu].runq_mask |= bit;
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    if (cpu != smp_cpu_id()) sched_kick(cpu);
}

/* 进程退出/回收时把它从所有 runqueue 摘掉 */
void sched_dequeue(proc_t *p)
{
    uint32_t flags;
    uint32_t bit;

    if (p == 0) return;
    bit = 1U << (uint32_t)(p - proc_table);

    flags = spin_lock_irqsave(&sched_lock);
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        per_cpu[i].runq_mask &= ~bit;
    }
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
    proc_t *announce = 0;

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
        p->state = PROC_RUNNING;
        found = p;
        break;
    }
    if (found != 0) {
        if (cpu != 0U && found->last_cpu != cpu) announce = found;
        found->last_cpu = cpu;
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    /* 日志放在锁外：console 自己还有锁，避免两把锁嵌套 */
    if (announce != 0) {
        kprintf("sched: pid %u (%s) running on cpu %u\n",
                announce->pid, announce->name, cpu);
    }
    return found;
}

/* 负载均衡：把最忙 CPU 上的一个 READY 进程挪到完全空转的 CPU。
 * 条件收紧（最忙 >=2 且最闲 ==0）以避免进程来回抖动。 */
void sched_balance(void)
{
    uint32_t flags;
    uint32_t busiest = SMP_MAX_CPUS;
    uint32_t idlest = SMP_MAX_CPUS;
    uint32_t max_load = 0;
    uint32_t min_load = 0xFFFFFFFFU;
    proc_t *victim = 0;
    uint32_t victim_bit = 0;

    if (smp_cpu_count < 2U) return;

    flags = spin_lock_irqsave(&sched_lock);

    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
        uint32_t load;

        if (!per_cpu[c].online || (c != 0U && !per_cpu[c].ready)) continue;
        load = load_locked(c);
        if (load > max_load) { max_load = load; busiest = c; }
        if (load < min_load) { min_load = load; idlest = c; }
    }

    /* 只在负载差 >=2 时迁移，避免进程在两颗核之间来回抖动 */
    if (busiest != SMP_MAX_CPUS && idlest != SMP_MAX_CPUS &&
        busiest != idlest && max_load >= 2U &&
        (max_load - min_load) >= 2U) {
        uint32_t mask = per_cpu[busiest].runq_mask;

        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if ((mask & (1U << i)) == 0U) continue;
            if (proc_table[i].state != PROC_READY) continue;   /* 别搬正在跑的 */
            victim = &proc_table[i];
            victim_bit = 1U << i;
            break;
        }
        if (victim != 0) {
            per_cpu[busiest].runq_mask &= ~victim_bit;
            per_cpu[idlest].runq_mask |= victim_bit;
        }
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (victim != 0) {
        kprintf("sched: migrate pid %u (%s) cpu %u -> %u\n",
                victim->pid, victim->name, busiest, idlest);
        sched_kick(idlest);
    }
}

void sched_tick(void)
{
    proc_t *cur = proc_current();
    proc_t *next;

    /* 本 CPU 空闲（没有 current）：有新活就立刻接管。
     * 只在本 CPU 真的停在 idle 循环里时才这么做——内核启动阶段同样没有 current，
     * 那时抢走 CPU 会把 kernel_main 卡死在半路（sched_start 还没切到第一个线程）。 */
    if (cur == 0) {
        if (!this_cpu()->idle_active) return;
        next = sched_next(0);
        if (next != 0) proc_switch_to(next);
        return;
    }

    /* 时间片没用完就继续跑 */
    if (cur->timeslice > 0U) {
        cur->timeslice--;
    } else {
        cur->timeslice = PROC_TIMESLICE;
        next = sched_next(cur);
        if (next != 0) {
            proc_switch_to(next);
        }
    }

    /* 周期性负载均衡：只在 BSP 上做，单点决策避免两个 CPU 互相抢进程 */
    if (smp_cpu_id() == 0U && smp_cpu_count > 1U) {
        if (++balance_ticks >= SCHED_BALANCE_INTERVAL) {
            balance_ticks = 0;
            sched_balance();
        }
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
