#ifndef SECURITY_SYSCALL_H
#define SECURITY_SYSCALL_H

#include "../types.h"
#include "include/proc.h"

/* 身份/权限组 legacy 系统调用（int 0x81）：getuid / getgid / setuid */
bool security_syscall_owns(uint32_t nr);
int  security_syscall(interrupt_frame_t *f);

#endif
