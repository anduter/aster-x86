/* =============================================================================
 * tests/unit/shim/include/kernel.h - host 单元测试用最小 kernel.h shim
 *
 * 只提供被测内核源文件实际用到的符号：自旋锁退化为单线程空实现，
 * 字符串/kprintf 由测试 TU 提供（编译时用 -D 改名，避免与 host libc 冲突）。
 * 这不是内核头；不要被内核源文件包含（只在 host 编译时通过 -I 优先命中）。
 * ========================================================================== */
#ifndef KERNEL_H_MODULE_H
#define KERNEL_H_MODULE_H

#include "../../../../src/kernel/include/types.h"
#include "../../../../src/kernel/core/error.h"

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0U }

static inline uint32_t spin_lock_irqsave(spinlock_t *l)
{
    (void)l;
    return 0U;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags)
{
    (void)l;
    (void)flags;
}

/* 内核字符串函数：测试 TU 用 -Dstrcmp=... 等改名后提供实现 */
void  *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strncpy(char *dst, const char *src, size_t n);

/* 内核打印：host 侧退化为 no-op（或按需打印） */
void kprintf(const char *fmt, ...);

#endif /* KERNEL_H_MODULE_H */
