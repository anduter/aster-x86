/* =============================================================================
 * proc/audit_smp_selftest.c - SMP 并发/竞态自检（audit-smp 会话）
 *
 * 三个内核线程级测试，全部在调度器启动前注册/放置（pin + prio 在 sched_start
 * 之前写完，避免“刚 spawn 就被调度、优先级/亲和还没写”的第二竞态）：
 *
 *  1) spawn/pid 竞态：两颗 CPU 的线程用两方屏障同时进入 proc_spawn()，
 *     记录返回的 proc_t 槽位与 pid。并发槽位分配/next_pid 若没有串行化，
 *     两个子进程会落到同一个 proc_t（同一内核栈）或拿到同一个 pid。
 *
 *  2) wake-vs-block 竞态（lost wakeup）：目标线程在“检查条件之后、真正
 *     proc_block 之前”的窗口里保持 RUNNING；另一颗 CPU 调 proc_wakeup()。
 *     旧实现看到 RUNNING 直接丢弃唤醒，目标随后永久阻塞。这里用 lw_ack
 *     把目标精确钉在窗口里，可 100% 复现。
 *
 *  3) vmalloc 并发：两个线程同时 vmalloc()/vfree()。vm_bitmap 没有锁时
 *     会把同一段虚拟区间分配给两个线程（同一份共享的内核页表），互相
 *     覆盖数据；vfree 还可能重复释放同一物理页（PMM double free）或
 *     把块头校验打坏（vmm_faults 增加）。
 *
 * 输出（回归脚本按这些行判定）：
 *   audit: smp selftest start (cpus=N)
 *   audit: smp spawn race PASS (rounds=N)
 *   audit: smp wake-vs-block PASS (latency=N ticks)
 *   audit: mm vmalloc concurrent PASS (iters=N)
 *   audit: smp selftest done
 * 任一项失败打印对应的 FAIL 行，最后一行固定是 done，便于宿主脚本收尾。
 * ============================================================================= */
#include "audit_smp.h"
#include "proc.h"
#include "proc_internal.h"
#include "proc_sync.h"
#include "percpu.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/userland.h"

#define AUD_LEAF_PROG  "aud_leaf"
#define AUD_MAIN_PROG  "aud_main"
#define AUD_W0_PROG    "aud_w0"
#define AUD_W1_PROG    "aud_w1"

/* 0..2 被 sched_selftest 的优先级/aging 回归占用；测试线程用 3（高于默认 4）。 */
#define AUD_PRIO          3U
#define AUD_SPAWN_ROUNDS  24U
#define AUD_REAP_TICKS    100U
#define AUD_WAIT_TICKS    600U
#define AUD_VM_ITERS      1200U
#define AUD_CHAN          0x41554431U   /* "AUD1" */

enum {
    AUD_PH_IDLE  = 0,
    AUD_PH_SPAWN = 1,
    AUD_PH_WAKE  = 2,
    AUD_PH_VM    = 3,
    AUD_PH_EXIT  = 9
};

static volatile uint32_t aud_phase;
static volatile uint32_t aud_ack[2];
static uint32_t aud_wpid[2];

/* 两方屏障（只用于两个 worker 之间） */
static volatile uint32_t aud_bar_in;
static volatile uint32_t aud_bar_gen;

/* 测试 1 现场 */
static volatile uint32_t aud_slot[2];
static volatile uint32_t aud_pid[2];
static volatile uint32_t aud_rounds;
static volatile uint32_t aud_sr_reason;
static volatile uint32_t aud_sr_timeouts;

/* 测试 2 现场 */
static volatile uint32_t aud_lw_pid;
static volatile uint32_t aud_lw_ready;
static volatile uint32_t aud_lw_ack;
static volatile uint32_t aud_lw_done;
static volatile uint32_t aud_lw_latency;

/* 测试 3 现场 */
static volatile uint32_t aud_vm_bad;
static volatile uint32_t aud_vm_iters;

/* 汇总 */
static volatile uint32_t aud_spawn_ok;
static volatile uint32_t aud_wake_ok;
static volatile uint32_t aud_vm_ok;

static void aud_barrier2(void)
{
    uint32_t gen = aud_bar_gen;

    if (__sync_add_and_fetch(&aud_bar_in, 1U) == 2U) {
        __sync_lock_test_and_set(&aud_bar_in, 0U);
        __sync_synchronize();
        aud_bar_gen = gen + 1U;
    } else {
        while (aud_bar_gen == gen) {
            __asm__ volatile ("pause");
        }
    }
}

