/*
 * ng WP1-5: 内核侧调度器自检（run-ng 的 5 条出口行的产生处）。
 *
 * 覆盖：
 *   1. 线程模型：3 个线程、16 KiB 栈、16 字节栈顶/上下文对齐、canary；
 *   2. round-robin：A/B/C 各循环 3 次“打印 + yield”，主线程计数等待；
 *      断言交错顺序严格是 ABCABCABC、每线程 3 次、切换次数 >= 6；
 *   3. exit/reap：3 个线程都 DEAD 且被回收，栈归还 heap（只剩 idle 的 1 个）；
 *   4. sleep/wake：D 用 TSC 定时睡（5M tick）被定时器唤醒，断言
 *      “确实进过 BLOCKED”“唤醒时刻 >= deadline”“原因是 TIMER”；
 *      E 睡 2^60 tick 后由主线程 thread_wake() 显式唤醒，断言原因 EXPLICIT、
 *      重复唤醒返回 EBAD、唤醒不存在的 tid 返回 ENOENT。
 *
 * 所有等待都有自旋上限，失败不会把 run-ng 拖到超时；失败行以
 * `ng: sched ... FAIL` 输出，run-ng 的失败模式会提前结束。
 */
#include "sched_selftest.h"

#include "serial.h"
#include "thread.h"

#define SELFTEST_ITERS 3u
#define SELFTEST_SPIN_LIMIT 1000000u
#define SLEEP_TICKS 5000000ULL       /* 约几毫秒（QEMU TSC） */
#define HUGE_TICKS (1ULL << 60)      /* 只可能被显式唤醒 */

static char trace_buf[32];
static u32 trace_len;
static u32 iter_count[3];

static volatile u64 sleep_start;
static volatile u64 sleep_deadline;
static volatile u64 sleep_wake;
static volatile int sleep_reason;
static volatile int explicit_reason;
static volatile int saw_d_blocked;
static volatile int saw_e_blocked;

static void trace_push(char tag)
{
    if (trace_len < (u32)(sizeof(trace_buf) - 1)) {
        trace_buf[trace_len++] = tag;
    }
}

static int trace_equals(const char *expect, u32 n)
{
    u32 i;

    if (trace_len != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (trace_buf[i] != expect[i]) {
            return 0;
        }
    }
    return 1;
}

static void print_trace(void)
{
    u32 i;

    serial_puts("ng: sched trace=");
    for (i = 0; i < trace_len; i++) {
        serial_putc(trace_buf[i]);
    }
    serial_puts("\r\n");
}

static int wait_state(thread_t *t, u32 want, u32 limit)
{
    u32 spins = 0;

    while (spins < limit) {
        if (t->state == want) {
            return 1;
        }
        if (t->state == THREAD_UNUSED || t->state == THREAD_DEAD) {
            return 0; /* 线程提前退出了，等不到了 */
        }
        sched_yield();
        spins++;
    }
    return 0;
}

