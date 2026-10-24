#ifndef MISC_SYSCALL_H
#define MISC_SYSCALL_H

#include "../types.h"
#include "../proc/trap/interrupt.h"

/* 杂项/调试组 legacy 系统调用（int 0x81）：
 * write / getchar / reboot / oops / panic / pci / poweroff / hang /
 * resinfo / eth / mouse */
bool misc_syscall_owns(uint32_t nr);
int  misc_syscall(interrupt_frame_t *f);

#endif
