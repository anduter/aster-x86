#ifndef CORE_ATOMIC_H
#define CORE_ATOMIC_H

#include "../types.h"

/* 原子操作与内存屏障（x86 上 lock 前缀 + pause/屏障指令） */

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline void mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
static inline void rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }

static inline uint32_t atomic_read(const volatile uint32_t *p)
{
    return *p;
}

static inline void atomic_set(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("movl %1, %0" : "=m"(*p) : "r"(v) : "memory");
}

static inline void atomic_inc(volatile uint32_t *p)
{
    __asm__ volatile("lock incl %0" : "+m"(*p) : : "memory");
}

static inline void atomic_dec(volatile uint32_t *p)
{
    __asm__ volatile("lock decl %0" : "+m"(*p) : : "memory");
}

static inline void atomic_add(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("lock addl %1, %0" : "+m"(*p) : "r"(v) : "memory");
}

/* 交换并返回旧值（自旋锁的核心） */
static inline uint32_t atomic_xchg(volatile uint32_t *p, uint32_t v)
{
    uint32_t old;

    __asm__ volatile("xchgl %0, %1" : "=r"(old), "+m"(*p) : "0"(v) : "memory");
    return old;
}

/* CAS：相等则写入新值并返回 1，否则返回 0 */
static inline int atomic_cmpxchg(volatile uint32_t *p, uint32_t expected, uint32_t newv)
{
    uint32_t old;

    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "=a"(old), "+m"(*p)
                     : "r"(newv), "0"(expected)
                     : "memory");
    return old == expected;
}

#endif
