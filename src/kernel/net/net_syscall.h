#ifndef NET_SYSCALL_H
#define NET_SYSCALL_H

#include "../types.h"
#include "include/proc.h"

bool net_syscall_owns(uint32_t nr);
int  net_syscall(interrupt_frame_t *f);

#endif