/* 内部等待：最多 ticks 个 tick；返回 1 表示条件成立 */
static int aud_wait_volatile(volatile uint32_t *flag, uint32_t want,
                             uint32_t ticks)
{
    for (uint32_t i = 0; i < ticks; i++) {
        if (*flag == want) return 1;
        proc_sleep_ticks(1U);
    }
    return (*flag == want) ? 1 : 0;
}

/* ---------------- 测试 1：并发 proc_spawn 的槽位/pid 分配 ---------------- */
static void aud_spawn_worker(uint32_t id)
{
    proc_t *me = proc_current();

    for (uint32_t r = 0; r < AUD_SPAWN_ROUNDS; r++) {
        bool skip = (aud_sr_reason != 0U);

        aud_barrier2();

        if (!skip) {
            proc_t *p = proc_spawn(AUD_LEAF_PROG);
            uint32_t pid = (p != 0) ? p->pid : 0U;
            uint32_t slot = (p != 0) ? (uint32_t)(p - proc_table) : 0xFFFFFFFFU;

            aud_pid[id] = pid;
            aud_slot[id] = slot;
            __sync_synchronize();

            aud_barrier2();             /* 两边的记录都可见后才比较 */

            if (p == 0) {
                aud_sr_reason = 0x10U + id;
            } else if (aud_slot[0] == aud_slot[1]) {
                aud_sr_reason = 1U;     /* 同一个 proc_t（同一内核栈） */
            } else if (aud_pid[0] == aud_pid[1]) {
                aud_sr_reason = 2U;     /* next_pid++ 丢更新 */
            }

            if (aud_sr_reason == 0U) {
                int got = 0;

                sched_set_priority(p, 2U);      /* 子进程尽快跑完，缩短回收等待 */
                p->parent_pid = me->pid;
                for (uint32_t t = 0; t < AUD_REAP_TICKS; t++) {
                    int w = proc_wait_options((int)pid, 0, PROC_WAIT_WNOHANG);

                    if (w == (int)pid) { got = 1; break; }
                    if (w < 0) break;
                    proc_sleep_ticks(1U);
                }
                if (!got) {
                    aud_sr_timeouts++;
                    if (aud_sr_reason == 0U) aud_sr_reason = 3U;
                } else {
                    aud_rounds = r + 1U;
                }
            }
        } else {
            aud_barrier2();             /* 失败后双方继续对齐屏障，避免互相卡死 */
        }
    }
}

/* ---------------- 测试 2：wake 落在 check -> block 窗口 ---------------- */
static void aud_wake_target(void)
{
    proc_t *me = proc_current();

    aud_lw_pid = me->pid;
    /* 清掉此前任何杂散 latch，保证后面 PASS 只可能来自 orchestrator 的那次 wake */
    sched_clear_wake(me);
    __sync_synchronize();
    aud_lw_ready = 1U;

    /* 目标保持 RUNNING，直到 orchestrator 发完 wake 才进入 proc_block。 */
    while (aud_lw_ack == 0U) {
        __asm__ volatile ("pause");
    }
    proc_block(AUD_CHAN);
    aud_lw_done = 1U;
}

/* ---------------- 测试 3：并发 vmalloc/vfree ---------------- */
static void aud_vm_worker(uint32_t id)
{
    unsigned int f0 = vmm_faults;
    unsigned int d0 = pmm_double_free;
    uint32_t bad = 0;
    uint32_t iters = 0;

    kprintf("audit: vm worker %u begin\n", (unsigned int)id);
    for (uint32_t i = 0; i < AUD_VM_ITERS; i++) {
        uint32_t pat = ((id + 1U) << 24) ^ i ^ 0xA5A50000U;
        volatile uint32_t *p;

        if ((i % 300U) == 0U && i != 0U) {
            kprintf("audit: vm worker %u progress %u tick=%u\n",
                    (unsigned int)id, (unsigned int)i,
                    (unsigned int)this_cpu()->ticks);
        }
        p = (volatile uint32_t *)vmalloc(128U);
        if (p == 0) { bad |= 0x1U; break; }
        p[0] = pat;
        p[31] = ~pat;
        for (volatile uint32_t s = 0; s < 300U; s++) {
            __asm__ volatile ("pause");
        }
        if (p[0] != pat || p[31] != ~pat) bad |= 0x2U;   /* 区间被另一个线程复用 */
        vfree((void *)p);
        iters++;
    }

    if (vmm_faults != f0) bad |= 0x4U;                    /* 块头损坏/重复释放 */
    if (pmm_double_free != d0) bad |= 0x8U;               /* 物理页 double free */
    __sync_fetch_and_add(&aud_vm_iters, iters);
    __sync_fetch_and_or(&aud_vm_bad, bad);
    kprintf("audit: vm worker %u end iters=%u bad=0x%x\n",
            (unsigned int)id, (unsigned int)iters, (unsigned int)bad);
}

