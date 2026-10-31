/*
 * ng WP1-5: 单核协作式调度器（round-robin + 睡眠/唤醒 + 延迟回收）。
 *
 * 结构：
 *   - ready 队列：FIFO 单链表（ready_head/ready_tail），round-robin 就是
 *     “队头出队运行、yield 时放回队尾”；
 *   - BLOCKED 链：thread_sleep() 的线程挂这里，wake_tick 是 TSC 到期时间；
 *     sched_core_poll() 在每次调度点/未来的 tick IRQ 里把到期的搬到 ready；
 *   - zombie 链：thread_exit() 只把自己标 DEAD 挂链，真正 kfree 栈由“下一次
 *     别人调度”完成（不能在正在用的栈上释放）；
 *   - idle 线程：sched_start() 创建，是普通 TCB，但 body 只做“轮询定时器 +
 *     yield”；当既没有 runnable 也没有睡着的线程时执行 hlt（无 IRQ 即终态）。
 *
 * 为什么没有锁：单核、IF 全程为 0，PMM/heap 也没有锁（WP1-4 遗留），调度点
 * 全是显式函数调用。抢占式调度/IRQ tick/SMP 留给 WP1-6 之后；本文件已经
 * 把“到期唤醒”单独做成 sched_tick()，接 tick 时只需要在 IRQ handler 里调用
 * 它，再决定是否在返回用户态前 yield。
 *
 * host 单测：sched.c 不 include 串口/端口 IO/heap，只依赖 sched_arch.h 的 6 个
 * 钩子；tests/unit-ng/test_sched.c 用 fake 时钟 + fake 栈 + 不切栈的 fake
 * switch 直接驱动 sched_core_*()。
 */
#include "thread.h"

#include "sched_arch.h"

/* 栈底 16 字节 canary（两段 ASCII 常量，溢出会被自检抓到）。 */
#define THREAD_CANARY0 0x4E472D5448524541ULL /* "NG-THREA" */
#define THREAD_CANARY1 0x44535441434B3031ULL /* "DSTACK01" */

static struct sched_core sched;
static thread_t thread_pool[NG_THREAD_MAX];
static u32 thread_stack_live;
static u32 thread_corrupt;
static u32 thread_started;

/* ------------------------------------------------------------------------- */
/* 小工具（内核无 libc）                                                      */
/* ------------------------------------------------------------------------- */

static void ng_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void thread_store_canary(thread_t *t)
{
    u64 *w;

    if (t == NULL || t->stack_base == NULL || t->stack_size < 16) {
        return;
    }
    w = (u64 *)t->stack_base;
    w[0] = THREAD_CANARY0;
    w[1] = THREAD_CANARY1;
}

int thread_stack_canary_ok(const thread_t *t)
{
    const u64 *w;

    if (t == NULL || t->stack_base == NULL || t->stack_size < 16) {
        return 0;
    }
    w = (const u64 *)t->stack_base;
    return w[0] == THREAD_CANARY0 && w[1] == THREAD_CANARY1;
}

/* ------------------------------------------------------------------------- */
/* 状态机：ready 队列                                                         */
/* ------------------------------------------------------------------------- */

void sched_core_init(struct sched_core *c)
{
    if (c == NULL) {
        return;
    }
    ng_memzero(c, sizeof(*c));
    c->next_tid = 1;
}

void sched_core_ready_push(struct sched_core *c, thread_t *t)
{
    if (c == NULL || t == NULL) {
        return;
    }
    t->next = NULL;
    t->state = THREAD_READY;
    if (c->ready_tail != NULL) {
        c->ready_tail->next = t;
    } else {
        c->ready_head = t;
    }
    c->ready_tail = t;
    c->ready_count++;
}

void sched_core_add(struct sched_core *c, thread_t *t)
{
    if (c == NULL || t == NULL) {
        return;
    }
    t->next = NULL;
    t->all_next = c->all_head;
    c->all_head = t;
    c->live_count++;
    if (t->state == THREAD_READY) {
        sched_core_ready_push(c, t);
    } else if (t->state == THREAD_RUNNING) {
        c->current = t;
    }
}

thread_t *sched_core_pick(struct sched_core *c)
{
    thread_t *t;

    if (c == NULL || c->ready_head == NULL) {
        return NULL;
    }
    t = c->ready_head;
    c->ready_head = t->next;
    if (c->ready_head == NULL) {
        c->ready_tail = NULL;
    }
    t->next = NULL;
    t->state = THREAD_RUNNING;
    t->switches++;
    c->current = t;
    c->picks++;
    if (c->ready_count > 0) {
        c->ready_count--;
    }
    return t;
}

