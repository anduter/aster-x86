#ifndef PROC_INTERNAL_H
#define PROC_INTERNAL_H

#include "proc.h"

/* process.c 与 sched.c 共享的内部状态 */
extern proc_t  proc_table[PROC_MAX];

/* 由 process.c 实现：保存当前上下文并切到 next。
 * 允许 next == 当前进程（直接返回）。 */
void proc_switch_to(proc_t *next);

/* 由 sched.c 实现：把进程挂到指定 CPU 的 runqueue（会先从其它 CPU 摘掉）。 */
void sched_enqueue(proc_t *p, uint32_t cpu);

/* 唤醒阻塞进程：加锁完成 READY + 入队 + （必要时）IPI 踢目标 CPU */
void sched_wake(proc_t *p);

/* VFS namespace 侧表：不改 proc_t 布局 */
const char *proc_root_of(proc_t *p);
int  proc_root_set(proc_t *p, const char *root);
void proc_root_copy(proc_t *parent, proc_t *child);

/* 从所有 runqueue 摘掉（进程退出/回收时调用）。 */
void sched_dequeue(proc_t *p);

/* 按当前负载选一个 CPU（最小负载，平局优先 BSP）。 */
uint32_t sched_place(const proc_t *p);

/* 设置静态优先级（0..7，越小越高）；返回 0 成功，-1 参数非法。
 * 已入队的进程会同时迁移到新优先级桶；运行中/阻塞中只改字段。 */
int sched_set_priority(proc_t *p, uint8_t prio);

/* 调整 aging 窗口（tick）；仅供自检缩短等待时间，正常路径用
 * SCHED_AGING_TICKS。 */
void sched_set_aging_ticks(uint32_t ticks);

/* 调度器自检入口（sched_selftest.c）：注册并创建 3 个不同优先级的
 * 忙等线程，验证优先级顺序与 aging。由 sched_start() 在选第一个进程前调用。 */
void sched_selftest_start(void);

/* 给指定 CPU 发重新调度 IPI。 */
void sched_kick(uint32_t cpu);

/* 负载均衡：最忙 CPU -> 空转 CPU 迁移一个 READY 进程。 */
void sched_balance(void);

/* 由 sched.c 实现：从 cur 之后环形找下一个 READY，找不到返回 0 */
proc_t *sched_next(proc_t *cur);

/* 汇编入口（switch.asm） */
void context_switch(void **old_esp, void *new_esp);
void thread_trampoline(void);

/* 新线程从这里进入 C（switch.asm 调用） */
void thread_entry_c(void);

#endif
