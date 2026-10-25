#ifndef INTR_SYSCALL_H
#define INTR_SYSCALL_H

/* 兼容转发：SYS_* 编号的唯一来源是 include/abi.h。
 * 旧原型调用走 int 0x81；Native ABI（int 0x80）见 native.h。 */
#include "../../include/abi.h"

/* 用户/内核代码统一用这个宏发起系统调用 */
static inline int sys_call(int num, int a, int b, int c)
{
    int ret;

    __asm__ volatile ("int $0x81"
                      : "=a"(ret)
                      : "a"(num), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

/* 已发生的系统调用次数（供日志/统计） */
unsigned int syscall_count(void);

#endif
