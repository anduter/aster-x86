/*
 * ng WP1-5: 调度器的内核侧钩子（时间源 / 栈 / 上下文切换 / idle）。
 *
 * sched.c 只通过 sched_arch.h 依赖环境；这里给真实内核实现，host 单测在
 * tests/unit-ng/test_sched.c 里给 fake 版本（同一组符号，不同实现）。
 */
#include "sched_arch.h"

#include "heap.h"
#include "io.h"

extern void switch_context(struct thread_context *old_ctx,
                           struct thread_context *new_ctx);

u64 sched_arch_time(void)
{
    u32 lo;
    u32 hi;

    /* RDTSC；单核 guest 上单调。WP1-5 不要求频率换算，只做相对超时。 */
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

void *sched_arch_stack_alloc(u64 size)
{
    /* heap 的 payload 至少 16 字节对齐，栈顶（base+size）因此也 16 对齐。 */
    return kmalloc(size);
}

void sched_arch_stack_free(void *stack, u64 size)
{
    (void)size;
    (void)kfree(stack);
}

void sched_arch_switch(thread_t *prev, thread_t *next)
{
    switch_context(&prev->ctx, &next->ctx);
}

u64 sched_arch_entry_rip(void)
{
    return (u64)(usize)thread_entry;
}

void sched_arch_idle(void)
{
    cpu_hlt();
}
