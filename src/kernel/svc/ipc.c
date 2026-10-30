/* =============================================================================
 * svc/ipc.c - 消息端口 + capability 授予（ABI 1.5，SYS_PORT_* / SYS_CAP_GRANT）
 *
 * 设计基线：docs/design/three-layer.md §3。实现要点：
 *   - 消息头固定 16B（magic/type/flags/ncaps/len），payload ≤4096，
 *     cap 数组紧随 payload（每项 8B）；SEND 的 len 是“整帧”字节数，
 *     必须与头部自洽。
 *   - 每个端口 16 条队列；满则 SEND 有界阻塞（-EAGAIN 超时），RECV 空则
 *     有界阻塞。proc_block() 无超时参数且 proc 目录不在可改范围，所以用
 *     1 tick 粒度的 sleep 轮询（10ms 唤醒延迟）。
 *   - 端口句柄带 generation；ticket = (pid<<16)|handle，REPLY 校验两者。
 *   - cap 只能由持有者授予：PORT 看端口归属、FILE/SOCKET 看 fd、
 *     MMIO/IRQ/DMA 仅 root（CAP_SYS_ADMIN 尚未落地，见 claims 设计问题）。
 * ============================================================================= */
#include "ipc.h"
#include "include/kernel.h"
#include "include/proc.h"

typedef struct {
    bool     used;
    uint32_t len;               /* 整帧长度（含头/payload/caps） */
    uint32_t sender_pid;
    uint32_t sender_uid;
    uint8_t  data[IPC_MSG_MAX];
} ipc_slot_t;

typedef struct {
    bool     used;
    uint32_t type;
    uint32_t id;
    uint32_t rights;
    uint32_t granter_uid;
} ipc_grant_t;

typedef struct {
    bool     used;
    uint16_t generation;
    uint32_t flags;
    uint32_t owner_pid;
    uint32_t owner_uid;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    ipc_slot_t q[IPC_QUEUE_DEPTH];
    ipc_grant_t grants[IPC_GRANT_MAX];
} ipc_port_t;

static ipc_port_t ipc_ports[IPC_PORT_MAX];
static spinlock_t ipc_lock = SPINLOCK_INIT;

/* ---------------- 小端字段读写（消息来自用户态，不能用对齐结构体直接读） ---- */

static void ipc_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static void ipc_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint16_t ipc_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ipc_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------- 句柄 / 权限 ---------------- */

static int ipc_resolve_locked(uint32_t handle, uint32_t *slot)
{
    uint32_t s = handle & IPC_SLOT_MASK;
    uint32_t gen;

    if (handle == 0U || s >= IPC_PORT_MAX) return -1;
    gen = handle >> IPC_GEN_SHIFT;
    if (gen == 0U || !ipc_ports[s].used) return -1;
    if ((uint32_t)ipc_ports[s].generation != gen) return -1;
    *slot = s;
    return 0;
}

static bool ipc_can_send(const ipc_port_t *pt, const proc_t *c)
{
    if (c == 0) return false;
    if (proc_is_root(c)) return true;
    if ((pt->flags & IPC_FLAG_PRIVATE) != 0U) {
        return pt->owner_uid == proc_euid_of(c);
    }
    return true;            /* 公共端口：任何进程可发（设计 §3.2） */
}

static bool ipc_can_recv(const ipc_port_t *pt, const proc_t *c)
{
    if (c == 0) return false;
    if (proc_is_root(c)) return true;
    return pt->owner_uid == proc_euid_of(c);
}

/* ---------------- 消息校验（设计 §3.1） ---------------- */

