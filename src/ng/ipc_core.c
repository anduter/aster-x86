/*
 * ng WP1-6: IPC 端口/句柄/等待队列的纯状态机。
 *
 * 这里不 include 串口、heap、线程或页表头：所有环境依赖都走 `struct
 * ipc_env`（时间/睡眠/唤醒/分配）。内核与 host 单测共用这份代码：
 *   - 内核 env：rdtsc / thread_sleep / thread_wake / kmalloc / kfree；
 *   - host env：fake 时钟 / fake sleep / 记录 wake / malloc / free。
 *
 * 单核协作式前提（与 WP1-5 一致）：没有抢占、没有并发执行，只有显式
 * sleep 点会切走，所以“检查条件 → 入等待队列 → 再检查 → sleep”之间不会
 * 被别的线程插入；等待队列的 `notified` 位用来把“有数据/有空位”的唤醒
 * 分给不同等待者，避免多个等待者只被叫醒一个而丢唤醒。
 *
 * 端口生命周期：
 *   used=0                      空闲
 *   used=1, closed=0            存活
 *   used=1, closed=1            已 close，等最后一批等待者离开后回收（ring
 *                               和槽位一起 free/清零）；这段时间里等待者
 *                               只看 closed 标志，不再碰队列字段。
 */
#include "ipc.h"

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void ipc_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void ipc_memcopy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

void ipc_core_init(struct ipc_core *c, const struct ipc_env *env)
{
    u32 i;

    if (c == NULL || env == NULL) {
        return;
    }
    ipc_memzero(c, sizeof(*c));
    handle_table_init(&c->handles);
    c->env = env;
    for (i = 0; i < IPC_WAITER_MAX; i++) {
        c->waiters[i].in_use = 0;
        c->waiters[i].next = (i + 1 < IPC_WAITER_MAX) ? &c->waiters[i + 1] : NULL;
    }
    c->free_waiters = (IPC_WAITER_MAX > 0) ? &c->waiters[0] : NULL;
}

/* ------------------------------------------------------------------------- */
/* 等待者池                                                                   */
/* ------------------------------------------------------------------------- */

static struct ipc_waiter *ipc_waiter_alloc(struct ipc_core *c, u32 tid, u32 kind)
{
    struct ipc_waiter *w = c->free_waiters;

    if (w == NULL) {
        return NULL;
    }
    c->free_waiters = w->next;
    w->tid = tid;
    w->kind = kind;
    w->notified = 0;
    w->in_use = 1;
    w->next = NULL;
    return w;
}

static void ipc_waiter_free(struct ipc_core *c, struct ipc_waiter *w)
{
    w->in_use = 0;
    w->tid = 0;
    w->notified = 0;
    w->next = c->free_waiters;
    c->free_waiters = w;
}

static void ipc_wait_list_add(struct ipc_waiter **head, struct ipc_waiter *w)
{
    struct ipc_waiter *p;

    w->next = NULL;
    if (*head == NULL) {
        *head = w;
        return;
    }
    p = *head;
    while (p->next != NULL) {
        p = p->next;
    }
    p->next = w;
}

static int ipc_wait_list_remove(struct ipc_waiter **head, struct ipc_waiter *w)
{
    struct ipc_waiter **pp = head;

    while (*pp != NULL && *pp != w) {
        pp = &(*pp)->next;
    }
    if (*pp != w) {
        return 0;
    }
    *pp = w->next;
    w->next = NULL;
    return 1;
}

static struct ipc_waiter **ipc_wait_list(struct ipc_port *p, u32 kind)
{
    return (kind == IPC_WAIT_RECV) ? &p->recv_waiters : &p->send_waiters;
}

/* 叫醒第一个还没被通知的等待者；没有可通知的返回 0。 */
static u32 ipc_wake_one(struct ipc_core *c, struct ipc_port *p, u32 kind)
{
    struct ipc_waiter *w;
    struct ipc_waiter **head = ipc_wait_list(p, kind);

    for (w = *head; w != NULL; w = w->next) {
        if (w->notified) {
            continue;
        }
        w->notified = 1;
        c->waiter_wakes++;
        (void)c->env->wake(w->tid, c->env->ctx);
        return 1;
    }
    return 0;
}

