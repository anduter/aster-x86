#ifndef IPC_PIPE_H
#define IPC_PIPE_H

#include "../include/types.h"

/* 无名管道：固定大小的环形缓冲，读写端通过 fd 共享。
 * 写端有引用计数（fork/dup2 会增加），全部关闭后读端看到 EOF。 */
int  pipe_create(void);

int  pipe_read(int idx, char *dst, uint32_t len);
int  pipe_write(int idx, const char *src, uint32_t len);

void pipe_ref_write(int idx);
void pipe_unref_write(int idx);
void pipe_unref_read(int idx);

struct process;
/* fork 时子进程继承了写端：给每个写 fd 增加引用 */
void pipe_fork_inherit(struct process *child);

#endif
