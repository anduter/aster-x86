/* =============================================================================
 * sched.c - 优先级 + aging 调度
 *
 * 策略：
 *   - 每个进程有 base_prio（0..7，数值越小优先级越高，默认 4）；
 *   - 每个 CPU 一个 runqueue 位图（per_cpu[cpu].runq_mask），并额外按优先级
 *     分成 8 个桶（sched_bucket[cpu][prio]），调度时先选最高优先级非空桶；
 *   - 同优先级仍沿用旧的“从 cur 之后环形扫描”，所以全部进程都是默认优先级
 *     4 时，选择顺序与改造前的纯轮转一致（默认行为等价）；
 *   - aging：READY 但没被调度的进程每 tick 累计 wait_ticks，超过
 *     SCHED_AGING_TICKS 就提升一级（eff_prio--），避免低优先级饿死；
 *     被调度上 CPU 时 eff_prio 回落到 base_prio（下界由 0 截住）；
 *   - sched_tick()：BSP 的 PIT 中断里递减时间片，到期重新选进程（抢占）；
 *   - sched_ap_tick()：AP 的 LAPIC timer / 唤醒 IPI 里取活或轮转；
 *   - proc_yield() 走 sched_next()：主动让出；
 *   - sched_start()：先跑优先级/aging 自检，再切到第一个可运行进程。
 *
 * 时间片到期时先把当前进程放回 READY 再选，否则 RUNNING 的进程会被
 * sched_next 排除，高优先级线程反而在自己时间片结束时被低优先级抢占。
 * ============================================================================= */
#include "proc.h"
#include "proc_internal.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/mm.h"           /* vmm_kernel_dir/vmm_current_dir：idle CR3 断言 */
#include "include/dev.h"          /* apic_send_ipi：跨 CPU 重新调度 IPI */

/* 全局调度锁：BSP/AP 不能同时改同一份 runqueue / 进程状态。 */
static spinlock_t sched_lock = SPINLOCK_INIT;
static volatile uint32_t smp_sched_enabled;

/* 每 N 个 BSP tick 做一次负载均衡检查 */
#define SCHED_BALANCE_INTERVAL 20U

static uint32_t balance_ticks;

/* 每 CPU、每优先级的 runqueue 桶；runq_mask 是它们的并集，保持同步。 */
static uint32_t sched_bucket[SMP_MAX_CPUS][PROC_PRIO_LEVELS];

/* aging 窗口，自检可临时缩短（正常路径始终是 SCHED_AGING_TICKS）。 */
static uint32_t sched_aging_ticks = SCHED_AGING_TICKS;

static inline bool runnable_state(const proc_t *p)
{
    return p->state == PROC_READY || p->state == PROC_RUNNING;
}

static inline uint32_t proc_bit(const proc_t *p)
{
    return 1U << (uint32_t)(p - proc_table);
}

static inline uint8_t prio_clamp(uint8_t prio)
{
    return (prio < PROC_PRIO_LEVELS) ? prio : (uint8_t)(PROC_PRIO_LEVELS - 1U);
}

/* 新进程（槽位清零后从未初始化过优先级）套默认值，保证 fork/spawn 出来的
 * 进程默认就是优先级 4；显式调用 sched_set_priority() 后 prio_set 为真，
 * 之后入队/唤醒都不会覆盖它（包括合法的优先级 0）。 */
static void prio_default_locked(proc_t *p)
{
    if (p->prio_set) return;
    p->base_prio = PROC_PRIO_DEFAULT;
    p->eff_prio  = PROC_PRIO_DEFAULT;
    p->wait_ticks = 0;
    p->prio_set  = true;
}

/* 从所有 CPU 的位图/优先级桶里摘掉（调用者必须持有 sched_lock） */
static void rq_unlink_locked(proc_t *p)
{
    uint32_t bit = proc_bit(p);

    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
        per_cpu[c].runq_mask &= ~bit;
        for (uint32_t q = 0; q < PROC_PRIO_LEVELS; q++) {
            sched_bucket[c][q] &= ~bit;
        }
    }
}

/* 挂到指定 CPU 的对应优先级桶（调用者必须持有 sched_lock，且保证不在别的桶里） */
static void rq_link_locked(uint32_t cpu, proc_t *p)
{
    uint32_t bit = proc_bit(p);

    per_cpu[cpu].runq_mask |= bit;
    sched_bucket[cpu][prio_clamp(p->eff_prio)] |= bit;
}

