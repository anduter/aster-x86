/*
 * ng WP1-5: 线程模型 + 单核协作式调度器接口。
 *
 * 分两层，方便 host 单测脱离真正切栈来驱动：
 *   1. `struct sched_core` + `sched_core_*()`：纯状态机（ready 队列 / BLOCKED
 *      链 / 定时唤醒 / zombie 回收）。不碰栈、不碰时钟、不切换上下文，
 *      host 单测可以直接构造 thread_t 数组来跑（tests/unit-ng/test_sched.c）。
 *   2. `sched_*()` / `thread_*()`：内核胶水。栈从 heap 分配、时间源用 TSC、
 *      上下文切换走 switch.S 的 `switch_context`，具体由 sched_arch.c 提供
 *      （host 单测用 fake 版本替换）。
 *
 * 线程状态机：
 *
 *   UNUSED --thread_create--> READY --pick--> RUNNING --yield--> READY
 *                               ^                 |  \--sleep--> BLOCKED
 *                               |                 |              |
 *                               +----wake---------+              |
 *                                                 \--exit--> DEAD --reap--> UNUSED
 *
 * 约定：
 *   - 单核、IF=0；调度只在显式 sched_yield()/thread_sleep()/thread_exit()
 *     处发生（协作式）。以后接 tick IRQ 时，IRQ 里只调用 sched_tick() 记
 *     到期线程，真正的切换仍留给调度点（WP1-6 的抢占式调度再改）。
 *   - TCB 放静态池（16 个槽，2 KiB 左右），16 KiB 栈从 heap 分配；thread_exit
 *     只把线程变 DEAD 并挂到 zombie 链，栈由“别的线程下一次调度”回收，
 *     避免在自己正在用的栈上 kfree。
 *   - 栈底 16 字节是 canary；线程退出时校验，溢出会计入
 *     sched_stack_corrupt_count()。守护页（unmap 一页）需要把 2 MiB 恒等
 *     映射拆成 4 KiB 页，属于 WP1-5 之外的页表工作，先不做。
 */
#ifndef NG_THREAD_H
#define NG_THREAD_H

#include "types.h"

/* 每个线程的内核栈大小（16 KiB，从 heap 分配）。 */
#define NG_THREAD_STACK_SIZE 16384ULL
/* TCB 静态池大小（含 boot 线程与 idle 线程）。 */
#define NG_THREAD_MAX 16u
/* switch.S 保存的上下文必须正好这么宽（8 个 u64）。 */
#define NG_THREAD_CONTEXT_SIZE 64u

enum thread_state {
    THREAD_UNUSED = 0, /* TCB 槽空闲 */
    THREAD_READY = 1,
    THREAD_RUNNING = 2,
    THREAD_BLOCKED = 3,
    THREAD_DEAD = 4 /* 等回收（zombie） */
};

/* thread_t.flags */
#define THREAD_F_IDLE 0x1u /* 调度器 idle 线程（hlt/轮询定时器） */
#define THREAD_F_BOOT 0x2u /* 引导线程（kmain 所在栈，不回收） */

/* 唤醒原因（thread_sleep 返回值 / thread_t.wake_reason） */
#define THREAD_WAKE_NONE 0
#define THREAD_WAKE_TIMER 1    /* 到期被 sched_core_poll 唤醒 */
#define THREAD_WAKE_EXPLICIT 2 /* 被 thread_wake(tid) 唤醒 */

/* 错误码 */
#define THREAD_OK 0
#define THREAD_ENOENT (-1) /* 没有这个 tid */
#define THREAD_EBAD (-2)   /* 状态不允许（比如唤醒一个没在睡的线程） */
#define THREAD_ENOMEM (-3) /* 没有空 TCB 槽 / 栈分配失败 */

/*
 * switch.S 保存/恢复的上下文。字段顺序与偏移被汇编硬编码：
 *   rbx=0 rbp=8 r12=16 r13=24 r14=32 r15=40 rsp=48 rip=56
 * rsp 存的是“switch_context 返回之后”的栈指针，rip 是返回地址；恢复时
 * 直接 jmp rip 并装载 rsp，等价于那次调用正常返回。
 */
struct thread_context {
    u64 rbx;
    u64 rbp;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rsp;
    u64 rip;
};

_Static_assert(sizeof(struct thread_context) == NG_THREAD_CONTEXT_SIZE,
               "thread_context must be 64 bytes");
_Static_assert(__builtin_offsetof(struct thread_context, rbx) == 0,
               "ctx.rbx offset");
_Static_assert(__builtin_offsetof(struct thread_context, rbp) == 8,
               "ctx.rbp offset");
_Static_assert(__builtin_offsetof(struct thread_context, r12) == 16,
               "ctx.r12 offset");
