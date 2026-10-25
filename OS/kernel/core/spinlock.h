#ifndef CORE_SPINLOCK_H
#define CORE_SPINLOCK_H

#include "atomic.h"

/* 自旋锁 + 关中断变体（中断上下文安全）
 * 用法：
 *   uint32_t flags = spin_lock_irqsave(&lock);
 *   ... 临界区 ...
 *   spin_unlock_irqrestore(&lock, flags);
 */

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0U }

static inline void spin_lock(spinlock_t *l)
{
    while (atomic_xchg(&l->locked, 1U) != 0U) {
        while (atomic_read(&l->locked) != 0U) cpu_relax();
    }
    mb();                       /* 拿到锁后再读共享数据 */
}

static inline void spin_unlock(spinlock_t *l)
{
    mb();
    atomic_set(&l->locked, 0U);
}

/* 保存并关闭中断，返回原 EFLAGS（供 irq_restore 恢复） */
static inline uint32_t irq_save(void)
{
    uint32_t flags;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    if ((flags & 0x200U) != 0U) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static inline uint32_t spin_lock_irqsave(spinlock_t *l)
{
    uint32_t flags = irq_save();

    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif
