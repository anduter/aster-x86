/* =============================================================================
 * net/tcp.c - 最小但"完整"的 TCP：客户端 + 服务端 listen/accept
 *
 * 目标不是实现完整 TCP，而是把"能用的 TCP"所需机制都放进来并逐项可回归：
 *
 *   - 序列号/确认号：snd_una/snd_nxt/rcv_nxt，纯 ACK 不推进；
 *   - MSS/窗口：MSS=536，通告窗口 2048（接收缓冲 2048）；
 *   - 校验和：伪首部 + 16 位反码和，坏校验和直接丢（计数）；
 *   - RTT/RTO：srtt/rttvar（RFC6298 简版），RTO 钳在 [10, 200] ticks；
 *   - 重传 + 指数退避：超时 cwnd 回到 1 MSS、RTO 翻倍；
 *   - 乱序重组：最多 4 个乱序段缓存，收到缺口段后按序交付；
 *   - FIN / RST / TIME_WAIT：四次挥手（简化）+ 2*MSL 的 TIME_WAIT；
 *   - 基本拥塞：慢启动 cwnd 翻倍、超时 ssthresh 减半；
 *   - 丢包注入：`loss_every=N` 时每第 N 个数据段本地丢弃一次，用来在真实
 *     回显链路上强制触发重传（QEMU slirp 是用户态网关，宿主侧无法造真丢包）；
 *   - 服务端（阶段 4d）：listener + SYN 队列（backlog）+ 完成队列 + accept，
 *     接受连接后支持有界阻塞 read/write/close，端口在 TIME_WAIT 内被占用；
 *     本地回环（目的 IP = 本机）让"客户端连本机服务端"在 QEMU slirp 下可测。
 *
 * 发送是"同步收发"（一个 512B 段在途），所以窗口/cwnd 主要作为状态与回归证据；
 * 文档里如实说明（docs/network.md、docs/claims/net.md）。
 * ============================================================================= */
#include "include/net.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/dev.h"
#include "net_syscall.h"

#define TCP_FIN     0x01U
#define TCP_SYN     0x02U
#define TCP_RST     0x04U
#define TCP_PSH     0x08U
#define TCP_ACK     0x10U

#define TCP_MSS         536U
#define TCP_RECV_MAX    2048U
#define TCP_OOO_MAX     4U
#define TCP_RTO_MIN     10U      /* ticks（100Hz -> 100ms） */
#define TCP_RTO_INIT    20U      /* 200ms */
#define TCP_RTO_MAX     200U     /* 2s */
#define TCP_MAX_RETRIES 6U
#define TCP_TIME_WAIT   50U      /* 2*MSL 简化成 500ms */

/* ---- 服务端（listen/accept） ---- */
#define TCP_SRV_MAX        4U    /* 同时存在的已建立/待关闭连接 */
#define TCP_SOCK_MAX       8U    /* socket 槽：监听 + 已接受连接 */
#define TCP_SRV_TW_TICKS   20U   /* 服务端 2*MSL 简化：200ms（端口占用窗口） */
#define TCP_HALFOPEN_TTL   300U  /* 半开/完成队列条目最长存活 3s */

/* 连接状态（服务端） */
#define TCP_SRV_CLOSED     0
#define TCP_SRV_ESTAB      1
#define TCP_SRV_CLOSE_WAIT 2
#define TCP_SRV_FIN_WAIT   3
#define TCP_SRV_TIME_WAIT  4

typedef struct {
    uint32_t seq;
    uint32_t ack;
    uint8_t  flags;
    uint16_t wnd;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  src_ip[4];
    const uint8_t *data;
    uint32_t len;
} tcp_seg_t;

typedef struct {
    uint32_t seq;
    uint32_t len;
    uint8_t  data[TCP_MSS];
    bool     used;
} tcp_ooo_t;

/* 客户端连接（一次只有一条在途：自检/用户 SYS_TCP_TEST 共用） */
typedef struct {
    int      state;             /* 0=closed 1=syn_sent 2=established 3=fin_wait 4=time_wait */
    bool     synthetic;         /* 单测模式：不真的收发包 */
    uint16_t lport;
    uint16_t peer_port;
    uint8_t  peer_ip[4];

    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    uint32_t srtt, rttvar, rto;
    uint32_t rtt_valid;
    uint32_t cwnd, ssthresh;
    uint32_t dup_acks;
    uint32_t loss_every;
    uint32_t tx_data_segs;
    uint32_t retransmits;
    uint32_t timeouts;
    uint32_t bad_checksum;
    uint32_t dropped_tx;
    uint8_t  peer_wnd_hi;

    uint8_t  rbuf[TCP_RECV_MAX];
    uint32_t rlen;
    bool     peer_fin;

    tcp_ooo_t ooo[TCP_OOO_MAX];
} tcp_conn_t;

/* 半开连接（SYN 已收、三次握手未完成）与已完成握手（等待 accept）共用布局 */
typedef struct {
    bool     used;
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint32_t iss;
    uint32_t snd_nxt;           /* iss + 1 */
    uint32_t rcv_nxt;           /* 对端 SYN seq + 1 */
    uint32_t created;           /* timer_ticks()，用于半开超时清理 */
} tcp_hs_t;

/* socket 槽：未绑定 / 监听 / 已接受连接三态 */
typedef struct {
    bool     used;
    int      owner_pid;         /* 0 = 内核持有（启动自检）；>0 = 用户进程 fd */
    int      owner_fd;
    uint16_t port;              /* 0 = 未绑定 */
    bool     listening;
    uint32_t backlog;           /* 1..NET_TCP_BACKLOG_MAX */
    int      conn;              /* >=0：已接受连接在 srv[] 的下标 */
    uint32_t syn_count;         /* 当前半开连接数（<= backlog） */
    uint32_t accept_count;      /* 已完成、等待 accept 的连接数 */
    uint32_t overflow;          /* backlog 溢出被丢弃的 SYN/完成 ACK 计数 */
    tcp_hs_t synq[NET_TCP_BACKLOG_MAX];
    tcp_hs_t acceptq[NET_TCP_BACKLOG_MAX];
} tcp_sock_t;

/* 服务端已建立连接 */
typedef struct {
    bool     used;
    int      state;
    bool     closed;            /* close 句柄已用掉：read/write 不再可用 */
    bool     fin_sent;
    bool     peer_fin;
    uint16_t lport;
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t tw_until;
    int      owner_pid;
    int      owner_fd;
    uint32_t rlen;
    uint8_t  rbuf[TCP_RECV_MAX];
} tcp_srv_t;

static tcp_conn_t conn;
static tcp_sock_t socks[TCP_SOCK_MAX];
static tcp_srv_t  srv[TCP_SRV_MAX];
static uint32_t   tcp_iss_next = 0x50000U;

/* -------------------------------------------------------------------------
 * 校验和
 * ------------------------------------------------------------------------- */
