#ifndef FS_SYSCALL_H
#define FS_SYSCALL_H

#include "../include/types.h"
#include "include/proc.h"

/* 文件系统组 legacy 系统调用（int 0x81）：open/read/write/close/list/drives/
 * mkdir/unlink/rename/stat/chdir/chroot/getcwd/sync/time/fsck/chmod/chown */
bool fs_syscall_owns(uint32_t nr);
int  fs_syscall(interrupt_frame_t *f);

/* SYS_CHROOT 用：把绝对路径复制进 proc_t.cwd（192B）。
 * 超长返回 E_INVAL 且不写 cwd；供 fs 审计自检直接验证。 */
int  fs_chroot_set_cwd(proc_t *p, const char *path);

#endif