_Static_assert(__builtin_offsetof(struct thread_context, r13) == 24,
               "ctx.r13 offset");
_Static_assert(__builtin_offsetof(struct thread_context, r14) == 32,
               "ctx.r14 offset");
_Static_assert(__builtin_offsetof(struct thread_context, r15) == 40,
               "ctx.r15 offset");
_Static_assert(__builtin_offsetof(struct thread_context, rsp) == 48,
               "ctx.rsp offset");
_Static_assert(__builtin_offsetof(struct thread_context, rip) == 56,
               "ctx.rip offset");

typedef struct thread {
    u32 tid;
    u32 state; /* enum thread_state */
    u32 flags; /* THREAD_F_* */
    u32 wake_reason;
    struct thread_context ctx;
    void (*start)(void *arg);
    void *arg;
    void *stack_base; /* 低地址；canary 在头 16 字节 */
    u64 stack_size;
    void *stack_top;
    u64 wake_tick;   /* BLOCKED 的到期 TSC；0 = 只等显式唤醒 */
    u64 switches;    /* 被选中运行的次数 */
    struct thread *next;     /* ready / blocked / zombie 链 */
    struct thread *all_next; /* 所有存活线程（按 tid 查、回收时摘链） */
} thread_t;

/* ---- switch.S ------------------------------------------------------------ */
void switch_context(struct thread_context *old_ctx, struct thread_context *new_ctx);
void thread_entry(void); /* 新线程第一次被调度到的入口 */
/* 由 thread_entry 调用：跑 start(arg)，返回时 thread_exit()。不返回。 */
void thread_bootstrap(void) __attribute__((noreturn));

/* ---- 调度器状态机（纯逻辑，host 单测直接驱动）--------------------------- */
struct sched_core {
    thread_t *ready_head;
    thread_t *ready_tail;
    thread_t *blocked_head;
    thread_t *zombie_head;
    thread_t *all_head;
    thread_t *current;
    u32 ready_count;
    u32 blocked_count;
    u32 zombie_count;
    u32 live_count; /* READY + RUNNING + BLOCKED */
    u32 next_tid;
    u64 created;
    u64 switches;
    u64 picks;
    u64 yields;
    u64 blocks;
    u64 wakes_timer;
    u64 wakes_explicit;
    u64 exits;
    u64 reaps;
    u64 idle_runs;
};

void sched_core_init(struct sched_core *c);
/* 注册线程：挂到 all 链、live_count++；state=READY 时同时入 ready 队尾。 */
void sched_core_add(struct sched_core *c, thread_t *t);
void sched_core_ready_push(struct sched_core *c, thread_t *t);
thread_t *sched_core_pick(struct sched_core *c); /* 队头出队，置 RUNNING */
int sched_core_block(struct sched_core *c, thread_t *t, u64 deadline);
int sched_core_wake(struct sched_core *c, thread_t *t, u32 reason);
u32 sched_core_poll(struct sched_core *c, u64 now); /* 到期唤醒数 */
void sched_core_zombie(struct sched_core *c, thread_t *t);
/* 取一个 zombie（跳过 skip，通常是当前正在跑的线程）；NULL = 没有可回收。 */
thread_t *sched_core_reap(struct sched_core *c, thread_t *skip);
thread_t *sched_core_find(struct sched_core *c, u32 tid);

/* ---- 内核调度器 ---------------------------------------------------------- */
void sched_init(void);  /* 建 boot 线程 TCB，清空队列（heap 就绪后调用） */
void sched_start(void); /* 建 idle 线程并第一次让出 CPU */
thread_t *thread_create(void (*fn)(void *), void *arg);
void sched_yield(void);
void sched_tick(void); /* 给未来的 tick IRQ 用：只做到期唤醒 */
void thread_exit(void) __attribute__((noreturn));
int thread_sleep(u64 ticks); /* 返回 THREAD_WAKE_*；ticks=0 等价 yield */
int thread_wake(u32 tid);

thread_t *thread_self(void);
struct sched_core *sched_core_state(void); /* 诊断/自检读队列状态 */
u64 sched_now(void); /* TSC */
u64 sched_switch_count(void);
u64 sched_yield_count(void);
u32 sched_live_count(void);
u32 sched_created_count(void);
u32 sched_reap_count(void);
u32 sched_zombie_count(void);
u32 sched_blocked_count(void);
u32 sched_stack_live(void);
u32 sched_stack_corrupt_count(void);
u32 sched_is_started(void);

/* 栈底 canary 校验（thread_exit 与自检共用）。 */
int thread_stack_canary_ok(const thread_t *t);

#endif /* NG_THREAD_H */
