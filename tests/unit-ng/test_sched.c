/*
 * ng WP1-5 host 单测：调度器状态机（runqueue / BLOCKED / 定时唤醒 / 回收）。
 *
 * 纯逻辑测试：不跑 QEMU、不切栈、不需要内核。sched.c 只通过 sched_arch.h 的
 * 6 个钩子依赖环境；这里给出 fake 环境：
 *   - fake 时钟（fake_now，测试自己推进）；
 *   - fake 栈分配（malloc/free，记录次数）；
 *   - fake 上下文切换（空实现：状态机测试不真的换栈）；
 *   - fake idle（no-op）。
 * 于是 sched_core_*() 的全部规则都能在宿主上确定性地验证。
 *
 * 内核侧真正切栈的路径由 QEMU 自检覆盖（tools/run-ng.ps1 的 WP1-5 行）。
 */
#include <stdlib.h>
#include <string.h>

#include "sched_arch.h"
#include "test_util.h"
#include "thread.h"

/* ---- fake 环境 ----------------------------------------------------------- */

static u64 fake_now;
static u32 fake_allocs;
static u32 fake_frees;

u64 sched_arch_time(void)
{
    return fake_now;
}

void *sched_arch_stack_alloc(u64 size)
{
    void *p = malloc((size_t)size);

    if (p != NULL) {
        fake_allocs++;
    }
    return p;
}

void sched_arch_stack_free(void *stack, u64 size)
{
    (void)size;
    fake_frees++;
    free(stack);
}

void sched_arch_switch(thread_t *prev, thread_t *next)
{
    /* 纯状态机测试不真的切栈：调用到这里说明测试设计错了，直接标记失败。 */
    (void)prev;
    (void)next;
    CHECK(0);
}

u64 sched_arch_entry_rip(void)
{
    return 0x1000ULL;
}

void sched_arch_idle(void)
{
}

/* ---- 测试用的 thread_t 池 ------------------------------------------------ */

static thread_t tp[8];

static void reset_all(void)
{
    memset(tp, 0, sizeof(tp));
    fake_now = 0;
    fake_allocs = 0;
    fake_frees = 0;
}

static thread_t *mk(struct sched_core *c, u32 tid)
{
    u32 i;

    for (i = 0; i < (u32)(sizeof(tp) / sizeof(tp[0])); i++) {
        if (tp[i].state == THREAD_UNUSED) {
            memset(&tp[i], 0, sizeof(tp[i]));
            tp[i].tid = tid;
            tp[i].state = THREAD_READY;
            sched_core_add(c, &tp[i]);
            return &tp[i];
        }
    }
    return NULL;
}

static void t_core_init(void)
{
    struct sched_core c;
    thread_t ghost;

    reset_all();
    sched_core_init(&c);
    CHECK(c.ready_head == NULL);
    CHECK(c.ready_tail == NULL);
    CHECK(c.blocked_head == NULL);
    CHECK(c.zombie_head == NULL);
    CHECK(c.all_head == NULL);
    CHECK(c.current == NULL);
    CHECK(c.live_count == 0);
    CHECK(c.next_tid == 1);
    CHECK(sched_core_pick(&c) == NULL);
    CHECK(sched_core_reap(&c, NULL) == NULL);
    CHECK(sched_core_find(&c, 1) == NULL);

    /* add(RUNNING) 只登记、不入 ready，并成为 current。 */
    memset(&ghost, 0, sizeof(ghost));
    ghost.tid = 7;
    ghost.state = THREAD_RUNNING;
    sched_core_add(&c, &ghost);
    CHECK(c.current == &ghost);
    CHECK(c.live_count == 1);
    CHECK(c.ready_count == 0);
    CHECK(sched_core_find(&c, 7) == &ghost);
    CHECK(sched_core_find(&c, 0) == NULL);
}

static void t_ready_fifo(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t *b;
    thread_t *cc;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    b = mk(&c, 2);
    cc = mk(&c, 3);
    CHECK(a != NULL && b != NULL && cc != NULL);
    CHECK(c.ready_count == 3);
    CHECK(c.live_count == 3);
    CHECK(c.ready_head == a);
    CHECK(c.ready_tail == cc);

    CHECK(sched_core_pick(&c) == a);
    CHECK(a->state == THREAD_RUNNING);
    CHECK(c.current == a);
    CHECK(c.ready_count == 2);
    CHECK(c.picks == 1);
    CHECK(sched_core_pick(&c) == b);
    CHECK(sched_core_pick(&c) == cc);
    CHECK(sched_core_pick(&c) == NULL);
    CHECK(c.ready_count == 0);
    CHECK(c.ready_head == NULL && c.ready_tail == NULL);
    CHECK(c.picks == 3);
}

