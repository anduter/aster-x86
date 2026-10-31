/*
 * ng WP1-6: 内核侧 IPC 自检（run-ng 的出口行产生处）。
 *
 * 覆盖（全部是真实线程 + 真实 thread_sleep/thread_wake 路径）：
 *   1. 句柄/ticket：port_create 返回 (gen<<16)|slot，端口槽/代与句柄一致；
 *   2. ping/pong：P/Q 两个内核线程各用一个端口，P 发 20 次请求（payload 带
 *      自己的 ticket），Q recv 后用 hdr.from 调 reply，P 收到 20 次回复；
 *   3. 队列满：depth=2 的端口塞满后非阻塞 send 返回 EAGAIN；阻塞的 send
 *      在 recv 腾出空间后被唤醒并成功；
 *   4. 超时：空端口 recv 用 TSC 超时返回 ETIMEDOUT；满端口 send 同样；
 *   5. 坏句柄/陈旧 generation：close 后 send/recv/reply/close 全部 EBADF，
 *      槽位复用后旧 ticket 仍被拒；
 *   6. close 唤醒：recv 永久阻塞的线程被 close 唤醒，返回 ECANCELED；
 *   7. 用户指针：用 WP1-3 映射的用户页（0x40100000）走 ipc_sys_* 的
 *      copyin/copyout；内核地址/null 返回 EFAULT。
 *
 * 所有等待都有自旋上限，失败不会把 run-ng 拖到超时；失败行以
 * `ng: ipc ... FAIL` 输出，run-ng 的失败模式会提前结束。
 */
#include "ipc_selftest.h"

#include "io.h"
#include "ipc.h"
#include "paging.h"
#include "serial.h"
#include "thread.h"

#define PP_ROUNDS 20u
#define SELFTEST_SPIN_LIMIT 200000u
#define TIMEOUT_TICKS 5000000ULL /* QEMU TSC 下约几毫秒 */

#define TYPE_PING 0x50494E47u /* "PING" */
#define TYPE_PONG 0x504F4E47u /* "PONG" */

/* 用户页（WP1-3 的 ring3 测试映射过：0x40100000 是 U|W|NX 的 4 KiB 栈页）。 */
#define USER_TEST_VA 0x40100000ULL

static u8 msg_a[IPC_MSG_MAX];
static u8 msg_b[IPC_MSG_MAX];
static u8 msg_c[IPC_MSG_MAX];
static u8 msg_d[IPC_MSG_MAX];

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void st_zero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static struct ipc_msg_hdr *st_hdr(u8 *m)
{
    return (struct ipc_msg_hdr *)m;
}

static u32 *st_payload(u8 *m)
{
    return (u32 *)(m + IPC_HDR_SIZE);
}

static u32 st_build(u8 *m, u32 type, u32 from, u32 w0, u32 w1)
{
    struct ipc_msg_hdr *h = st_hdr(m);
    u32 *p = st_payload(m);

    h->type = type;
    h->len = 8;
    h->from = from;
    h->flags = 0;
    p[0] = w0;
    p[1] = w1;
    return IPC_HDR_SIZE + 8u;
}

static struct ipc_port *st_port(u32 handle)
{
    struct ipc_core *g = ipc_core_global();
    u32 i;

    for (i = 0; i < IPC_PORT_MAX; i++) {
        struct ipc_port *p = &g->ports[i];

        if (!p->used) {
            continue;
        }
        if (p->slot == handle_slot_of(handle) &&
            p->gen == handle_gen_of(handle)) {
            return p;
        }
    }
    return NULL;
}

static int st_wait_flag(volatile int *flag, u32 limit)
{
    u32 spins = 0;

    while (spins < limit) {
        if (*flag) {
            return 1;
        }
        sched_yield();
        spins++;
    }
    return 0;
}

