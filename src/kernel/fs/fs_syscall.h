#ifndef FS_SYSCALL_H
#define FS_SYSCALL_H

#include "../types.h"
#include "include/proc.h"

/* 文件系统组 legacy 系统调用（int 0x81）：open/read/write/close/list/drives/
 * mkdir/unlink/stat/chdir/chroot/getcwd/sync/time/fsck/chmod/chown */
bool fs_syscall_owns(uint32_t nr);
int  fs_syscall(interrupt_frame_t *f);

#endif
