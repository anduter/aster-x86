/* =============================================================================
 * panic.h - 内核致命错误处理与断言
 * ============================================================================= */
#ifndef PANIC_H
#define PANIC_H

#include "../types.h"

/* 打印原因并停机（不返回）。后续接入中断后，这里会顺带 dump 寄存器现场。 */
__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

/* Reboot the machine (8042 0xFE command) */
void machine_reboot(void) __attribute__((noreturn));

/* 条件不成立即 panic，打印表达式与位置 */
#define kassert(cond)                                                   \
    do {                                                                \
        if (!(cond)) {                                                  \
            panic("assertion failed: %s  (%s:%d)",                      \
                  #cond, __FILE__, __LINE__);                           \
        }                                                               \
    } while (0)

#endif /* PANIC_H */
