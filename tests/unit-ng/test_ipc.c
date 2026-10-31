/*
 * ng WP1-6 host 单测：句柄表 + 端口队列 + 阻塞状态机。
 *
 * 纯逻辑测试：不跑 QEMU、不切线程、不需要内核。ipc_core.c 只通过
 * `struct ipc_env` 的 6 个钩子依赖环境；这里给一套 fake 环境：
 *   - fake 时钟（f_now，测试/回调自己推进）；
 *   - fake 睡眠（可注册 on_sleep 回调，用来模拟“另一个线程”发/收/关闭）；
 *   - fake 唤醒（只记录 tid，不真的调度）；
 *   - malloc/free 计数（验证消息/ring 的释放）。
 *
 * 内核侧真实阻塞（thread_sleep/thread_wake）由 QEMU 自检覆盖
 * （tools/run-ng.ps1 的 WP1-6 行）。
 */
#include <stdlib.h>
#include <string.h>

#include "handle.h"
#include "ipc.h"
#include "test_util.h"

/* ------------------------------------------------------------------------- */
/* fake 环境                                                                  */
/* ------------------------------------------------------------------------- */

static u64 f_now;
static u32 f_self;
static u32 f_sleeps;
static u32 f_wakes;
static u32 f_last_wake;
static u32 f_allocs;
static u32 f_frees;
static int f_alloc_fail;
static void (*f_on_sleep)(void *);
static void *f_on_sleep_arg;

static void *fe_alloc(u64 size, void *ctx)
{
    void *p;

    (void)ctx;
    if (f_alloc_fail) {
        return NULL;
    }
    p = malloc((size_t)size);
    if (p != NULL) {
        f_allocs++;
    }
    return p;
}

static void fe_free(void *p, void *ctx)
{
    (void)ctx;
    f_frees++;
    free(p);
}

static u32 fe_tid(void *ctx)
{
    (void)ctx;
    return f_self;
}

static u64 fe_now(void *ctx)
{
    (void)ctx;
    return f_now;
}

static void fe_sleep(u64 ticks, void *ctx)
{
    void (*cb)(void *);

    (void)ctx;
    f_sleeps++;
    cb = f_on_sleep;
    if (cb != NULL) {
        f_on_sleep = NULL; /* 只触发一次，避免 12 层嵌套 */
        cb(f_on_sleep_arg);
    }
    if (ticks < IPC_FOREVER_SLEEP) {
        f_now += ticks;
    } else {
        f_now += 1;
    }
}

static int fe_wake(u32 tid, void *ctx)
{
    (void)ctx;
    f_wakes++;
    f_last_wake = tid;
    return 0;
}

static const struct ipc_env fake_env = {
    NULL, fe_tid, fe_now, fe_sleep, fe_wake, fe_alloc, fe_free};

static void fe_reset(void)
{
    f_now = 1000;
    f_self = 1;
    f_sleeps = 0;
    f_wakes = 0;
    f_last_wake = 0;
    f_allocs = 0;
    f_frees = 0;
    f_alloc_fail = 0;
    f_on_sleep = NULL;
    f_on_sleep_arg = NULL;
}

static void core_init(struct ipc_core *c)
{
    fe_reset();
    ipc_core_init(c, &fake_env);
}

/* ------------------------------------------------------------------------- */
/* 测试用消息/端口 helper                                                     */
/* ------------------------------------------------------------------------- */

static u8 sbuf[IPC_MSG_MAX];
static u8 sbuf2[IPC_MSG_MAX];
static u8 rbuf[IPC_MSG_MAX];
static u8 rbuf2[IPC_MSG_MAX];
static int marker;

static u32 build(u8 *m, u32 type, u32 from, u32 plen, u32 seed)
{
    struct ipc_msg_hdr *h = (struct ipc_msg_hdr *)m;
    u8 *p = m + IPC_HDR_SIZE;
    u32 i;

    h->type = type;
    h->len = plen;
    h->from = from;
    h->flags = 0;
    for (i = 0; i < plen; i++) {
        p[i] = (u8)(seed + i);
    }
    return IPC_HDR_SIZE + plen;
}

