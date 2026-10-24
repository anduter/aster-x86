#ifndef MM_SYSCALL_H
#define MM_SYSCALL_H

#include "../types.h"
#include "../proc/trap/interrupt.h"

/* 内存组 legacy 系统调用（int 0x81）：brk / mmap / munmap */
bool mm_syscall_owns(uint32_t nr);
int  mm_syscall(interrupt_frame_t *f);

#endif