/* ---------------- worker 主循环 ---------------- */
static void aud_worker(uint32_t id)
{
    for (;;) {
        uint32_t ph = aud_phase;

        if (ph == AUD_PH_SPAWN) {
            aud_spawn_worker(id);
            aud_ack[id] = ph;
        } else if (ph == AUD_PH_WAKE) {
            /* 目标线程在 proc_block 里等唤醒；ack 由它醒来后再补，
             * 因此 wake 阶段不能用 aud_wait_ack 等 id==0。 */
            if (id == 0U) {
                aud_wake_target();
            }
            aud_ack[id] = ph;
        } else if (ph == AUD_PH_VM) {
            aud_vm_worker(id);
            aud_ack[id] = ph;
        } else if (ph == AUD_PH_EXIT) {
            proc_exit(0);
        } else {
            proc_sleep_ticks(1U);
            continue;
        }

        while (aud_phase == ph) proc_sleep_ticks(1U);
    }
}

static void aud_w0_entry(void) { aud_worker(0U); }
static void aud_w1_entry(void) { aud_worker(1U); }

/* 子进程：立刻退出，父进程负责回收 */
static void aud_leaf_entry(void) { proc_exit(0); }

/* ---------------- orchestrator ---------------- */
static int aud_wait_ack(uint32_t ph)
{
    for (uint32_t t = 0; t < AUD_WAIT_TICKS; t++) {
        if (aud_ack[0] == ph && aud_ack[1] == ph) return 1;
        proc_sleep_ticks(1U);
    }
    return (aud_ack[0] == ph && aud_ack[1] == ph) ? 1 : 0;
}

static void aud_run_spawn_test(void)
{
    aud_ack[0] = aud_ack[1] = 0U;
    aud_sr_reason = 0U;
    aud_sr_timeouts = 0U;
    aud_rounds = 0U;
    aud_slot[0] = aud_slot[1] = 0xFFFFFFFFU;
    aud_pid[0] = aud_pid[1] = 0U;
    __sync_synchronize();
    aud_phase = AUD_PH_SPAWN;

    if (!aud_wait_ack(AUD_PH_SPAWN)) {
        kprintln("audit: smp spawn race FAIL (worker timeout)");
        aud_phase = AUD_PH_IDLE;
        return;
    }
    aud_phase = AUD_PH_IDLE;

    if (aud_sr_reason == 0U && aud_rounds == AUD_SPAWN_ROUNDS) {
        aud_spawn_ok = 1U;
        kprintf("audit: smp spawn race PASS (rounds=%u)\n",
                (unsigned int)aud_rounds);
    } else {
        kprintf("audit: smp spawn race FAIL (reason=%u rounds=%u timeouts=%u "
                "slot0=%u slot1=%u pid0=%u pid1=%u)\n",
                (unsigned int)aud_sr_reason, (unsigned int)aud_rounds,
                (unsigned int)aud_sr_timeouts,
                (unsigned int)aud_slot[0], (unsigned int)aud_slot[1],
                (unsigned int)aud_pid[0], (unsigned int)aud_pid[1]);
    }
}

