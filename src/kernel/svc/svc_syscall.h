#ifndef SVC_SYSCALL_H
#define SVC_SYSCALL_H

#include "../types.h"
#include "include/proc.h"

/* 服务/能力组 legacy 系统调用（int 0x81）：pipe/dup2/cap_drop/cap_send/input_set */
bool svc_syscall_owns(uint32_t nr);
int  svc_syscall(interrupt_frame_t *f);

#endif