static u32 ipc_wake_all(struct ipc_core *c, struct ipc_port *p, u32 kind)
{
    struct ipc_waiter *w;
    struct ipc_waiter **head = ipc_wait_list(p, kind);
    u32 n = 0;

    for (w = *head; w != NULL; w = w->next) {
        w->notified = 1;
        c->waiter_wakes++;
        (void)c->env->wake(w->tid, c->env->ctx);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 端口槽位 / 句柄 → 端口                                                     */
/* ------------------------------------------------------------------------- */

static void ipc_reap_ports(struct ipc_core *c)
{
    u32 i;

    for (i = 0; i < IPC_PORT_MAX; i++) {
        struct ipc_port *p = &c->ports[i];

        if (!p->used || !p->closed) {
            continue;
        }
        if (p->recv_waiters != NULL || p->send_waiters != NULL) {
            continue; /* 等待者还在用 p（只看 closed 标志），不能回收 */
        }
        if (p->ring != NULL) {
            c->env->free(p->ring, c->env->ctx);
        }
        ipc_memzero(p, sizeof(*p));
        if (c->port_live > 0) {
            c->port_live--;
        }
    }
}

/*
 * 句柄 → 端口。除句柄表校验外，再交叉校验 ticket 的 slot/generation 与
 * 端口创建时记录的一致：等 ticket 能跨表传递（cap grant）后，这一层能
 * 拦住“句柄槽位复用后旧 ticket 指到新端口”的情况。
 */
static struct ipc_port *ipc_handle_to_port(struct ipc_core *c, u32 handle,
                                           int *err)
{
    void *obj = NULL;

    if (handle_lookup(&c->handles, handle, HANDLE_TYPE_PORT, &obj) !=
            HANDLE_OK ||
        obj == NULL) {
        c->bad_handles++;
        if (err != NULL) {
            *err = IPC_EBADF;
        }
        return NULL;
    }
    {
        struct ipc_port *p = (struct ipc_port *)obj;

        if (!p->used || p->slot != handle_slot_of(handle) ||
            p->gen != handle_gen_of(handle)) {
            c->bad_handles++;
            if (err != NULL) {
                *err = IPC_EBADF;
            }
            return NULL;
        }
        return p;
    }
}

/* 消息里的 from 必须是本表里存活的 PORT 句柄（0 = 单向消息）。 */
static int ipc_ticket_ok(struct ipc_core *c, u32 ticket)
{
    void *obj = NULL;

    if (ticket == 0) {
        return 1;
    }
    return handle_lookup(&c->handles, ticket, HANDLE_TYPE_PORT, &obj) ==
               HANDLE_OK &&
           obj != NULL;
}

static int ipc_perm_ok(struct ipc_core *c, struct ipc_port *p, u32 caller_uid)
{
    /* TODO(进程模型)：现在没有 uid，内核路径一律 root；逻辑先按
     * “同 uid 或 root”实现并单测，等 WP1-7 的进程接上真实 uid。 */
    if (caller_uid == IPC_UID_ROOT || caller_uid == p->owner_uid) {
        return 1;
    }
    c->perms++;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 消息 helper                                                                */
/* ------------------------------------------------------------------------- */

static int ipc_msg_check(const void *msg, u32 len)
{
    const struct ipc_msg_hdr *h;

    if (msg == NULL) {
        return IPC_EINVAL;
    }
    if (len < IPC_HDR_SIZE) {
        return IPC_EINVAL;
    }
    if (len > IPC_MSG_MAX) {
        return IPC_EMSGSIZE;
    }
    h = (const struct ipc_msg_hdr *)msg;
    if (h->len > IPC_PAYLOAD_MAX) {
        return IPC_EMSGSIZE;
    }
    if ((u64)h->len + (u64)IPC_HDR_SIZE != (u64)len) {
        return IPC_EINVAL;
    }
    if (h->flags != IPC_FLAG_NONE) {
        return IPC_EINVAL;
    }
    return IPC_OK;
}

static struct ipc_msg *ipc_msg_dup(struct ipc_core *c, const void *msg, u32 len)
{
    const struct ipc_msg_hdr *h = (const struct ipc_msg_hdr *)msg;
    struct ipc_msg *m;
    u64 total = (u64)IPC_HDR_SIZE + (u64)h->len;

    (void)len; /* 已由 ipc_msg_check 校验；分配长度用 hdr.len */
    m = (struct ipc_msg *)c->env->alloc(total, c->env->ctx);
    if (m == NULL) {
        return NULL;
    }
    m->hdr.type = h->type;
    m->hdr.len = h->len;
    m->hdr.from = h->from;
    m->hdr.flags = h->flags;
    ipc_memcopy(m->payload, (const u8 *)msg + IPC_HDR_SIZE, h->len);
    return m;
}

static void ipc_port_push(struct ipc_core *c, struct ipc_port *p,
                          struct ipc_msg *m)
{
    p->ring[p->tail] = m;
    p->tail = (p->tail + 1 >= p->depth) ? 0 : p->tail + 1;
    p->count++;
    p->sent++;
    (void)c; /* 统计在调用方；参数保留给未来 per-port 计费 */
}

static struct ipc_msg *ipc_port_pop(struct ipc_port *p)
{
    struct ipc_msg *m;

    if (p->count == 0) {
        return NULL;
    }
    m = p->ring[p->head];
    p->ring[p->head] = NULL;
    p->head = (p->head + 1 >= p->depth) ? 0 : p->head + 1;
    p->count--;
    p->recvd++;
    return m;
}

/* ------------------------------------------------------------------------- */
/* 阻塞                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * 阻塞到“条件满足 / 端口 close / 超时”三者之一。
 * 返回 IPC_OK / IPC_ECANCELED / IPC_ETIMEDOUT / IPC_EAGAIN（等待者池满）。
 */
static int ipc_wait(struct ipc_core *c, struct ipc_port *p, u32 kind,
                    u64 deadline, int forever)
{
    struct ipc_waiter *w;
    struct ipc_waiter **head = ipc_wait_list(p, kind);
    int rc;

    w = ipc_waiter_alloc(c, c->env->self_tid(c->env->ctx), kind);
    if (w == NULL) {
        c->eagains++;
        return IPC_EAGAIN;
    }
    ipc_wait_list_add(head, w);

    for (;;) {
        if (p->closed) {
            rc = IPC_ECANCELED;
            break;
        }
        if (kind == IPC_WAIT_RECV) {
            if (p->count > 0) {
                rc = IPC_OK;
                break;
            }
        } else if (p->count < p->depth) {
            rc = IPC_OK;
            break;
        }
        if (!forever) {
            u64 now = c->env->now(c->env->ctx);

            if (now >= deadline) {
                c->timeouts++;
                rc = IPC_ETIMEDOUT;
                break;
            }
            w->notified = 0;
            c->env->sleep(deadline - now, c->env->ctx);
        } else {
            w->notified = 0;
            c->env->sleep(IPC_FOREVER_SLEEP, c->env->ctx);
        }
    }

    (void)ipc_wait_list_remove(head, w);
    ipc_waiter_free(c, w);
    return rc;
}

/* 计算 deadline；返回 1 = 永久等待。timeout 已排除 0。 */
static int ipc_deadline(struct ipc_core *c, u64 timeout, u64 *out)
{
    u64 now;
    u64 d;

    if (timeout == IPC_TIMEOUT_FOREVER) {
        *out = 0;
        return 1;
    }
    now = c->env->now(c->env->ctx);
    d = now + timeout;
    if (d < now) {
        d = ~0ULL; /* 饱和，避免回绕成立即超时 */
    }
    if (d == 0) {
        d = 1;
    }
    *out = d;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 发送（send / reply 共用的核心）                                            */
/* ------------------------------------------------------------------------- */

static int ipc_send_to_port(struct ipc_core *c, struct ipc_port *p,
                            const void *msg, u32 len, u32 caller_uid,
                            u64 timeout, int is_reply)
{
    const struct ipc_msg_hdr *h = (const struct ipc_msg_hdr *)msg;
    struct ipc_msg *m;
    u64 deadline = 0;
    int forever = 0;
    int rc;

    if (!ipc_perm_ok(c, p, caller_uid)) {
        return IPC_EPERM;
    }
    if (p->closed) {
        return IPC_EBADF;
    }
    rc = ipc_msg_check(msg, len);
    if (rc != IPC_OK) {
        return rc;
    }
    if (!ipc_ticket_ok(c, h->from)) {
        c->bad_handles++;
        return IPC_EBADF;
    }

    if (timeout != IPC_TIMEOUT_NOW) {
        forever = ipc_deadline(c, timeout, &deadline);
    }

    if (p->count >= p->depth) {
        if (timeout == IPC_TIMEOUT_NOW) {
            c->eagains++;
            return IPC_EAGAIN;
        }
        p->sent_blocked++;
        rc = ipc_wait(c, p, IPC_WAIT_SEND, deadline, forever);
        if (rc != IPC_OK) {
            ipc_reap_ports(c);
            return rc;
        }
        if (p->closed) {
            ipc_reap_ports(c);
            return IPC_ECANCELED;
        }
    }

    m = ipc_msg_dup(c, msg, len);
    if (m == NULL) {
        return IPC_ENOMEM;
    }
    ipc_port_push(c, p, m);
    (void)ipc_wake_one(c, p, IPC_WAIT_RECV);
    if (is_reply) {
        c->replies++;
    } else {
        c->sends++;
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------------- */
/* 公开 API                                                                   */
/* ------------------------------------------------------------------------- */

int ipc_core_port_create(struct ipc_core *c, u32 depth, u32 owner_uid,
                         u32 *out_handle)
{
    struct ipc_port *p = NULL;
    struct ipc_msg **ring;
    u32 d;
    u32 i;
    u32 handle = 0;
    int rc;
    u64 bytes;

    if (c == NULL || c->env == NULL || out_handle == NULL) {
        return IPC_EINVAL;
    }
    ipc_reap_ports(c);

    d = (depth == 0) ? IPC_DEPTH_DEFAULT : depth;
    if (d > IPC_DEPTH_MAX) {
        return IPC_EINVAL;
    }
    for (i = 0; i < IPC_PORT_MAX; i++) {
        if (!c->ports[i].used) {
            p = &c->ports[i];
            break;
        }
    }
    if (p == NULL) {
        return IPC_ENOSPC;
    }

    bytes = (u64)d * (u64)sizeof(struct ipc_msg *);
    ring = (struct ipc_msg **)c->env->alloc(bytes, c->env->ctx);
    if (ring == NULL) {
        return IPC_ENOMEM;
    }
    ipc_memzero(ring, bytes);

    rc = handle_alloc(&c->handles, HANDLE_TYPE_PORT, p, &handle);
    if (rc != HANDLE_OK) {
        c->env->free(ring, c->env->ctx);
        return rc;
    }

    ipc_memzero(p, sizeof(*p));
    p->used = 1;
    p->depth = d;
    p->ring = ring;
    p->owner_uid = owner_uid;
    p->slot = handle_slot_of(handle);
    p->gen = handle_gen_of(handle);
    c->port_live++;

    *out_handle = handle;
    return IPC_OK;
}

int ipc_core_send(struct ipc_core *c, u32 port_handle, const void *msg, u32 len,
                  u32 caller_uid, u64 timeout)
{
    struct ipc_port *p;
    int err = IPC_EBADF;

    if (c == NULL || c->env == NULL) {
        return IPC_EINVAL;
    }
    ipc_reap_ports(c);
    p = ipc_handle_to_port(c, port_handle, &err);
    if (p == NULL) {
        return err;
    }
    return ipc_send_to_port(c, p, msg, len, caller_uid, timeout, 0);
}

int ipc_core_reply(struct ipc_core *c, u32 ticket, const void *msg, u32 len,
                   u32 caller_uid, u64 timeout)
{
    struct ipc_port *p;
    int err = IPC_EBADF;

    if (c == NULL || c->env == NULL) {
        return IPC_EINVAL;
    }
    ipc_reap_ports(c);
    p = ipc_handle_to_port(c, ticket, &err);
    if (p == NULL) {
        return err;
    }
    /* ticket 与端口身份交叉校验（来源校验；跨表 ticket 的前置检查）。 */
    if (p->slot != handle_slot_of(ticket) || p->gen != handle_gen_of(ticket)) {
        c->bad_handles++;
        return IPC_EBADF;
    }
    return ipc_send_to_port(c, p, msg, len, caller_uid, timeout, 1);
}

int ipc_core_recv(struct ipc_core *c, u32 port_handle, void *buf, u32 max,
                  u32 caller_uid, u64 timeout, u32 *out_len)
{
    struct ipc_port *p;
    struct ipc_msg *m;
    u32 total;
    u32 n;
    u64 deadline = 0;
    int forever = 0;
    int err = IPC_EBADF;
    int rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (c == NULL || c->env == NULL) {
        return IPC_EINVAL;
    }
    ipc_reap_ports(c);
    p = ipc_handle_to_port(c, port_handle, &err);
    if (p == NULL) {
        return err;
    }
    if (!ipc_perm_ok(c, p, caller_uid)) {
        return IPC_EPERM;
    }
    if (buf == NULL && max > 0) {
        return IPC_EINVAL;
    }
    if (p->count == 0) {
        if (timeout == IPC_TIMEOUT_NOW) {
            c->eagains++;
            return IPC_EAGAIN;
        }
        forever = ipc_deadline(c, timeout, &deadline);
        p->recv_blocked++;
        rc = ipc_wait(c, p, IPC_WAIT_RECV, deadline, forever);
        if (rc != IPC_OK) {
            ipc_reap_ports(c);
            return rc;
        }
        if (p->closed) {
            ipc_reap_ports(c);
            return IPC_ECANCELED;
        }
    }

    m = ipc_port_pop(p);
    if (m == NULL) {
        /* 正常不会发生；防御 close/等待队列错配。 */
        ipc_reap_ports(c);
        return IPC_ECANCELED;
    }
    total = IPC_HDR_SIZE + m->hdr.len;
    n = (max < total) ? max : total;
    if (n > 0) {
        ipc_memcopy(buf, &m->hdr, n);
    }
    c->env->free(m, c->env->ctx);
    if (out_len != NULL) {
        *out_len = total;
    }
    (void)ipc_wake_one(c, p, IPC_WAIT_SEND);
    c->recvs++;
    ipc_reap_ports(c);
    return (int)total;
}

int ipc_core_close(struct ipc_core *c, u32 port_handle, u32 caller_uid)
{
    struct ipc_port *p;
    u32 type = HANDLE_TYPE_NONE;
    void *obj = NULL;
    int err = IPC_EBADF;
    u32 wakes = 0;

    if (c == NULL || c->env == NULL) {
        return IPC_EINVAL;
    }
    ipc_reap_ports(c);
    p = ipc_handle_to_port(c, port_handle, &err);
    if (p == NULL) {
        return err;
    }
    if (!ipc_perm_ok(c, p, caller_uid)) {
        return IPC_EPERM;
    }

    /* 先失效句柄（stale generation），再清队列、唤醒等待者。 */
    if (handle_close(&c->handles, port_handle, &type, &obj) != HANDLE_OK) {
        return IPC_EBADF;
    }
    p->closed = 1;
    while (p->count > 0) {
        struct ipc_msg *m = ipc_port_pop(p);

        if (m != NULL) {
            c->env->free(m, c->env->ctx);
        }
    }
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    c->closes++;

    wakes += ipc_wake_all(c, p, IPC_WAIT_RECV);
    wakes += ipc_wake_all(c, p, IPC_WAIT_SEND);
    p->closure_wakes += wakes;

    /* 没有等待者时立即回收；有等待者时由它们离开后的下一次 IPC 操作回收。 */
    ipc_reap_ports(c);
    return IPC_OK;
}