static int payload_is(const u8 *m, u32 plen, u32 seed)
{
    const u8 *p = m + IPC_HDR_SIZE;
    u32 i;

    for (i = 0; i < plen; i++) {
        if (p[i] != (u8)(seed + i)) {
            return 0;
        }
    }
    return 1;
}

static struct ipc_port *find_port(struct ipc_core *c, u32 handle)
{
    u32 i;

    for (i = 0; i < IPC_PORT_MAX; i++) {
        struct ipc_port *p = &c->ports[i];

        if (p->used && p->slot == handle_slot_of(handle) &&
            p->gen == handle_gen_of(handle)) {
            return p;
        }
    }
    return NULL;
}

struct cb_arg {
    struct ipc_core *c;
    u32 handle;
};

/* ------------------------------------------------------------------------- */
/* 1. 句柄表                                                                  */
/* ------------------------------------------------------------------------- */

static void t_handle_table(void)
{
    struct handle_table t;
    struct handle_table t2;
    struct handle_table t3;
    int obj_a = 1;
    int obj_b = 2;
    int objs[64];
    void *out = NULL;
    u32 h1 = 0;
    u32 h2 = 0;
    u32 h3 = 0;
    u32 type = 0;
    u32 i;

    handle_table_init(&t);
    CHECK(handle_live(&t) == 0);
    CHECK(handle_lookup(&t, 0, HANDLE_TYPE_PORT, &out) == HANDLE_EBADF);
    CHECK(handle_lookup(&t, 0xDEADBEEFu, HANDLE_TYPE_PORT, &out) ==
          HANDLE_EBADF);
    CHECK(handle_close(&t, 0, NULL, NULL) == HANDLE_EBADF);
    CHECK(handle_alloc(&t, HANDLE_TYPE_NONE, &obj_a, &h1) == HANDLE_EINVAL);
    CHECK(handle_alloc(&t, HANDLE_TYPE_PORT, NULL, &h1) == HANDLE_EINVAL);
    CHECK(handle_alloc(&t, 4u, &obj_a, &h1) == HANDLE_EINVAL);

    CHECK(handle_alloc(&t, HANDLE_TYPE_PORT, &obj_a, &h1) == HANDLE_OK);
    CHECK(h1 == handle_make(1, 0));
    CHECK(handle_slot_of(h1) == 0);
    CHECK(handle_gen_of(h1) == 1);
    CHECK(handle_live(&t) == 1);
    CHECK(handle_peak(&t) == 1);
    CHECK(t.allocs == 1);
    CHECK(handle_is_valid(&t, h1) == 1);
    CHECK(handle_is_valid(&t, 0) == 0);
    CHECK(handle_lookup(&t, h1, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(out == &obj_a);

    /* 类型不匹配：PORT 查 THREAD。 */
    CHECK(handle_lookup(&t, h1, HANDLE_TYPE_THREAD, &out) == HANDLE_EBADF);
    CHECK(t.type_errors == 1);
    CHECK(handle_lookup_any(&t, h1, &type, &out) == HANDLE_OK);
    CHECK(type == HANDLE_TYPE_PORT);

    /* close：generation +1、对象摘除、旧句柄失效。 */
    CHECK(handle_close(&t, h1, &type, &out) == HANDLE_OK);
    CHECK(type == HANDLE_TYPE_PORT);
    CHECK(out == &obj_a);
    CHECK(handle_live(&t) == 0);
    CHECK(t.closes == 1);
    CHECK(handle_lookup(&t, h1, HANDLE_TYPE_PORT, &out) == HANDLE_EBADF);
    CHECK(handle_is_valid(&t, h1) == 0);
    CHECK(handle_close(&t, h1, NULL, NULL) == HANDLE_EBADF);

    /* 槽位复用：新句柄 gen=2，旧句柄仍然无效。 */
    CHECK(handle_alloc(&t, HANDLE_TYPE_PORT, &obj_b, &h2) == HANDLE_OK);
    CHECK(h2 == handle_make(2, 0));
    CHECK(h2 != h1);
    CHECK(handle_lookup(&t, h2, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(out == &obj_b);
    CHECK(handle_lookup(&t, h1, HANDLE_TYPE_PORT, &out) == HANDLE_EBADF);
    CHECK(handle_peak(&t) == 1);

    /* 第二个槽：THREAD 预留类型。 */
    CHECK(handle_alloc(&t, HANDLE_TYPE_THREAD, &obj_a, &h3) == HANDLE_OK);
    CHECK(handle_slot_of(h3) == 1);
    CHECK(handle_gen_of(h3) == 1);
    CHECK(handle_lookup(&t, h3, HANDLE_TYPE_PORT, &out) == HANDLE_EBADF);
    CHECK(handle_live(&t) == 2);
    CHECK(handle_peak(&t) == 2);
    CHECK(handle_close(&t, h3, NULL, NULL) == HANDLE_OK);
    CHECK(handle_close(&t, h2, NULL, NULL) == HANDLE_OK);
    CHECK(handle_live(&t) == 0);

    /* 表满：64 槽，多一个 ENOSPC。 */
    handle_table_init(&t2);
    for (i = 0; i < HANDLE_MAX; i++) {
        objs[i] = (int)i;
        CHECK(handle_alloc(&t2, HANDLE_TYPE_FILE, &objs[i], &h1) == HANDLE_OK);
    }
    CHECK(handle_live(&t2) == HANDLE_MAX);
    CHECK(handle_alloc(&t2, HANDLE_TYPE_FILE, &objs[0], &h1) == HANDLE_ENOSPC);
    CHECK(t2.full_rejects == 1);
    CHECK(handle_peak(&t2) == HANDLE_MAX);
    CHECK(handle_close(&t2, handle_make(1, 5), &type, &out) == HANDLE_OK);
    CHECK(handle_live(&t2) == HANDLE_MAX - 1);
    CHECK(handle_alloc(&t2, HANDLE_TYPE_FILE, &objs[0], &h1) == HANDLE_OK);
    CHECK(handle_live(&t2) == HANDLE_MAX);

    /* generation 16 位回绕：跳过 0，旧 ticket 不会“复活”。 */
    handle_table_init(&t3);
    CHECK(handle_alloc(&t3, HANDLE_TYPE_PORT, &obj_a, &h1) == HANDLE_OK);
    CHECK(handle_close(&t3, h1, NULL, NULL) == HANDLE_OK);
    t3.slots[0].gen = 0xFFFF; /* 等价于已经关了 65534 次 */
    CHECK(handle_alloc(&t3, HANDLE_TYPE_PORT, &obj_a, &h2) == HANDLE_OK);
    CHECK(handle_gen_of(h2) == 0xFFFF);
    CHECK(handle_close(&t3, h2, NULL, NULL) == HANDLE_OK);
    CHECK(handle_alloc(&t3, HANDLE_TYPE_PORT, &obj_a, &h3) == HANDLE_OK);
    CHECK(handle_slot_of(h3) == 0);
    CHECK(handle_gen_of(h3) == 1); /* 0 被跳过 */
    CHECK(handle_lookup(&t3, h2, HANDLE_TYPE_PORT, &out) == HANDLE_EBADF);
    CHECK(handle_lookup(&t3, h3, HANDLE_TYPE_PORT, &out) == HANDLE_OK);
    CHECK(handle_make(1, 2) == 0x00010002u);
    CHECK(handle_slot_of(0x00010002u) == 2);
    CHECK(handle_gen_of(0x00010002u) == 1);
}

/* ------------------------------------------------------------------------- */
/* 2. port_create                                                             */
/* ------------------------------------------------------------------------- */

static void t_port_create(void)
{
    struct ipc_core c;
    struct ipc_port *p;
    u32 h = 0;
    u32 h2 = 0;
    u32 hs[IPC_PORT_MAX];
    u32 n = 0;
    u32 i;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 5, &h) == IPC_OK);
    CHECK(h != 0);
    CHECK(handle_gen_of(h) >= 1);
    CHECK(handle_slot_of(h) < HANDLE_MAX);
    p = find_port(&c, h);
    CHECK(p != NULL);
    if (p != NULL) {
        CHECK(p->depth == IPC_DEPTH_DEFAULT);
        CHECK(p->owner_uid == 5);
        CHECK(p->ring != NULL);
        CHECK(p->count == 0);
        CHECK(p->slot == handle_slot_of(h));
        CHECK(p->gen == handle_gen_of(h));
    }
    CHECK(c.port_live == 1);
    CHECK(handle_live(&c.handles) == 1);

    CHECK(ipc_core_port_create(&c, 3, 0, &h2) == IPC_OK);
    p = find_port(&c, h2);
    CHECK(p != NULL && p->depth == 3);

    CHECK(ipc_core_port_create(&c, IPC_DEPTH_MAX + 1, 0, &h2) == IPC_EINVAL);
    CHECK(ipc_core_port_create(&c, 0, 0, NULL) == IPC_EINVAL);

    /* ring 分配失败：ENOMEM 且不占槽位。 */
    f_alloc_fail = 1;
    CHECK(ipc_core_port_create(&c, 0, 0, &h2) == IPC_ENOMEM);
    f_alloc_fail = 0;
    CHECK(c.port_live == 2);
    CHECK(handle_live(&c.handles) == 2);
    CHECK(f_allocs == 2); /* 两个 ring；失败那次没有分配 */

    /* 端口池满：IPC_PORT_MAX 个。 */
    hs[0] = h;
    hs[1] = h2;
    n = 2;
    while (n < IPC_PORT_MAX) {
        u32 hh = 0;

        CHECK(ipc_core_port_create(&c, 1, 0, &hh) == IPC_OK);
        hs[n++] = hh;
    }
    CHECK(c.port_live == IPC_PORT_MAX);
    CHECK(ipc_core_port_create(&c, 1, 0, &h2) == IPC_ENOSPC);

    for (i = 0; i < n; i++) {
        CHECK(ipc_core_close(&c, hs[i], IPC_UID_ROOT) == IPC_OK);
    }
    CHECK(c.port_live == 0);
    CHECK(handle_live(&c.handles) == 0);
    CHECK(f_frees > 0);
}

/* ------------------------------------------------------------------------- */
/* 3. send/recv 基础 + FIFO + 截断                                            */
/* ------------------------------------------------------------------------- */

static void t_send_recv(void)
{
    struct ipc_core c;
    struct ipc_port *p;
    u32 h = 0;
    u32 len;
    u32 out_len = 0;
    int rc;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);

    len = build(sbuf, 7, 0, 8, 0x10);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(c.sends == 1);
    p = find_port(&c, h);
    CHECK(p != NULL && p->count == 1 && p->sent == 1);

    memset(rbuf, 0, sizeof(rbuf));
    rc = ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW, &out_len);
    CHECK(rc == (int)len);
    CHECK(out_len == len);
    CHECK(((struct ipc_msg_hdr *)rbuf)->type == 7);
    CHECK(((struct ipc_msg_hdr *)rbuf)->len == 8);
    CHECK(payload_is(rbuf, 8, 0x10));
    CHECK(c.recvs == 1);
    CHECK(p != NULL && p->count == 0 && p->recvd == 1);

    /* FIFO：1,2,3。 */
    CHECK(ipc_core_send(&c, h, sbuf, build(sbuf, 1, 0, 4, 1), 0,
                        IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_send(&c, h, sbuf, build(sbuf, 2, 0, 4, 2), 0,
                        IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_send(&c, h, sbuf, build(sbuf, 3, 0, 4, 3), 0,
                        IPC_TIMEOUT_NOW) == IPC_OK);
    for (u32 want = 1; want <= 3; want++) {
        out_len = 0;
        rc = ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                           &out_len);
        CHECK(rc == (int)(IPC_HDR_SIZE + 4));
        CHECK(((struct ipc_msg_hdr *)rbuf)->type == want);
        CHECK(payload_is(rbuf, 4, want));
    }
    /* 空队列非阻塞 recv：EAGAIN。 */
    out_len = 0xFFFFu;
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                        &out_len) == IPC_EAGAIN);
    CHECK(out_len == 0);
    CHECK(c.eagains == 1);

    /* 截断：max 小于真实长度时只拷 max，返回值仍是真实长度。 */
    len = build(sbuf, 9, 0, 64, 0x40);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    memset(rbuf, 0xEE, sizeof(rbuf));
    out_len = 0;
    rc = ipc_core_recv(&c, h, rbuf, 20, 0, IPC_TIMEOUT_NOW, &out_len);
    CHECK(rc == (int)len);
    CHECK(out_len == len);
    CHECK(((struct ipc_msg_hdr *)rbuf)->type == 9);
    CHECK(rbuf[19] != 0xEE);
    CHECK(rbuf[20] == 0xEE); /* 截断点之后没写 */
    CHECK(ipc_core_recv(&c, h, NULL, 10, 0, IPC_TIMEOUT_NOW, &out_len) ==
          IPC_EINVAL);

    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 4. 消息校验 + from 校验                                                    */