static void t_round_robin(void)
{
    struct sched_core c;
    thread_t *expect[3];
    thread_t *t;
    u32 r;
    u32 i;

    reset_all();
    sched_core_init(&c);
    expect[0] = mk(&c, 1);
    expect[1] = mk(&c, 2);
    expect[2] = mk(&c, 3);
    CHECK(expect[0] != NULL && expect[1] != NULL && expect[2] != NULL);

    for (r = 0; r < 4; r++) {
        for (i = 0; i < 3; i++) {
            t = sched_core_pick(&c);
            CHECK(t == expect[i]);
            sched_core_ready_push(&c, t);
        }
    }
    CHECK(expect[0]->switches == 4);
    CHECK(expect[1]->switches == 4);
    CHECK(expect[2]->switches == 4);
    CHECK(c.ready_count == 3);
    CHECK(c.picks == 12);
    CHECK(c.ready_head == expect[0] && c.ready_tail == expect[2]);
}

static void t_blocked(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t *b;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    b = mk(&c, 2);
    CHECK(sched_core_pick(&c) == a);
    CHECK(sched_core_block(&c, a, 100) == THREAD_OK);
    CHECK(a->state == THREAD_BLOCKED);
    CHECK(c.blocked_count == 1);
    CHECK(c.blocks == 1);
    CHECK(a->wake_tick == 100);
    CHECK(a->wake_reason == THREAD_WAKE_NONE);
    CHECK(c.ready_count == 1);

    /* BLOCKED 的线程不会被 pick 到。 */
    CHECK(sched_core_pick(&c) == b);
    CHECK(sched_core_pick(&c) == NULL);

    /* 状态不对的 block/wake 都拒绝。 */
    CHECK(sched_core_block(&c, b, 5) == THREAD_OK);
    CHECK(sched_core_block(&c, a, 5) == THREAD_EBAD);
    CHECK(c.blocked_count == 2);
}

static void t_poll(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t *b;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    b = mk(&c, 2);
    CHECK(sched_core_pick(&c) == a);
    CHECK(sched_core_block(&c, a, 100) == THREAD_OK);
    CHECK(sched_core_pick(&c) == b);
    CHECK(sched_core_block(&c, b, 200) == THREAD_OK);
    CHECK(c.blocked_count == 2);

    CHECK(sched_core_poll(&c, 99) == 0);
    CHECK(a->state == THREAD_BLOCKED);
    CHECK(sched_core_poll(&c, 100) == 1);
    CHECK(a->state == THREAD_READY);
    CHECK(a->wake_reason == THREAD_WAKE_TIMER);
    CHECK(a->wake_tick == 0);
    CHECK(c.wakes_timer == 1);
    CHECK(c.blocked_count == 1);
    CHECK(c.ready_count == 1);
    CHECK(sched_core_poll(&c, 199) == 0);
    CHECK(sched_core_poll(&c, 200) == 1);
    CHECK(b->state == THREAD_READY);
    CHECK(c.wakes_timer == 2);
    CHECK(c.blocked_count == 0);
    CHECK(c.ready_count == 2);
    /* 定时唤醒按 BLOCKED 链顺序入 ready 队尾。 */
    CHECK(sched_core_pick(&c) == a);
    CHECK(sched_core_pick(&c) == b);
}

static void t_poll_no_deadline(void)
{
    struct sched_core c;
    thread_t *a;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    CHECK(sched_core_pick(&c) == a);
    CHECK(sched_core_block(&c, a, 0) == THREAD_OK);
    CHECK(sched_core_poll(&c, ~0ULL) == 0);
    CHECK(a->state == THREAD_BLOCKED);
    CHECK(c.wakes_timer == 0);
}

static void t_explicit_wake(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t *b;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    b = mk(&c, 2);
    CHECK(sched_core_pick(&c) == a);
    CHECK(sched_core_block(&c, a, 0) == THREAD_OK);
    CHECK(sched_core_wake(&c, a, THREAD_WAKE_EXPLICIT) == THREAD_OK);
    CHECK(a->state == THREAD_READY);
    CHECK(a->wake_reason == THREAD_WAKE_EXPLICIT);
    CHECK(a->wake_tick == 0);
    CHECK(c.wakes_explicit == 1);
    CHECK(c.blocked_count == 0);
    /* 显式唤醒排到 ready 队尾（b 在它前面）。 */
    CHECK(c.ready_head == b);
    CHECK(c.ready_tail == a);
    CHECK(sched_core_pick(&c) == b);
    CHECK(sched_core_pick(&c) == a);
}