static int ipc_validate(const void *msg, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)msg;
    uint32_t magic, plen;
    uint16_t flags, ncaps;

    if (msg == 0) return -1;
    if (len < IPC_HDR_SIZE || len > IPC_MSG_MAX) return -1;

    magic = ipc_get32(p);
    flags = ipc_get16(p + 8);
    ncaps = ipc_get16(p + 10);
    plen  = ipc_get32(p + 12);

    if (magic != IPC_MAGIC) return -1;
    if ((flags & ~(uint16_t)IPC_F_ALL) != 0U) return -1;
    if ((flags & IPC_F_NEED_REPLY) != 0U && (flags & IPC_F_NO_REPLY) != 0U) {
        return -1;          /* 互斥 */
    }
    if (ncaps > IPC_NCAPS_MAX) return -1;
    if (plen > IPC_PAYLOAD_MAX) return -1;
    if (IPC_HDR_SIZE + plen + (uint32_t)ncaps * IPC_CAP_SIZE != len) {
        return -1;
    }

    /* cap 数组边界/类型/id 合法性（P1 只校验，不安装给接收方） */
    for (uint32_t i = 0; i < (uint32_t)ncaps; i++) {
        const uint8_t *e = p + IPC_HDR_SIZE + plen + i * IPC_CAP_SIZE;
        uint32_t type = ipc_get32(e);
        uint32_t id = ipc_get32(e + 4);

        if (type < IPC_CAP_PORT || type > IPC_CAP_SOCKET) return -1;
        if ((type == IPC_CAP_MMIO || type == IPC_CAP_DMA) && id == 0U) {
            return -1;
        }
        if (type == IPC_CAP_IRQ && id >= 256U) return -1;
    }
    return 0;
}

/* 构造一帧（自检用；用户态在 ulib 里自己组帧） */
static uint32_t ipc_build(uint8_t *out, const void *payload, uint32_t plen,
                          uint32_t type, uint16_t flags)
{
    ipc_put32(out, IPC_MAGIC);
    ipc_put32(out + 4, type);
    ipc_put16(out + 8, flags);
    ipc_put16(out + 10, 0U);
    ipc_put32(out + 12, plen);
    if (plen != 0U && payload != 0) memcpy(out + IPC_HDR_SIZE, payload, plen);
    return IPC_HDR_SIZE + plen;
}

/* ---------------- 端口生命周期 ---------------- */

/* 清空一个槽位但保留 generation（否则复用后旧句柄又“活”了）。持锁调用。 */
static void ipc_port_clear_locked(ipc_port_t *pt)
{
    uint16_t gen = pt->generation;

    memset(pt, 0, sizeof(*pt));
    pt->generation = gen;
}

/* 表满时回收 owner 已消失的端口；调用方持锁。 */
static uint32_t ipc_reap_locked(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < IPC_PORT_MAX; i++) {
        if (!ipc_ports[i].used) continue;
        if (proc_find(ipc_ports[i].owner_pid) != 0) continue;
        ipc_port_clear_locked(&ipc_ports[i]);
        n++;
    }
    return n;
}

int ipc_port_create(uint32_t flags, struct process *caller)
{
    uint32_t f;
    uint32_t slot = IPC_PORT_MAX;
    ipc_port_t *pt;
    uint32_t handle;

    if (caller == 0) return -1;             /* 必须有进程上下文 */
    if ((flags & ~(uint32_t)IPC_FLAG_PRIVATE) != 0U) return -1;

    f = spin_lock_irqsave(&ipc_lock);
    for (uint32_t i = 0; i < IPC_PORT_MAX; i++) {
        if (!ipc_ports[i].used) { slot = i; break; }
    }
    if (slot == IPC_PORT_MAX && ipc_reap_locked() != 0U) {
        for (uint32_t i = 0; i < IPC_PORT_MAX; i++) {
            if (!ipc_ports[i].used) { slot = i; break; }
        }
    }
    if (slot == IPC_PORT_MAX) {
        spin_unlock_irqrestore(&ipc_lock, f);
        return -1;
    }

    pt = &ipc_ports[slot];
    {
        uint16_t gen = (uint16_t)(pt->generation + 1U);  /* 复用后旧句柄失效 */

        if (gen == 0U) gen = 1U;
        memset(pt, 0, sizeof(*pt));
        pt->generation = gen;
    }
    pt->used = true;
    pt->flags = flags;
    pt->owner_pid = caller->pid;
    pt->owner_uid = proc_euid_of(caller);
    handle = ((uint32_t)pt->generation << IPC_GEN_SHIFT) |
             (slot & IPC_SLOT_MASK);
    spin_unlock_irqrestore(&ipc_lock, f);
    return (int)handle;
}