/* ------------------------------------------------------------------------- */

static void t_message_check(void)
{
    struct ipc_core c;
    struct ipc_msg_hdr *mh = (struct ipc_msg_hdr *)sbuf;
    u32 h = 0;
    u32 q = 0;
    u32 th = 0;
    u32 len;
    u32 out_len = 0;
    int rc;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);
    CHECK(ipc_core_port_create(&c, 0, 0, &q) == IPC_OK);
    CHECK(handle_alloc(&c.handles, HANDLE_TYPE_THREAD, &marker, &th) ==
          HANDLE_OK);

    len = build(sbuf, 1, 0, 8, 0);
    CHECK(ipc_core_send(&c, h, NULL, len, 0, IPC_TIMEOUT_NOW) == IPC_EINVAL);
    CHECK(ipc_core_send(&c, h, sbuf, 0, 0, IPC_TIMEOUT_NOW) == IPC_EINVAL);
    CHECK(ipc_core_send(&c, h, sbuf, IPC_HDR_SIZE - 1, 0, IPC_TIMEOUT_NOW) ==
          IPC_EINVAL);
    CHECK(ipc_core_send(&c, h, sbuf, IPC_MSG_MAX + 1, 0, IPC_TIMEOUT_NOW) ==
          IPC_EMSGSIZE);

    mh->len = 7; /* 与总长度不自洽 */
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_EINVAL);
    mh->len = 8;

    mh->flags = 1;
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_EINVAL);
    mh->flags = 0;

    mh->from = 0xDEADu;
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_EBADF);
    mh->from = th; /* THREAD 句柄不是端口 */
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_EBADF);
    mh->from = q; /* 合法端口 ticket */
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    rc = ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                       &out_len);
    CHECK(rc == (int)len);
    CHECK(((struct ipc_msg_hdr *)rbuf)->from == q);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    (void)ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                        &out_len);

    (void)handle_close(&c.handles, th, NULL, NULL);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
    CHECK(ipc_core_close(&c, q, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 5. 深度满 + 非阻塞 EAGAIN + 阻塞 send 超时                                 */
/* ------------------------------------------------------------------------- */

static void t_depth_and_send_timeout(void)
{
    struct ipc_core c;
    struct ipc_port *p;
    u32 h = 0;
    u32 len;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 2, 0, &h) == IPC_OK);
    len = build(sbuf, 1, 0, 8, 1);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_EAGAIN);
    CHECK(c.eagains == 1);
    p = find_port(&c, h);
    CHECK(p != NULL && p->count == 2 && p->depth == 2);

    /* 阻塞 send 超时：fake 时钟被 fe_sleep 推进到 deadline。 */
    f_now = 1000;
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, 100) == IPC_ETIMEDOUT);
    CHECK(c.timeouts == 1);
    CHECK(f_sleeps == 1);
    CHECK(f_wakes == 0);
    CHECK(p != NULL && p->count == 2); /* 超时消息没有入队 */
    CHECK(p != NULL && p->sent_blocked == 1);

    /* 非阻塞 recv 再取一条，腾出位置后 send 成功。 */
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW, NULL) ==
          (int)len);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(p != NULL && p->count == 2);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 6. 阻塞 recv：被“另一个线程”的 send 唤醒                                   */
