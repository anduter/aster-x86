#ifndef PROC_SLOT_H
#define PROC_SLOT_H

#include "proc.h"

/* 进程槽位分配/回收的串行化接口（audit-smp）。
 *
 * 旧实现里 proc_spawn()/proc_fork() 都是“无锁扫描 proc_table 找 FREE 槽 →
 * 写 pid/state”，两颗 CPU 同时 fork/spawn 时会选中同一个槽位：
 *   - 两个进程共用一份 proc_t / 内核栈（同一 esp 被两颗 CPU 执行）；
 *   - next_pid++ 丢更新 → 两个进程同 pid，waitpid 认错子进程；
 *   - 后写者的 stack_base/page_dir 覆盖前者 → 内核栈泄漏。
 *
 * 现在槽位扫描、pid 分配、占位（state=READY）在同一把 proc_slot_lock 里完成；
 * 回收（memset）也在同一把锁里做，避免 allocator 看到半清零的槽位。 */
proc_t *proc_slot_alloc(void);       /* 保留一个槽位并分配 pid（state=READY） */
void    proc_slot_discard(proc_t *p);/* 释放“还没入队”的保留槽位 */
void    proc_slot_clear(proc_t *p);  /* memset；调用者需持有 rq_all + sched_lock */

#endif