void ipc_port_release(uint32_t handle, struct process *caller)
{
    uint32_t f, slot;

    if (caller == 0) return;
    f = spin_lock_irqsave(&ipc_lock);
    if (ipc_resolve_locked(handle, &slot) == 0) {
        ipc_port_t *pt = &ipc_ports[slot];

        if (proc_is_root(caller) || pt->owner_pid == caller->pid ||
            pt->owner_uid == proc_euid_of(caller)) {
            ipc_port_clear_locked(pt);
        }
    }
    spin_unlock_irqrestore(&ipc_lock, f);
}

bool ipc_port_exists(uint32_t handle)
{
    uint32_t f, slot;
    bool ok;

    f = spin_lock_irqsave(&ipc_lock);
    ok = (ipc_resolve_locked(handle, &slot) == 0);
    spin_unlock_irqrestore(&ipc_lock, f);
    return ok;
}

/* ---------------- 入队 / 出队 ---------------- */

/* 调用方持锁；成功返回 0，队列满返回 -IPC_EAGAIN。 */
static int ipc_enqueue_locked(ipc_port_t *pt, const void *msg, uint32_t len,
                              const proc_t *caller)
{
    ipc_slot_t *s;

    if (pt->count >= IPC_QUEUE_DEPTH) return -IPC_EAGAIN;
    s = &pt->q[pt->tail];
    memcpy(s->data, msg, len);
    s->len = len;
    s->sender_pid = caller->pid;
    s->sender_uid = proc_euid_of(caller);
    pt->tail = (pt->tail + 1U) % IPC_QUEUE_DEPTH;
    pt->count++;
    return 0;
}

int ipc_port_send(uint32_t handle, const void *msg, uint32_t len,
                  struct process *caller)
{
    uint32_t deadline;

    if (caller == 0) return -1;
    if (ipc_validate(msg, len) != 0) return -1;

    deadline = timer_ticks() + IPC_SEND_TIMEOUT_TICKS;
    for (;;) {
        uint32_t f = spin_lock_irqsave(&ipc_lock);
        uint32_t slot;
        int r;

        if (ipc_resolve_locked(handle, &slot) != 0) {
            spin_unlock_irqrestore(&ipc_lock, f);
            return -1;
        }
        if (!ipc_can_send(&ipc_ports[slot], caller)) {
            spin_unlock_irqrestore(&ipc_lock, f);
            return -1;
        }
        r = ipc_enqueue_locked(&ipc_ports[slot], msg, len, caller);
        spin_unlock_irqrestore(&ipc_lock, f);
        if (r == 0) return 0;

        if (caller->pending_sig != 0U) return -IPC_EINTR;
        if (proc_current() == 0) return -IPC_EAGAIN;   /* 无调度上下文 */
        if ((int32_t)(timer_ticks() - deadline) >= 0) return -IPC_EAGAIN;
        proc_sleep_ticks(1U);
    }
}

int ipc_port_recv(uint32_t handle, void *buf, uint32_t maxlen,
                  struct process *caller)
{
    uint32_t deadline;

    if (caller == 0 || buf == 0 || maxlen == 0U) return -1;

    deadline = timer_ticks() + IPC_RECV_TIMEOUT_TICKS;
    for (;;) {
        uint32_t f = spin_lock_irqsave(&ipc_lock);
        uint32_t slot;

        if (ipc_resolve_locked(handle, &slot) != 0) {
            spin_unlock_irqrestore(&ipc_lock, f);
            return -1;
        }
        {
            ipc_port_t *pt = &ipc_ports[slot];
            ipc_slot_t *s;
            uint32_t n;

            if (!ipc_can_recv(pt, caller)) {
                spin_unlock_irqrestore(&ipc_lock, f);
                return -1;
            }
            if (pt->count != 0U) {
                s = &pt->q[pt->head];
                n = s->len;
                memcpy(buf, s->data, (n > maxlen) ? maxlen : n);
                pt->head = (pt->head + 1U) % IPC_QUEUE_DEPTH;
                pt->count--;
                spin_unlock_irqrestore(&ipc_lock, f);
                return (int)n;          /* 真实长度；缓冲不足时按 maxlen 截断 */
            }
        }
        spin_unlock_irqrestore(&ipc_lock, f);

        if (caller->pending_sig != 0U) return -IPC_EINTR;
        if (proc_current() == 0) return -IPC_EAGAIN;
        if ((int32_t)(timer_ticks() - deadline) >= 0) return -IPC_EAGAIN;
        proc_sleep_ticks(1U);
    }
}

