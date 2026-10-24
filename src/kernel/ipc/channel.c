/* =============================================================================
 * ipc/channel.c - 成对 channel endpoint + 小消息队列
 *
 * 这是服务化 IPC 的最小内核机制：两个 endpoint 互相连接，send 进 peer 队列，
 * recv 从自己队列取。当前没有阻塞等待/ Port / 跨进程传递，先提供非阻塞语义。
 * ============================================================================= */
#include "channel.h"
#include "../lib/string.h"
#include "../core/spinlock.h"
#include "../proc/proc.h"

#define CHAN_WAIT_KEY(id) (0x43480000U | ((id) & 0xFFFFU))

typedef struct {
    bool     used;
    uint32_t refs;
    uint32_t peer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint16_t len[CHAN_MSG_MAX];
    uint8_t  data[CHAN_MSG_MAX][CHAN_MSG_LEN];
} chan_t;

static chan_t channels[CHAN_MAX];
static spinlock_t chan_lock = SPINLOCK_INIT;

int chan_create_pair(uint32_t *a, uint32_t *b)
{
    uint32_t flags;
    uint32_t first = CHAN_MAX;
    uint32_t second = CHAN_MAX;

    if (a == 0 || b == 0) return -1;
    flags = spin_lock_irqsave(&chan_lock);
    for (uint32_t i = 0; i < CHAN_MAX; i++) {
        if (channels[i].used) continue;
        if (first == CHAN_MAX) first = i;
        else { second = i; break; }
    }
    if (second == CHAN_MAX) {
        spin_unlock_irqrestore(&chan_lock, flags);
        return -1;
    }

    memset(&channels[first], 0, sizeof(channels[first]));
    memset(&channels[second], 0, sizeof(channels[second]));
    channels[first].used = true;
    channels[first].refs = 1;
    channels[first].peer = second;
    channels[second].used = true;
    channels[second].refs = 1;
    channels[second].peer = first;
    spin_unlock_irqrestore(&chan_lock, flags);

    *a = first;
    *b = second;
    return 0;
}

int chan_send(uint32_t id, const void *buf, uint32_t len)
{
    uint32_t flags;
    chan_t *dst;
    uint32_t slot;
    uint32_t n;

    if (id >= CHAN_MAX || buf == 0) return -1;
    if (len == 0U) return 0;
    if (len > CHAN_MSG_LEN) len = CHAN_MSG_LEN;

    flags = spin_lock_irqsave(&chan_lock);
    if (!channels[id].used) {
        spin_unlock_irqrestore(&chan_lock, flags);
        return -1;
    }
    dst = &channels[channels[id].peer];
    if (!dst->used || dst->count >= CHAN_MSG_MAX) {
        spin_unlock_irqrestore(&chan_lock, flags);
        return CHAN_E_AGAIN;
    }

    slot = dst->tail;
    n = len;
    memcpy(dst->data[slot], buf, n);
    dst->len[slot] = (uint16_t)n;
    dst->tail = (dst->tail + 1U) % CHAN_MSG_MAX;
    dst->count++;
    spin_unlock_irqrestore(&chan_lock, flags);

    proc_wakeup(CHAN_WAIT_KEY(channels[id].peer));
    return (int)n;
}

int chan_recv(uint32_t id, void *buf, uint32_t max_len)
{
    uint32_t flags;
    chan_t *src;
    uint32_t slot;
    uint32_t n;

    if (id >= CHAN_MAX || buf == 0) return -1;

    flags = spin_lock_irqsave(&chan_lock);
    if (!channels[id].used) {
        spin_unlock_irqrestore(&chan_lock, flags);
        return -1;
    }
    src = &channels[id];
    if (src->count == 0U) {
        spin_unlock_irqrestore(&chan_lock, flags);
        return CHAN_E_AGAIN;
    }

    slot = src->head;
    n = src->len[slot];
    if (n > max_len) n = max_len;
    memcpy(buf, src->data[slot], n);
    src->head = (src->head + 1U) % CHAN_MSG_MAX;
    src->count--;
    spin_unlock_irqrestore(&chan_lock, flags);
    return (int)n;
}

int chan_recv_block(uint32_t id, void *buf, uint32_t max_len)
{
    int n;

    if (id >= CHAN_MAX) return -1;
    for (;;) {
        n = chan_recv(id, buf, max_len);
        if (n != CHAN_E_AGAIN) return n;
        proc_block(CHAN_WAIT_KEY(id));
    }
}

void chan_release(uint32_t id)
{
    uint32_t flags;

    if (id >= CHAN_MAX) return;
    flags = spin_lock_irqsave(&chan_lock);
    if (channels[id].used) {
        if (channels[id].refs > 1U) {
            channels[id].refs--;
        } else {
            channels[id].used = false;
            channels[id].refs = 0;
        }
    }
    spin_unlock_irqrestore(&chan_lock, flags);
}

void chan_retain(uint32_t id)
{
    uint32_t flags;

    if (id >= CHAN_MAX) return;
    flags = spin_lock_irqsave(&chan_lock);
    if (channels[id].used && channels[id].refs < 0xFFFFFFFFU) {
        channels[id].refs++;
    }
    spin_unlock_irqrestore(&chan_lock, flags);
}