static int st_wait_state(thread_t *t, u32 want, u32 limit)
{
    u32 spins = 0;

    while (spins < limit) {
        if (t->state == want) {
            return 1;
        }
        if (t->state == THREAD_UNUSED || t->state == THREAD_DEAD) {
            return 0;
        }
        sched_yield();
        spins++;
    }
    return 0;
}

static int st_wait_reaps(u32 base, u32 want, u32 limit)
{
    u32 spins = 0;

    while (spins < limit) {
        if (sched_reap_count() - base >= want) {
            return 1;
        }
        sched_yield();
        spins++;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 1. ping/pong                                                               */
/* ------------------------------------------------------------------------- */

static u32 pp_p_h;
static u32 pp_q_h;
static volatile u32 pp_recv;
static volatile u32 pp_replies;
static volatile int pp_err;
static volatile int pp_p_done;
static volatile int pp_q_done;

static void ping_thread(void *arg)
{
    u32 i;

    (void)arg;
    for (i = 0; i < PP_ROUNDS; i++) {
        struct ipc_msg_hdr *h;
        u32 *pl;
        int rc;
        u32 rlen = 0;
        u32 len;

        len = st_build(msg_a, TYPE_PING, pp_p_h, i, TYPE_PING);
        rc = ipc_port_send(pp_q_h, msg_a, len, IPC_TIMEOUT_FOREVER);
        if (rc != IPC_OK) {
            pp_err = 1000 + rc;
            break;
        }
        rc = ipc_port_recv(pp_p_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_FOREVER,
                           &rlen);
        if (rc < 0) {
            pp_err = 2000 + rc;
            break;
        }
        h = st_hdr(msg_b);
        pl = st_payload(msg_b);
        if (rc != (int)(IPC_HDR_SIZE + 8u) || h->type != TYPE_PONG ||
            h->len != 8 || h->flags != 0 || h->from != pp_q_h ||
            pl[0] != i || pl[1] != TYPE_PONG) {
            pp_err = 3000 + (int)i;
            break;
        }
        pp_recv++;
    }
    pp_p_done = 1;
    thread_exit();
}

static void pong_thread(void *arg)
{
    u32 i;

    (void)arg;
    for (i = 0; i < PP_ROUNDS; i++) {
        struct ipc_msg_hdr *h;
        u32 *pl;
        int rc;
        u32 rlen = 0;
        u32 from;
        u32 len;

        rc = ipc_port_recv(pp_q_h, msg_c, IPC_MSG_MAX, IPC_TIMEOUT_FOREVER,
                           &rlen);
        if (rc < 0) {
            pp_err = 4000 + rc;
            break;
        }
        h = st_hdr(msg_c);
        pl = st_payload(msg_c);
        from = h->from;
        if (h->type != TYPE_PING || h->len != 8 || from == 0) {
            pp_err = 4100;
            break;
        }
        len = st_build(msg_d, TYPE_PONG, pp_q_h, pl[0], TYPE_PONG);
        rc = ipc_port_reply(from, msg_d, len, IPC_TIMEOUT_FOREVER);
        if (rc != IPC_OK) {
            pp_err = 4200 + rc;
            break;
        }
        pp_replies++;
    }
    pp_q_done = 1;
    thread_exit();
}

static int t_ping_pong(void)
{
    thread_t *pt;
    thread_t *qt;
    u32 base;
    struct ipc_port *pp;
    struct ipc_port *pq;
    int ok = 1;
    int rc;

    pp_recv = 0;
    pp_replies = 0;
    pp_err = 0;
    pp_p_done = 0;
    pp_q_done = 0;

    rc = ipc_port_create(0, &pp_p_h);
    if (rc != IPC_OK) {
        serial_puts("ng: ipc ping/pong FAIL (create p)\r\n");
        return 1;
    }
    rc = ipc_port_create(0, &pp_q_h);
    if (rc != IPC_OK) {
        (void)ipc_port_close(pp_p_h);
        serial_puts("ng: ipc ping/pong FAIL (create q)\r\n");
        return 1;
    }
    pp = st_port(pp_p_h);
    pq = st_port(pp_q_h);
    if (pp == NULL || pq == NULL || pp->depth != IPC_DEPTH_DEFAULT ||
        pp->slot != handle_slot_of(pp_p_h) ||
        pp->gen != handle_gen_of(pp_p_h) || handle_gen_of(pp_p_h) == 0) {
        (void)ipc_port_close(pp_p_h);
        (void)ipc_port_close(pp_q_h);
        serial_puts("ng: ipc ticket FAIL\r\n");
        return 1;
    }

    pt = thread_create(ping_thread, NULL);
    qt = thread_create(pong_thread, NULL);
    if (pt == NULL || qt == NULL) {
        (void)ipc_port_close(pp_p_h);
        (void)ipc_port_close(pp_q_h);
        serial_puts("ng: ipc ping/pong FAIL (thread)\r\n");
        return 1;
    }
    base = sched_reap_count();

    /* 等 P/Q 各自跑完 20 轮；失败时最多自旋 SELFTEST_SPIN_LIMIT 次。 */
    if (!st_wait_flag(&pp_p_done, SELFTEST_SPIN_LIMIT)) {
        pp_err = 5000;
    }
    if (!st_wait_flag(&pp_q_done, SELFTEST_SPIN_LIMIT)) {
        pp_err = 5001;
    }
    (void)st_wait_reaps(base, 2, SELFTEST_SPIN_LIMIT);
    (void)ipc_port_close(pp_q_h);
    (void)ipc_port_close(pp_p_h);

    ok = (pp_recv == PP_ROUNDS) && (pp_replies == PP_ROUNDS) && (pp_err == 0);
    if (ok) {
        serial_puts("ng: ipc ping/pong ok (n=20)\r\n");
        serial_puts("ng: ipc ticket ok\r\n");
    } else {
        serial_puts("ng: ipc ping/pong FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 2. 超时（recv 空 / send 满 / 非阻塞 EAGAIN）                                */
/* ------------------------------------------------------------------------- */

static u32 to_h;
static u32 to_full_h;
static volatile int to_recv_done;
static volatile int to_recv_rc;
static volatile int to_send_done;
static volatile int to_send_rc;

static void timeout_recv_thread(void *arg)
{
    u32 len = 0;

    (void)arg;
    to_recv_rc = ipc_port_recv(to_h, msg_b, IPC_MSG_MAX, TIMEOUT_TICKS, &len);
    to_recv_done = 1;
    thread_exit();
}

static void timeout_send_thread(void *arg)
{
    u32 len;

    (void)arg;
    len = st_build(msg_d, 0x54u, 0, 0, 0);
    to_send_rc = ipc_port_send(to_full_h, msg_d, len, TIMEOUT_TICKS);
    to_send_done = 1;
    thread_exit();
}

static int t_timeout(void)
{
    thread_t *rt;
    thread_t *st;
    u32 len = 0;
    u32 base;
    u64 t0;
    u64 t1;
    int ok = 1;
    int rc;
    u32 rlen = 0;

    to_recv_done = 0;
    to_recv_rc = 0;
    to_send_done = 0;
    to_send_rc = 0;
    base = sched_reap_count();

    rc = ipc_port_create(0, &to_h);
    if (rc != IPC_OK) {
        serial_puts("ng: ipc timeout FAIL (create)\r\n");
        return 1;
    }
    /* 非阻塞空队列：EAGAIN。 */
    rc = ipc_port_recv(to_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen);
    if (rc != IPC_EAGAIN) {
        ok = 0;
    }
    rt = thread_create(timeout_recv_thread, NULL);
    if (rt == NULL) {
        ok = 0;
    } else {
        t0 = sched_now();
        if (!st_wait_flag(&to_recv_done, SELFTEST_SPIN_LIMIT)) {
            ok = 0;
        }
        t1 = sched_now();
        if (to_recv_rc != IPC_ETIMEDOUT) {
            ok = 0;
        }
        if (t1 - t0 < TIMEOUT_TICKS) {
            ok = 0; /* 不是“立刻超时” */
        }
    }
    (void)ipc_port_close(to_h);

    /* 满队列的 send 超时。 */
    rc = ipc_port_create(1, &to_full_h);
    if (rc != IPC_OK) {
        serial_puts("ng: ipc timeout FAIL (create full)\r\n");
        return 1;
    }
    len = st_build(msg_a, 0x51u, 0, 1, 0);
    if (ipc_port_send(to_full_h, msg_a, len, IPC_TIMEOUT_NOW) != IPC_OK) {
        ok = 0;
    }
    if (ipc_port_send(to_full_h, msg_a, len, IPC_TIMEOUT_NOW) != IPC_EAGAIN) {
        ok = 0;
    }
    st = thread_create(timeout_send_thread, NULL);
    if (st == NULL) {
        ok = 0;
    } else {
        if (!st_wait_flag(&to_send_done, SELFTEST_SPIN_LIMIT)) {
            ok = 0;
        }
        if (to_send_rc != IPC_ETIMEDOUT) {
            ok = 0;
        }
    }
    rlen = 0;
    if (ipc_port_recv(to_full_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) !=
        (int)len) {
        ok = 0;
    }
    (void)ipc_port_close(to_full_h);
    (void)st_wait_reaps(base, 2, SELFTEST_SPIN_LIMIT);

    if (ok) {
        serial_puts("ng: ipc timeout ok\r\n");
    } else {
        serial_puts("ng: ipc timeout FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 3. 队列满 + 阻塞 send 被 recv 唤醒                                          */
/* ------------------------------------------------------------------------- */

static u32 qf_h;
static volatile int qf_done;
static volatile int qf_rc;

static void queue_full_sender(void *arg)
{
    u32 len;

    (void)arg;
    len = st_build(msg_c, 0x23u, 0, 3, 0);
    qf_rc = ipc_port_send(qf_h, msg_c, len, IPC_TIMEOUT_FOREVER);
    qf_done = 1;
    thread_exit();
}

static int t_queue_full(void)
{
    thread_t *st;
    u32 len;
    u32 rlen = 0;
    int ok = 1;
    int saw_blocked;
    u32 base;

    qf_done = 0;
    qf_rc = -1;

    if (ipc_port_create(2, &qf_h) != IPC_OK) {
        serial_puts("ng: ipc queue full FAIL (create)\r\n");
        return 1;
    }
    len = st_build(msg_a, 0x21u, 0, 1, 0);
    if (ipc_port_send(qf_h, msg_a, len, IPC_TIMEOUT_NOW) != IPC_OK) {
        ok = 0;
    }
    len = st_build(msg_b, 0x22u, 0, 2, 0);
    if (ipc_port_send(qf_h, msg_b, len, IPC_TIMEOUT_NOW) != IPC_OK) {
        ok = 0;
    }
    len = st_build(msg_c, 0x23u, 0, 3, 0);
    if (ipc_port_send(qf_h, msg_c, len, IPC_TIMEOUT_NOW) != IPC_EAGAIN) {
        ok = 0; /* 队列满 -> 非阻塞 send 必须 EAGAIN */
    }

    st = thread_create(queue_full_sender, NULL);
    if (st == NULL) {
        (void)ipc_port_close(qf_h);
        serial_puts("ng: ipc queue full FAIL (thread)\r\n");
        return 1;
    }
    base = sched_reap_count();
    saw_blocked = st_wait_state(st, THREAD_BLOCKED, SELFTEST_SPIN_LIMIT);
    if (!saw_blocked) {
        ok = 0;
    }
    /* 腾一个位置：recv 会唤醒阻塞的 sender。 */
    if (ipc_port_recv(qf_h, msg_d, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) < 0) {
        ok = 0;
    } else if (st_hdr(msg_d)->type != 0x21u) {
        ok = 0;
    }
    if (!st_wait_flag(&qf_done, SELFTEST_SPIN_LIMIT)) {
        ok = 0;
    }
    if (qf_rc != IPC_OK) {
        ok = 0;
    }
    (void)st_wait_reaps(base, 1, SELFTEST_SPIN_LIMIT);

    /* 剩下两条按 FIFO：0x22、0x23。 */
    rlen = 0;
    if (ipc_port_recv(qf_h, msg_d, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) < 0 ||
        st_hdr(msg_d)->type != 0x22u) {
        ok = 0;
    }
    rlen = 0;
    if (ipc_port_recv(qf_h, msg_d, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) < 0 ||
        st_hdr(msg_d)->type != 0x23u) {
        ok = 0;
    }
    (void)ipc_port_close(qf_h);

    if (ok) {
        serial_puts("ng: ipc queue full ok\r\n");
    } else {
        serial_puts("ng: ipc queue full FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 4. close 唤醒等待者                                                        */
/* ------------------------------------------------------------------------- */

static u32 cw_h;
static volatile int cw_done;
static volatile int cw_rc;

static void close_waiter(void *arg)
{
    u32 len = 0;

    (void)arg;
    cw_rc = ipc_port_recv(cw_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_FOREVER, &len);
    cw_done = 1;
    thread_exit();
}

static int t_close_wake(void)
{
    thread_t *wt;
    int ok = 1;
    u32 base;

    cw_done = 0;
    cw_rc = 0;

    if (ipc_port_create(0, &cw_h) != IPC_OK) {
        serial_puts("ng: ipc close wakes waiter FAIL (create)\r\n");
        return 1;
    }
    wt = thread_create(close_waiter, NULL);
    if (wt == NULL) {
        (void)ipc_port_close(cw_h);
        serial_puts("ng: ipc close wakes waiter FAIL (thread)\r\n");
        return 1;
    }
    base = sched_reap_count();
    if (!st_wait_state(wt, THREAD_BLOCKED, SELFTEST_SPIN_LIMIT)) {
        ok = 0;
    }
    if (ipc_port_close(cw_h) != IPC_OK) {
        ok = 0;
    }
    if (!st_wait_flag(&cw_done, SELFTEST_SPIN_LIMIT)) {
        ok = 0;
    }
    if (cw_rc != IPC_ECANCELED) {
        ok = 0;
    }
    (void)st_wait_reaps(base, 1, SELFTEST_SPIN_LIMIT);

    if (ok) {
        serial_puts("ng: ipc close wakes waiter ok\r\n");
    } else {
        serial_puts("ng: ipc close wakes waiter FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 5. 坏句柄 / 陈旧 generation / 类型不匹配                                    */
/* ------------------------------------------------------------------------- */

static int t_bad_handle(void)
{
    struct ipc_core *g = ipc_core_global();
    u32 stale = cw_h; /* close 过的 ticket */
    u32 d_h = 0;
    u32 e_h = 0;
    u32 th;
    void *marker = (void *)&g->ports[0];
    u32 len;
    u32 rlen = 0;
    int ok = 1;
    int rc;

    len = st_build(msg_a, 0x61u, 0, 0, 0);

    if (ipc_port_recv(stale, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) !=
        IPC_EBADF) {
        ok = 0;
    }
    if (ipc_port_send(stale, msg_a, len, IPC_TIMEOUT_NOW) != IPC_EBADF) {
        ok = 0;
    }
    if (ipc_port_reply(stale, msg_a, len, IPC_TIMEOUT_NOW) != IPC_EBADF) {
        ok = 0;
    }
    if (ipc_port_close(stale) != IPC_EBADF) {
        ok = 0;
    }
    if (ipc_port_recv(0, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) !=
        IPC_EBADF) {
        ok = 0;
    }
    if (ipc_port_recv(0xDEADBEEFu, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW,
                      &rlen) != IPC_EBADF) {
        ok = 0;
    }

    /* 槽位复用后 generation 必须变化：旧 ticket 仍 EBADF，新句柄可用。 */
    if (ipc_port_create(1, &d_h) != IPC_OK) {
        ok = 0;
    } else {
        u32 d_slot = handle_slot_of(d_h);

        if (ipc_port_close(d_h) != IPC_OK) {
            ok = 0;
        }
        if (ipc_port_create(1, &e_h) != IPC_OK) {
            ok = 0;
        } else {
            if (handle_slot_of(e_h) != d_slot ||
                handle_gen_of(e_h) == handle_gen_of(d_h)) {
                ok = 0;
            }
            rlen = 0;
            if (ipc_port_recv(d_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW,
                              &rlen) != IPC_EBADF) {
                ok = 0;
            }
            rlen = 0;
            if (ipc_port_recv(e_h, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW,
                              &rlen) != IPC_EAGAIN) {
                ok = 0;
            }
            (void)ipc_port_close(e_h);
        }
    }

    /* THREAD 类型句柄不能当端口用（类型校验）。 */
    rc = handle_alloc(&g->handles, HANDLE_TYPE_THREAD, marker, &th);
    if (rc != HANDLE_OK) {
        ok = 0;
    } else {
        rlen = 0;
        if (ipc_port_recv(th, msg_b, IPC_MSG_MAX, IPC_TIMEOUT_NOW, &rlen) !=
            IPC_EBADF) {
            ok = 0;
        }
        if (ipc_port_close(th) != IPC_EBADF) {
            ok = 0;
        }
        (void)handle_close(&g->handles, th, NULL, NULL);
    }

    if (ok) {
        serial_puts("ng: ipc bad handle rejected\r\n");
    } else {
        serial_puts("ng: ipc bad handle FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 6. 表/池上限                                                               */
/* ------------------------------------------------------------------------- */

static int t_limits(void)
{
    struct ipc_core *g = ipc_core_global();
    u32 ports[IPC_PORT_MAX];
    u32 handles[HANDLE_MAX];
    u32 nports = 0;
    u32 nhandles = 0;
    u32 h;
    u32 i;
    void *marker = (void *)&g->ports[0];
    int ok = 1;
    int rc;

    /* 端口池满：最多 IPC_PORT_MAX 个，再多 ENOSPC。 */
    for (;;) {
        rc = ipc_port_create(1, &h);
        if (rc == IPC_OK) {
            if (nports >= IPC_PORT_MAX) {
                (void)ipc_port_close(h);
                ok = 0;
                break;
            }
            ports[nports++] = h;
            continue;
        }
        if (rc != IPC_ENOSPC) {
            ok = 0;
        }
        break;
    }
    if (nports != IPC_PORT_MAX) {
        ok = 0;
    }
    for (i = 0; i < nports; i++) {
        if (ipc_port_close(ports[i]) != IPC_OK) {
            ok = 0;
        }
    }

    /* 句柄表满：PORT 之外用 THREAD 类型填满（本 WP 只是预留类型）。 */
    for (;;) {
        rc = handle_alloc(&g->handles, HANDLE_TYPE_THREAD, marker, &h);
        if (rc != HANDLE_OK) {
            if (rc != HANDLE_ENOSPC) {
                ok = 0;
            }
            break;
        }
        if (nhandles >= HANDLE_MAX) {
            ok = 0;
            break;
        }
        handles[nhandles++] = h;
    }
    if (nhandles != HANDLE_MAX) {
        ok = 0;
    }
    for (i = 0; i < nhandles; i++) {
        if (handle_close(&g->handles, handles[i], NULL, NULL) != HANDLE_OK) {
            ok = 0;
        }
    }
    if (handle_live(&g->handles) != 0) {
        ok = 0;
    }

    if (ok) {
        serial_puts("ng: ipc handle table ok (slots=64 ports=16)\r\n");
    } else {
        serial_puts("ng: ipc handle table FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 7. 用户指针（copyin/copyout + SMAP）                                       */
/* ------------------------------------------------------------------------- */

static int t_user_ptr(void)
{
    u64 uva = USER_TEST_VA;
    u8 *m = (u8 *)(usize)uva;
    u8 *rb = (u8 *)(usize)(USER_TEST_VA + 2048ULL);
    struct ipc_msg_hdr *h;
    u32 *pl;
    u32 u_h = 0;
    u32 len;
    int ok = 1;
    u64 rc;

    if (ipc_port_create(0, &u_h) != IPC_OK) {
        serial_puts("ng: ipc user ptr FAIL (create)\r\n");
        return 1;
    }
    len = IPC_HDR_SIZE + 8u;

    /* 以“用户”身份写消息头：SMAP 开着时内核直接写用户页会 #PF，必须 stac。 */
    if (pg_smap_on()) {
        stac();
    }
    st_zero(m, 64);
    h = (struct ipc_msg_hdr *)m;
    h->type = 0x7u;
    h->len = 8;
    h->from = u_h;
    h->flags = 0;
    pl = (u32 *)(m + IPC_HDR_SIZE);
    pl[0] = 0x12345678u;
    pl[1] = 0xA5A5A5A5u;
    if (pg_smap_on()) {
        clac();
    }

    rc = ipc_sys_send(u_h, uva, len, IPC_TIMEOUT_NOW);
    if (rc != 0) {
        ok = 0;
    }
    rc = ipc_sys_recv(u_h, USER_TEST_VA + 2048ULL, 128, IPC_TIMEOUT_NOW);
    if (rc != (u64)len) {
        ok = 0;
    } else {
        if (pg_smap_on()) {
            stac();
        }
        h = (struct ipc_msg_hdr *)rb;
        pl = (u32 *)(rb + IPC_HDR_SIZE);
        if (h->type != 0x7u || h->len != 8 || h->from != u_h ||
            pl[0] != 0x12345678u || pl[1] != 0xA5A5A5A5u) {
            ok = 0;
        }
        if (pg_smap_on()) {
            clac();
        }
    }

    /* 内核地址 / null / 0 长度：syscall 层必须拒绝。 */
    rc = ipc_sys_send(u_h, (u64)(usize)&ok, len, IPC_TIMEOUT_NOW);
    if (rc != (u64)(i64)IPC_EFAULT) {
        ok = 0;
    }
    rc = ipc_sys_recv(u_h, 0, 128, IPC_TIMEOUT_NOW);
    if (rc != (u64)(i64)IPC_EFAULT) {
        ok = 0;
    }
    rc = ipc_sys_recv(u_h, USER_TEST_VA + 2048ULL, 0, IPC_TIMEOUT_NOW);
    if (rc != (u64)(i64)IPC_EINVAL) {
        ok = 0;
    }
    if (ipc_sys_close(u_h) != 0) {
        ok = 0;
    }

    if (ok) {
        serial_puts("ng: ipc user ptr ok\r\n");
    } else {
        serial_puts("ng: ipc user ptr FAIL\r\n");
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int ipc_selftest(void)
{
    struct ipc_core *g = ipc_core_global();
    int fails = 0;

    ipc_init(); /* 幂等：main 早先已初始化，这里保证表/统计从零开始 */

    fails += t_ping_pong();
    fails += t_timeout();
    fails += t_queue_full();
    fails += t_close_wake();
    fails += t_bad_handle();
    fails += t_limits();
    fails += t_user_ptr();

    serial_puts("ng: ipc stat sends=");
    serial_putdec(g->sends);
    serial_puts(" recvs=");
    serial_putdec(g->recvs);
    serial_puts(" replies=");
    serial_putdec(g->replies);
    serial_puts(" timeouts=");
    serial_putdec(g->timeouts);
    serial_puts(" eagains=");
    serial_putdec(g->eagains);
    serial_puts(" bad=");
    serial_putdec(g->bad_handles);
    serial_puts(" live=");
    serial_putdec((u64)handle_live(&g->handles));
    serial_puts("\r\n");

    return fails;
}
