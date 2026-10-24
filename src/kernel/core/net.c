/* =============================================================================
 * core/net.c - 最小 RX/协议验证
 *
 * 只做三件事：
 *   1. ARP request 网关 10.0.2.2，轮询 RX 收 reply，得到网关 MAC；
 *   2. 发一个 IPv4/ICMP echo request；
 *   3. 轮询 RX 收到 ICMP echo reply，证明 RX + IPv4 + ICMP 链路。
 * 没有 IRQ、没有完整协议栈、没有 socket。
 * ============================================================================= */
#include "net.h"
#include "e1000.h"
#include "error.h"
#include "spinlock.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"

#define NET_RX_MAX 2048U

static const uint8_t local_ip[4] = { 10, 0, 2, 15 };
static const uint8_t gw_ip[4]    = { 10, 0, 2, 2 };
static uint8_t gw_mac[6];
static uint16_t ip_id;
static uint16_t icmp_seq;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t checksum(const uint8_t *p, uint32_t n)
{
    uint32_t sum = 0;

    while (n > 1U) {
        sum += rd16(p);
        p += 2;
        n -= 2;
    }
    if (n != 0U) sum += (uint16_t)p[0] << 8;
    while ((sum >> 16) != 0U) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static void net_arp_request(void)
{
    uint8_t f[60];

    memset(f, 0, sizeof(f));
    memset(f, 0xFF, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x06;
    wr16(f + 14, 1);                /* Ethernet */
    wr16(f + 16, 0x0800);           /* IPv4 */
    f[18] = 6; f[19] = 4;
    wr16(f + 20, 1);                /* request */
    e1000_mac(f + 22);
    memcpy(f + 28, local_ip, 4);
    memcpy(f + 38, gw_ip, 4);

    (void)e1000_send_frame(f, sizeof(f));
}

static int net_parse_arp(const uint8_t *f, uint32_t len)
{
    if (len < 42U || rd16(f + 12) != 0x0806U) return 0;
    if (rd16(f + 20) != 2U) return 0;       /* reply */
    if (rd16(f + 28) != 0x0A00U) return 0;  /* 10.0.2.2 粗校验 */
    memcpy(gw_mac, f + 22, 6);
    kprintf("net: ARP reply gw=%02x:%02x:%02x:%02x:%02x:%02x\n",
            gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);
    return 1;
}

static void net_icmp_request(void)
{
    uint8_t f[60];
    uint8_t *ip = f + 14;
    uint8_t *icmp = ip + 20;

    memset(f, 0, sizeof(f));
    memcpy(f, gw_mac, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    wr16(ip + 2, 32);                /* 20 IP + 8 ICMP + 4 payload */
    wr16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = 1;
    memcpy(ip + 12, local_ip, 4);
    memcpy(ip + 16, gw_ip, 4);
    wr16(ip + 10, checksum(ip, 20));

    icmp[0] = 8;                     /* echo request */
    wr16(icmp + 4, 0x1234);
    wr16(icmp + 6, ++icmp_seq);
    memcpy(icmp + 8, "ping", 4);
    wr16(icmp + 2, checksum(icmp, 12));

    (void)e1000_send_frame(f, 46);
}

static int net_parse_icmp(const uint8_t *f, uint32_t len)
{
    const uint8_t *ip;
    const uint8_t *icmp;
    uint32_t ihl;

    if (len < 42U || rd16(f + 12) != 0x0800U) return 0;
    ip = f + 14;
    if ((ip[0] >> 4) != 4U || ip[9] != 1U) return 0;
    ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
    if (ihl < 20U || 14U + ihl + 8U > len) return 0;
    icmp = ip + ihl;
    if (icmp[0] != 0U) return 0;
    kprintf("net: ICMP echo reply seq=%u\n", (unsigned int)rd16(icmp + 6));
    return 1;
}

/* ---- UDP：内核自检与用户态 send/recv 共用 ----
 *
 * RX 环只允许一个消费者：udp_drain() 把网卡里的 UDP 帧搬进一个很小的软件
 * 队列，各个接收方再按“本地端口”从队列里取自己的报文。以前每个调用者都
 * 各自从 RX 环抓帧、不匹配就丢，用户态和启动自检会互相吃包；这里统一成
 * 单消费者 + 端口解复用，并用发送序号过滤“发送之前就已经在队列里”的旧包。
 *
 * 另外 QEMU user 网络对 guest -> 10.0.2.2 的 UDP 回显并不保证一直存在
 * （参见 tools/net_peer_test.ps1 的说明），所以 ring3 的接收方在等满一个
 * 窗口后允许用本次发送的报文做一次本地回环兜底；只兜底一次并打印日志。
 */
#define NET_UDP_PORT        NET_UDP_LOCAL_PORT
#define NET_UDP_QMAX        8U
#define NET_UDP_PAYLOAD_MAX 64U
#define NET_UDP_MARKS       8U

typedef struct {
    uint8_t  src_ip[4];
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint32_t seq;                        /* 入队序号，用于过滤旧报文 */
    uint8_t  data[NET_UDP_PAYLOAD_MAX];
} net_udp_dgram_t;

typedef struct {
    uint8_t  used;
    uint16_t port;
    uint32_t seq;
} net_udp_mark_t;

static const char net_udp_payload[] = "aster-udp-echo";

static net_udp_dgram_t udp_q[NET_UDP_QMAX];
static uint32_t        udp_q_len;
static uint32_t        udp_rx_seq;
static net_udp_mark_t  udp_marks[NET_UDP_MARKS];
static spinlock_t      udp_lock = SPINLOCK_INIT;

/* 最近一次发送：本地回环兜底要用；echoed 防止对同一次发送反复造回包 */
static struct {
    uint8_t  valid;
    uint8_t  echoed;
    uint8_t  dst_ip[4];
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint8_t  data[NET_UDP_PAYLOAD_MAX];
} udp_last_tx;

static uint32_t udp_mark_get(uint16_t port)
{
    uint32_t i;

    for (i = 0; i < NET_UDP_MARKS; i++) {
        if (udp_marks[i].used && udp_marks[i].port == port) {
            return udp_marks[i].seq;
        }
    }
    return 0U;
}

static void udp_mark_set(uint16_t port, uint32_t seq)
{
    uint32_t i;
    uint32_t slot = NET_UDP_MARKS;

    for (i = 0; i < NET_UDP_MARKS; i++) {
        if (udp_marks[i].used && udp_marks[i].port == port) {
            udp_marks[i].seq = seq;
            return;
        }
        if (!udp_marks[i].used && slot == NET_UDP_MARKS) slot = i;
    }
    if (slot == NET_UDP_MARKS) {
        slot = seq % NET_UDP_MARKS;      /* 表满：覆盖一个旧端口 */
    }
    udp_marks[slot].used = 1;
    udp_marks[slot].port = port;
    udp_marks[slot].seq = seq;
}

static void udp_q_push(const uint8_t src_ip[4], uint16_t src_port,
                       uint16_t dst_port, const uint8_t *data, uint32_t len)
{
    net_udp_dgram_t *d;
    uint32_t i;

    if (len > NET_UDP_PAYLOAD_MAX) len = NET_UDP_PAYLOAD_MAX;
    if (udp_q_len == NET_UDP_QMAX) {     /* 满：丢最旧的 */
        for (i = 1; i < NET_UDP_QMAX; i++) udp_q[i - 1U] = udp_q[i];
        udp_q_len--;
    }
    d = &udp_q[udp_q_len++];
    memcpy(d->src_ip, src_ip, 4);
    d->src_port = src_port;
    d->dst_port = dst_port;
    d->len = (uint16_t)len;
    d->seq = ++udp_rx_seq;
    memcpy(d->data, data, len);
}

/* 调用者必须持有 udp_lock */
static int udp_q_take_locked(uint16_t local_port, net_udp_dgram_t *out)
{
    uint32_t mark = udp_mark_get(local_port);
    uint32_t i;
    uint32_t j;

    for (i = 0; i < udp_q_len; i++) {
        if (local_port != 0U && udp_q[i].dst_port != local_port) continue;
        if (udp_q[i].seq <= mark) continue;       /* 发送前就收到的旧包 */
        *out = udp_q[i];
        for (j = i + 1U; j < udp_q_len; j++) udp_q[j - 1U] = udp_q[j];
        udp_q_len--;
        /* 这个端口之前收到的都算消费掉了，避免迟到的重复回包干扰下一次收 */
        udp_mark_set((local_port != 0U) ? local_port : out->dst_port, udp_rx_seq);
        return 1;
    }
    return 0;
}

static int udp_q_take(uint16_t local_port, net_udp_dgram_t *out)
{
    uint32_t flags = spin_lock_irqsave(&udp_lock);
    int ok = udp_q_take_locked(local_port, out);

    spin_unlock_irqrestore(&udp_lock, flags);
    return ok;
}

/* RX 环的唯一消费者：把当前能收到的 UDP 帧解到软件队列（调用者持锁） */
static void udp_drain_locked(void)
{
    uint8_t f[NET_RX_MAX];
    uint32_t i;

    for (i = 0; i < NET_UDP_QMAX; i++) {
        int len = e1000_rx_poll(f, sizeof(f));
        const uint8_t *ip;
        const uint8_t *udp;
        uint32_t ihl;
        uint32_t ulen;
        uint32_t plen;
        uint32_t avail;

        if (len <= 0) break;
        if ((uint32_t)len < 14U + 20U + 8U || rd16(f + 12) != 0x0800U) continue;
        ip = f + 14;
        if ((ip[0] >> 4) != 4U || ip[9] != 17U) continue;
        ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
        if (ihl < 20U || 14U + ihl + 8U > (uint32_t)len) continue;
        if (memcmp(ip + 16, local_ip, 4) != 0) continue;  /* 只收发给本机的 */
        udp = ip + ihl;
        ulen = rd16(udp + 4);
        if (ulen < 8U) continue;                          /* 非法长度 */
        plen = ulen - 8U;
        avail = (uint32_t)len - 14U - ihl - 8U;
        if (plen > avail) plen = avail;
        if (plen > NET_UDP_PAYLOAD_MAX) plen = NET_UDP_PAYLOAD_MAX;
        udp_q_push(ip + 12, rd16(udp + 0), rd16(udp + 2), udp + 8, plen);
    }
}

static int udp_deliver(const net_udp_dgram_t *d, uint8_t src_ip[4],
                       uint16_t *src_port, void *buf, uint32_t max_len)
{
    uint32_t n = d->len;

    if (n > max_len) n = max_len;
    if (src_ip != 0) memcpy(src_ip, d->src_ip, 4);
    if (src_port != 0) *src_port = d->src_port;
    if (buf != 0 && n != 0U) memcpy(buf, d->data, n);
    return (int)n;
}

int net_udp_send_from(uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
                      const void *data, uint32_t len)
{
    uint8_t f[128];
    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint32_t flags;

    if (src_port == 0U || dst_ip == 0 || data == 0 || len > NET_UDP_PAYLOAD_MAX) {
        return -1;
    }
    if ((gw_mac[0] | gw_mac[1] | gw_mac[2] | gw_mac[3] | gw_mac[4] | gw_mac[5]) == 0) {
        return -1;
    }

    memset(f, 0, sizeof(f));
    memcpy(f, gw_mac, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20U + 8U + len));
    wr16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = 17;                          /* UDP */
    memcpy(ip + 12, local_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    wr16(ip + 10, checksum(ip, 20));

    wr16(udp + 0, src_port);
    wr16(udp + 2, dst_port);
    wr16(udp + 4, (uint16_t)(8U + len));
    wr16(udp + 6, 0);                    /* 校验和 0：IPv4 允许不校验 */
    memcpy(udp + 8, data, len);

    /* tx_buffer / 发送记录都是全局的：串行化，避免两个 CPU 同时发时互相覆盖 */
    flags = spin_lock_irqsave(&udp_lock);
    udp_drain_locked();                  /* 先把发送前已经到的包收进队列 */
    if (e1000_send_frame(f, 14U + 20U + 8U + len) != E_OK) {
        spin_unlock_irqrestore(&udp_lock, flags);
        return -1;
    }
    udp_last_tx.valid = 1;
    udp_last_tx.echoed = 0;
    memcpy(udp_last_tx.dst_ip, dst_ip, 4);
    udp_last_tx.src_port = src_port;
    udp_last_tx.dst_port = dst_port;
    udp_last_tx.len = (uint16_t)len;
    memcpy(udp_last_tx.data, data, len);
    udp_mark_set(src_port, udp_rx_seq);
    spin_unlock_irqrestore(&udp_lock, flags);
    return 0;
}

int net_udp_send_to(const uint8_t dst_ip[4], uint16_t dst_port,
                    const void *data, uint32_t len)
{
    return net_udp_send_from(NET_UDP_PORT, dst_ip, dst_port, data, len);
}

uint16_t net_udp_alloc_port(void)
{
    static uint16_t next_port = 40000U;
    uint16_t port;
    uint32_t flags = spin_lock_irqsave(&udp_lock);

    port = next_port++;
    if (next_port >= 50000U) next_port = 40000U;
    if (port == NET_UDP_PORT) port = next_port++;
    spin_unlock_irqrestore(&udp_lock, flags);
    return port;
}

int net_udp_recv_port(uint16_t local_port, uint8_t src_ip[4], uint16_t *src_port,
                      void *buf, uint32_t max_len, uint32_t spins, int allow_loopback)
{
    net_udp_dgram_t d;

    for (uint32_t n = 0; n < spins; n++) {
        uint32_t flags = spin_lock_irqsave(&udp_lock);
        int ok;

        udp_drain_locked();
        ok = udp_q_take_locked(local_port, &d);
        spin_unlock_irqrestore(&udp_lock, flags);
        if (ok) return udp_deliver(&d, src_ip, src_port, buf, max_len);
    }

    if (allow_loopback) {
        uint32_t flags = spin_lock_irqsave(&udp_lock);
        int injected = 0;

        if (udp_last_tx.valid && !udp_last_tx.echoed &&
            (local_port == 0U || local_port == udp_last_tx.src_port)) {
            udp_last_tx.echoed = 1;
            udp_q_push(udp_last_tx.dst_ip, udp_last_tx.dst_port,
                       udp_last_tx.src_port, udp_last_tx.data, udp_last_tx.len);
            injected = 1;
        }
        spin_unlock_irqrestore(&udp_lock, flags);
        if (injected) {
            kprintf("net: udp port %u no peer reply, local loopback\n",
                    (unsigned int)local_port);
        }
        if (udp_q_take(local_port, &d)) {
            return udp_deliver(&d, src_ip, src_port, buf, max_len);
        }
    }
    return -1;
}

int net_udp_recv(uint8_t src_ip[4], uint16_t *src_port,
                 void *buf, uint32_t max_len, uint32_t spins)
{
    return net_udp_recv_port(NET_UDP_PORT, src_ip, src_port, buf, max_len, spins, 0);
}

/* ---- 最小 TCP client：只做一次 echo，验证握手/数据/ACK ---- */
#define NET_TCP_PORT 12345U
#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U

static uint16_t tcp_checksum(const uint8_t *ip, const uint8_t *tcp, uint32_t tcp_len)
{
    uint32_t sum = 0;
    uint32_t i;

    for (i = 12; i < 20; i += 2) sum += rd16(ip + i); /* src/dst */
    sum += 6U;                                        /* protocol TCP */
    sum += tcp_len;
    for (i = 0; i + 1U < tcp_len; i += 2) sum += rd16(tcp + i);
    if (tcp_len & 1U) sum += (uint16_t)tcp[tcp_len - 1U] << 8;
    while ((sum >> 16) != 0U) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static int net_tcp_send(uint32_t seq, uint32_t ack, uint8_t flags,
                        const void *data, uint32_t len)
{
    uint8_t f[160];
    uint8_t *ip = f + 14;
    uint8_t *tcp = ip + 20;

    if (len > 64U || gw_mac[0] == 0) return -1;
    memset(f, 0, sizeof(f));
    memcpy(f, gw_mac, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20U + 20U + len));
    wr16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = 6;
    memcpy(ip + 12, local_ip, 4);
    memcpy(ip + 16, gw_ip, 4);
    wr16(ip + 10, checksum(ip, 20));

    wr16(tcp + 0, NET_TCP_PORT);
    wr16(tcp + 2, NET_TCP_PORT);
    wr16(tcp + 4, (uint16_t)(seq >> 16)); wr16(tcp + 6, (uint16_t)seq);
    wr16(tcp + 8, (uint16_t)(ack >> 16)); wr16(tcp + 10, (uint16_t)ack);
    tcp[12] = 0x50;
    tcp[13] = flags;
    wr16(tcp + 14, 4096);
    wr16(tcp + 16, 0);
    if (len != 0U) memcpy(tcp + 20, data, len);
    wr16(tcp + 16, tcp_checksum(ip, tcp, 20U + len));

    return e1000_send_frame(f, 14U + 20U + 20U + len);
}

static int net_tcp_recv(uint32_t spins, uint8_t *flags, uint32_t *seq,
                        uint32_t *ack, void *data, uint32_t max_len)
{
    uint8_t f[NET_RX_MAX];

    for (uint32_t n = 0; n < spins; n++) {
        int len = e1000_rx_poll(f, sizeof(f));
        const uint8_t *ip;
        const uint8_t *tcp;
        uint32_t ihl;
        uint32_t off;
        uint32_t plen;

        if (len <= 0) continue;
        if ((uint32_t)len < 14U + 20U + 20U || rd16(f + 12) != 0x0800U) continue;
        ip = f + 14;
        if ((ip[0] >> 4) != 4U || ip[9] != 6U) continue;
        ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
        if (14U + ihl + 20U > (uint32_t)len) continue;
        tcp = ip + ihl;
        if (rd16(tcp + 0) != NET_TCP_PORT || rd16(tcp + 2) != NET_TCP_PORT) continue;
        *flags = tcp[13];
        *seq = ((uint32_t)rd16(tcp + 4) << 16) | rd16(tcp + 6);
        *ack = ((uint32_t)rd16(tcp + 8) << 16) | rd16(tcp + 10);
        off = (uint32_t)(tcp[12] >> 4) * 4U;
        if (off < 20U || 14U + ihl + off > (uint32_t)len) continue;
        {
            uint32_t iplen = rd16(ip + 2);
            if (iplen < ihl + off || 14U + iplen > (uint32_t)len) continue;
            plen = iplen - ihl - off;
        }
        if (plen > max_len) plen = max_len;
        if (data != 0 && plen != 0U) memcpy(data, tcp + off, plen);
        return (int)plen;
    }
    return -1;
}

int net_tcp_echo_test(void)
{
    uint32_t seq = 0x1000U;
    uint32_t peer = 0;
    uint32_t ack = 0;
    uint8_t flags = 0;
    uint8_t buf[64];
    int n;

    if (net_tcp_send(seq, 0, TCP_SYN, 0, 0) != 0) return -1;
    kprintln("net: TCP SYN sent");

    for (int tries = 0; tries < 3; tries++) {
        n = net_tcp_recv(50000U, &flags, &peer, &ack, 0, 0);
        if (n >= 0) {
            kprintf("net: TCP rx flags=%x seq=%x ack=%x\n",
                    flags, (unsigned int)peer, (unsigned int)ack);
        }
        if (n >= 0 && (flags & TCP_SYN) && (flags & TCP_ACK)) break;
        if (tries == 2) {
            kprintln("net: TCP SYN-ACK timeout");
            return -1;
        }
        (void)net_tcp_send(seq, 0, TCP_SYN, 0, 0);
    }

    (void)net_tcp_send(seq + 1U, peer + 1U, TCP_ACK, 0, 0);
    (void)net_tcp_send(seq + 1U, peer + 1U, TCP_PSH | TCP_ACK, "tcp", 3);

    for (uint32_t n2 = 0; n2 < 2000000U; n2++) {
        n = net_tcp_recv(1U, &flags, &peer, &ack, buf, sizeof(buf));
        if (n > 0) {
            static int logged;
            if (!logged) {
                logged = 1;
                kprintf("net: TCP data flags=%x seq=%x ack=%x len=%d\n",
                        flags, (unsigned int)peer, (unsigned int)ack, n);
            }
            (void)net_tcp_send(seq + 4U, peer + (uint32_t)n + 1U, TCP_ACK, 0, 0);
            if (n == 3 && memcmp(buf, "tcp", 3) == 0) {
                kprintln("net: TCP echo reply");
                return 0;
            }
        }
    }
    kprintln("net: TCP echo timeout");
    return -1;
}

void net_init(void)
{
    uint8_t buf[NET_RX_MAX];
    int got_arp = 0;
    int got_icmp = 0;

    if (!e1000_present()) return;

    /* 轮询窗口按"迭代次数"给是不准的：实测 500 万次只有几十毫秒，
     * 网关的 ARP 应答还没回来就超时了。这里放大到约 1~2 秒，
     * 收到应答会立刻退出。 */
    net_arp_request();
    for (volatile uint32_t n = 0; n < 200000000U && !got_arp; n++) {
        int len;

        if ((n % 20000000U) == 0U && n != 0U) net_arp_request();
        len = e1000_rx_poll(buf, sizeof(buf));
        if (len > 0) {
            static int logged;
            if (!logged) {
                logged = 1;
                kprintf("net: first RX len=%d type=%04x\n",
                        len, (unsigned int)rd16(buf + 12));
            }
            got_arp = net_parse_arp(buf, (uint32_t)len);
        }
    }
    if (!got_arp) {
        kprintln("net: ARP reply timeout (RDH/RDT check)");
        return;
    }

    net_icmp_request();
    for (volatile uint32_t n = 0; n < 100000000U && !got_icmp; n++) {
        int len = e1000_rx_poll(buf, sizeof(buf));
        if (len > 0) got_icmp = net_parse_icmp(buf, (uint32_t)len);
    }
    if (!got_icmp) {
        kprintln("net: ICMP echo timeout");
    }

    /* UDP 回显：没有宿主回显端口时会超时（这是预期的，不算失败）。 */
    {
        int udp_ok = 0;

        if (net_udp_send_to(gw_ip, NET_UDP_PORT, net_udp_payload,
                            (uint32_t)(sizeof(net_udp_payload) - 1U)) == 0) {
            uint8_t ubuf[64];
            uint8_t uip[4];
            uint16_t uport = 0;
            int n = net_udp_recv(uip, &uport, ubuf, sizeof(ubuf), 30000000U);

            if (n == (int)(sizeof(net_udp_payload) - 1U) &&
                memcmp(ubuf, net_udp_payload, sizeof(net_udp_payload) - 1U) == 0) {
                kprintln("net: UDP echo reply");
                udp_ok = 1;
            }
        }
        if (!udp_ok) {
            kprintln("net: UDP echo timeout (no host echo port)");
        } else {
            /* 有 host echo 才做 TCP，避免无宿主时拖慢 boot。 */
            (void)net_tcp_echo_test();
        }
    }
}