/* ------------------------------------------------------------------------- */
/* 状态机：BLOCKED / 唤醒                                                     */
/* ------------------------------------------------------------------------- */

int sched_core_block(struct sched_core *c, thread_t *t, u64 deadline)
{
    if (c == NULL || t == NULL || t->state != THREAD_RUNNING) {
        return THREAD_EBAD;
    }
    t->state = THREAD_BLOCKED;
    t->wake_tick = deadline;
    t->wake_reason = THREAD_WAKE_NONE;
    t->next = c->blocked_head;
    c->blocked_head = t;
    c->blocked_count++;
    c->blocks++;
    return THREAD_OK;
}

int sched_core_wake(struct sched_core *c, thread_t *t, u32 reason)
{
    thread_t **pp;

    if (c == NULL || t == NULL || t->state != THREAD_BLOCKED) {
        return THREAD_EBAD;
    }
    pp = &c->blocked_head;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp != t) {
        return THREAD_EBAD;
    }
    *pp = t->next;
    t->next = NULL;
    if (c->blocked_count > 0) {
        c->blocked_count--;
    }
    t->wake_tick = 0;
    t->wake_reason = reason;
    sched_core_ready_push(c, t);
    if (reason == THREAD_WAKE_TIMER) {
        c->wakes_timer++;
    } else if (reason == THREAD_WAKE_EXPLICIT) {
        c->wakes_explicit++;
    }
    return THREAD_OK;
}

u32 sched_core_poll(struct sched_core *c, u64 now)
{
    thread_t *t;
    u32 woken = 0;

    if (c == NULL) {
        return 0;
    }
    t = c->blocked_head;
    while (t != NULL) {
        thread_t *next = t->next;

        if (t->wake_tick != 0 && now >= t->wake_tick) {
            if (sched_core_wake(c, t, THREAD_WAKE_TIMER) == THREAD_OK) {
                woken++;
            }
        }
        t = next;
    }
    return woken;
}

/* ------------------------------------------------------------------------- */
/* 状态机：DEAD / 回收                                                        */
/* ------------------------------------------------------------------------- */

void sched_core_zombie(struct sched_core *c, thread_t *t)
{
    if (c == NULL || t == NULL) {
        return;
    }
    t->state = THREAD_DEAD;
    t->wake_tick = 0;
    t->next = c->zombie_head;
    c->zombie_head = t;
    c->zombie_count++;
    c->exits++;
    if (c->live_count > 0) {
        c->live_count--;
    }
}

thread_t *sched_core_reap(struct sched_core *c, thread_t *skip)
{
    thread_t **pp;
    thread_t **ap;
    thread_t *t;

    if (c == NULL) {
        return NULL;
    }
    pp = &c->zombie_head;
    while ((t = *pp) != NULL && t == skip) {
        pp = &t->next;
    }
    if (t == NULL) {
        return NULL;
    }
    *pp = t->next;

    ap = &c->all_head;
    while (*ap != NULL && *ap != t) {
        ap = &(*ap)->all_next;
    }
    if (*ap == t) {
        *ap = t->all_next;
    }
    t->next = NULL;
    t->all_next = NULL;
    t->state = THREAD_UNUSED;
    t->wake_tick = 0;
    if (c->zombie_count > 0) {
        c->zombie_count--;
    }
    c->reaps++;
    return t;
}

thread_t *sched_core_find(struct sched_core *c, u32 tid)
{
    thread_t *t;

    if (c == NULL || tid == 0) {
        return NULL;
    }
    for (t = c->all_head; t != NULL; t = t->all_next) {
        if (t->tid == tid) {
            return t;
        }
    }
    return NULL;
}

/* 把 t 从 ready 或 blocked 链上摘下来（状态查询由调用方保证）。 */
int sched_core_remove(struct sched_core *c, thread_t *t)
{
    thread_t **pp;

    if (c == NULL || t == NULL) {
        return THREAD_EBAD;
    }
    if (t->state == THREAD_READY) {
        pp = &c->ready_head;
        while (*pp != NULL && *pp != t) {
            pp = &(*pp)->next;
        }
        if (*pp != t) {
            return THREAD_EBAD;
        }
        *pp = t->next;
        if (c->ready_tail == t) {
            thread_t *q = c->ready_head;

            c->ready_tail = NULL;
            while (q != NULL) {
                c->ready_tail = q;
                q = q->next;
            }
        }
        if (c->ready_count > 0) {
            c->ready_count--;
        }
        t->next = NULL;
        return THREAD_OK;
    }
    if (t->state == THREAD_BLOCKED) {
        pp = &c->blocked_head;
        while (*pp != NULL && *pp != t) {
            pp = &(*pp)->next;
        }
        if (*pp != t) {
            return THREAD_EBAD;
        }
        *pp = t->next;
        t->next = NULL;
        t->wake_tick = 0;
        if (c->blocked_count > 0) {
            c->blocked_count--;
        }
        return THREAD_OK;
    }
    return THREAD_EBAD;
}

