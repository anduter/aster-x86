/* =============================================================================
 * ipc/pipe.c - 无名管道
 *
 * 8 个管道，每个 1KB 环形缓冲。写端用引用计数判断 EOF：
 * 所有持有写端的 fd（fork / dup2 会复制）都关闭后，读端读空返回 0。
 * ============================================================================= */
#include "pipe.h"
#include "include/proc.h"
#include "include/kernel.h"
#include "include/kernel.h"

#define PIPE_MAX   8
#define PIPE_SIZE  1024U

typedef struct {
    bool     used;
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, tail, count;
    uint32_t writers;
    uint32_t readers;       /* 读端引用计数（fork/dup2 会增加） */
} pipe_t;

static pipe_t pipes[PIPE_MAX];
/* SMP：管道缓冲是全局共享的，读/写/引用计数都要串行化。
 * 阻塞前必须放锁——proc_block 会切到别的线程，它可能也要用同一根管道。 */
static spinlock_t pipe_lock = SPINLOCK_INIT;

int pipe_create(void)
{
    uint32_t flags;

    flags = spin_lock_irqsave(&pipe_lock);
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = true;
            pipes[i].writers = 1U;      /* 创建时算一个写端 */
            pipes[i].readers = 1U;      /* 创建时算一个读端 */
            spin_unlock_irqrestore(&pipe_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&pipe_lock, flags);
    return -1;
}

int pipe_read(int idx, char *dst, uint32_t len)
{
    pipe_t *pp;
    uint32_t n = 0;

    if (idx < 0 || idx >= PIPE_MAX) return -1;
    pp = &pipes[idx];

    for (;;) {
        uint32_t flags = spin_lock_irqsave(&pipe_lock);

        /* 槽位可能已被回收并复用给别的管道：继续按 idx 读写会串到别人的数据 */
        if (!pp->used) {
            spin_unlock_irqrestore(&pipe_lock, flags);
            return -1;
        }

        while (n < len && pp->count > 0U) {
            dst[n++] = (char)pp->buf[pp->tail];
            pp->tail = (pp->tail + 1U) % PIPE_SIZE;
            pp->count--;
        }
        /* 读够了，或者所有写端都关了 -> EOF */
        if (n >= len || pp->writers == 0U) {
            spin_unlock_irqrestore(&pipe_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&pipe_lock, flags);
        /* 未屏蔽信号：已读到的部分照常返回，没读到则 -EINTR，不吞信号 */
        if (proc_signal_pending()) return (n > 0U) ? (int)n : -EINTR;
        proc_block(PROC_WAIT_PIPE);
    }

    proc_wakeup(PROC_WAIT_PIPE);
    return (int)n;
}

int pipe_write(int idx, const char *src, uint32_t len)
{
    pipe_t *pp;
    uint32_t n = 0;

    if (idx < 0 || idx >= PIPE_MAX) return -1;
    pp = &pipes[idx];

    for (;;) {
        uint32_t flags = spin_lock_irqsave(&pipe_lock);

        /* 同 pipe_read：槽位被回收后不能再往里写 */
        if (!pp->used) {
            spin_unlock_irqrestore(&pipe_lock, flags);
            return -1;
        }

        while (n < len && pp->count < PIPE_SIZE) {
            pp->buf[pp->head] = (uint8_t)src[n++];
            pp->head = (pp->head + 1U) % PIPE_SIZE;
            pp->count++;
        }
        if (n >= len) {
            spin_unlock_irqrestore(&pipe_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&pipe_lock, flags);
        /* 管道满 + 有未屏蔽信号：返回已写入字节数，一个都没写则 -EINTR */
        if (proc_signal_pending()) return (n > 0U) ? (int)n : -EINTR;
        proc_block(PROC_WAIT_PIPE);
    }

    proc_wakeup(PROC_WAIT_PIPE);
    return (int)n;
}

void pipe_ref_write(int idx)
{
    uint32_t flags;

    if (idx < 0 || idx >= PIPE_MAX) return;
    flags = spin_lock_irqsave(&pipe_lock);
    if (pipes[idx].used) pipes[idx].writers++;
    spin_unlock_irqrestore(&pipe_lock, flags);
}

/* 读写端引用都归零才释放槽位。管道里残留的未读数据此时已没有合法读者，
 * 可以一并丢弃；否则“先关读端、后关写端”会让槽位永久泄漏（8 个槽位用完后
 * 所有 pipe() 都失败）。调用方必须已持有 pipe_lock。 */
static void pipe_maybe_free_locked(int idx)
{
    if (pipes[idx].used &&
        pipes[idx].readers == 0U && pipes[idx].writers == 0U) {
        pipes[idx].used = false;
    }
}

void pipe_unref_write(int idx)
{
    uint32_t flags;

    if (idx < 0 || idx >= PIPE_MAX) return;
    flags = spin_lock_irqsave(&pipe_lock);
    if (pipes[idx].used && pipes[idx].writers > 0U) {
        pipes[idx].writers--;
        pipe_maybe_free_locked(idx);
    }
    spin_unlock_irqrestore(&pipe_lock, flags);
    proc_wakeup(PROC_WAIT_PIPE);
}

/* fork/dup2 复制读端时增加引用：否则任一副本 close 时 readers 提前归零，
 * 槽位被释放（甚至被新 pipe 复用），另一份 fd 读到 -1 或别的管道的数据。 */
void pipe_ref_read(int idx)
{
    uint32_t flags;

    if (idx < 0 || idx >= PIPE_MAX) return;
    flags = spin_lock_irqsave(&pipe_lock);
    if (pipes[idx].used && pipes[idx].readers < 0xFFFFFFFFU) {
        pipes[idx].readers++;
    }
    spin_unlock_irqrestore(&pipe_lock, flags);
}

void pipe_unref_read(int idx)
{
    uint32_t flags;

    if (idx < 0 || idx >= PIPE_MAX) return;
    flags = spin_lock_irqsave(&pipe_lock);
    if (pipes[idx].used && pipes[idx].readers > 0U) {
        pipes[idx].readers--;
        pipe_maybe_free_locked(idx);
    }
    spin_unlock_irqrestore(&pipe_lock, flags);
    proc_wakeup(PROC_WAIT_PIPE);
}

void pipe_fork_inherit(struct process *child)
{
    if (child == 0) return;

    for (int i = 0; i < PROC_FD_MAX; i++) {
        if (!child->fds[i].used) continue;
        if (child->fds[i].kind == FD_KIND_PIPE_W) {
            pipe_ref_write((int)child->fds[i].handle);
        } else if (child->fds[i].kind == FD_KIND_PIPE_R) {
            pipe_ref_read((int)child->fds[i].handle);
        }
    }
}