int ipc_port_reply(uint32_t ticket, const void *msg, uint32_t len,
                   struct process *caller)
{
    uint32_t pid = ticket >> 16;
    uint32_t handle = ticket & 0xFFFFU;
    uint32_t f, slot;

    if (caller == 0) return -1;
    if (ipc_validate(msg, len) != 0) return -1;

    f = spin_lock_irqsave(&ipc_lock);
    if (ipc_resolve_locked(handle, &slot) != 0) {
        spin_unlock_irqrestore(&ipc_lock, f);
        return -1;
    }
    /* ticket 必须指向“那个 pid 的端口”；端口复用由 generation 挡住 */
    if (ipc_ports[slot].owner_pid != pid ||
        !ipc_can_send(&ipc_ports[slot], caller)) {
        spin_unlock_irqrestore(&ipc_lock, f);
        return -1;
    }
    spin_unlock_irqrestore(&ipc_lock, f);

    /* 回复同样受满队列阻塞/超时约束 */
    return ipc_port_send(handle, msg, len, caller);
}

/* ---------------- capability 授予（设计 §3.3） ---------------- */

static int ipc_grant_store_locked(ipc_port_t *pt, uint32_t type, uint32_t id,
                                  uint32_t rights, const proc_t *caller)
{
    for (uint32_t i = 0; i < IPC_GRANT_MAX; i++) {
        if (pt->grants[i].used && pt->grants[i].type == type &&
            pt->grants[i].id == id) {
            pt->grants[i].rights = rights;
            pt->grants[i].granter_uid = proc_euid_of(caller);
            return 0;
        }
    }
    for (uint32_t i = 0; i < IPC_GRANT_MAX; i++) {
        if (!pt->grants[i].used) {
            pt->grants[i].used = true;
            pt->grants[i].type = type;
            pt->grants[i].id = id;
            pt->grants[i].rights = rights;
            pt->grants[i].granter_uid = proc_euid_of(caller);
            return 0;
        }
    }
    return -1;                  /* 每端口 grant 表满 */
}

int ipc_cap_grant(uint32_t handle, uint32_t type, uint32_t id, uint32_t rights,
                  struct process *caller)
{
    uint32_t f, slot;
    ipc_port_t *pt;
    int r = -1;

    if (caller == 0) return -1;
    /* 未实现/未知类型一律拒绝（P1 只认 1..6） */
    if (type < IPC_CAP_PORT || type > IPC_CAP_SOCKET) return -1;

    if (rights == 0U) rights = IPC_RIGHTS_DEFAULT;
    rights &= IPC_RIGHTS_ALL;
    if (rights == 0U) return -1;

    f = spin_lock_irqsave(&ipc_lock);
    if (ipc_resolve_locked(handle, &slot) != 0) goto out;
    pt = &ipc_ports[slot];
    if (!ipc_can_recv(pt, caller)) goto out;    /* 只能给可管理的端口授权 */