/* ------------------------------------------------------------------------- */

static void cb_send_one(void *arg)
{
    struct cb_arg *a = (struct cb_arg *)arg;
    u32 len;

    f_self = 2;
    len = build(sbuf2, 0x99, 0, 8, 0x30);
    CHECK(ipc_core_send(a->c, a->handle, sbuf2, len, 0, IPC_TIMEOUT_NOW) ==
          IPC_OK);
    f_self = 1;
}

static void t_blocked_recv_wake(void)
{
    struct ipc_core c;
    struct cb_arg a;
    struct ipc_port *p;
    u32 h = 0;
    u32 out_len = 0;
    int rc;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);
    a.c = &c;
    a.handle = h;
    f_on_sleep = cb_send_one;
    f_on_sleep_arg = &a;

    rc = ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_FOREVER,
                       &out_len);
    CHECK(rc == (int)(IPC_HDR_SIZE + 8));
    CHECK(out_len == IPC_HDR_SIZE + 8);
    CHECK(((struct ipc_msg_hdr *)rbuf)->type == 0x99);
    CHECK(payload_is(rbuf, 8, 0x30));
    CHECK(f_sleeps == 1);
    CHECK(f_wakes == 1);
    CHECK(f_last_wake == 1); /* 被唤醒的是等待线程 tid=1 */
    CHECK(c.waiter_wakes == 1);
    p = find_port(&c, h);
    CHECK(p != NULL && p->recv_blocked == 1);
    CHECK(p != NULL && p->count == 0);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 7. 阻塞 send：被“另一个线程”的 recv 唤醒                                   */
