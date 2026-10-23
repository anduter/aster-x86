/* =============================================================================
 * ipc/pipe.c - 无名管道
 *
 * 8 个管道，每个 1KB 环形缓冲。写端用引用计数判断 EOF：
 * 所有持有写端的 fd（fork / dup2 会复制）都关闭后，读端读空返回 0。
 * ============================================================================= */
#include "pipe.h"
#include "../proc/proc.h"
#include "../lib/string.h"

#define PIPE_MAX   8
#define PIPE_SIZE  1024U

typedef struct {
    bool     used;
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, tail, count;
    uint32_t writers;
} pipe_t;

static pipe_t pipes[PIPE_MAX];

int pipe_create(void)
{
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = true;
            pipes[i].writers = 1U;      /* 创建时算一个写端 */
            return i;
        }
    }
    return -1;
}

int pipe_read(int idx, char *dst, uint32_t len)
{
    pipe_t *pp = &pipes[idx];
    uint32_t n = 0;

    while (n < len) {
        if (pp->count == 0U) {
            if (pp->writers == 0U) break;   /* 所有写端都关了 -> EOF */
            proc_block(PROC_WAIT_PIPE);
            continue;
        }
        dst[n++] = (char)pp->buf[pp->tail];
        pp->tail = (pp->tail + 1U) % PIPE_SIZE;
        pp->count--;
    }
    proc_wakeup(PROC_WAIT_PIPE);
    return (int)n;
}

int pipe_write(int idx, const char *src, uint32_t len)
{
    pipe_t *pp = &pipes[idx];
    uint32_t n = 0;

    while (n < len) {
        if (pp->count == PIPE_SIZE) {
            proc_block(PROC_WAIT_PIPE);
            continue;
        }
        pp->buf[pp->head] = (uint8_t)src[n++];
        pp->head = (pp->head + 1U) % PIPE_SIZE;
        pp->count++;
    }
    proc_wakeup(PROC_WAIT_PIPE);
    return (int)n;
}

void pipe_ref_write(int idx)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipes[idx].used) return;
    pipes[idx].writers++;
}

void pipe_unref_write(int idx)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipes[idx].used) return;
    if (pipes[idx].writers > 0U) pipes[idx].writers--;
    proc_wakeup(PROC_WAIT_PIPE);
}

void pipe_unref_read(int idx)
{
    if (idx < 0 || idx >= PIPE_MAX) return;
    if (!pipes[idx].used) return;
    if (pipes[idx].writers == 0U && pipes[idx].count == 0U) {
        pipes[idx].used = false;
    }
    proc_wakeup(PROC_WAIT_PIPE);
}

void pipe_fork_inherit(struct process *child)
{
    if (child == 0) return;

    for (int i = 0; i < PROC_FD_MAX; i++) {
        if (!child->fds[i].used) continue;
        if (child->fds[i].kind == FD_KIND_PIPE_W) {
            pipe_ref_write((int)child->fds[i].handle);
        }
    }
}