/* 进程已经在某个 CPU 的 runqueue 上时，把它挪到 eff_prio 对应的桶 */
static void rq_reset_bucket_locked(proc_t *p)
{
    uint32_t bit = proc_bit(p);

    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) {
        if ((per_cpu[c].runq_mask & bit) == 0U) continue;
        for (uint32_t q = 0; q < PROC_PRIO_LEVELS; q++) {
            sched_bucket[c][q] &= ~bit;
        }
        sched_bucket[c][prio_clamp(p->eff_prio)] |= bit;
        return;
    }
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

    if (p == 0 || cpu >= SMP_MAX_CPUS) return;

    flags = spin_lock_irqsave(&sched_lock);
    prio_default_locked(p);
    rq_unlink_locked(p);
    rq_link_locked(cpu, p);
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

    if (p == 0) return;

    /* 调度器还没启动（内核初始化阶段）：只迁移状态，不入队 */
    if (!smp_sched_enabled) {
        if (p->state == PROC_BLOCKED) p->state = PROC_READY;
        return;
    }

    cpu = sched_place(p);                 /* 内部自己拿锁 */

    flags = spin_lock_irqsave(&sched_lock);
    prio_default_locked(p);
    if (p->state != PROC_ZOMBIE && p->state != PROC_RUNNING) {
        p->state = PROC_READY;
        rq_unlink_locked(p);
        rq_link_locked(cpu, p);
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    if (cpu != smp_cpu_id()) sched_kick(cpu);
}