/* ------------------------------------------------------------------------- */

static void cb_recv_one(void *arg)
{
    struct cb_arg *a = (struct cb_arg *)arg;

    f_self = 2;
    CHECK(ipc_core_recv(a->c, a->handle, rbuf2, 32, 0, IPC_TIMEOUT_NOW, NULL) >
          0);
    f_self = 1;
}

static void t_blocked_send_wake(void)
{
    struct ipc_core c;
    struct cb_arg a;
    struct ipc_port *p;
    u32 h = 0;
    u32 len;
    u32 out_len = 0;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 1, 0, &h) == IPC_OK);
    len = build(sbuf, 0x11, 0, 8, 0x50);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);

    a.c = &c;
    a.handle = h;
    f_on_sleep = cb_recv_one;
    f_on_sleep_arg = &a;

    len = build(sbuf2, 0x12, 0, 8, 0x60);
    CHECK(ipc_core_send(&c, h, sbuf2, len, 0, IPC_TIMEOUT_FOREVER) == IPC_OK);
    CHECK(f_sleeps == 1);
    CHECK(f_wakes == 1);
    CHECK(f_last_wake == 1);
    p = find_port(&c, h);
    CHECK(p != NULL && p->sent_blocked == 1);
    CHECK(p != NULL && p->count == 1);
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                        &out_len) == (int)len);
    CHECK(((struct ipc_msg_hdr *)rbuf)->type == 0x12);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 8. 阻塞 recv 超时（与 wake 互斥）                                          */