/* ------------------------------------------------------------------------- */
/* 内核胶水：调度点                                                           */
/* ------------------------------------------------------------------------- */

static void sched_idle_body(void *arg);

static void sched_reap_zombies(thread_t *skip)
{
    thread_t *t;

    while ((t = sched_core_reap(&sched, skip)) != NULL) {
        void *stack = t->stack_base;
        u64 size = t->stack_size;
        /* WP3-0：用户进程线程的专用 SYSCALL 栈也在这里归还（它不属于
         * 进程的 AS，proc.c 不在自己正在用的栈上释放）。 */
        void *sys_stack = t->syscall_stack_base;
        u64 sys_size = t->syscall_stack_size;

        ng_memzero(t, sizeof(*t)); /* 槽位回到 UNUSED，tid=0 */
        if (stack != NULL) {
            sched_arch_stack_free(stack, size);
            if (thread_stack_live > 0) {
                thread_stack_live--;
            }
        }
        if (sys_stack != NULL) {
            sched_arch_stack_free(sys_stack, sys_size);
        }
    }
}

/*
 * 唯一的切换函数：yield / sleep / exit 都走这里。
 * 返回时表示“当前线程被重新调度到了”；此时 pick 已把状态置 RUNNING。
 */
static void sched_switch(void)
{
    thread_t *prev = sched.current;
    thread_t *next;

    /* 先回收（跳过 prev：它可能正是刚退出的线程，栈还在用）。 */
    sched_reap_zombies(prev);
    (void)sched_core_poll(&sched, sched_arch_time());
    next = sched_core_pick(&sched);

    if (next == NULL) {
        if (prev != NULL && prev->state == THREAD_RUNNING) {
            /* 没有别的可运行线程：继续跑 prev，不做无谓切换。 */
            sched.current = prev;
            return;
        }
        /* 连 idle 都没有：停 CPU（避免在已死线程的栈上继续执行）。 */
        for (;;) {
            sched_arch_idle();
        }
    }
    if (next == prev) {
        return; /* 队列里只有自己，pick 已把它置回 RUNNING */
    }

    sched.current = next;
    sched.switches++;
    sched_arch_switch(prev, next);
    /* prev 重新被调度后从这里返回；栈帧/寄存器由 switch_context 恢复。 */
}

void sched_yield(void)
{
    thread_t *self = sched.current;

    if (self == NULL) {
        return;
    }
    sched.yields++;
    if (self->state == THREAD_RUNNING) {
        self->state = THREAD_READY;
        sched_core_ready_push(&sched, self);
    }
    sched_switch();
}

void sched_tick(void)
{
    (void)sched_core_poll(&sched, sched_arch_time());
}

/* ------------------------------------------------------------------------- */
/* 内核胶水：线程生命周期                                                     */
/* ------------------------------------------------------------------------- */

void thread_bootstrap(void)
{
    thread_t *self = sched.current;

    if (self != NULL && self->start != NULL) {
        self->start(self->arg);
    }
    thread_exit();
}

static void sched_idle_body(void *arg)
{
    (void)arg;

    for (;;) {
        if (sched.ready_count == 0 && sched.blocked_count == 0) {
            /* 没有 runnable、也没有等定时器的线程：hlt（IF=0，即终态）。 */
            sched.idle_runs++;
            sched_arch_idle();
        }
        sched_yield();
    }
}

static thread_t *thread_slot_alloc(void)
{
    u32 i;

    for (i = 0; i < NG_THREAD_MAX; i++) {
        if (thread_pool[i].state == THREAD_UNUSED) {
            return &thread_pool[i];
        }
    }
    return NULL;
}

thread_t *thread_create(void (*fn)(void *), void *arg)
{
    thread_t *t;
    void *stack;

    if (fn == NULL) {
        return NULL;
    }
    t = thread_slot_alloc();
    if (t == NULL) {
        return NULL;
    }
    stack = sched_arch_stack_alloc(NG_THREAD_STACK_SIZE);
    if (stack == NULL) {
        return NULL;
    }

    ng_memzero(t, sizeof(*t));
    t->tid = sched.next_tid++;
    t->state = THREAD_READY;
    t->start = fn;
    t->arg = arg;
    t->stack_base = stack;
    t->stack_size = NG_THREAD_STACK_SIZE;
    t->stack_top = (void *)((u64)(usize)stack + NG_THREAD_STACK_SIZE);
    t->wake_reason = THREAD_WAKE_NONE;
    t->ctx.rip = sched_arch_entry_rip();
    t->ctx.rsp = (u64)(usize)t->stack_top;
    thread_store_canary(t);

    sched_core_add(&sched, t);
    sched.created++;
    thread_stack_live++;
    return t;
}

