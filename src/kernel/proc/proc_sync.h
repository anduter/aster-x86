#ifndef PROC_SYNC_H
#define PROC_SYNC_H

#include "proc.h"

/* proc_block() 与 sched_wake()/proc_wakeup() 的“唤醒不丢”握手。
 *
 * 两边都用 sched.c 的 sched_lock 串行化：
 *   - waker：在锁内读 state；RUNNING/READY 只置 wake_pending（不搬队列），
 *     BLOCKED 走原来的入队/延迟补投逻辑；
 *   - blocker：在锁内先消费 wake_pending，有就立刻返回（假唤醒，调用方
 *     重新检查条件），否则把 state 置 BLOCKED。
 *
 * 这样可以消除“检查条件之后、真正睡眠之前到达的唤醒被丢弃”的窗口
 * （pipe_read / waitpid / getchar / port_accept 都用的 check-then-block）。
 *
 * 返回 true 表示已有 pending 唤醒，调用方不应睡眠。
 * 由 sched.c 实现；调用前必须已关中断（proc_block 的约定）。
 */
bool sched_block_begin(proc_t *p, unsigned int chan);

/* 清掉进程的 wake_pending（自检用来消除此前的杂散 latch）。 */
void sched_clear_wake(proc_t *p);

#endif