/* ------------------------------------------------------------------------- */

static void t_blocked_recv_timeout(void)
{
    struct ipc_core c;
    u32 h = 0;
    u32 out_len = 0xFFFFu;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);
    f_now = 1000;
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, 50, &out_len) ==
          IPC_ETIMEDOUT);
    CHECK(out_len == 0);
    CHECK(c.timeouts == 1);
    CHECK(f_sleeps == 1);
    CHECK(f_wakes == 0);
    CHECK(f_now == 1050);

    /* deadline=1（回绕保护）不会立刻超时：sleep 只推进到 1001。 */
    f_now = 0;
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, 1, &out_len) ==
          IPC_ETIMEDOUT);
    CHECK(f_now == 1);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
}

/* ------------------------------------------------------------------------- */
/* 9. close：唤醒等待者 + 清队列 + 句柄失效                                   */
/* ------------------------------------------------------------------------- */

static void cb_close_port(void *arg)
{
    struct cb_arg *a = (struct cb_arg *)arg;

    f_self = 2;
    CHECK(ipc_core_close(a->c, a->handle, IPC_UID_ROOT) == IPC_OK);
    f_self = 1;
}

static void t_close(void)
{
    struct ipc_core c;
    struct cb_arg a;
    struct ipc_port *p;
    u32 h = 0;
    u32 len;
    u32 out_len = 0;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 2, 5, &h) == IPC_OK);
    len = build(sbuf, 1, 0, 8, 1);
    CHECK(ipc_core_send(&c, h, sbuf, len, 5, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_send(&c, h, sbuf, len, 5, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_close(&c, h, 5) == IPC_OK);
    CHECK(c.closes == 1);
    CHECK(c.port_live == 0);
    CHECK(f_frees >= 3); /* 2 条消息 + ring */
    CHECK(handle_live(&c.handles) == 0);
    CHECK(ipc_core_close(&c, h, 5) == IPC_EBADF);
    CHECK(ipc_core_send(&c, h, sbuf, len, 5, IPC_TIMEOUT_NOW) == IPC_EBADF);
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 5, IPC_TIMEOUT_NOW,
                        &out_len) == IPC_EBADF);
    CHECK(ipc_core_reply(&c, h, sbuf, len, 5, IPC_TIMEOUT_NOW) == IPC_EBADF);

    /* 阻塞 recv 被 close 唤醒：返回 ECANCELED，端口等待者退出后回收。 */
    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);
    a.c = &c;
    a.handle = h;
    f_on_sleep = cb_close_port;
    f_on_sleep_arg = &a;
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_FOREVER,
                        &out_len) == IPC_ECANCELED);
    CHECK(f_wakes == 1);
    CHECK(f_last_wake == 1);
    CHECK(c.port_live == 0);
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                        &out_len) == IPC_EBADF);

    /* 满队列里阻塞的 send 也会被 close 唤醒（ECANCELED）。 */
    core_init(&c);
    CHECK(ipc_core_port_create(&c, 1, 0, &h) == IPC_OK);
    len = build(sbuf, 1, 0, 8, 1);
    CHECK(ipc_core_send(&c, h, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    a.c = &c;
    a.handle = h;
    f_on_sleep = cb_close_port;
    f_on_sleep_arg = &a;
    len = build(sbuf2, 2, 0, 8, 2);
    CHECK(ipc_core_send(&c, h, sbuf2, len, 0, IPC_TIMEOUT_FOREVER) ==
          IPC_ECANCELED);
    CHECK(f_wakes == 1);
    p = find_port(&c, h);
    CHECK(p == NULL); /* 端口已回收 */
    CHECK(c.port_live == 0);
}

/* ------------------------------------------------------------------------- */
/* 10. 权限（同 uid / root）                                                  */
/* ------------------------------------------------------------------------- */

static void t_permissions(void)
{
    struct ipc_core c;
    u32 h = 0;
    u32 len;
    u32 out_len = 0;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 1, 5, &h) == IPC_OK);
    len = build(sbuf, 1, 0, 8, 1);

    CHECK(ipc_core_send(&c, h, sbuf, len, 6, IPC_TIMEOUT_NOW) == IPC_EPERM);
    CHECK(c.perms == 1);
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 6, IPC_TIMEOUT_NOW,
                        &out_len) == IPC_EPERM);
    CHECK(c.perms == 2);
    CHECK(ipc_core_close(&c, h, 6) == IPC_EPERM);
    CHECK(c.perms == 3);
    CHECK(handle_live(&c.handles) == 1); /* 拒绝的 close 不失效句柄 */

    CHECK(ipc_core_send(&c, h, sbuf, len, 5, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                        &out_len) == (int)len); /* root 可读 */
    CHECK(ipc_core_close(&c, h, 5) == IPC_OK);
    CHECK(c.port_live == 0);
}