static void aud_run_wake_test(void)
{
    proc_t *t;
    uint32_t start;
    uint32_t waited = 0;

    aud_ack[0] = aud_ack[1] = 0U;
    aud_lw_pid = 0U;
    aud_lw_ready = 0U;
    aud_lw_ack = 0U;
    aud_lw_done = 0U;
    aud_lw_latency = 0U;
    __sync_synchronize();
    aud_phase = AUD_PH_WAKE;

    /* id==0 的 ack 要等它从 proc_block 醒来才写；这里只需要确认 worker1
     * 已进入该阶段、目标已把 lw_ready 立起来。 */
    while (aud_ack[1] != AUD_PH_WAKE || aud_lw_ready == 0U) {
        if (++waited > AUD_WAIT_TICKS) break;
        proc_sleep_ticks(1U);
    }
    if (aud_ack[1] != AUD_PH_WAKE || aud_lw_ready == 0U) {
        kprintln("audit: smp wake-vs-block FAIL (target not ready)");
        aud_phase = AUD_PH_IDLE;
        return;
    }

    t = proc_find(aud_lw_pid);
    if (t == 0) {
        kprintln("audit: smp wake-vs-block FAIL (target gone)");
        aud_phase = AUD_PH_IDLE;
        return;
    }

    /* 目标此刻仍在自旋（RUNNING），唤醒必须被“记住”，否则后续 proc_block
     * 会永久睡眠。旧实现只对 PROC_BLOCKED 入队，这里就是丢失窗口。 */
    proc_wakeup(AUD_CHAN);
    __sync_synchronize();
    aud_lw_ack = 1U;
    start = timer_ticks();
    waited = 0U;

    while (aud_lw_done == 0U && waited < AUD_WAIT_TICKS) {
        proc_sleep_ticks(1U);
        waited++;
    }

    if (aud_lw_done != 0U) {
        aud_lw_latency = timer_ticks() - start;
        aud_wake_ok = 1U;
        kprintf("audit: smp wake-vs-block PASS (latency=%u ticks)\n",
                (unsigned int)aud_lw_latency);
    } else {
        /* 失败兜底：目标已经 BLOCKED 且不在任何 CPU 上，再直接 sched_wake
         * 一次把它放行，避免测试线程永久占着槽位。 */
        kprintf("audit: smp wake-vs-block FAIL (lost wakeup, waited=%u ticks)\n",
                (unsigned int)waited);
        sched_wake(t);
        (void)aud_wait_volatile(&aud_lw_done, 1U, 50U);
    }
    aud_phase = AUD_PH_IDLE;
}

static void aud_dbg_worker(const char *tag, proc_t *w)
{
    uint32_t bit = (w != 0) ? (1U << (uint32_t)(w - proc_table)) : 0U;
    proc_t *c0 = (proc_t *)per_cpu[0].current;
    proc_t *c1 = (proc_t *)per_cpu[1].current;

    if (w == 0) {
        kprintf("audit: dbg %s worker gone\n", tag);
        return;
    }
    kprintf("audit: dbg %s pid=%u state=%u eff=%u base=%u pin=%d wp=%u "
            "chan=0x%x bit=0x%x mask=%x/%x cur=%u/%u\n", tag,
            (unsigned int)w->pid, (unsigned int)w->state,
            (unsigned int)w->eff_prio, (unsigned int)w->base_prio,
            (int)w->cpu_pin, (unsigned int)w->wake_pending,
            (unsigned int)w->wait_chan, (unsigned int)bit,
            (unsigned int)per_cpu[0].runq_mask,
            (unsigned int)per_cpu[1].runq_mask,
            (unsigned int)((c0 != 0) ? c0->pid : 0U),
            (unsigned int)((c1 != 0) ? c1->pid : 0U));
}

static void aud_run_vm_test(void)
{
    unsigned int f0 = vmm_faults;
    unsigned int d0 = pmm_double_free;

    aud_ack[0] = aud_ack[1] = 0U;
    aud_vm_bad = 0U;
    aud_vm_iters = 0U;
    __sync_synchronize();
    aud_phase = AUD_PH_VM;

    if (!aud_wait_ack(AUD_PH_VM)) {
        proc_t *w0 = proc_find(aud_wpid[0]);
        uint32_t t0a = per_cpu[0].ticks;
        uint32_t t1a = per_cpu[1].ticks;

        proc_sleep_ticks(10U);
        aud_dbg_worker("vm-timeout", proc_find(aud_wpid[0]));

        kprintf("audit: mm vmalloc concurrent FAIL (worker timeout ack0=%u "
                "ack1=%u iters=%u tick0=%u+%u tick1=%u+%u w0state=%u)\n",
                (unsigned int)aud_ack[0], (unsigned int)aud_ack[1],
                (unsigned int)aud_vm_iters,
                (unsigned int)t0a, (unsigned int)(per_cpu[0].ticks - t0a),
                (unsigned int)t1a, (unsigned int)(per_cpu[1].ticks - t1a),
                (unsigned int)((w0 != 0) ? w0->state : 0xFFU));
        aud_phase = AUD_PH_IDLE;
        return;
    }
    aud_phase = AUD_PH_IDLE;

    if (aud_vm_bad == 0U && vmm_faults == f0 && pmm_double_free == d0) {
        aud_vm_ok = 1U;
        kprintf("audit: mm vmalloc concurrent PASS (iters=%u)\n",
                (unsigned int)aud_vm_iters);
    } else {
        kprintf("audit: mm vmalloc concurrent FAIL (iters=%u bad=0x%x "
                "vmm_faults=%u pmm_double_free=%u)\n",
                (unsigned int)aud_vm_iters, (unsigned int)aud_vm_bad,
                (unsigned int)(vmm_faults - f0),
                (unsigned int)(pmm_double_free - d0));
    }
}