    /* 只能授予“自己持有”的 cap */
    switch (type) {
    case IPC_CAP_PORT: {
        uint32_t src;
        if (ipc_resolve_locked(id, &src) != 0) goto out;
        if (ipc_ports[src].owner_pid != caller->pid) goto out;
        break;
    }
    case IPC_CAP_FILE: {
        proc_fd_t *fd;
        if (id >= PROC_FD_MAX) goto out;
        fd = &caller->fds[id];
        if (!fd->used || fd->kind != FD_KIND_FILE) goto out;
        rights &= fd->rights;               /* 不能授出自己没有的权限 */
        if (rights == 0U) goto out;
        break;
    }
    case IPC_CAP_SOCKET: {
        proc_fd_t *fd;
        if (id >= PROC_FD_MAX) goto out;
        fd = &caller->fds[id];
        if (!fd->used || fd->kind != FD_KIND_UDP) goto out;
        break;
    }
    case IPC_CAP_MMIO:
        /* root = 持有全部硬件 cap；CAP_SYS_ADMIN 尚未落地（claims 设计问题） */
        if (!proc_is_root(caller) || id == 0U) goto out;
        break;
    case IPC_CAP_IRQ:
        if (!proc_is_root(caller) || id >= 256U) goto out;
        break;
    case IPC_CAP_DMA:
        if (!proc_is_root(caller) || id == 0U) goto out;
        break;
    default:
        goto out;
    }

    r = ipc_grant_store_locked(pt, type, id, rights, caller);
out:
    spin_unlock_irqrestore(&ipc_lock, f);
    return r;
}

bool ipc_cap_granted(uint32_t handle, uint32_t type, uint32_t id)
{
    uint32_t f, slot;
    bool ok = false;

    f = spin_lock_irqsave(&ipc_lock);
    if (ipc_resolve_locked(handle, &slot) == 0) {
        for (uint32_t i = 0; i < IPC_GRANT_MAX; i++) {
            const ipc_grant_t *g = &ipc_ports[slot].grants[i];
            if (g->used && g->type == type && g->id == id) { ok = true; break; }
        }
    }
    spin_unlock_irqrestore(&ipc_lock, f);
    return ok;
}

/* ---------------- 内核自检（-Test 构建，supervisor 线程里跑一次） -------------- */

static int ipc_expect(bool cond, const char *step, int ok)
{
    if (!cond) {
        kprintf("ipc: selftest FAIL (%s)\n", step);
        return 0;
    }
    return ok;
}