/* 进程退出/回收时把它从所有 runqueue 摘掉 */
void sched_dequeue(proc_t *p)
{
    uint32_t flags;

    if (p == 0) return;

    flags = spin_lock_irqsave(&sched_lock);
    rq_unlink_locked(p);
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* 设置静态优先级：同时重置 aging 状态，已入队的进程迁移到新桶。 */
int sched_set_priority(proc_t *p, uint8_t prio)
{
    uint32_t flags;

    if (p == 0 || prio >= PROC_PRIO_LEVELS) return -1;

    flags = spin_lock_irqsave(&sched_lock);
    p->base_prio  = prio;
    p->eff_prio   = prio;
    p->wait_ticks = 0;
    p->prio_set   = true;
    rq_reset_bucket_locked(p);
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
}

void sched_set_aging_ticks(uint32_t ticks)
{
    uint32_t flags;

    if (ticks == 0U) ticks = 1U;
    if (ticks > 100000U) ticks = 100000U;

    flags = spin_lock_irqsave(&sched_lock);
    sched_aging_ticks = ticks;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* 从本 CPU runqueue 里选最高优先级的一个 READY：
 *   1. 单遍扫描 runq_mask，找出最小的 eff_prio，得到该优先级的候选位图；
 *   2. 候选位图里从 cur 之后环形扫描（同优先级仍轮转）。
 * 选中后把 eff_prio 回落到 base_prio，并清零等待计数。 */
proc_t *sched_next(proc_t *cur)
{
    uint32_t start = 0;
    uint32_t flags;
    uint32_t cpu = smp_cpu_id();
    uint32_t mask;
    uint32_t bucket = 0;
    uint32_t best = PROC_PRIO_LEVELS;
    proc_t *found = 0;
    proc_t *announce = 0;

    if (cur != 0) {
        start = (uint32_t)(cur - proc_table) + 1U;
    }

    flags = spin_lock_irqsave(&sched_lock);
    mask = (cpu < SMP_MAX_CPUS) ? per_cpu[cpu].runq_mask : 0U;

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        uint32_t bit = 1U << i;
        proc_t *p;
        uint32_t q;

        if ((mask & bit) == 0U) continue;
        p = &proc_table[i];
        if (p->state != PROC_READY) continue;
        q = (uint32_t)prio_clamp(p->eff_prio);
        if (q < best) {
            best = q;
            bucket = bit;
        } else if (q == best) {
            bucket |= bit;
        }
    }

    for (uint32_t i = 0; i < PROC_MAX; i++) {
        uint32_t idx = (start + i) % PROC_MAX;
        proc_t *p = &proc_table[idx];

        if ((bucket & (1U << idx)) == 0U) continue;
        p->state = PROC_RUNNING;
        p->eff_prio = p->base_prio;       /* 运行后回落到 base */
        p->wait_ticks = 0;
        p->runs++;
        rq_reset_bucket_locked(p);        /* 桶位置同步到 base 优先级 */
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

/* aging：本 CPU runqueue 上 READY 但不是当前的进程，等待超窗口就提升一级。
 * 当前进程（正在跑）不参与，运行后 eff_prio 已经在 sched_next 里回落。 */
static void sched_age_runqueue(const proc_t *cur)
{
    uint32_t flags = spin_lock_irqsave(&sched_lock);
    uint32_t cpu = smp_cpu_id();

    if (cpu < SMP_MAX_CPUS) {
        uint32_t mask = per_cpu[cpu].runq_mask;

        for (uint32_t i = 0; i < PROC_MAX; i++) {
            proc_t *p;

            if ((mask & (1U << i)) == 0U) continue;
            p = &proc_table[i];
            if (p == cur || p->state != PROC_READY) continue;
            if (p->eff_prio == 0U) { p->wait_ticks = 0; continue; }

            if (++p->wait_ticks >= sched_aging_ticks) {
                p->wait_ticks = 0;
                p->eff_prio--;
                p->aged++;
                rq_reset_bucket_locked(p);
            }
        }
    }

    spin_unlock_irqrestore(&sched_lock, flags);
}

/* 负载均衡：把最忙 CPU 上的一个 READY 进程挪到完全空转的 CPU。
 * 条件收紧（最忙 >=2 且最闲 ==0）以避免进程来回抖动；
 * 显式亲和（cpu_pin >= 0）的进程不迁移，否则会破坏 AP 探针与自检。 */
void sched_balance(void)
{
    uint32_t flags;
    uint32_t busiest = SMP_MAX_CPUS;
    uint32_t idlest = SMP_MAX_CPUS;
    uint32_t max_load = 0;
    uint32_t min_load = 0xFFFFFFFFU;
    proc_t *victim = 0;

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
            if (proc_table[i].cpu_pin >= 0) continue;          /* 亲和进程不搬 */
            victim = &proc_table[i];
            break;
        }
        if (victim != 0) {
            rq_unlink_locked(victim);
            rq_link_locked(idlest, victim);
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

        /* 不变量：idle 的 CPU 必须停在内核自己的地址空间（vmm_kernel_dir）。
         * 若仍在某个进程的页目录上 idle，父进程一旦 proc_reap → 
         * vmm_destroy_dir 就会释放本 CPU 正在用的 PDPT/页表，之后取指
         * 就是 eip/cr2 在 0x0/0x18/0x1b 附近的 oops。这里显式 panic 留现场，
         * 不静默继续跑。 */
        if (vmm_kernel_dir() != 0 && vmm_current_dir() != vmm_kernel_dir()) {
            panic("idle cpu%u on foreign page dir (cr3=%p kernel=%p)",
                  (unsigned int)smp_cpu_id(), (void *)vmm_current_dir(),
                  (void *)vmm_kernel_dir());
        }

        sched_age_runqueue(0);
        next = sched_next(0);
        if (next != 0) proc_switch_to(next);
        return;
    }

    /* 等待中的低优先级线程按 tick 老化 */
    sched_age_runqueue(cur);

    /* 时间片没用完就继续跑 */
    if (cur->timeslice > 0U) {
        cur->timeslice--;
    } else {
        cur->timeslice = PROC_TIMESLICE;

        /* 先回 READY 再选：否则当前 RUNNING 进程被排除在候选之外，
         * 高优先级线程会在自己的时间片结束时被低优先级线程抢占。
         * 同优先级时 sched_next 从 cur+1 开始扫，仍然是轮转。
         * 只在 RUNNING 时这样做：BLOCKED/ZOMBIE 不能被时间片“复活”。 */
        if (cur->state == PROC_RUNNING) {
            cur->state = PROC_READY;
            next = sched_next(cur);
            if (next == 0) {
                cur->state = PROC_RUNNING;
            } else if (next != cur) {
                proc_switch_to(next);
            }
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
    /* 优先级/aging 自检线程必须在选第一个进程之前入队（全部固定在 BSP）。 */
    sched_selftest_start();

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