static int wait_reaps(u32 base, u32 want, u32 limit)
{
    u32 spins = 0;

    while (spins < limit) {
        if (sched_reap_count() - base >= want) {
            return 1;
        }
        sched_yield();
        spins++;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 3 个 round-robin 线程                                                      */
/* ------------------------------------------------------------------------- */

static void worker_run(char tag, u32 idx)
{
    u32 i;

    for (i = 0; i < SELFTEST_ITERS; i++) {
        serial_puts("ng: thread ");
        serial_putc(tag);
        serial_puts(" ");
        serial_putdec((u64)i);
        serial_puts("\r\n");
        iter_count[idx]++;
        trace_push(tag);
        sched_yield();
    }
    thread_exit();
}

static void worker_a(void *arg)
{
    (void)arg;
    worker_run('A', 0);
}

static void worker_b(void *arg)
{
    (void)arg;
    worker_run('B', 1);
}

static void worker_c(void *arg)
{
    (void)arg;
    worker_run('C', 2);
}

/* ------------------------------------------------------------------------- */
/* sleep / wake                                                               */
/* ------------------------------------------------------------------------- */

static void sleeper_timer(void *arg)
{
    (void)arg;

    serial_puts("ng: thread D sleep\r\n");
    sleep_start = sched_now();
    sleep_deadline = sleep_start + SLEEP_TICKS;
    sleep_reason = thread_sleep(SLEEP_TICKS);
    sleep_wake = sched_now();
    serial_puts("ng: thread D woke\r\n");
    thread_exit();
}

static void sleeper_explicit(void *arg)
{
    (void)arg;

    serial_puts("ng: thread E sleep (explicit wake)\r\n");
    explicit_reason = thread_sleep(HUGE_TICKS);
    serial_puts("ng: thread E woken\r\n");
    thread_exit();
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int sched_selftest(void)
{
    static const char expect_trace[] = "ABCABCABC";
    thread_t *a;
    thread_t *b;
    thread_t *c;
    thread_t *d;
    thread_t *e;
    u32 i;
    u32 base_reaps;
    u64 rounds;
    u64 yields;
    int ok;
    int switch_ok;
    int reap_ok;
    int sleep_ok;
    int wake_ok;
    int w_ok;
    int w_bad;
    int w_enoent;
    u64 late;
    int fails = 0;

    trace_len = 0;
    for (i = 0; i < 3; i++) {
        iter_count[i] = 0;
    }
    sleep_start = 0;
    sleep_deadline = 0;
    sleep_wake = 0;
    sleep_reason = 0;
    explicit_reason = 0;
    saw_d_blocked = 0;
    saw_e_blocked = 0;

    /* ---- 1. 线程模型 + 创建 ------------------------------------------- */
    sched_init();

    a = thread_create(worker_a, NULL);
    b = thread_create(worker_b, NULL);
    c = thread_create(worker_c, NULL);

    ok = a != NULL && b != NULL && c != NULL;
    if (ok) {
        ok = sched_created_count() == 3 && sched_live_count() == 4 &&
             a->stack_size == NG_THREAD_STACK_SIZE &&
             ((u64)(usize)a->stack_base & 15ULL) == 0 &&
             a->ctx.rsp == (u64)(usize)a->stack_top &&
             (a->ctx.rsp & 15ULL) == 0 && a->ctx.rip != 0 &&
             a->tid != b->tid && a->tid != c->tid && b->tid != c->tid &&
             thread_stack_canary_ok(a) && thread_stack_canary_ok(b) &&
             thread_stack_canary_ok(c);
    }
    if (!ok) {
        serial_puts("ng: sched create FAIL\r\n");
        return fails + 1;
    }
    serial_puts("ng: sched stack=16384 ctx=64 align=16 canary=ok\r\n");
    serial_puts("ng: sched create ok (3)\r\n");

    /* ---- 2. 开始调度（建 idle，第一次让出）---------------------------- */
    sched_start();
    if (!sched_is_started()) {
        serial_puts("ng: sched start FAIL\r\n");
        return fails + 1;
    }

    /* 主线程计数等待：3 个 worker 跑完 3 轮并退出、被回收。 */
    base_reaps = sched_reap_count();
    (void)wait_reaps(base_reaps, 3, SELFTEST_SPIN_LIMIT);
    rounds = sched_switch_count();
    yields = sched_yield_count();

    print_trace();
    serial_puts("ng: sched rounds=");
    serial_putdec(rounds);
    serial_puts(" yields=");
    serial_putdec(yields);
    serial_puts(" reaps=");
    serial_putdec((u64)(sched_reap_count() - base_reaps));
    serial_puts(" stacks=");
    serial_putdec((u64)sched_stack_live());
    serial_puts("\r\n");

    switch_ok = rounds >= 6 && trace_equals(expect_trace, 9) &&
                iter_count[0] == SELFTEST_ITERS &&
                iter_count[1] == SELFTEST_ITERS &&
                iter_count[2] == SELFTEST_ITERS;
    if (switch_ok) {
        serial_puts("ng: sched order ok (ABCABCABC)\r\n");
        serial_puts("ng: sched switch ok (rounds>=6)\r\n");
    } else {
        fails++;
        serial_puts("ng: sched switch FAIL\r\n");
    }

    reap_ok = sched_reap_count() - base_reaps == 3 &&
              sched_zombie_count() == 0 && sched_blocked_count() == 0 &&
              sched_stack_live() == 1; /* 只剩 idle 的栈 */
    if (reap_ok) {
        serial_puts("ng: sched exit reap ok\r\n");
    } else {
        fails++;
        serial_puts("ng: sched exit reap FAIL\r\n");
    }

    /* ---- 3. sleep：TSC 定时唤醒 --------------------------------------- */
    sleep_ok = 0;
    d = thread_create(sleeper_timer, NULL);
    if (d != NULL) {
        u32 before = sched_reap_count();

        saw_d_blocked = wait_state(d, THREAD_BLOCKED, SELFTEST_SPIN_LIMIT);
        ok = saw_d_blocked && wait_reaps(before, 1, SELFTEST_SPIN_LIMIT);
        late = (sleep_wake >= sleep_deadline) ? sleep_wake - sleep_deadline : 0;
        sleep_ok = ok && (sched_reap_count() - before == 1) &&
                   sleep_reason == THREAD_WAKE_TIMER &&
                   sleep_wake >= sleep_deadline &&
                   sleep_wake - sleep_start >= SLEEP_TICKS;
        serial_puts("ng: sched sleep ticks=");
        serial_putdec(SLEEP_TICKS);
        serial_puts(" blocked=");
        serial_putdec((u64)(saw_d_blocked ? 1 : 0));
        serial_puts(" reason=");
        serial_putdec((u64)(sleep_reason > 0 ? sleep_reason : 0));
        serial_puts(" late=");
        serial_putdec(late);
        serial_puts("\r\n");
    } else {
        serial_puts("ng: sched sleep FAIL (create)\r\n");
    }
    if (sleep_ok) {
        serial_puts("ng: sched sleep timer ok\r\n");
    } else {
        fails++;
        serial_puts("ng: sched sleep timer FAIL\r\n");
    }

    /* ---- 4. wake：显式唤醒 ------------------------------------------- */
    wake_ok = 0;
    e = thread_create(sleeper_explicit, NULL);
    if (e != NULL) {
        u32 before = sched_reap_count();
        u32 tid = e->tid;

        saw_e_blocked = wait_state(e, THREAD_BLOCKED, SELFTEST_SPIN_LIMIT);
        w_ok = 0;
        w_bad = 0;
        w_enoent = 0;
        if (saw_e_blocked) {
            w_ok = thread_wake(tid) == THREAD_OK;
            w_bad = thread_wake(tid) == THREAD_EBAD;       /* 已不在睡 */
            w_enoent = thread_wake(0x7FFFFFFFu) == THREAD_ENOENT;
        }
        ok = saw_e_blocked && w_ok && w_bad && w_enoent &&
             wait_reaps(before, 1, SELFTEST_SPIN_LIMIT);
        wake_ok = ok && (sched_reap_count() - before == 1) &&
                  explicit_reason == THREAD_WAKE_EXPLICIT;
        serial_puts("ng: sched wake rc=");
        serial_putdec((u64)(w_ok ? 1 : 0));
        serial_putdec((u64)(w_bad ? 1 : 0));
        serial_putdec((u64)(w_enoent ? 1 : 0));
        serial_puts(" blocked=");
        serial_putdec((u64)(saw_e_blocked ? 1 : 0));
        serial_puts(" reason=");
        serial_putdec((u64)(explicit_reason > 0 ? explicit_reason : 0));
        serial_puts("\r\n");
    } else {
        serial_puts("ng: sched wake FAIL (create)\r\n");
    }
    if (wake_ok) {
        serial_puts("ng: sched wake explicit ok\r\n");
    } else {
        fails++;
        serial_puts("ng: sched wake explicit FAIL\r\n");
    }

    if (sleep_ok && wake_ok) {
        serial_puts("ng: sched sleep/wake ok\r\n");
    } else {
        fails++;
        serial_puts("ng: sched sleep/wake FAIL\r\n");
    }

    /* 收尾：栈只应剩 idle 的 1 个，canary 没有报错。 */
    if (sched_stack_live() != 1 || sched_stack_corrupt_count() != 0 ||
        sched_zombie_count() != 0) {
        fails++;
        serial_puts("ng: sched stacks FAIL\r\n");
    } else {
        serial_puts("ng: sched stacks=1 corrupt=0\r\n");
    }

    return fails;
}