/* ------------------------------------------------------------------------- */
/* 11. reply / ticket                                                         */
/* ------------------------------------------------------------------------- */

static void t_reply(void)
{
    struct ipc_core c;
    struct ipc_port *pp;
    struct ipc_port *pq;
    u32 p = 0;
    u32 q = 0;
    u32 th = 0;
    u32 len;
    u32 out_len = 0;
    u32 from;
    int rc;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &p) == IPC_OK);
    CHECK(ipc_core_port_create(&c, 0, 0, &q) == IPC_OK);
    CHECK(handle_alloc(&c.handles, HANDLE_TYPE_THREAD, &marker, &th) ==
          HANDLE_OK);
    pp = find_port(&c, p);
    pq = find_port(&c, q);
    CHECK(pp != NULL && pq != NULL);
    CHECK(pp != NULL && pp->slot == handle_slot_of(p));
    CHECK(pq != NULL && pq->gen == handle_gen_of(q));

    /* client -> server：from 带自己的 ticket。 */
    len = build(sbuf, 0x41, p, 8, 0x70);
    CHECK(ipc_core_send(&c, q, sbuf, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    rc = ipc_core_recv(&c, q, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                       &out_len);
    CHECK(rc == (int)len);
    from = ((struct ipc_msg_hdr *)rbuf)->from;
    CHECK(from == p);

    /* server 用 from 调 reply，client 收到；reply 计数单独统计。 */
    len = build(sbuf2, 0x42, q, 8, 0x80);
    CHECK(ipc_core_reply(&c, from, sbuf2, len, 0, IPC_TIMEOUT_NOW) == IPC_OK);
    CHECK(c.replies == 1);
    CHECK(c.sends == 1);
    rc = ipc_core_recv(&c, p, rbuf, sizeof(rbuf), 0, IPC_TIMEOUT_NOW,
                       &out_len);
    CHECK(rc == (int)len);
    CHECK(((struct ipc_msg_hdr *)rbuf)->type == 0x42);
    CHECK(((struct ipc_msg_hdr *)rbuf)->from == q);
    CHECK(payload_is(rbuf, 8, 0x80));

    /* 坏 ticket：越界 / 类型不对 / 已关闭。 */
    CHECK(ipc_core_reply(&c, 0xDEADu, sbuf2, len, 0, IPC_TIMEOUT_NOW) ==
          IPC_EBADF);
    CHECK(ipc_core_reply(&c, th, sbuf2, len, 0, IPC_TIMEOUT_NOW) == IPC_EBADF);
    CHECK(ipc_core_close(&c, p, 0) == IPC_OK);
    CHECK(ipc_core_reply(&c, p, sbuf2, len, 0, IPC_TIMEOUT_NOW) == IPC_EBADF);
    CHECK(ipc_core_close(&c, q, 0) == IPC_OK);
    (void)handle_close(&c.handles, th, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* 12. 等待者池复用 + 统计                                                    */
/* ------------------------------------------------------------------------- */

static void t_waiter_reuse(void)
{
    struct ipc_core c;
    u32 h = 0;
    u32 i;
    u32 out_len = 0;

    core_init(&c);
    CHECK(ipc_core_port_create(&c, 0, 0, &h) == IPC_OK);
    for (i = 0; i < 20; i++) {
        f_now = 1000;
        CHECK(ipc_core_recv(&c, h, rbuf, sizeof(rbuf), 0, 5, &out_len) ==
              IPC_ETIMEDOUT);
    }
    CHECK(c.timeouts == 20);
    CHECK(f_sleeps == 20);
    CHECK(c.free_waiters != NULL); /* 等待者都归还了 */

    /* 20 次连续 close/port_create 也能复用端口/句柄槽。 */
    for (i = 0; i < 20; i++) {
        CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
        CHECK(ipc_core_port_create(&c, 1, 0, &h) == IPC_OK);
    }
    CHECK(c.port_live == 1);
    CHECK(handle_live(&c.handles) == 1);
    CHECK(ipc_core_close(&c, h, 0) == IPC_OK);
    CHECK(c.port_live == 0);

    /* handle 池被 THREAD 类型占满时 port_create 失败但端口槽回滚。 */
    core_init(&c);
    {
        u32 th = 0;
        u32 n = 0;

        while (handle_alloc(&c.handles, HANDLE_TYPE_THREAD, &marker, &th) ==
               HANDLE_OK) {
            n++;
        }
        CHECK(n == HANDLE_MAX);
        CHECK(ipc_core_port_create(&c, 0, 0, &h) == HANDLE_ENOSPC);
        CHECK(c.port_live == 0);
        CHECK(c.ports[0].used == 0);
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int test_ipc(void)
{
    t_handle_table();
    t_port_create();
    t_send_recv();
    t_message_check();
    t_depth_and_send_timeout();
    t_blocked_recv_wake();
    t_blocked_send_wake();
    t_blocked_recv_timeout();
    t_close();
    t_permissions();
    t_reply();
    t_waiter_reuse();
    return 0;
}
