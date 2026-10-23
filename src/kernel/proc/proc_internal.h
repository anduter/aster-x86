#ifndef PROC_INTERNAL_H
#define PROC_INTERNAL_H

#include "proc.h"

/* process.c 与 sched.c 共享的内部状态 */
extern proc_t  proc_table[PROC_MAX];

/* 由 process.c 实现：保存当前上下文并切到 next。
 * 允许 next == 当前进程（直接返回）。 */
void proc_switch_to(proc_t *next);

/* 由 sched.c 实现：把进程挂到指定 CPU 的 runqueue。 */
void sched_enqueue(proc_t *p, uint32_t cpu);

/* 由 sched.c 实现：从 cur 之后环形找下一个 READY，找不到返回 0 */
proc_t *sched_next(proc_t *cur);

/* 汇编入口（switch.asm） */
void context_switch(void **old_esp, void *new_esp);
void thread_trampoline(void);

/* 新线程从这里进入 C（switch.asm 调用） */
void thread_entry_c(void);

#endif