void thread_exit(void)
{
    thread_t *self = sched.current;

    if (self == NULL) {
        for (;;) {
            sched_arch_idle();
        }
    }
    if (!thread_stack_canary_ok(self)) {
        thread_corrupt++;
    }
    sched_core_zombie(&sched, self);
    sched_switch();
    /* 已死线程不应该被再次调度；真回来了就停住。 */
    for (;;) {
        sched_arch_idle();
    }
}

int thread_sleep(u64 ticks)
{
    thread_t *self = sched.current;
    u64 deadline;

    if (self == NULL) {
        return THREAD_EBAD;
    }
    if (ticks == 0) {
        sched_yield();
        return THREAD_WAKE_NONE;
    }
    deadline = sched_arch_time() + ticks;
    if (deadline == 0) {
        deadline = 1; /* 0 表示“只等显式唤醒”，别和到期冲突 */
    }
    if (sched_core_block(&sched, self, deadline) != THREAD_OK) {
        return THREAD_EBAD;
    }
    sched_switch();
    return (int)self->wake_reason;
}

int thread_wake(u32 tid)
{
    thread_t *t = sched_core_find(&sched, tid);

    if (t == NULL) {
        return THREAD_ENOENT;
    }
    if (sched_core_wake(&sched, t, THREAD_WAKE_EXPLICIT) != THREAD_OK) {
        return THREAD_EBAD;
    }
    return THREAD_OK;
}

int sched_kill_thread(u32 tid)
{
    thread_t *t = sched_core_find(&sched, tid);

    if (t == NULL) {
        return THREAD_ENOENT;
    }
    if (t == sched.current || t->state == THREAD_RUNNING) {
        return THREAD_EBAD; /* 自己：必须走 thread_exit() */
    }
    if (t->state == THREAD_DEAD) {
        return THREAD_OK;
    }
    if (sched_core_remove(&sched, t) != THREAD_OK) {
        return THREAD_EBAD;
    }
    sched_core_zombie(&sched, t);
    return THREAD_OK;
}

void sched_init(void)
{
    thread_t *boot;

    ng_memzero(&sched, sizeof(sched));
    ng_memzero(thread_pool, sizeof(thread_pool));
    sched_core_init(&sched);
    thread_stack_live = 0;
    thread_corrupt = 0;
    thread_started = 0;

    /* 0 号槽是 boot 线程：栈是 entry.S 的引导栈，不分配、不回收。 */
    boot = &thread_pool[0];
    boot->tid = sched.next_tid++;
    boot->state = THREAD_RUNNING;
    boot->flags = THREAD_F_BOOT;
    sched_core_add(&sched, boot);
}

void sched_start(void)
{
    thread_t *idle;

    if (thread_started) {
        return;
    }
    thread_started = 1;
    idle = thread_create(sched_idle_body, NULL);
    if (idle != NULL) {
        idle->flags |= THREAD_F_IDLE;
    }
    sched_yield(); /* 第一次让出：ready 队列开始按 round-robin 跑 */
}

/* ------------------------------------------------------------------------- */
/* 诊断                                                                       */
/* ------------------------------------------------------------------------- */

thread_t *thread_self(void)
{
    return sched.current;
}

struct sched_core *sched_core_state(void)
{
    return &sched;
}

u64 sched_now(void)
{
    return sched_arch_time();
}

u64 sched_switch_count(void)
{
    return sched.switches;
}

u64 sched_yield_count(void)
{
    return sched.yields;
}

u32 sched_live_count(void)
{
    return sched.live_count;
}

u32 sched_created_count(void)
{
    return (u32)sched.created;
}

u32 sched_reap_count(void)
{
    return (u32)sched.reaps;
}

u32 sched_zombie_count(void)
{
    return sched.zombie_count;
}

u32 sched_blocked_count(void)
{
    return sched.blocked_count;
}

u32 sched_stack_live(void)
{
    return thread_stack_live;
}

u32 sched_stack_corrupt_count(void)
{
    return thread_corrupt;
}

u32 sched_is_started(void)
{
    return thread_started;
}