static void t_wake_errors(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t ghost;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    memset(&ghost, 0, sizeof(ghost));
    ghost.tid = 99;
    ghost.state = THREAD_UNUSED;

    CHECK(sched_core_wake(&c, a, THREAD_WAKE_EXPLICIT) == THREAD_EBAD); /* READY */
    CHECK(sched_core_wake(&c, NULL, THREAD_WAKE_EXPLICIT) == THREAD_EBAD);
    CHECK(sched_core_wake(&c, &ghost, THREAD_WAKE_EXPLICIT) == THREAD_EBAD);
    CHECK(c.wakes_explicit == 0);
    CHECK(a->state == THREAD_READY);
}

static void t_zombie_reap(void)
{
    struct sched_core c;
    thread_t *a;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    CHECK(c.live_count == 1);
    CHECK(sched_core_pick(&c) == a);
    sched_core_zombie(&c, a);
    CHECK(a->state == THREAD_DEAD);
    CHECK(c.zombie_count == 1);
    CHECK(c.exits == 1);
    CHECK(c.live_count == 0);
    CHECK(sched_core_pick(&c) == NULL);
    /* 回收前还在 all 链上（tid 仍可查），回收后摘掉。 */
    CHECK(sched_core_find(&c, a->tid) == a);
    CHECK(sched_core_reap(&c, NULL) == a);
    CHECK(a->state == THREAD_UNUSED);
    CHECK(c.zombie_count == 0);
    CHECK(c.reaps == 1);
    CHECK(sched_core_find(&c, a->tid) == NULL);
    CHECK(sched_core_reap(&c, NULL) == NULL);
}

static void t_reap_skip(void)
{
    struct sched_core c;
    thread_t *a;
    thread_t *b;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    b = mk(&c, 2);
    CHECK(sched_core_pick(&c) == a);
    sched_core_zombie(&c, a);
    CHECK(sched_core_pick(&c) == b);
    sched_core_zombie(&c, b);
    CHECK(c.zombie_count == 2);

    /* skip 是给“正在退出的线程”用的：不能回收它自己。 */
    CHECK(sched_core_reap(&c, a) == b);
    CHECK(sched_core_find(&c, b->tid) == NULL);
    CHECK(sched_core_find(&c, a->tid) == a);
    CHECK(sched_core_reap(&c, a) == NULL); /* 只剩 skip 自己 */
    CHECK(c.zombie_count == 1);
    CHECK(sched_core_reap(&c, NULL) == a);
    CHECK(c.zombie_count == 0);
    CHECK(c.reaps == 2);
}

static void t_fairness(void)
{
    struct sched_core c;
    thread_t *expect[3];
    u32 i;

    reset_all();
    sched_core_init(&c);
    expect[0] = mk(&c, 1);
    expect[1] = mk(&c, 2);
    expect[2] = mk(&c, 3);
    for (i = 0; i < 30; i++) {
        thread_t *t = sched_core_pick(&c);

        CHECK(t == expect[i % 3]);
        sched_core_ready_push(&c, t);
    }
    CHECK(expect[0]->switches == 10);
    CHECK(expect[1]->switches == 10);
    CHECK(expect[2]->switches == 10);
    CHECK(c.ready_count == 3);
}

static void t_live_count(void)
{
    struct sched_core c;
    thread_t *a;

    reset_all();
    sched_core_init(&c);
    a = mk(&c, 1);
    mk(&c, 2);
    mk(&c, 3);
    CHECK(c.live_count == 3);
    CHECK(sched_core_pick(&c) == a);
    sched_core_zombie(&c, a);
    CHECK(c.live_count == 2);
    CHECK(sched_core_reap(&c, NULL) == a);
    CHECK(c.live_count == 2); /* 回收不改变“存活数” */
}

/* ---- 内核胶水（fake 环境下可安全测试的部分）------------------------------ */

static void dummy_fn(void *arg)
{
    (void)arg;
}

