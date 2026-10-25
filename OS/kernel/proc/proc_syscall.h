#ifndef PROC_SYSCALL_H
#define PROC_SYSCALL_H

#include "../include/types.h"
#include "../proc/trap/interrupt.h"

/* 进程组 legacy 系统调用（int 0x81）：getpid/uptime/yield/exit/kill/sleep/
 * fork/exec/wait/sigaction/sigreturn */
bool proc_syscall_owns(uint32_t nr);
int  proc_syscall(interrupt_frame_t *f);

#endif