static void aud_main_entry(void)
{
    kprintf("audit: smp selftest start (cpus=%u)\n",
            (unsigned int)smp_cpu_count);

    aud_run_spawn_test();
    aud_run_wake_test();
    aud_run_vm_test();

    aud_phase = AUD_PH_EXIT;
    /* 给 worker 一个时间片退出；退出后不占进程表槽位 */
    for (uint32_t t = 0; t < 100U; t++) {
        if (proc_find(aud_wpid[0]) == 0 && proc_find(aud_wpid[1]) == 0) break;
        proc_sleep_ticks(1U);
    }

    if (aud_spawn_ok && aud_wake_ok && aud_vm_ok) {
        kprintln("audit: smp selftest PASS");
    } else {
        kprintf("audit: smp selftest FAIL (spawn=%u wake=%u vm=%u)\n",
                (unsigned int)aud_spawn_ok, (unsigned int)aud_wake_ok,
                (unsigned int)aud_vm_ok);
    }
    kprintln("audit: smp selftest done");
    proc_exit(0);
}

static proc_t *aud_spawn_pinned(const char *name, uint32_t cpu, uint8_t prio)
{
    proc_t *p = proc_spawn(name);

    if (p == 0) return 0;
    p->cpu_pin = (int32_t)cpu;
    (void)sched_set_priority(p, prio);
    sched_enqueue(p, cpu);          /* 从 spawn 的负载放置迁移到指定 CPU */
    return p;
}

void audit_smp_selftest_start(void)
{
    proc_t *m;
    proc_t *a;
    proc_t *b;

    if (smp_cpu_count < 2U) {
        kprintln("audit: smp selftest skipped (single cpu)");
        return;
    }

    if (prog_lookup(AUD_LEAF_PROG) == 0 &&
        prog_register(AUD_LEAF_PROG, 1, "audit-smp leaf", aud_leaf_entry) != E_OK) {
        kprintln("audit: smp selftest FAIL (register leaf)");
        return;
    }
    if (prog_lookup(AUD_MAIN_PROG) == 0 &&
        prog_register(AUD_MAIN_PROG, 1, "audit-smp orchestrator", aud_main_entry) != E_OK) {
        kprintln("audit: smp selftest FAIL (register main)");
        return;
    }
    if (prog_lookup(AUD_W0_PROG) == 0 &&
        prog_register(AUD_W0_PROG, 1, "audit-smp worker 0", aud_w0_entry) != E_OK) {
        kprintln("audit: smp selftest FAIL (register w0)");
        return;
    }
    if (prog_lookup(AUD_W1_PROG) == 0 &&
        prog_register(AUD_W1_PROG, 1, "audit-smp worker 1", aud_w1_entry) != E_OK) {
        kprintln("audit: smp selftest FAIL (register w1)");
        return;
    }

    /* 全部在 sched_start() 选第一个进程之前创建：此时没有任何进程在跑，
     * 亲和/优先级/入队都可以安全地写完（不存在“子线程先跑起来”的窗口）。
     * orchestrator 放 CPU1：wake-vs-block 测试要求 waker 与目标在不同 CPU 上
     * 同时运行（目标在 CPU0 自旋，必须是真正的 RUNNING 才落在丢失窗口里）。 */
    m = aud_spawn_pinned(AUD_MAIN_PROG, 1U, AUD_PRIO);
    a = aud_spawn_pinned(AUD_W0_PROG, 0U, AUD_PRIO);
    b = aud_spawn_pinned(AUD_W1_PROG, 1U, AUD_PRIO);
    if (m == 0 || a == 0 || b == 0) {
        kprintln("audit: smp selftest FAIL (spawn workers)");
        return;
    }
    aud_wpid[0] = a->pid;
    aud_wpid[1] = b->pid;
}
