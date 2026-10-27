#ifndef PROC_H_MODULE_H
#define PROC_H_MODULE_H

/* Module interface header (M1). Cross-module code includes this, not internals. */

#include "proc/proc.h"
#include "proc/smp.h"
#include "proc/percpu.h"
#include "proc/gdt.h"
#include "proc/trap/fault.h"
#include "proc/trap/interrupt.h"
#include "proc/trap/native.h"
#include "proc/trap/syscall.h"
#include "proc/trap/uaccess.h"

/* VFS namespace side table: fs switches a process's root without touching
 * proc_t layout. Declared here because fs_syscall.c (M4) is a legitimate
 * cross-module caller; the implementation stays private to proc/process.c. */
int proc_root_set(proc_t *p, const char *root);
/* 当前进程的 chroot 根（"system:/jail" 形式；空串 = 未 chroot）。
 * fs 的符号链接解析用它把绝对目标重定位到 chroot 根。 */
const char *proc_root_of(proc_t *p);

#endif
