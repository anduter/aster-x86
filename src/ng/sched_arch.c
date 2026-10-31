/*
 * ng WP1-5: 调度器的内核侧钩子（时间源 / 栈 / 上下文切换 / idle）。
 *
 * sched.c 只通过 sched_arch.h 依赖环境；这里给真实内核实现，host 单测在
 * tests/unit-ng/test_sched.c 里给 fake 版本（同一组符号，不同实现）。
 */
#include "sched_arch.h"

#include "heap.h"
#include "io.h"
#include "paging.h"
#include "syscall.h"

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
    /*
     * WP1-7：per-thread 内核栈。每个线程的 syscall 入口栈 / TSS.RSP0 都取自
     * 它自己的 TCB（boot 线程没有 TCB 栈，回退到 isr.S 的静态栈）。写的是
     * .bss/.data，可以在 IF=0 的调度点上安全做。
     *
     * WP3-0：多进程用户态。每个线程记录自己运行时的用户 PML4（内核线程为
     * NULL）。切栈之前先写 CR3 —— 所有 AS 都映射了内核 0..1 GiB 恒等区，
     * 内核代码/栈/TCB 在任何 AS 下都可访问，所以这个顺序是安全的；反过来
     * （先切栈再切 CR3）会让新线程在自己的栈上短暂跑在旧 AS 里。
     */
    syscall_set_thread_stack(next);
    {
        u64 *want = (next->pml4 != NULL) ? (u64 *)(usize)next->pml4
                                         : pg_kernel_pml4();

        if (want != pg_current_pml4()) {
            pg_switch_pml4(want);
        }
    }
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