void ipc_selftest(void)
{
    proc_t *p = proc_current();
    int srv = -1, cli = -1;
    int ok = 1;
    uint8_t frame[IPC_MSG_MAX];
    uint8_t reply[64];
    uint8_t rbuf[IPC_MSG_MAX];
    uint32_t len, ticket, plen;
    int r;

    if (p == 0) {
        kprintln("ipc: selftest SKIP (no process context)");
        return;
    }

    /* 1) 建端口 -> 发 -> 收 */
    srv = ipc_port_create(0U, p);
    cli = ipc_port_create(0U, p);
    ok = ipc_expect(srv > 0 && cli > 0, "create", ok);

    ticket = ipc_ticket_make(p->pid, (uint32_t)cli);
    plen = 4U;
    ipc_put32(frame + IPC_HDR_SIZE, ticket);
    len = ipc_build(frame, frame + IPC_HDR_SIZE, plen, 1U,
                    (uint16_t)IPC_F_NEED_REPLY);
    r = ipc_port_send((uint32_t)srv, frame, len, p);
    ok = ipc_expect(r == 0, "send", ok);
    r = ipc_port_recv((uint32_t)srv, rbuf, sizeof(rbuf), p);
    ok = ipc_expect(r == (int)len, "recv len", ok);
    ok = ipc_expect(ipc_get32(rbuf) == IPC_MAGIC, "recv magic", ok);
    ok = ipc_expect(ipc_get32(rbuf + IPC_HDR_SIZE) == ticket, "recv ticket", ok);
    if (ok) kprintf("ipc: port send/recv ok (port=%d len=%u)\n", srv, (unsigned)len);

    /* 2) 回复：服务用刚收到的 ticket 回 cli */
    len = ipc_build(reply, "pong", 4U, 2U, 0U);
    r = ipc_port_reply(ticket, reply, len, p);
    ok = ipc_expect(r == 0, "reply send", ok);
    r = ipc_port_recv((uint32_t)cli, rbuf, sizeof(rbuf), p);
    ok = ipc_expect(r == (int)len && memcmp(rbuf + IPC_HDR_SIZE, "pong", 4U) == 0,
                    "reply recv", ok);
    if (ok) kprintf("ipc: reply ok (ticket=0x%x)\n", (unsigned)ticket);

    /* 3) 队列满：16 条必须都成功，第 17 条有界阻塞后 -EAGAIN */
    len = ipc_build(frame, "q", 1U, 3U, 0U);
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; i++) {
        r = ipc_port_send((uint32_t)srv, frame, len, p);
        ok = ipc_expect(r == 0, "fill", ok);
    }
    {
        uint32_t t0 = timer_ticks();

        r = ipc_port_send((uint32_t)srv, frame, len, p);
        ok = ipc_expect(r == -IPC_EAGAIN, "queue full", ok);
        ok = ipc_expect((timer_ticks() - t0) >= IPC_SEND_TIMEOUT_TICKS - 1U,
                        "send timeout ticks", ok);
        if (ok) {
            kprintf("ipc: queue full ok (depth=%u)\n",
                    (unsigned)IPC_QUEUE_DEPTH);
            kprintf("ipc: timeout ok (send %u ticks, recv %u ticks)\n",
                    (unsigned)IPC_SEND_TIMEOUT_TICKS,
                    (unsigned)IPC_RECV_TIMEOUT_TICKS);
        }
    }
    /* 队列里的 16 条要原样收回来（不丢消息） */
    for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; i++) {
        r = ipc_port_recv((uint32_t)srv, rbuf, sizeof(rbuf), p);
        ok = ipc_expect(r == (int)len, "drain", ok);
    }

    /* RECV 空端口也要有界超时 */
    {
        uint32_t t0 = timer_ticks();

        r = ipc_port_recv((uint32_t)cli, rbuf, sizeof(rbuf), p);
        ok = ipc_expect(r == -IPC_EAGAIN, "recv timeout", ok);
        ok = ipc_expect((timer_ticks() - t0) >= IPC_RECV_TIMEOUT_TICKS - 1U,
                        "recv timeout ticks", ok);
    }

    /* 4) 坏消息：magic/超长 payload/ncaps 超限/长度不自洽/保留 flag */
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, IPC_HDR_SIZE - 1U, p) == -1,
                    "short frame", ok);
    len = ipc_build(frame, "x", 1U, 4U, 0U);
    ipc_put32(frame, 0xDEADBEEFU);
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, len, p) == -1,
                    "bad magic", ok);
    ipc_put32(frame, IPC_MAGIC);
    ipc_put32(frame + 12, IPC_PAYLOAD_MAX + 1U);
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, len, p) == -1,
                    "oversize payload", ok);
    ipc_put32(frame + 12, 1U);
    ipc_put16(frame + 10, IPC_NCAPS_MAX + 1U);
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, len, p) == -1,
                    "ncaps > 4", ok);
    ipc_put16(frame + 10, 0U);
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, len + 1U, p) == -1,
                    "len mismatch", ok);
    ipc_put16(frame + 8, 0x80U);
    ok = ipc_expect(ipc_port_send((uint32_t)srv, frame, len, p) == -1,
                    "reserved flag", ok);
    ipc_put16(frame + 8, 0U);
    if (ok) kprintln("ipc: bad msg rejected");

    /* 5) -EINTR：未屏蔽信号打断 SEND / RECV（直接置 pending_sig 模拟投递） */
    {
        uint32_t saved = p->pending_sig;

        p->pending_sig = (uint32_t)SIGINT;
        r = ipc_port_recv((uint32_t)cli, rbuf, sizeof(rbuf), p);
        ok = ipc_expect(r == -IPC_EINTR, "recv EINTR", ok);
        p->pending_sig = saved;
        if (ok) kprintln("ipc: recv EINTR ok");

        len = ipc_build(frame, "e", 1U, 5U, 0U);
        for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; i++) {
            (void)ipc_port_send((uint32_t)srv, frame, len, p);
        }
        p->pending_sig = (uint32_t)SIGINT;
        r = ipc_port_send((uint32_t)srv, frame, len, p);
        ok = ipc_expect(r == -IPC_EINTR, "send EINTR", ok);
        p->pending_sig = saved;
        for (uint32_t i = 0; i < IPC_QUEUE_DEPTH; i++) {
            (void)ipc_port_recv((uint32_t)srv, rbuf, sizeof(rbuf), p);
        }
        if (ok) kprintln("ipc: send EINTR ok");
    }

    /* 6) cap 授予：root 可授 PORT/MMIO/IRQ/DMA，普通用户被拒 */
    {
        uint32_t saved = p->euid;
        uint32_t base = 0xFEBC0000U;

        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_PORT, (uint32_t)cli, 0U, p);
        ok = ipc_expect(r == 0, "grant PORT", ok);
        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_MMIO, base, 0U, p);
        ok = ipc_expect(r == 0, "grant MMIO", ok);
        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_IRQ, 11U, 0U, p);
        ok = ipc_expect(r == 0, "grant IRQ", ok);
        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_DMA, 65536U, 0U, p);
        ok = ipc_expect(r == 0, "grant DMA", ok);
        ok = ipc_expect(ipc_cap_granted((uint32_t)srv, IPC_CAP_MMIO, base),
                        "grant recorded", ok);
        /* 不持有的 cap：fd 0 是控制台，不是文件/套接字 */
        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_FILE, 0U, 0U, p);
        ok = ipc_expect(r == -1, "grant FILE unheld", ok);
        r = ipc_cap_grant((uint32_t)srv, IPC_CAP_SOCKET, 0U, 0U, p);
        ok = ipc_expect(r == -1, "grant SOCKET unheld", ok);
        if (ok) kprintln("ipc: cap grant ok (PORT/MMIO/IRQ/DMA)");

        p->euid = 1000U;
        /* 非 root 也要先拥有端口，才能走到“特权类型”判定；用 euid=1000
         * 新建的临时端口（owner_uid=1000）验证 MMIO/IRQ/DMA 被拒。 */
        {
            int tmp = ipc_port_create(0U, p);

            ok = ipc_expect(tmp > 0, "non-root port", ok);
            ok = ipc_expect(ipc_cap_grant((uint32_t)tmp, IPC_CAP_MMIO, base,
                                          0U, p) == -1, "non-root MMIO", ok);
            ok = ipc_expect(ipc_cap_grant((uint32_t)tmp, IPC_CAP_IRQ, 11U,
                                          0U, p) == -1, "non-root IRQ", ok);
            ok = ipc_expect(ipc_cap_grant((uint32_t)tmp, IPC_CAP_DMA, 65536U,
                                          0U, p) == -1, "non-root DMA", ok);
            p->euid = saved;
            if (tmp > 0) ipc_port_release((uint32_t)tmp, p);
        }
        if (ok) kprintln("ipc: cap grant denied uid=1000 (MMIO/IRQ/DMA)");

        /* 未实现类型 */
        ok = ipc_expect(ipc_cap_grant((uint32_t)srv, 7U, 1U, 0U, p) == -1,
                        "cap type 7", ok);
        ok = ipc_expect(ipc_cap_grant((uint32_t)srv, 0U, 1U, 0U, p) == -1,
                        "cap type 0", ok);
        if (ok) kprintln("ipc: cap type unsupported rejected");
    }

    /* 7) 端口句柄 generation：释放后旧句柄必须失效 */
    {
        uint32_t old = (uint32_t)cli;

        ipc_port_release(old, p);
        ok = ipc_expect(!ipc_port_exists(old), "stale handle", ok);
        cli = ipc_port_create(0U, p);
        ok = ipc_expect(cli > 0 && (uint32_t)cli != old, "generation bump", ok);
        if (ok) kprintln("ipc: handle generation ok");
    }

    if (srv > 0) ipc_port_release((uint32_t)srv, p);
    if (cli > 0) ipc_port_release((uint32_t)cli, p);

    if (ok) kprintln("ipc: selftest PASS");
}
