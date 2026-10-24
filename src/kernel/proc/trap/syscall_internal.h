#ifndef PROC_TRAP_SYSCALL_INTERNAL_H
#define PROC_TRAP_SYSCALL_INTERNAL_H

/* syscall 层内部共享辅助（实现在 syscall_util.c）。
 * 这些函数只供 trap/syscall*.c 与后续按模块拆分的 *_syscall.c 使用。 */

#include "../../types.h"
#include "interrupt.h"
#include "../proc.h"

#define USER_ADDR_LIMIT 0xC0000000U
#define USER_LEN_LIMIT  (64U * 1024U)

bool        user_range_ok(uint32_t ptr, uint32_t len);
int         copy_user_string(char *dst, uint32_t dst_size, uint32_t uptr);
int         path_has_parent(const char *p);
void        make_abs_path(char *out, uint32_t out_max, const char *in);
proc_fd_t  *fd_get(proc_t *p, int fd);
int         fd_alloc_kind(proc_t *p, uint8_t kind, uint32_t handle, uint32_t rights);
int         fd_alloc(proc_t *p, uint32_t handle, uint32_t rights);
uint32_t    open_rights(uint32_t flags);
int         sys_getchar_block(void);

#endif