static void t_thread_create(void)
{
    thread_t *t;
    u64 saved;

    reset_all();
    sched_init();
    CHECK(thread_self() != NULL);
    CHECK(thread_self()->state == THREAD_RUNNING);
    CHECK(thread_self()->flags == THREAD_F_BOOT);
    CHECK(sched_live_count() == 1);
    CHECK(sched_created_count() == 0);
    CHECK(sched_stack_live() == 0);
    CHECK(sched_switch_count() == 0);
    CHECK(sched_is_started() == 0);

    t = thread_create(dummy_fn, (void *)(usize)0x1234);
    CHECK(t != NULL);
    CHECK(t->tid == 2);
    CHECK(t->state == THREAD_READY);
    CHECK(t->stack_size == NG_THREAD_STACK_SIZE);
    CHECK(t->stack_base != NULL);
    CHECK(((u64)(usize)t->stack_base & 15ULL) == 0);
    CHECK(t->stack_top ==
          (void *)((u64)(usize)t->stack_base + NG_THREAD_STACK_SIZE));
    CHECK(t->ctx.rsp == (u64)(usize)t->stack_top);
    CHECK((t->ctx.rsp & 15ULL) == 0);
    CHECK(t->ctx.rip == sched_arch_entry_rip());
    CHECK(t->ctx.rsp > t->ctx.rip);
    CHECK(t->arg == (void *)(usize)0x1234);
    CHECK(t->start == dummy_fn);
    CHECK(thread_stack_canary_ok(t));
    CHECK(fake_allocs == 1);
    CHECK(fake_frees == 0);
    CHECK(sched_created_count() == 1);
    CHECK(sched_live_count() == 2);
    CHECK(sched_stack_live() == 1);

    /* create 按 FIFO 入 ready。 */
    {
        thread_t *t2 = thread_create(dummy_fn, NULL);

        CHECK(t2 != NULL && t2->tid == 3);
        CHECK(sched_core_state()->ready_head == t);
        CHECK(sched_core_state()->ready_tail == t2);
        CHECK(sched_core_state()->ready_count == 2);
        CHECK(sched_core_state()->current == thread_self());
    }

    /* canary 坏了必须能被发现（线程退出路径据此计入 corrupt）。 */
    saved = ((u64 *)t->stack_base)[0];
    ((u64 *)t->stack_base)[0] = 0;
    CHECK(!thread_stack_canary_ok(t));
    ((u64 *)t->stack_base)[0] = saved;
    CHECK(thread_stack_canary_ok(t));
    CHECK(sched_stack_corrupt_count() == 0);
}

static void t_thread_create_limits(void)
{
    u32 n = 0;

    reset_all();
    sched_init();
    CHECK(thread_create(NULL, NULL) == NULL);
    CHECK(sched_created_count() == 0);
    CHECK(sched_live_count() == 1);

    while (n < 100 && thread_create(dummy_fn, NULL) != NULL) {
        n++;
    }
    CHECK(n == NG_THREAD_MAX - 1); /* 0 号槽是 boot 线程 */
    CHECK(sched_created_count() == NG_THREAD_MAX - 1);
    CHECK(sched_live_count() == NG_THREAD_MAX);
    CHECK(fake_allocs == NG_THREAD_MAX - 1);
}

static void t_thread_wake_and_sleep0(void)
{
    thread_t *t;

    reset_all();
    sched_init();
    CHECK(thread_wake(1) == THREAD_EBAD); /* boot 是 RUNNING，不是 BLOCKED */
    CHECK(thread_wake(0) == THREAD_ENOENT);
    CHECK(thread_wake(9999) == THREAD_ENOENT);

    /* ticks=0 等价一次 yield，且没有别的线程时不会真的切换。 */
    CHECK(thread_sleep(0) == THREAD_WAKE_NONE);
    CHECK(sched_switch_count() == 0);
    CHECK(thread_self()->state == THREAD_RUNNING);
    CHECK(sched_yield_count() == 1);

    t = thread_create(dummy_fn, NULL);
    CHECK(t != NULL);
    CHECK(thread_wake(t->tid) == THREAD_EBAD); /* READY 不是 BLOCKED */
    CHECK(sched_yield_count() == 1);
}

int test_sched(void)
{
    t_core_init();
    t_ready_fifo();
    t_round_robin();
    t_blocked();
    t_poll();
    t_poll_no_deadline();
    t_explicit_wake();
    t_wake_errors();
    t_zombie_reap();
    t_reap_skip();
    t_fairness();
    t_live_count();
    t_thread_create();
    t_thread_create_limits();
    t_thread_wake_and_sleep0();
    return 0;
}
