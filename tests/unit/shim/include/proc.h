/* =============================================================================
 * tests/unit/shim/include/proc.h - host 单元测试用最小 proc 接口
 *
 * pipe.c 只需要 fd 种类常量、proc_fd_t/process 的最小布局，以及
 * proc_block/proc_wakeup 的声明；阻塞原语在测试里退化为 no-op。
 * ========================================================================== */
#ifndef PROC_H_MODULE_H
#define PROC_H_MODULE_H

#include "../../../../src/kernel/include/types.h"

#define PROC_FD_MAX     16
#define FD_KIND_FILE    0
#define FD_KIND_PIPE_W  3
#define PROC_WAIT_PIPE  0x50495045U   /* "PIPE" */

typedef struct {
    bool     used;
    uint8_t  kind;
    uint8_t  fs_be;
    uint32_t handle;
    uint32_t rights;
} proc_fd_t;

struct process {
    proc_fd_t fds[PROC_FD_MAX];
};

void proc_block(unsigned int chan);
void proc_wakeup(unsigned int chan);

#endif /* PROC_H_MODULE_H */
