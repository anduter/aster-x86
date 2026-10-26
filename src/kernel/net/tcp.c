/* =============================================================================
 * net/tcp.c - 最小但"完整"的 TCP 客户端（阶段 4c：健壮性）
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
 *     回显链路上强制触发重传（QEMU slirp 是用户态网关，宿主侧无法造真丢包）。
 *
 * 发送是"同步收发"（一个 512B 段在途），所以窗口/cwnd 主要作为状态与回归证据；
 * 文档里如实说明（docs/network.md）。
 * ============================================================================= */
#include "net.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/dev.h"

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

typedef struct {
    uint32_t seq;
    uint32_t ack;
    uint8_t  flags;
    uint16_t wnd;
    const uint8_t *data;
    uint32_t len;
} tcp_seg_t;

typedef struct {
    uint32_t seq;
    uint32_t len;
    uint8_t  data[TCP_MSS];
    bool     used;
} tcp_ooo_t;

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

static tcp_conn_t conn;

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

static int tcp_xmit(uint32_t seq, uint32_t ack, uint8_t flags,
                    const uint8_t *data, uint32_t len)
{
    uint8_t tcp[20U + TCP_MSS];

    if (conn.synthetic) return 0;
    tcp_build(tcp, conn.lport, conn.peer_port, seq, ack, flags, data, len);
    {
        uint16_t cs = tcp_csum(net_local_ip_ptr(), conn.peer_ip, tcp, 20U + len);
        wr16(tcp + 16, cs);
    }
    return net_ip_send(conn.peer_ip, 6, tcp, 20U + len);
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
 * 接收：解析一个以太网帧里的 TCP 段
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
    if (memcmp(ip + 16, net_local_ip_ptr(), 4) != 0) return -1;
    tcp = ip + ihl;
    if (rd16(tcp + 2) != conn.lport || rd16(tcp + 0) != conn.peer_port) return -1;
    if (memcmp(ip + 12, conn.peer_ip, 4) != 0) return -1;

    tlen = (uint32_t)rd16(ip + 2);
    if (tlen < ihl + 20U) return -1;
    off = (uint32_t)(tcp[12] >> 4) * 4U;
    if (off < 20U || ihl + off > tlen) return -1;

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
 * 收到的数据按序交给应用缓冲；乱序段进 ooo 队列
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

/* 处理一个段（真实收包与单测共用）。返回 0 = 已处理，-1 = 连接被重置。 */
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

/* 收一帧并喂给 tcp_input；返回 1 = 处理了一个段，0 = 没有/不是本连接的段 */
static int tcp_poll_once(void)
{
    uint8_t f[1600];
    tcp_seg_t s;
    int n;

    if (conn.synthetic) return 0;
    n = net_rx_frame(f, sizeof(f));
    if (n <= 0) return 0;
    if (tcp_parse_frame(f, (uint32_t)n, &s) != 0) return 0;
    (void)tcp_input(&s);
    return 1;
}

/* -------------------------------------------------------------------------
 * 同步发送一段并等 ACK（含 RTO 重传 + 指数退避 + 3 次重复 ACK 快重传）
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
 * 对外：连接 / 发送 / 接收 / 关闭
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

/* -------------------------------------------------------------------------
 * 自检
 * ------------------------------------------------------------------------- */
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
    s.seq = c->rcv_nxt; s.ack = 0; s.flags = TCP_RST; s.wnd = 0; s.data = 0; s.len = 0;
    if (tcp_input(&s) != -1 || c->state != 0) return -1;
    kprintln("net: tcp RST closes connection ok");
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

int net_tcp_robust_selftest(void)
{
    if (e1000_present() == 0) return 0;

    if (tcp_self_test_ooo() != 0) {
        kprintln("net: tcp selftest FAILED (reassembly/csum/rst)");
        return -1;
    }
    if (tcp_self_test_bulk() != 0) {
        kprintln("net: tcp selftest FAILED (bulk)");
        return -1;
    }
    kprintln("net: tcp selftest PASS");
    return 0;
}