static uint32_t csum_add(const uint8_t *p, uint32_t n)
{
    uint32_t sum = 0;

    while (n > 1U) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n != 0U) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t fold_sum(uint32_t sum)
{
    while ((sum >> 16) != 0U) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_csum(const uint8_t src[4], const uint8_t dst[4],
                         const uint8_t *tcp, uint32_t len)
{
    uint32_t sum = 0;

    sum += csum_add(src, 4);
    sum += csum_add(dst, 4);
    sum += 6U;                 /* protocol TCP */
    sum += len;                /* TCP 长度（含首部） */
    sum += csum_add(tcp, len);
    return fold_sum(sum);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* -------------------------------------------------------------------------
 * 发送
 * ------------------------------------------------------------------------- */
static void tcp_build(uint8_t *tcp, uint16_t lport, uint16_t dport,
                      uint32_t seq, uint32_t ack, uint8_t flags,
                      const uint8_t *data, uint32_t len)
{
    memset(tcp, 0, 20U + len);
    wr16(tcp + 0, lport);
    wr16(tcp + 2, dport);
    wr32(tcp + 4, seq);
    wr32(tcp + 8, ack);
    tcp[12] = 0x50;
    tcp[13] = flags;
    wr16(tcp + 14, (uint16_t)TCP_RECV_MAX);
    if (len != 0U && data != 0) memcpy(tcp + 20, data, len);
}

/* 通用发送：任意本地端口 -> 任意对端（客户端/服务端/监听器共用） */
static int tcp_send_seg(uint16_t lport, const uint8_t dst_ip[4], uint16_t dport,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        const uint8_t *data, uint32_t len)
{
    uint8_t tcp[20U + TCP_MSS];
    uint16_t cs;

    if (len > TCP_MSS) len = TCP_MSS;
    tcp_build(tcp, lport, dport, seq, ack, flags, data, len);
    cs = tcp_csum(net_local_ip_ptr(), dst_ip, tcp, 20U + len);
    wr16(tcp + 16, cs);
    return net_ip_send(dst_ip, 6, tcp, 20U + len);
}

/* 没有匹配连接/监听器时给对端回 RST（避免半开连接一直重试） */
static void tcp_rst_to(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t lport,
                       uint32_t seq, uint32_t ack)
{
    (void)tcp_send_seg(lport, dst_ip, dst_port, seq, ack, TCP_RST, 0, 0);
}

static int tcp_xmit(uint32_t seq, uint32_t ack, uint8_t flags,
                    const uint8_t *data, uint32_t len)
{
    if (conn.synthetic) return 0;
    return tcp_send_seg(conn.lport, conn.peer_ip, conn.peer_port,
                        seq, ack, flags, data, len);
}

/* 数据段发送带丢包注入：第 N 个数据段本地丢弃（模拟路径丢包 -> 触发重传） */
static int tcp_xmit_data(uint32_t seq, uint32_t ack, uint8_t flags,
                         const uint8_t *data, uint32_t len)
{
    conn.tx_data_segs++;
    if (conn.loss_every != 0U && (conn.tx_data_segs % conn.loss_every) == 0U) {
        conn.dropped_tx++;
        return 0;                 /* 假装发出去了，实际丢掉 */
    }
    return tcp_xmit(seq, ack, flags, data, len);
}

/* -------------------------------------------------------------------------
 * 接收：解析一个以太网帧里的 TCP 段（不按连接过滤，交给分发）
 * ------------------------------------------------------------------------- */
static int tcp_parse_frame(const uint8_t *f, uint32_t len, tcp_seg_t *out)
{
    const uint8_t *ip;
    const uint8_t *tcp;
    uint32_t ihl;
    uint32_t tlen;
    uint32_t off;

    if (len < 14U + 20U + 20U || rd16(f + 12) != 0x0800U) return -1;
    ip = f + 14;
    if ((ip[0] >> 4) != 4U || ip[9] != 6U) return -1;
    ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
    if (ihl < 20U || 14U + ihl + 20U > len) return -1;
    if (memcmp(ip + 16, net_local_ip_ptr(), 4) != 0) return -1;   /* 只收发给本机的 */
    tcp = ip + ihl;

    tlen = (uint32_t)rd16(ip + 2);
    if (tlen < ihl + 20U || 14U + tlen > len) return -1;
    off = (uint32_t)(tcp[12] >> 4) * 4U;
    if (off < 20U || ihl + off > tlen) return -1;

    out->src_port = rd16(tcp + 0);
    out->dst_port = rd16(tcp + 2);
    memcpy(out->src_ip, ip + 12, 4);
    out->seq = rd32(tcp + 4);
    out->ack = rd32(tcp + 8);
    out->flags = tcp[13];
    out->wnd = rd16(tcp + 14);
    out->data = tcp + off;
    out->len = tlen - ihl - off;
    if (out->len > 1480U) out->len = 1480U;

    if (tcp_csum(ip + 12, ip + 16, tcp, tlen - ihl) != 0U) {
        conn.bad_checksum++;
        return -1;                /* 校验和错：丢弃 */
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 客户端：收到的数据按序交给应用缓冲；乱序段进 ooo 队列
 * ------------------------------------------------------------------------- */
static void tcp_deliver(const uint8_t *data, uint32_t len)
{
    uint32_t room = TCP_RECV_MAX - conn.rlen;

    if (len > room) len = room;
    memcpy(conn.rbuf + conn.rlen, data, len);
    conn.rlen += len;
}

/* 把 ooo 队列里能从 rcv_nxt 接上的段按序交付 */
static void tcp_drain_ooo(void)
{
    int progress = 1;

    while (progress) {
        progress = 0;
        for (uint32_t i = 0; i < TCP_OOO_MAX; i++) {
            if (!conn.ooo[i].used) continue;
            if (conn.ooo[i].seq == conn.rcv_nxt) {
                tcp_deliver(conn.ooo[i].data, conn.ooo[i].len);
                conn.rcv_nxt += conn.ooo[i].len;
                conn.ooo[i].used = false;
                progress = 1;
            }
        }
    }
}

static void tcp_ooo_push(uint32_t seq, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < TCP_OOO_MAX; i++) {
        if (conn.ooo[i].used && conn.ooo[i].seq == seq) return;   /* 重复 */
    }
    for (uint32_t i = 0; i < TCP_OOO_MAX; i++) {
        if (!conn.ooo[i].used) {
            conn.ooo[i].seq = seq;
            conn.ooo[i].len = (len > TCP_MSS) ? TCP_MSS : len;
            memcpy(conn.ooo[i].data, data, conn.ooo[i].len);
            conn.ooo[i].used = true;
            return;
        }
    }
}

/* RTT 采样（Karn：重传过的段不采样由调用方保证） */
static void tcp_rtt_sample(uint32_t rtt)
{
    if (rtt == 0U) rtt = 1U;
    if (!conn.rtt_valid) {
        conn.srtt = rtt;
        conn.rttvar = rtt / 2U;
        conn.rtt_valid = 1;
    } else {
        uint32_t delta = (conn.srtt > rtt) ? (conn.srtt - rtt) : (rtt - conn.srtt);
        conn.rttvar = (3U * conn.rttvar + delta) / 4U;
        conn.srtt = (7U * conn.srtt + rtt) / 8U;
    }
    {
        uint32_t rto = conn.srtt + 4U * conn.rttvar;

        if (rto < TCP_RTO_MIN) rto = TCP_RTO_MIN;
        if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
        conn.rto = rto;
    }
}

static void tcp_ack_received(uint32_t ack)
{
    if (ack == conn.snd_una) {
        conn.dup_acks++;
        return;
    }
    if ((int32_t)(ack - conn.snd_una) > 0) {
        conn.dup_acks = 0;
        conn.snd_una = ack;
        /* 慢启动/拥塞避免的简化版 */
        if (conn.cwnd < conn.ssthresh) conn.cwnd += TCP_MSS;
        else conn.cwnd += (TCP_MSS * TCP_MSS) / conn.cwnd;
        if (conn.cwnd > TCP_RECV_MAX) conn.cwnd = TCP_RECV_MAX;
    }
}

/* 处理一个段（客户端；真实收包与单测共用）。返回 0 = 已处理，-1 = 被重置。 */
static int tcp_input(const tcp_seg_t *s)
{
    if ((s->flags & TCP_RST) != 0U) {
        conn.state = 0;
        return -1;
    }

    if ((s->flags & TCP_SYN) != 0U) {
        conn.rcv_nxt = s->seq + 1U;
        conn.snd_una = s->ack;
        conn.state = 2;                     /* ESTABLISHED */
        (void)tcp_xmit(conn.snd_nxt, conn.rcv_nxt, TCP_ACK, 0, 0);
        return 0;
    }

    if ((s->flags & TCP_ACK) != 0U) tcp_ack_received(s->ack);

    if (s->len != 0U) {
        if (s->seq == conn.rcv_nxt) {
            tcp_deliver(s->data, s->len);
            conn.rcv_nxt += s->len;
            tcp_drain_ooo();
            (void)tcp_xmit(conn.snd_nxt, conn.rcv_nxt, TCP_ACK, 0, 0);
        } else if ((int32_t)(s->seq - conn.rcv_nxt) < 0) {
            /* 完全重复：只回 ACK */
            (void)tcp_xmit(conn.snd_nxt, conn.rcv_nxt, TCP_ACK, 0, 0);
        } else {
            /* 乱序：缓存 + 重复 ACK（让对端快速重传缺口） */
            tcp_ooo_push(s->seq, s->data, s->len);
            (void)tcp_xmit(conn.snd_nxt, conn.rcv_nxt, TCP_ACK, 0, 0);
        }
    }

    if ((s->flags & TCP_FIN) != 0U) {
        conn.peer_fin = true;
        if (s->seq + s->len == conn.rcv_nxt) conn.rcv_nxt++;
        (void)tcp_xmit(conn.snd_nxt, conn.rcv_nxt, TCP_ACK, 0, 0);
    }

    if ((s->flags & TCP_ACK) != 0U && conn.state == 3 && conn.snd_una == conn.snd_nxt) {
        conn.state = 4;                     /* FIN 被 ACK -> TIME_WAIT */
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 服务端连接 / socket 表
 * ------------------------------------------------------------------------- */
static tcp_sock_t *sock_get(int id)
{
    if (id < 0 || id >= (int)TCP_SOCK_MAX) return 0;
    if (!socks[id].used) return 0;
    return &socks[id];
}

static tcp_srv_t *srv_get(int id)
{
    if (id < 0 || id >= (int)TCP_SRV_MAX) return 0;
    if (!srv[id].used) return 0;
    return &srv[id];
}

/* 释放一条已建立连接：同时断开引用它的 socket 槽 */
static void tcp_srv_free(tcp_srv_t *c)
{
    int id = (int)(c - srv);

    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        if (socks[i].used && socks[i].conn == id) socks[i].conn = -1;
    }
    memset(c, 0, sizeof(*c));
}

static tcp_srv_t *tcp_srv_alloc(uint16_t lport, const tcp_hs_t *hs)
{
    for (uint32_t i = 0; i < TCP_SRV_MAX; i++) {
        tcp_srv_t *c = &srv[i];

        if (c->used) continue;
        memset(c, 0, sizeof(*c));
        c->used = true;
        c->state = TCP_SRV_ESTAB;
        c->lport = lport;
        memcpy(c->peer_ip, hs->peer_ip, 4);
        c->peer_port = hs->peer_port;
        c->iss = hs->iss;
        c->snd_una = hs->snd_nxt;
        c->snd_nxt = hs->snd_nxt;
        c->rcv_nxt = hs->rcv_nxt;
        c->owner_fd = -1;
        return c;
    }
    return 0;
}

/* 端口是否已被占用：监听器 / 已建立连接 / TIME_WAIT 连接都算（重复 bind 拒绝） */
static int tcp_port_busy(uint16_t port)
{
    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        if (socks[i].used && socks[i].listening && socks[i].port == port) return 1;
    }
    for (uint32_t i = 0; i < TCP_SRV_MAX; i++) {
        if (srv[i].used && srv[i].lport == port) return 1;
    }
    return 0;
}

static tcp_sock_t *tcp_listen_match(uint16_t port)
{
    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        if (socks[i].used && socks[i].listening && socks[i].port == port) return &socks[i];
    }
    return 0;
}

static tcp_srv_t *tcp_srv_match(const tcp_seg_t *s)
{
    for (uint32_t i = 0; i < TCP_SRV_MAX; i++) {
        tcp_srv_t *c = &srv[i];

        if (!c->used) continue;
        if (c->lport != s->dst_port || c->peer_port != s->src_port) continue;
        if (memcmp(c->peer_ip, s->src_ip, 4) != 0) continue;
        return c;
    }
    return 0;
}

/* 半开/完成队列的超时清理（对端崩掉不会永久占 backlog） */
static void tcp_listen_expire(tcp_sock_t *l, uint32_t now)
{
    for (uint32_t i = 0; i < NET_TCP_BACKLOG_MAX; i++) {
        if (l->synq[i].used && (now - l->synq[i].created) > TCP_HALFOPEN_TTL) {
            l->synq[i].used = false;
            if (l->syn_count != 0U) l->syn_count--;
        }
    }
    for (uint32_t i = 0; i < l->accept_count; ) {
        if ((now - l->acceptq[i].created) > TCP_HALFOPEN_TTL) {
            for (uint32_t j = i + 1U; j < l->accept_count; j++) l->acceptq[j - 1U] = l->acceptq[j];
            l->accept_count--;
        } else {
            i++;
        }
    }
}

/* 回收：超时 TIME_WAIT 连接 + 半开条目 */
static void tcp_reap(void)
{
    uint32_t now = timer_ticks();

    for (uint32_t i = 0; i < TCP_SRV_MAX; i++) {
        tcp_srv_t *c = &srv[i];

        if (!c->used) continue;
        if (c->state == TCP_SRV_TIME_WAIT && (int32_t)(now - c->tw_until) >= 0) tcp_srv_free(c);
    }
    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        if (socks[i].used && socks[i].listening) tcp_listen_expire(&socks[i], now);
    }
}

/* 服务端收包：ACK 推进 / 数据入缓冲 / FIN -> CLOSE_WAIT */
static int tcp_srv_input(tcp_srv_t *c, const tcp_seg_t *s)
{
    if ((s->flags & TCP_RST) != 0U) {
        tcp_srv_free(c);
        return -1;
    }

    if ((s->flags & TCP_ACK) != 0U) {
        if ((int32_t)(s->ack - c->snd_una) > 0) c->snd_una = s->ack;
        if (c->state == TCP_SRV_FIN_WAIT && (int32_t)(c->snd_una - c->snd_nxt) >= 0) {
            c->state = TCP_SRV_TIME_WAIT;
            c->tw_until = timer_ticks() + TCP_SRV_TW_TICKS;
        }
    }

    if (s->len != 0U) {
        if (s->seq == c->rcv_nxt) {
            uint32_t room = TCP_RECV_MAX - c->rlen;
            uint32_t n = s->len;

            if (n > room) n = room;
            memcpy(c->rbuf + c->rlen, s->data, n);
            c->rlen += n;
            c->rcv_nxt += n;
        }
        (void)tcp_send_seg(c->lport, c->peer_ip, c->peer_port,
                           c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
    }

    if ((s->flags & TCP_FIN) != 0U) {
        if (s->seq + s->len == c->rcv_nxt) c->rcv_nxt++;
        c->peer_fin = true;
        if (c->state == TCP_SRV_ESTAB) c->state = TCP_SRV_CLOSE_WAIT;
        (void)tcp_send_seg(c->lport, c->peer_ip, c->peer_port,
                           c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
    }
    return 0;
}

/* 收到新 SYN：进 SYN 队列（backlog 满就丢弃并计数），回 SYN-ACK */
static void tcp_listen_syn(tcp_sock_t *l, const tcp_seg_t *s)
{
    tcp_hs_t *h = 0;

    for (uint32_t i = 0; i < NET_TCP_BACKLOG_MAX; i++) {
        if (l->synq[i].used && l->synq[i].peer_port == s->src_port &&
            memcmp(l->synq[i].peer_ip, s->src_ip, 4) == 0) {
            /* 对端重传 SYN：重发 SYN-ACK，不占新槽 */
            (void)tcp_send_seg(l->port, s->src_ip, s->src_port,
                               l->synq[i].iss, l->synq[i].rcv_nxt,
                               TCP_SYN | TCP_ACK, 0, 0);
            return;
        }
    }

    if (l->syn_count >= l->backlog) {          /* backlog 溢出：丢弃 */
        l->overflow++;
        return;
    }
    for (uint32_t i = 0; i < NET_TCP_BACKLOG_MAX; i++) {
        if (!l->synq[i].used) { h = &l->synq[i]; break; }
    }
    if (h == 0) {
        l->overflow++;
        return;
    }

    memset(h, 0, sizeof(*h));
    h->used = true;
    memcpy(h->peer_ip, s->src_ip, 4);
    h->peer_port = s->src_port;
    h->rcv_nxt = s->seq + 1U;
    h->iss = tcp_iss_next;
    tcp_iss_next += 0x1000U;
    h->snd_nxt = h->iss + 1U;
    h->created = timer_ticks();
    l->syn_count++;

    (void)tcp_send_seg(l->port, h->peer_ip, h->peer_port,
                       h->iss, h->rcv_nxt, TCP_SYN | TCP_ACK, 0, 0);
}

/* 三次握手的最后一个 ACK：半开 -> 完成队列（等 accept 取走） */
static bool tcp_listen_complete(tcp_sock_t *l, const tcp_seg_t *s)
{
    for (uint32_t i = 0; i < NET_TCP_BACKLOG_MAX; i++) {
        tcp_hs_t *h = &l->synq[i];

        if (!h->used) continue;
        if (h->peer_port != s->src_port || memcmp(h->peer_ip, s->src_ip, 4) != 0) continue;
        if (s->ack != h->iss + 1U) return false;

        if (l->accept_count >= l->backlog) {   /* 完成队列也满：丢掉这次 ACK */
            l->overflow++;
            h->used = false;
            if (l->syn_count != 0U) l->syn_count--;
            return true;
        }
        l->acceptq[l->accept_count] = *h;
        l->accept_count++;
        h->used = false;
        if (l->syn_count != 0U) l->syn_count--;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 帧分发：已建立连接 -> 监听器（半开/完成握手）-> 客户端连接
 * ------------------------------------------------------------------------- */
static void tcp_frame_input(const uint8_t *f, uint32_t len)
{
    tcp_seg_t s;
    tcp_srv_t *c;
    tcp_sock_t *l;

    if (tcp_parse_frame(f, len, &s) != 0) return;

    c = tcp_srv_match(&s);
    if (c != 0) {
        (void)tcp_srv_input(c, &s);
        return;
    }

    l = tcp_listen_match(s.dst_port);
    if (l != 0) {
        if (((s.flags & TCP_SYN) != 0U) && ((s.flags & TCP_ACK) == 0U)) {
            tcp_listen_syn(l, &s);
            return;
        }
        if (((s.flags & TCP_ACK) != 0U) && ((s.flags & TCP_SYN) == 0U)) {
            if (tcp_listen_complete(l, &s)) return;
        }
        /* 监听端口上没有对应连接的数据/FIN：回 RST */
        if (s.len != 0U || (s.flags & TCP_FIN) != 0U) {
            tcp_rst_to(s.src_ip, s.src_port, s.dst_port, s.ack, s.seq + s.len);
        }
        return;
    }

    if (conn.state != 0 && conn.lport == s.dst_port && conn.peer_port == s.src_port &&
        memcmp(conn.peer_ip, s.src_ip, 4) == 0) {
        (void)tcp_input(&s);
        return;
    }
    /* 谁也不认识：静默丢弃（真实网卡上可能有广播/别的会话的包） */
}

/* 收一帧并分发；返回 1 = 处理了一帧，0 = 没有帧 */
static int tcp_poll_once(void)
{
    uint8_t f[1600];
    int n;

    if (conn.synthetic) return 0;
    n = net_rx_frame(f, sizeof(f));
    if (n <= 0) return 0;
    tcp_frame_input(f, (uint32_t)n);
    return 1;
}

/* 把当前能收的帧都处理掉（有界：一次最多 64 帧） */
static void tcp_drain(void)
{
    for (uint32_t i = 0; i < 64U; i++) {
        if (tcp_poll_once() == 0) break;
    }
}

/* -------------------------------------------------------------------------
 * 客户端：同步发送一段并等 ACK（含 RTO 重传 + 指数退避 + 3 次重复 ACK 快重传）
 * ------------------------------------------------------------------------- */
static int tcp_send_wait(uint32_t seq, uint8_t flags,
                         const uint8_t *data, uint32_t len)
{
    uint32_t start;
    uint32_t retries = 0;
    bool retransmitted = false;

    if (tcp_xmit_data(seq, conn.rcv_nxt, flags, data, len) != 0) return -1;
    start = timer_ticks();

    for (;;) {
        uint32_t now = timer_ticks();

        while (tcp_poll_once()) {
            if (conn.state == 0) return -1;
            if ((int32_t)(conn.snd_una - (seq + len)) >= 0) {
                if (!retransmitted) tcp_rtt_sample(now - start);
                return 0;
            }
            if (conn.dup_acks >= 3U && !retransmitted) {
                retransmitted = true;
                conn.retransmits++;
                conn.ssthresh = (conn.cwnd / 2U < 2U * TCP_MSS) ? 2U * TCP_MSS : conn.cwnd / 2U;
                conn.cwnd = conn.ssthresh;
                (void)tcp_xmit(seq, conn.rcv_nxt, flags, data, len);
                start = now;
            }
        }

        if ((int32_t)(conn.snd_una - (seq + len)) >= 0) {
            if (!retransmitted) tcp_rtt_sample(now - start);
            if (conn.rtt_valid) {
                uint32_t r = conn.srtt + 4U * conn.rttvar;
                if (r < TCP_RTO_MIN) r = TCP_RTO_MIN;
                if (r > TCP_RTO_MAX) r = TCP_RTO_MAX;
                conn.rto = r;
            }
            return 0;
        }
        if ((now - start) >= conn.rto) {
            if (++retries > TCP_MAX_RETRIES) return -1;
            conn.timeouts++;
            conn.retransmits++;
            retransmitted = true;
            conn.ssthresh = (conn.cwnd / 2U < 2U * TCP_MSS) ? 2U * TCP_MSS : conn.cwnd / 2U;
            conn.cwnd = TCP_MSS;
            conn.rto = (conn.rto * 2U > TCP_RTO_MAX) ? TCP_RTO_MAX : conn.rto * 2U;
            if (tcp_xmit(seq, conn.rcv_nxt, flags, data, len) != 0) return -1;
            start = now;
        }
        __asm__ volatile ("pause");
    }
}

/* -------------------------------------------------------------------------
 * 对外：客户端连接 / 发送 / 接收 / 关闭
 * ------------------------------------------------------------------------- */
static void tcp_reset_state(uint16_t lport, const uint8_t peer_ip[4], uint16_t peer_port)
{
    memset(&conn, 0, sizeof(conn));
    conn.lport = lport;
    conn.peer_port = peer_port;
    if (peer_ip != 0) memcpy(conn.peer_ip, peer_ip, 4);
    conn.iss = 0x30000U + (uint32_t)(timer_ticks() & 0xFFFFU);
    conn.snd_una = conn.iss;
    conn.snd_nxt = conn.iss + 1U;
    conn.rto = TCP_RTO_INIT;
    conn.cwnd = TCP_MSS;
    conn.ssthresh = TCP_RECV_MAX;
}

static int tcp_connect(const uint8_t peer_ip[4], uint16_t peer_port, uint16_t lport)
{
    tcp_reset_state(lport, peer_ip, peer_port);
    conn.state = 1;                         /* SYN_SENT */

    if (tcp_xmit(conn.iss, 0, TCP_SYN, 0, 0) != 0) return -1;
    {
        uint32_t start = timer_ticks();
        uint32_t retries = 0;

        for (;;) {
            tcp_poll_once();
            if (conn.state == 2) return 0;
            if ((timer_ticks() - start) >= conn.rto) {
                if (++retries > TCP_MAX_RETRIES) return -1;
                conn.retransmits++;
                conn.rto = (conn.rto * 2U > TCP_RTO_MAX) ? TCP_RTO_MAX : conn.rto * 2U;
                if (tcp_xmit(conn.iss, 0, TCP_SYN, 0, 0) != 0) return -1;
                start = timer_ticks();
            }
            __asm__ volatile ("pause");
        }
    }
}

static int tcp_send_data(const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;

    while (off < len) {
        uint32_t n = len - off;

        if (n > TCP_MSS) n = TCP_MSS;
        if (n > conn.cwnd) n = conn.cwnd;
        if (n > TCP_RECV_MAX) n = TCP_RECV_MAX;
        if (tcp_send_wait(conn.snd_nxt, TCP_PSH | TCP_ACK, data + off, n) != 0) return -1;
        conn.snd_nxt += n;
        off += n;
    }
    return 0;
}

/* 收满 need 字节（应用缓冲里已有的先取走）；返回实际字节数 */
static int tcp_recv_data(uint8_t *buf, uint32_t need)
{
    uint32_t got = 0;
    uint32_t deadline = timer_ticks() + TCP_RTO_MAX;

    while (got < need) {
        if (conn.rlen > 0U) {
            uint32_t n = need - got;
            if (n > conn.rlen) n = conn.rlen;
            memcpy(buf + got, conn.rbuf, n);
            got += n;
            if (n < conn.rlen) memmove(conn.rbuf, conn.rbuf + n, conn.rlen - n);
            conn.rlen -= n;
            if (got >= need) break;
        }
        if (tcp_poll_once()) { deadline = timer_ticks() + TCP_RTO_MAX; continue; }
        if ((int32_t)(timer_ticks() - deadline) >= 0) break;
        __asm__ volatile ("pause");
    }
    return (int)got;
}

static int tcp_close(void)
{
    if (conn.state == 0) return 0;
    if (tcp_send_wait(conn.snd_nxt, TCP_FIN | TCP_ACK, 0, 0) != 0) return -1;
    conn.snd_nxt += 1U;
    conn.state = 4;                          /* TIME_WAIT */
    {
        uint32_t start = timer_ticks();

        while ((timer_ticks() - start) < TCP_TIME_WAIT) {
            tcp_poll_once();
            if (conn.state == 0) break;
            __asm__ volatile ("pause");
        }
    }
    conn.state = 0;
    return 0;
}

/* =============================================================================
 * 服务端对外 API（SYS_LISTEN / SYS_ACCEPT 的实现层）
 * ========================================================================== */
int net_tcp_socket(int pid)
{
    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        tcp_sock_t *s = &socks[i];

        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->owner_pid = pid;
        s->owner_fd = -1;
        s->conn = -1;
        return (int)i;
    }
    return -1;
}

int net_tcp_listen(int sock, uint16_t port, uint32_t backlog)
{
    tcp_sock_t *s = sock_get(sock);

    if (s == 0 || port == 0U) return -1;
    if (s->listening) return -1;                       /* 同一 socket 重复 listen */
    if (s->port != 0U && s->port != port) return -1;   /* 已经绑到别的端口 */

    tcp_reap();
    if (tcp_port_busy(port) != 0) return -1;           /* 重复 bind / TIME_WAIT 占用 */

    if (backlog == 0U) backlog = 1U;
    if (backlog > NET_TCP_BACKLOG_MAX) backlog = NET_TCP_BACKLOG_MAX;

    memset(s->synq, 0, sizeof(s->synq));
    memset(s->acceptq, 0, sizeof(s->acceptq));
    s->syn_count = 0;
    s->accept_count = 0;
    s->overflow = 0;
    s->backlog = backlog;
    s->port = port;
    s->listening = true;
    return 0;
}

int net_tcp_listen_port(uint16_t port, uint32_t backlog)
{
    int sock = net_tcp_socket(0);

    if (sock < 0) return -1;
    if (net_tcp_listen(sock, port, backlog) != 0) {
        memset(&socks[sock], 0, sizeof(socks[sock]));
        return -1;
    }
    return sock;
}

int net_tcp_accept(int sock, uint8_t peer_ip[4], uint16_t *peer_port,
                   uint32_t timeout_ticks)
{
    tcp_sock_t *l = sock_get(sock);

    if (l == 0 || !l->listening) return -1;

    for (uint32_t waited = 0; ; waited++) {
        if (l->accept_count != 0U) {
            tcp_hs_t hs = l->acceptq[0];
            tcp_srv_t *c;

            for (uint32_t i = 1U; i < l->accept_count; i++) l->acceptq[i - 1U] = l->acceptq[i];
            l->accept_count--;

            c = tcp_srv_alloc(l->port, &hs);
            if (c == 0) {                              /* 连接表满 */
                l->overflow++;
                return -1;
            }
            if (peer_ip != 0) memcpy(peer_ip, c->peer_ip, 4);
            if (peer_port != 0) *peer_port = c->peer_port;
            return (int)(c - srv);
        }
        if (waited >= timeout_ticks) return -1;
        tcp_drain();
        proc_sleep_ticks(1U);
    }
}

int net_tcp_accept_port(uint16_t port, uint8_t peer_ip[4], uint16_t *peer_port,
                        uint32_t timeout_ticks)
{
    tcp_sock_t *l = tcp_listen_match(port);

    if (l == 0) return -1;
    return net_tcp_accept((int)(l - socks), peer_ip, peer_port, timeout_ticks);
}

int net_tcp_conn_read(int conn_id, void *buf, uint32_t max_len, uint32_t timeout_ticks)
{
    tcp_srv_t *c = srv_get(conn_id);

    if (c == 0 || buf == 0) return -1;

    for (uint32_t waited = 0; ; waited++) {
        if (c->rlen != 0U) {
            uint32_t n = c->rlen;

            if (n > max_len) n = max_len;
            memcpy(buf, c->rbuf, n);
            if (n < c->rlen) memmove(c->rbuf, c->rbuf + n, c->rlen - n);
            c->rlen -= n;
            return (int)n;
        }
        if (c->peer_fin || c->state == TCP_SRV_CLOSED || c->closed) return 0;
        if (waited >= timeout_ticks) return -1;
        tcp_drain();
        proc_sleep_ticks(1U);
    }
}

int net_tcp_conn_write(int conn_id, const void *buf, uint32_t len)
{
    tcp_srv_t *c = srv_get(conn_id);
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t off = 0;

    if (c == 0 || (buf == 0 && len != 0U)) return -1;
    if (c->closed || c->fin_sent || c->state == TCP_SRV_CLOSED) return -1;

    while (off < len) {
        uint32_t n = len - off;

        if (n > TCP_MSS) n = TCP_MSS;
        if (tcp_send_seg(c->lport, c->peer_ip, c->peer_port,
                         c->snd_nxt, c->rcv_nxt, TCP_PSH | TCP_ACK, p + off, n) != 0) {
            return -1;
        }
        c->snd_nxt += n;
        off += n;
    }
    tcp_drain();                                        /* 顺手收掉对端 ACK */
    return (int)len;
}

/* 主动关闭的公共实现：发 FIN -> FIN_WAIT -> TIME_WAIT。
 * wait = true 时有界等 FIN 的 ACK（进程上下文，会 sleep）；GC 路径用 false
 * （可能在 syscall 关中断上下文里，不能睡）。 */
static void tcp_srv_shutdown(tcp_srv_t *c, bool wait)
{
    if (c->state == TCP_SRV_TIME_WAIT) return;
    if (!c->fin_sent) {
        if (tcp_send_seg(c->lport, c->peer_ip, c->peer_port,
                         c->snd_nxt, c->rcv_nxt, TCP_FIN | TCP_ACK, 0, 0) == 0) {
            c->snd_nxt += 1U;
            c->fin_sent = true;
            c->state = TCP_SRV_FIN_WAIT;
        } else {
            c->state = TCP_SRV_TIME_WAIT;
            c->tw_until = timer_ticks() + TCP_SRV_TW_TICKS;
            return;
        }
    }
    if (c->state == TCP_SRV_FIN_WAIT && wait) {
        for (uint32_t i = 0; i < 10U; i++) {
            tcp_drain();
            if (c->state == TCP_SRV_TIME_WAIT) break;
            proc_sleep_ticks(1U);
        }
    }
    if (c->state == TCP_SRV_FIN_WAIT) {
        c->state = TCP_SRV_TIME_WAIT;
        c->tw_until = timer_ticks() + TCP_SRV_TW_TICKS;
    }
}

int net_tcp_conn_close(int conn_id)
{
    tcp_srv_t *c = srv_get(conn_id);

    if (c == 0) return -1;
    if (c->closed) return 0;

    c->closed = true;
    tcp_srv_shutdown(c, true);
    return 0;
}

int net_tcp_conn_port(int conn_id)
{
    tcp_srv_t *c = srv_get(conn_id);

    return (c != 0) ? (int)c->lport : -1;
}

void net_tcp_conn_owner(int conn_id, int pid, int fd)
{
    tcp_srv_t *c = srv_get(conn_id);

    if (c == 0) return;
    c->owner_pid = pid;
    c->owner_fd = fd;
}

int net_tcp_sock_close(int sock)
{
    tcp_sock_t *s = sock_get(sock);

    if (s == 0) return -1;

    if (s->listening) {
        /* 监听关闭：端口立即释放；未取走的完成连接回 RST 丢弃 */
        for (uint32_t i = 0; i < s->accept_count; i++) {
            tcp_rst_to(s->acceptq[i].peer_ip, s->acceptq[i].peer_port, s->port,
                       s->acceptq[i].snd_nxt, s->acceptq[i].rcv_nxt);
        }
        s->listening = false;
        s->port = 0U;
        s->syn_count = 0;
        s->accept_count = 0;
        memset(s->synq, 0, sizeof(s->synq));
        memset(s->acceptq, 0, sizeof(s->acceptq));
    }
    if (s->conn >= 0) {
        (void)net_tcp_conn_close(s->conn);
        s->conn = -1;
    }
    memset(s, 0, sizeof(*s));
    return 0;
}

int net_tcp_sock_conn(int sock)
{
    tcp_sock_t *s = sock_get(sock);

    return (s != 0) ? s->conn : -1;
}

int net_tcp_sock_port(int sock)
{
    tcp_sock_t *s = sock_get(sock);

    return (s != 0) ? (int)s->port : -1;
}

int net_tcp_sock_listening(int sock)
{
    tcp_sock_t *s = sock_get(sock);

    return (s != 0 && s->listening) ? 1 : 0;
}

int net_tcp_sock_of_port(uint16_t port)
{
    tcp_sock_t *l = tcp_listen_match(port);

    return (l != 0) ? (int)(l - socks) : -1;
}

void net_tcp_sock_owner(int sock, int pid, int fd)
{
    tcp_sock_t *s = sock_get(sock);

    if (s == 0) return;
    s->owner_pid = pid;
    s->owner_fd = fd;
}

/* fd 是否仍指向 (tag|id)：SYS_CLOSE 只清 fd 表项，网络侧靠这里回收 */
static bool tcp_fd_alive(int pid, int fd, uint32_t tag, int id)
{
    proc_t *p;
    proc_fd_t *f;

    if (pid <= 0 || fd < 3 || fd >= PROC_FD_MAX) return false;
    p = proc_find((uint32_t)pid);
    if (p == 0) return false;
    f = &p->fds[fd];
    if (!f->used || f->kind != FD_KIND_TCP) return false;
    if ((f->handle & NET_TCP_HDL_TAG_MASK) != tag) return false;
    if ((int)(f->handle & NET_TCP_HDL_ID_MASK) != id) return false;
    return true;
}

void net_tcp_sock_gc(void)
{
    tcp_reap();

    /* 先回收“fd 已经不在”的已建立连接（进程退出也会走这里） */
    for (uint32_t i = 0; i < TCP_SRV_MAX; i++) {
        tcp_srv_t *c = &srv[i];

        if (!c->used || c->owner_pid <= 0) continue;
        if (!tcp_fd_alive(c->owner_pid, c->owner_fd, NET_TCP_HDL_CONN, (int)i)) {
            c->owner_pid = 0;
            c->owner_fd = -1;
            c->closed = true;
            tcp_srv_shutdown(c, false);      /* 进程上下文的 syscall 里不能睡 */
        }
    }
    /* 再回收监听 socket（关闭 fd -> 端口立即释放） */
    for (uint32_t i = 0; i < TCP_SOCK_MAX; i++) {
        tcp_sock_t *s = &socks[i];

        if (!s->used || s->owner_pid <= 0) continue;
        if (!tcp_fd_alive(s->owner_pid, s->owner_fd, NET_TCP_HDL_LISTEN, (int)i)) {
            (void)net_tcp_sock_close((int)i);
        }
    }
}

/* =============================================================================
 * 自检
 * ========================================================================== */
static int tcp_self_test_ooo(void)
{
    uint8_t a[16];
    uint8_t b[100];
    tcp_conn_t *c = &conn;
    tcp_seg_t s;

    /* 合成连接：ESTABLISHED，rcv_nxt=1000，synthetic（不发包） */
    memset(c, 0, sizeof(*c));
    c->synthetic = true;
    c->state = 2;
    c->rcv_nxt = 1000U;
    c->snd_una = 5000U;
    c->snd_nxt = 5000U;
    c->rto = TCP_RTO_INIT;
    c->lport = 15010U;
    c->peer_port = NET_ECHO_PORT;
    memcpy(c->peer_ip, net_gw_ip_ptr(), 4);

    memset(b, 'B', sizeof(b));
    memset(a, 'A', sizeof(a));

    /* 先到 seq=1016（乱序），再到 seq=1000（缺口补齐） */
    memset(&s, 0, sizeof(s));
    s.seq = 1016U; s.ack = 5000U; s.flags = TCP_ACK; s.wnd = 2048U;
    s.data = b; s.len = (uint32_t)sizeof(b);
    tcp_input(&s);
    if (c->rlen != 0U || c->dup_acks == 0U) return -1;

    s.seq = 1000U; s.data = a; s.len = (uint32_t)sizeof(a);
    tcp_input(&s);
    if (c->rlen != sizeof(a) + sizeof(b)) return -1;
    if (c->rbuf[0] != 'A' || c->rbuf[sizeof(a)] != 'B') return -1;
    if (c->rcv_nxt != 1116U) return -1;
    kprintln("net: tcp out-of-order reassembly ok");

    /* 坏校验和：构造帧走 parse（伪首部故意算错），必须被丢弃 */
    {
        uint8_t f[14 + 20 + 20 + 4];
        uint8_t *ip = f + 14;
        uint8_t *tcp = ip + 20;

        memset(f, 0, sizeof(f));
        memcpy(f, (const uint8_t *)"AAAAAA", 6);
        f[12] = 0x08; f[13] = 0x00;
        ip[0] = 0x45;
        wr16(ip + 2, 20 + 20 + 4);
        ip[8] = 64;
        ip[9] = 6;
        memcpy(ip + 12, net_gw_ip_ptr(), 4);   /* src = 对端 */
        memcpy(ip + 16, net_local_ip_ptr(), 4);
        wr16(ip + 10, 0);
        wr16(tcp + 0, c->peer_port);
        wr16(tcp + 2, c->lport);
        wr32(tcp + 4, c->rcv_nxt + 200U);
        tcp[12] = 0x50;
        tcp[13] = TCP_ACK;
        memcpy(tcp + 20, "evil", 4);
        wr16(tcp + 16, 0x1234);           /* 故意错 */
        {
            uint32_t before = c->bad_checksum;

            if (tcp_parse_frame(f, sizeof(f), &s) == 0) return -1;
            if (c->bad_checksum == before) return -1;
        }
    }
    kprintln("net: tcp checksum rejects corrupt ok");

    /* RST：连接立即关闭 */
    memset(&s, 0, sizeof(s));
    s.seq = c->rcv_nxt; s.ack = 0; s.flags = TCP_RST; s.wnd = 0; s.data = 0; s.len = 0;
    if (tcp_input(&s) != -1 || c->state != 0) return -1;
    kprintln("net: tcp RST closes connection ok");
    return 0;
}

/* 自检端口（避开回显端口 12345 与既有 15010/15011） */
#define TCP_TEST_LISTEN_PORT      15060U
#define TCP_TEST_CLIENT_PORT      15061U
#define TCP_TEST_BACKLOG_PORT     15062U
#define TCP_TEST_SYS_PORT         15064U
#define TCP_TEST_SYS_CLIENT_PORT  15065U

/* 把一帧“外部对端 -> 本机”的 TCP 段塞进本地回环队列（走完整解析/校验和） */
static void tcp_test_inject(const uint8_t src_ip[4], uint16_t sport, uint16_t dport,
                            uint32_t seq, uint32_t ack, uint8_t flags,
                            const uint8_t *data, uint32_t len)
{
    uint8_t f[14U + 20U + 20U + TCP_MSS];
    uint8_t *ip = f + 14;
    uint8_t *tcp = ip + 20;
    uint32_t total = 20U + 20U + len;
    static const uint8_t peer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };

    if (len > TCP_MSS) len = TCP_MSS;
    total = 20U + 20U + len;
    memset(f, 0, 14U + total);
    memcpy(f, peer_mac, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)total);
    wr16(ip + 4, 0x4000U);
    ip[8] = 64;
    ip[9] = 6;
    memcpy(ip + 12, src_ip, 4);
    memcpy(ip + 16, net_local_ip_ptr(), 4);

    wr16(tcp + 0, sport);
    wr16(tcp + 2, dport);
    wr32(tcp + 4, seq);
    wr32(tcp + 8, ack);
    tcp[12] = 0x50;
    tcp[13] = flags;
    wr16(tcp + 14, (uint16_t)TCP_RECV_MAX);
    if (len != 0U && data != 0) memcpy(tcp + 20, data, len);
    wr16(tcp + 16, 0);
    wr16(tcp + 16, tcp_csum(src_ip, net_local_ip_ptr(), tcp, 20U + len));
    wr16(ip + 10, 0);

    (void)net_loopback_push(f, 14U + total);
}

/* 本机回环上的 listen -> connect -> 收发 -> 关闭，外加 backlog/TIME_WAIT 负向用例 */
static int tcp_self_test_listen(void)
{
    uint8_t peer[4];
    uint16_t pport = 0;
    uint8_t rx[64];
    uint8_t tx[20];
    int lsock;
    int cid;
    int n;

    if (net_local_ip_ptr()[0] == 0U) return -1;

    /* 1) listen + 重复 bind 拒绝 */
    lsock = net_tcp_listen_port(TCP_TEST_LISTEN_PORT, 2U);
    if (lsock < 0) {
        kprintln("net: tcp listen failed (no socket/bind)");
        return -1;
    }
    if (net_tcp_listen_port(TCP_TEST_LISTEN_PORT, 2U) >= 0) {
        kprintln("net: tcp duplicate bind NOT rejected");
        net_tcp_sock_close(lsock);
        return -1;
    }
    kprintln("net: tcp listen ok (port=15060 backlog=2, duplicate bind rejected)");

    /* 2) 客户端 connect：本机回环（slirp 不转发 127.0.0.1，用本机 IP 回环） */
    if (tcp_connect(net_local_ip_ptr(), TCP_TEST_LISTEN_PORT,
                    TCP_TEST_CLIENT_PORT) != 0) {
        kprintln("net: tcp listen selftest: loopback connect failed");
        net_tcp_sock_close(lsock);
        return -1;
    }
    kprintln("net: tcp loopback connect ok (SYN/SYN-ACK/ACK to 10.0.2.15)");

    /* 3) accept：有界等待（1s） */
    cid = net_tcp_accept(lsock, peer, &pport, 100U);
    if (cid < 0) {
        kprintln("net: tcp accept failed (timeout)");
        (void)tcp_close();
        net_tcp_sock_close(lsock);
        return -1;
    }
    if (pport != TCP_TEST_CLIENT_PORT || memcmp(peer, net_local_ip_ptr(), 4) != 0) {
        kprintf("net: tcp accept peer mismatch (port=%u)\n", (unsigned int)pport);
        (void)tcp_close();
        net_tcp_sock_close(lsock);
        return -1;
    }

    /* 4) 客户端 -> 服务端数据 */
    for (uint32_t i = 0; i < (uint32_t)sizeof(tx); i++) tx[i] = (uint8_t)('a' + i);
    if (tcp_send_data(tx, (uint32_t)sizeof(tx)) != 0) {
        kprintln("net: tcp accept data: client send failed");
        net_tcp_sock_close(lsock);
        return -1;
    }
    n = net_tcp_conn_read(cid, rx, sizeof(rx), 100U);
    if (n != (int)sizeof(tx) || memcmp(rx, tx, sizeof(tx)) != 0) {
        kprintf("net: tcp accept data mismatch (%d/%u)\n", n, (unsigned int)sizeof(tx));
        net_tcp_sock_close(lsock);
        return -1;
    }
    kprintln("net: tcp accept data ok (client->server 20 bytes)");

    /* 5) 服务端 -> 客户端回包 */
    if (net_tcp_conn_write(cid, "server-reply", 12U) != 12) {
        kprintln("net: tcp accept reply: server write failed");
        net_tcp_sock_close(lsock);
        return -1;
    }
    n = tcp_recv_data(rx, 12U);
    if (n != 12 || memcmp(rx, "server-reply", 12) != 0) {
        kprintf("net: tcp accept reply mismatch (%d/12)\n", n);
        net_tcp_sock_close(lsock);
        return -1;
    }
    kprintln("net: tcp accept reply ok (server->client 12 bytes)");

    /* 6) 服务端主动关：FIN -> TIME_WAIT；监听关闭后端口仍被 TIME_WAIT 占用 */
    if (net_tcp_conn_close(cid) != 0) {
        kprintln("net: tcp accept close failed");
        net_tcp_sock_close(lsock);
        return -1;
    }
    net_tcp_sock_close(lsock);
    if (net_tcp_listen_port(TCP_TEST_LISTEN_PORT, 2U) >= 0) {
        kprintln("net: tcp TIME_WAIT did NOT hold port");
        return -1;
    }
    kprintln("net: tcp TIME_WAIT holds port ok (rebind rejected)");

    /* 7) 客户端收尾（FIN 由服务端 TIME_WAIT 连接回 ACK） */
    (void)tcp_close();

    /* 8) TIME_WAIT 过期后端口可复用 */
    {
        int re = -1;

        for (uint32_t i = 0; i < 200U; i++) {
            re = net_tcp_listen_port(TCP_TEST_LISTEN_PORT, 2U);
            if (re >= 0) break;
            proc_sleep_ticks(1U);
        }
        if (re < 0) {
            kprintln("net: tcp port reuse after TIME_WAIT failed");
            return -1;
        }
        net_tcp_sock_close(re);
        kprintln("net: tcp port reuse after close ok (TIME_WAIT expired)");
    }

    /* 9) backlog=1：第二个 SYN 溢出被丢弃；half-open 不会被 accept 取走 */
    {
        uint8_t peer_a[4];
        uint8_t peer_b[4];
        int s2 = net_tcp_listen_port(TCP_TEST_BACKLOG_PORT, 1U);
        tcp_sock_t *l;
        uint32_t iss;

        if (s2 < 0) {
            kprintln("net: tcp backlog listener bind failed");
            return -1;
        }
        l = sock_get(s2);
        if (l == 0) return -1;

        /* 用本机地址 + 不同端口扮演两个对端：SYN-ACK 走本地回环，不上真网卡 */
        memcpy(peer_a, net_local_ip_ptr(), 4);
        memcpy(peer_b, net_local_ip_ptr(), 4);

        /* 第一个对端：占住唯一的 backlog 槽 */
        tcp_test_inject(peer_a, 40001U, TCP_TEST_BACKLOG_PORT, 1000U, 0U, TCP_SYN, 0, 0);
        tcp_drain();
        if (l->syn_count != 1U) {
            kprintln("net: tcp backlog did not accept first SYN");
            net_tcp_sock_close(s2);
            return -1;
        }
        /* 第二个对端：backlog 满，必须被丢弃 */
        tcp_test_inject(peer_b, 40002U, TCP_TEST_BACKLOG_PORT, 2000U, 0U, TCP_SYN, 0, 0);
        tcp_drain();
        if (l->overflow != 1U || l->syn_count != 1U) {
            kprintf("net: tcp backlog overflow wrong (overflow=%u syn=%u)\n",
                    (unsigned int)l->overflow, (unsigned int)l->syn_count);
            net_tcp_sock_close(s2);
            return -1;
        }
        kprintln("net: tcp backlog overflow ok (backlog=1, surplus SYN dropped)");

        /* 半开连接不能被 accept 取走：接受必须有界超时 */
        if (net_tcp_accept(s2, peer, &pport, 10U) >= 0) {
            kprintln("net: tcp accept returned a half-open connection");
            net_tcp_sock_close(s2);
            return -1;
        }
        kprintln("net: tcp accept timeout ok (half-open not accepted, bounded wait)");

        /* 完成第一个对端的三次握手 -> accept 拿到它 */
        iss = l->synq[0].iss;
        tcp_test_inject(peer_a, 40001U, TCP_TEST_BACKLOG_PORT, 1001U, iss + 1U,
                        TCP_ACK, 0, 0);
        tcp_drain();
        cid = net_tcp_accept(s2, peer, &pport, 30U);
        if (cid < 0 || pport != 40001U || memcmp(peer, peer_a, 4) != 0) {
            kprintf("net: tcp backlog accept failed (cid=%d port=%u)\n",
                    cid, (unsigned int)pport);
            net_tcp_sock_close(s2);
            return -1;
        }
        kprintln("net: tcp backlog handshake ok (completed conn accepted)");
        (void)net_tcp_conn_close(cid);
        net_tcp_sock_close(s2);
    }

    kprintln("net: tcp listen/accept selftest PASS");
    return 0;
}

/* 通过真正的 legacy syscall 处理函数走一遍：SYS_SOCKET -> SYS_LISTEN ->
 * SYS_ACCEPT -> SYS_SENDTO/SYS_RECVFROM -> SYS_CLOSE(语义) -> 端口回收。
 * 直接调用 net_syscall() 而不是 int 0x81，是为了在内核线程里拿到确定结果；
 * 参数与 ABI 完全一致（ebx/ecx/edx + 用户指针）。 */
static int tcp_self_test_syscall(void)
{
    interrupt_frame_t f;
    struct udp_req req;
    uint8_t addr[NET_TCP_ADDR_LEN];
    uint8_t rx[64];
    uint16_t aport;
    proc_t *p = proc_current();
    int lfd;
    int cfd;
    int n;

    if (p == 0) return 0;             /* 没有进程上下文（无 fd 表）：跳过 */

    memset(&f, 0, sizeof(f));
    memset(&req, 0, sizeof(req));
    memset(addr, 0, sizeof(addr));

    /* AF_INET / SOCK_STREAM：拿一个 TCP socket fd */
    f.eax = (unsigned int)SYS_SOCKET;
    f.ebx = 2U;
    f.ecx = 1U;
    f.edx = 0U;
    lfd = net_syscall(&f);
    if (lfd < 3) {
        kprintln("net: tcp syscall SYS_SOCKET failed");
        return -1;
    }

    /* SYS_LISTEN(port, backlog)：绑定刚分配的 socket */
    f.eax = (unsigned int)SYS_LISTEN;
    f.ebx = TCP_TEST_SYS_PORT;
    f.ecx = 2U;
    if (net_syscall(&f) != 0) {
        kprintln("net: tcp syscall SYS_LISTEN failed");
        return -1;
    }

    /* 客户端连上来（本机回环） */
    if (tcp_connect(net_local_ip_ptr(), TCP_TEST_SYS_PORT,
                    TCP_TEST_SYS_CLIENT_PORT) != 0) {
        kprintln("net: tcp syscall selftest: loopback connect failed");
        return -1;
    }

    /* SYS_ACCEPT(sock=fd, &addr)：返回连接 fd，并填 {ip[4], port} */
    f.eax = (unsigned int)SYS_ACCEPT;
    f.ebx = (unsigned int)lfd;
    f.ecx = (uint32_t)(uintptr_t)addr;
    cfd = net_syscall(&f);
    if (cfd < 3) {
        kprintln("net: tcp syscall SYS_ACCEPT failed");
        (void)tcp_close();
        return -1;
    }
    memcpy(&aport, addr + 4, sizeof(aport));
    if (aport != TCP_TEST_SYS_CLIENT_PORT ||
        memcmp(addr, net_local_ip_ptr(), 4) != 0) {
        kprintf("net: tcp syscall accept addr mismatch (port=%u)\n", (unsigned int)aport);
        (void)tcp_close();
        return -1;
    }

    /* SYS_SENDTO：服务端 -> 客户端写 11 字节 */
    memcpy(req.data, "syscall-tcp", 11U);
    req.len = 11U;
    f.eax = (unsigned int)SYS_SENDTO;
    f.ebx = (unsigned int)cfd;
    f.ecx = (uint32_t)(uintptr_t)&req;
    if (net_syscall(&f) != 11) {
        kprintln("net: tcp syscall SYS_SENDTO failed");
        (void)tcp_close();
        return -1;
    }
    n = tcp_recv_data(rx, 11U);
    if (n != 11 || memcmp(rx, "syscall-tcp", 11U) != 0) {
        kprintf("net: tcp syscall data mismatch (%d/11)\n", n);
        (void)tcp_close();
        return -1;
    }

    /* 客户端发 14 字节，SYS_RECVFROM 读出来 */
    {
        static const char cpayload[] = "client-payload";

        if (tcp_send_data((const uint8_t *)cpayload, 14U) != 0) {
            kprintln("net: tcp syscall client send failed");
            (void)tcp_close();
            return -1;
        }
    }
    memset(&req, 0, sizeof(req));
    f.eax = (unsigned int)SYS_RECVFROM;
    f.ebx = (unsigned int)cfd;
    f.ecx = (uint32_t)(uintptr_t)&req;
    n = net_syscall(&f);
    if (n != 14 || req.len != 14U || memcmp(req.data, "client-payload", 14U) != 0) {
        kprintf("net: tcp syscall SYS_RECVFROM mismatch (%d/%u)\n",
                n, (unsigned int)req.len);
        (void)tcp_close();
        return -1;
    }
    kprintln("net: tcp listen/accept syscall ok (fd + addr + read/write)");

    /* 客户端先收尾，再模拟 SYS_CLOSE 清 fd 表项：网络侧在下一个网络
     * syscall 入口的 GC 里关闭连接（TIME_WAIT），过期后端口可复用。 */
    (void)tcp_close();
    memset(&p->fds[cfd], 0, sizeof(p->fds[cfd]));
    memset(&p->fds[lfd], 0, sizeof(p->fds[lfd]));

    {
        int ok = 0;

        for (uint32_t i = 0; i < 200U; i++) {
            f.eax = (unsigned int)SYS_LISTEN;
            f.ebx = TCP_TEST_SYS_PORT;
            f.ecx = 2U;
            if (net_syscall(&f) == 0) { ok = 1; break; }
            proc_sleep_ticks(1U);
        }
        if (!ok) {
            kprintln("net: tcp socket fd close did not release port");
            return -1;
        }
        kprintln("net: tcp socket fd close releases port ok");
        /* 清掉本次隐式创建的监听 socket（pid=0，不会被 GC 回收） */
        {
            int s = net_tcp_sock_of_port(TCP_TEST_SYS_PORT);

            if (s >= 0) (void)net_tcp_sock_close(s);
        }
    }
    return 0;
}

/* 真实链路：大传输（16KB，回显）+ 丢包注入，验证重传/退避/RTO 收敛 */
static int tcp_self_test_bulk(void)
{
    static uint8_t tx[TCP_MSS];
    static uint8_t rx[TCP_MSS];
    uint32_t total = 0;
    uint32_t sent = 0;
    int fail = -1;

    if (net_local_ip_ptr()[0] == 0) return -1;
    if (tcp_connect(net_gw_ip_ptr(), NET_ECHO_PORT, 15010U) != 0) {
        kprintln("net: tcp robust connect failed");
        return -1;
    }
    kprintln("net: tcp handshake ok (SYN/SYN-ACK/ACK)");

    conn.loss_every = 5U;                    /* 每 5 个数据段丢 1 个 -> 强制重传 */

    while (total < 16384U) {
        uint32_t n = 512U;

        for (uint32_t i = 0; i < n; i++) tx[i] = (uint8_t)((sent + i) & 0xFFU);
        if (tcp_send_data(tx, n) != 0) {
            kprintln("net: tcp bulk send failed");
            goto out;
        }
        {
            int got = tcp_recv_data(rx, n);

            if (got != (int)n) {
                kprintf("net: tcp bulk recv short (%d/%u)\n", got, (unsigned int)n);
                goto out;
            }
            for (uint32_t i = 0; i < n; i++) {
                if (rx[i] != (uint8_t)((sent + i) & 0xFFU)) {
                    kprintf("net: tcp bulk data mismatch at %u\n", (unsigned int)(sent + i));
                    goto out;
                }
            }
        }
        sent += n;
        total += n;
    }

    if (tcp_close() != 0) {
        kprintln("net: tcp bulk close failed");
        goto out;
    }
    kprintf("net: tcp large transfer ok (%u bytes, retrans=%u, timeouts=%u, dropped=%u)\n",
            (unsigned int)total, (unsigned int)conn.retransmits,
            (unsigned int)conn.timeouts, (unsigned int)conn.dropped_tx);
    kprintf("net: tcp rto ok (srtt=%uticks rto=%uticks)\n",
            (unsigned int)conn.srtt, (unsigned int)conn.rto);
    kprintln("net: tcp FIN/TIME_WAIT ok");
    fail = 0;
out:
    conn.loss_every = 0;
    conn.state = 0;
    return fail;
}

int net_tcp_listen_selftest(void)
{
    if (e1000_present() == 0) return 0;

    if (tcp_self_test_listen() != 0) {
        kprintln("net: tcp listen selftest FAILED (loopback/backlog)");
        return -1;
    }
    if (tcp_self_test_syscall() != 0) {
        kprintln("net: tcp listen selftest FAILED (syscall)");
        return -1;
    }
    return 0;
}

int net_tcp_robust_selftest(void)
{
    if (e1000_present() == 0) return 0;

    if (tcp_self_test_ooo() != 0) {
        kprintln("net: tcp selftest FAILED (reassembly/csum/rst)");
        return -1;
    }
    /* 先跑真实链路（宿主 echo），再跑本机回环的 listen/accept：
     * 后者出问题不应连带既有 TCP 断言（182 项里的 TCP 那组）。 */
    if (tcp_self_test_bulk() != 0) {
        kprintln("net: tcp selftest FAILED (bulk)");
        return -1;
    }
    if (net_tcp_listen_selftest() != 0) {
        kprintln("net: tcp selftest FAILED (listen/accept)");
        return -1;
    }
    kprintln("net: tcp selftest PASS");
    return 0;
}
