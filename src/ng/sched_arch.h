/*
 * ng WP1-5: 调度器的 arch/环境钩子。
 *
 * sched.c 只依赖这 6 个函数，于是同一份状态机既能跑在内核（TSC/switch.S/
 * kmalloc），也能在 host 单测里用 fake 环境驱动（tests/unit-ng/test_sched.c）。
 */
#ifndef NG_SCHED_ARCH_H
#define NG_SCHED_ARCH_H

#include "thread.h"

/* 时间源：内核 = RDTSC；host = fake 计数器。 */
u64 sched_arch_time(void);

/* 内核栈：内核 = kmalloc/kfree；host = malloc/free。返回 16 字节对齐。 */
void *sched_arch_stack_alloc(u64 size);
void sched_arch_stack_free(void *stack, u64 size);

/* 保存 prev 的上下文、恢复 next 的上下文（switch.S 的 switch_context）。 */
void sched_arch_switch(thread_t *prev, thread_t *next);

/* 新线程第一次被调度到的入口地址（switch.S 的 thread_entry）。 */
u64 sched_arch_entry_rip(void);

/* 没有 runnable/blocked 线程时的停机原语（内核 = hlt；host = no-op）。 */
void sched_arch_idle(void);

#endif /* NG_SCHED_ARCH_H */
