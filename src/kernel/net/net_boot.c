/* =============================================================================
 * net/net_boot.c - 启动期网络自检：DHCP / DNS / 主动 ICMP
 *
 * 目的：把"IP 写死"的网络补成真正可用的基础件，并在启动日志里给出可断言
 * 的证据：
 *   net: dhcp ok ip=... gw=... dns=...   （失败：net: dhcp timeout, keep static）
 *   net: dns ok <name>=<ip>              （失败：net: dns timeout）
 *   net: ping ok seq=N rtt=X.Yms         （失败：net: ping timeout）
 *
 * 设计约束：
 *   - net_init() 在 interrupt_init() 之前运行，timer_ticks() 此时不会前进，
 *     所以这里用 TSC 做时间基准，并用 PIT 通道 2 的一次性计数校准
 *     （约 20ms，不依赖任何中断）。
 *   - 所有等待都有毫秒上界 + 迭代次数上界；失败只降级，不阻塞启动。
 *   - RX 环在这个阶段只有一个消费者：本模块直接轮询 e1000，只认领自己
 *     关心的报文（DHCP 67->68、DNS 53、ICMP echo reply），其余丢弃。
 *   - 报文构造只覆盖这个内核需要的字段，不做通用协议栈。
 * ============================================================================= */
#include "net_boot.h"
#include "net_internal.h"
#include "include/dev.h"
#include "include/kernel.h"

#define NB_RX_MAX        2048U

/* ---- 超时预算（毫秒） ---- */
#define NB_ARP_BUDGET_MS 300U
#define NB_DHCP_BUDGET_MS 3000U     /* 任务要求 DHCP 总预算 <= 3s */
#define NB_DHCP_RETRY_MS 500U
#define NB_DNS_BUDGET_MS 800U       /* 每个候选域名 */
#define NB_DNS_CANDIDATES 2U
#define NB_PING_COUNT    3U
#define NB_PING_BUDGET_MS 300U      /* 每个 seq */

/* ---- 安全网：即使 TSC 不前进也不会死循环 ---- */
#define NB_ARP_SPIN_LIMIT  60000000U
#define NB_DHCP_SPIN_LIMIT 400000000U
#define NB_DNS_SPIN_LIMIT  300000000U
#define NB_PING_SPIN_LIMIT 150000000U

#define NB_ARP_CACHE_N   8U
#define NB_ARP_TTL_MS    60000U

#define NB_DHCP_CLIENT_PORT 68U
#define NB_DHCP_SERVER_PORT 67U

#define NB_PING_ID 0x4E45U          /* 'NE'：与 net.c 的 ICMP 自检 id=0x1234 区分 */

static const uint8_t nb_ip_zero[4]    = { 0, 0, 0, 0 };
static const uint8_t nb_ip_broadcast[4] = { 255, 255, 255, 255 };
static const uint8_t nb_mac_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* =============================================================================
 * 小工具
 * ============================================================================= */
static uint16_t nb_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t nb_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void nb_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static void nb_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t nb_sum16(const uint8_t *p, uint32_t n)
{
    uint32_t sum = 0;

    while (n > 1U) {
        sum += nb_rd16(p);
        p += 2;
        n -= 2U;
    }
    if (n != 0U) sum += (uint16_t)p[0] << 8;
    while ((sum >> 16) != 0U) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static int nb_ip_zero_p(const uint8_t ip[4])
{
    return (ip[0] | ip[1] | ip[2] | ip[3]) == 0;
}

/* "a.b.c.d"，调用者给 >=16 字节 */
static void nb_ip_str(const uint8_t ip[4], char out[16])
{
    size_t n = utoa(ip[0], out, 10U);

    out[n++] = '.';
    n += utoa(ip[1], out + n, 10U);
    out[n++] = '.';
    n += utoa(ip[2], out + n, 10U);
    out[n++] = '.';
    n += utoa(ip[3], out + n, 10U);
    out[n] = '\0';
}

/* =============================================================================
 * 时间基准：TSC + PIT 通道 2 校准（无需中断）
 *
 * net_init() 早于 interrupt_init()，timer_ticks() 恒为 0，因此不能用它做
 * 超时。PIT 通道 2 可以独立于中断做一次性延时，用它测出 20ms 内 TSC 走了
 * 多少拍，得到 cycles/ms；之后全部用 TSC 计算 deadline。
 * ============================================================================= */
static uint32_t nb_tsc_per_ms;      /* 0 = 尚未校准 */
static uint32_t nb_mono_ms;         /* 累积毫秒（32 位，约 49 天回绕） */
static uint32_t nb_last_tsc;

static uint32_t nb_rdtsc32(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

/* 用 PIT 通道 2 模式 0 等约 20ms；成功返回 0。喇叭保持关闭。 */
static int nb_pit2_wait_20ms(void)
{
    unsigned char p61 = inb(0x61);

    outb(0x61, (unsigned char)(p61 & ~0x03U));   /* 门控低：停计数，喇叭关 */
    outb(0x43, 0xB0U);                           /* 通道2，模式0，二进制，先低后高 */
    outb(0x42, (unsigned char)(23864U & 0xFFU)); /* 1193182 / 50 ~= 23864 拍 = 20ms */
    outb(0x42, (unsigned char)(23864U >> 8));
    outb(0x61, (unsigned char)((p61 & ~0x02U) | 0x01U)); /* 门控高，OUT2 计数中 */

    for (uint32_t i = 0; i < 20000000U; i++) {
        if ((inb(0x61) & 0x20U) != 0U) return 0;
    }
    return -1;
}

static void nb_time_init(void)
{
    uint32_t t0;

    if (nb_tsc_per_ms != 0U) return;

    t0 = nb_rdtsc32();
    if (nb_pit2_wait_20ms() == 0) {
        uint32_t d = nb_rdtsc32() - t0;

        nb_tsc_per_ms = d / 20U;
        /* 合理范围：1MHz~20GHz。离谱说明 PIT/TSC 不可信，走兜底。 */
        if (nb_tsc_per_ms < 1000U || nb_tsc_per_ms > 20000000U) nb_tsc_per_ms = 0U;
    }
    if (nb_tsc_per_ms == 0U) {
        /* 兜底：按 ~2.5GHz 估算。只影响超时长短，仍是有界的。 */
        nb_tsc_per_ms = 2500000U;
    }
    nb_last_tsc = nb_rdtsc32();
    nb_mono_ms = 0U;
}

/* 单调毫秒。频繁调用（等待循环里每次迭代/每几十次迭代）时增量很小。 */
static uint32_t nb_now_ms(void)
{
    uint32_t now;
    uint32_t d;

    if (nb_tsc_per_ms == 0U) nb_time_init();
    now = nb_rdtsc32();
    d = now - nb_last_tsc;
    nb_last_tsc = now;
    /* 超过 2^31 拍视为"很久没调用/被中断打断"，本次不计，避免时间跳变。 */
    if (d < 0x80000000U) nb_mono_ms += d / nb_tsc_per_ms;
    return nb_mono_ms;
}

static int nb_deadline_ok(uint32_t deadline)
{
    return (int32_t)(nb_now_ms() - deadline) < 0;
}

/* =============================================================================
 * 链路层 / IPv4 / UDP 收发
 * ============================================================================= */
static uint16_t nb_ip_id;

static int nb_send_udp(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                       const uint8_t dst_mac[6], uint16_t sport, uint16_t dport,
                       const uint8_t *payload, uint32_t len)
{
    uint8_t f[700];
    uint8_t smac[6];
    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;

    if (len > sizeof(f) - 14U - 20U - 8U) return -1;

    memset(f, 0, sizeof(f));
    memcpy(f, dst_mac, 6);
    e1000_mac(smac);
    memcpy(f + 6, smac, 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    nb_wr16(ip + 2, (uint16_t)(20U + 8U + len));
    nb_wr16(ip + 4, ++nb_ip_id);
    ip[6] = 0x40;                       /* DF：报文都小于 MTU，禁止分片 */
    ip[8] = 64;
    ip[9] = 17;                         /* UDP */
    memcpy(ip + 12, src_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    nb_wr16(ip + 10, nb_sum16(ip, 20U));

    nb_wr16(udp + 0, sport);
    nb_wr16(udp + 2, dport);
    nb_wr16(udp + 4, (uint16_t)(8U + len));
    nb_wr16(udp + 6, 0);                /* IPv4 允许 UDP 校验和为 0 */
    if (len != 0U) memcpy(udp + 8, payload, len);

    return e1000_send_frame(f, 14U + 20U + 8U + len);
}

typedef struct {
    const uint8_t *src_ip;
    const uint8_t *udp;
    const uint8_t *payload;
    uint16_t sport;
    uint16_t dport;
    uint32_t plen;
} nb_udp_view_t;

static int nb_parse_udp(const uint8_t *f, uint32_t len, nb_udp_view_t *v)
{
    const uint8_t *ip;
    int ihl_i;
    uint32_t ihl;
    uint32_t ulen;

    if (len < 14U + 20U + 8U || nb_rd16(f + 12) != 0x0800U) return -1;
    ip = f + 14;
    ihl_i = net_ipv4_validate(ip, len - 14U);
    if (ihl_i < 0) return -1;
    ihl = (uint32_t)ihl_i;
    if (ip[9] != 17U) return -1;

    v->src_ip = ip + 12;
    v->udp = ip + ihl;
    /* 长度/校验和校验放在读端口之前：ihl 可以到 60，帧可能只到 IP 头 */
    if (net_udp_validate(ip, v->udp, len - 14U - ihl, &ulen) != 0) return -1;
    v->sport = nb_rd16(v->udp + 0);
    v->dport = nb_rd16(v->udp + 2);
    v->payload = v->udp + 8;
    v->plen = ulen - 8U;
    return 0;
}

/* =============================================================================
 * 最小 ARP 缓存 + 解析
 *
 * 启动自检只需要"发之前知道目的 MAC"：网关的 MAC 由 net.c 的 ARP 自检写好，
 * 这里先取来预热；DNS 服务器（10.0.2.3）等其它地址按需 ARP。
 * 条目带 TTL（默认 60s），过期后重新解析。
 * ============================================================================= */
typedef struct {
    uint8_t  used;
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint32_t expire_ms;
} nb_arp_entry_t;

static nb_arp_entry_t nb_arp_cache[NB_ARP_CACHE_N];

static int nb_arp_lookup(const uint8_t ip[4], uint8_t mac[6])
{
    uint32_t now = nb_now_ms();

    for (uint32_t i = 0; i < NB_ARP_CACHE_N; i++) {
        if (!nb_arp_cache[i].used) continue;
        if (memcmp(nb_arp_cache[i].ip, ip, 4) != 0) continue;
        if ((int32_t)(now - nb_arp_cache[i].expire_ms) >= 0) continue; /* 已过期 */
        memcpy(mac, nb_arp_cache[i].mac, 6);
        return 0;
    }
    return -1;
}

static void nb_arp_store(const uint8_t ip[4], const uint8_t mac[6])
{
    uint32_t slot = NB_ARP_CACHE_N;

    for (uint32_t i = 0; i < NB_ARP_CACHE_N; i++) {
        if (nb_arp_cache[i].used && memcmp(nb_arp_cache[i].ip, ip, 4) == 0) {
            slot = i;
            break;
        }
        if (!nb_arp_cache[i].used && slot == NB_ARP_CACHE_N) slot = i;
    }
    if (slot == NB_ARP_CACHE_N) slot = 0;    /* 表满：覆盖第 0 项 */

    nb_arp_cache[slot].used = 1;
    memcpy(nb_arp_cache[slot].ip, ip, 4);
    memcpy(nb_arp_cache[slot].mac, mac, 6);
    nb_arp_cache[slot].expire_ms = nb_now_ms() + NB_ARP_TTL_MS;
}

static int nb_arp_request(const uint8_t ip[4])
{
    uint8_t f[60];
    uint8_t self[4], gw[4], dns[4], mask[4];

    net_cfg_get(self, gw, dns, mask);
    (void)gw; (void)dns; (void)mask;

    memset(f, 0, sizeof(f));
    memset(f, 0xFF, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x06;
    nb_wr16(f + 14, 1U);                /* Ethernet */
    nb_wr16(f + 16, 0x0800U);           /* IPv4 */
    f[18] = 6; f[19] = 4;
    nb_wr16(f + 20, 1U);                /* request */
    e1000_mac(f + 22);
    memcpy(f + 28, self, 4);
    memcpy(f + 38, ip, 4);

    return e1000_send_frame(f, sizeof(f));
}

static int nb_arp_parse(const uint8_t *f, uint32_t len, const uint8_t ip[4], uint8_t mac[6])
{
    if (len < 42U || nb_rd16(f + 12) != 0x0806U) return 0;
    if (nb_rd16(f + 14) != 1U || nb_rd16(f + 16) != 0x0800U) return 0;
    if (f[18] != 6U || f[19] != 4U) return 0;
    if (nb_rd16(f + 20) != 2U) return 0;     /* reply */
    if (memcmp(f + 28, ip, 4) != 0) return 0; /* 只认这个发送者 */
    memcpy(mac, f + 22, 6);
    return 1;
}

static int nb_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t budget_ms)
{
    uint8_t f[NB_RX_MAX];
    uint32_t deadline;

    if (nb_arp_lookup(ip, mac) == 0) return 0;
    if (nb_arp_request(ip) != 0) return -1;

    deadline = nb_now_ms() + budget_ms;
    for (uint32_t spins = 0; spins < NB_ARP_SPIN_LIMIT; spins++) {
        uint32_t len;

        if ((spins & 31U) == 0U && !nb_deadline_ok(deadline)) break;
        len = (uint32_t)e1000_rx_poll(f, sizeof(f));
        if (len == 0U) continue;
        if (nb_arp_parse(f, len, ip, mac) != 0) {
            nb_arp_store(ip, mac);
            return 0;
        }
    }
    return -1;
}

/* 用 net.c 已经学到的网关 MAC 预热缓存，省掉一次 ARP 往返。 */
static void nb_arp_seed(void)
{
    uint8_t ip[4], gw[4], dns[4], mask[4], mac[6];

    net_cfg_get(ip, gw, dns, mask);
    net_gw_mac_get(mac);
    (void)ip; (void)dns; (void)mask;
    if (nb_ip_zero_p(gw)) return;
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) return;
    nb_arp_store(gw, mac);
}

/* =============================================================================
 * DHCP 客户端
 *
 * DISCOVER(0.0.0.0:68 -> 255.255.255.255:67) -> OFFER -> REQUEST -> ACK
 * 只实现 QEMU slirp 及常见服务器都会用的最小选项集：
 *   53 消息类型、1 掩码、3 路由器、6 DNS、50 请求 IP、54 服务器标识。
 * ============================================================================= */
typedef struct {
    uint8_t type;
    uint8_t yiaddr[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint8_t dns[4];
    uint8_t server[4];
    uint8_t src_mac[6];
} nb_dhcp_msg_t;

static uint32_t nb_dhcp_xid;

static uint32_t nb_dhcp_build(uint8_t *p, size_t cap, uint8_t type,
                              const uint8_t requested_ip[4],
                              const uint8_t server_ip[4])
{
    uint32_t o = 240U;
    uint8_t mac[6];

    if (cap < 300U) return 0;
    memset(p, 0, 300U);

    p[0] = 1;                           /* BOOTREQUEST */
    p[1] = 1;                           /* Ethernet */
    p[2] = 6;                           /* MAC 长度 */
    p[3] = 0;
    nb_wr32(p + 4, nb_dhcp_xid);
    nb_wr16(p + 8, 0);
    nb_wr16(p + 10, 0x8000U);           /* 广播标志：OFFER/ACK 可以广播回来 */
    e1000_mac(mac);
    memcpy(p + 28, mac, 6);             /* chaddr */
    p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;

    p[o++] = 53; p[o++] = 1; p[o++] = type;
    if (type == 3U && requested_ip != 0) {
        p[o++] = 50; p[o++] = 4; memcpy(p + o, requested_ip, 4); o += 4U;
    }
    if (type == 3U && server_ip != 0) {
        p[o++] = 54; p[o++] = 4; memcpy(p + o, server_ip, 4); o += 4U;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;
    p[o++] = 255;
    return 300U;                        /* BOOTP 最小报文长度 */
}

static int nb_dhcp_parse(const uint8_t *f, uint32_t len, nb_dhcp_msg_t *m)
{
    nb_udp_view_t v;
    const uint8_t *b;
    uint32_t o;
    uint32_t end;

    if (nb_parse_udp(f, len, &v) != 0) return -1;
    if (v.sport != NB_DHCP_SERVER_PORT || v.dport != NB_DHCP_CLIENT_PORT) return -1;
    if (v.plen < 241U) return -1;
    b = v.payload;
    if (b[0] != 2U) return -1;                       /* BOOTREPLY */
    if (nb_rd32(b + 4) != nb_dhcp_xid) return -1;
    if (!(b[236] == 0x63U && b[237] == 0x82U && b[238] == 0x53U && b[239] == 0x63U)) {
        return -1;
    }

    memset(m, 0, sizeof(*m));
    memcpy(m->yiaddr, b + 16, 4);
    memcpy(m->server, b + 20, 4);                    /* siaddr 兜底 */
    memcpy(m->src_mac, f + 6, 6);

    end = v.plen;
    o = 240;
    while (o < end) {
        uint8_t code = b[o++];
        uint8_t olen;

        if (code == 0U) continue;                    /* pad */
        if (code == 255U) break;                     /* end */
        if (o >= end) break;
        olen = b[o++];
        if (o + olen > end) break;
        switch (code) {
        case 53: if (olen >= 1U) m->type = b[o]; break;
        case 1:  if (olen >= 4U) memcpy(m->mask, b + o, 4); break;
        case 3:  if (olen >= 4U) memcpy(m->gw, b + o, 4); break;
        case 6:  if (olen >= 4U) memcpy(m->dns, b + o, 4); break;
        case 54: if (olen >= 4U) memcpy(m->server, b + o, 4); break;
        default: break;
        }
        o += olen;
    }
    return (m->type != 0U) ? 0 : -1;
}

static int nb_dhcp_finish(const nb_dhcp_msg_t *m, uint32_t deadline)
{
    uint8_t ip[4], gw[4], dns[4], mask[4];
    uint8_t cur_ip[4], cur_gw[4], cur_dns[4], cur_mask[4];
    uint8_t gwmac[6];
    uint32_t now;
    uint32_t arp_budget;
    char ip_s[16], gw_s[16], dns_s[16];

    memcpy(ip, m->yiaddr, 4);
    if (nb_ip_zero_p(ip)) return -1;

    net_cfg_get(cur_ip, cur_gw, cur_dns, cur_mask);
    (void)cur_ip;

    if (nb_ip_zero_p(m->gw)) memcpy(gw, cur_gw, 4); else memcpy(gw, m->gw, 4);
    if (nb_ip_zero_p(m->mask)) {
        mask[0] = 255; mask[1] = 255; mask[2] = 255; mask[3] = 0;
    } else {
        memcpy(mask, m->mask, 4);
    }
    if (nb_ip_zero_p(m->dns)) memcpy(dns, gw, 4); else memcpy(dns, m->dns, 4);
    (void)cur_dns; (void)cur_mask;

    if (net_cfg_set(ip, gw, mask, dns) != 0) return -1;

    /* 网关 MAC：优先 ARP；ARP 失败就用 DHCP 响应的源 MAC 兜底
     *（slirp 的网关和 DHCP 服务器是同一台）。ARP 只花总预算里剩下的时间，
     * 保证 DISCOVER..ACK + ARP 整体不超过 NB_DHCP_BUDGET_MS。 */
    now = nb_now_ms();
    arp_budget = ((int32_t)(deadline - now) > 0) ? (deadline - now) : 0U;
    if (arp_budget > NB_ARP_BUDGET_MS) arp_budget = NB_ARP_BUDGET_MS;
    if (arp_budget < 50U || nb_arp_resolve(gw, gwmac, arp_budget) != 0) {
        memcpy(gwmac, m->src_mac, 6);
    }
    nb_arp_store(gw, gwmac);
    net_gw_mac_set(gwmac);

    nb_ip_str(ip, ip_s);
    nb_ip_str(gw, gw_s);
    nb_ip_str(dns, dns_s);
    kprintf("net: dhcp ok ip=%s gw=%s dns=%s\n", ip_s, gw_s, dns_s);
    return 0;
}

static int nb_dhcp_run(void)
{
    uint8_t msg[300];
    uint8_t frame[NB_RX_MAX];
    uint8_t mac[6];
    uint8_t requested[4];
    uint8_t server[4];
    nb_dhcp_msg_t m;
    uint32_t t0;
    uint32_t deadline;
    uint32_t next_tx;
    uint32_t spins;
    int phase = 0;                       /* 0=DISCOVER，1=REQUEST */

    e1000_mac(mac);
    nb_dhcp_xid = nb_rdtsc32() ^ ((uint32_t)mac[4] << 16) ^
                  ((uint32_t)mac[5] << 8) ^ 0x41535445U;
    if (nb_dhcp_xid == 0U) nb_dhcp_xid = 0x41535445U;
    memset(requested, 0, 4);
    memset(server, 0, 4);

    t0 = nb_now_ms();
    deadline = t0 + NB_DHCP_BUDGET_MS;
    next_tx = t0;

    for (spins = 0; spins < NB_DHCP_SPIN_LIMIT; spins++) {
        uint32_t now;

        if ((spins & 31U) == 0U && !nb_deadline_ok(deadline)) break;
        now = nb_now_ms();

        if ((int32_t)(now - next_tx) >= 0) {
            uint32_t n;

            if (phase == 0) {
                n = nb_dhcp_build(msg, sizeof(msg), 1U, 0, 0);
            } else {
                n = nb_dhcp_build(msg, sizeof(msg), 3U, requested, server);
            }
            (void)nb_send_udp(nb_ip_zero, nb_ip_broadcast, nb_mac_broadcast,
                              NB_DHCP_CLIENT_PORT, NB_DHCP_SERVER_PORT, msg, n);
            next_tx = now + NB_DHCP_RETRY_MS;
        }

        {
            int len = e1000_rx_poll(frame, sizeof(frame));

            if (len <= 0) continue;
            if (nb_dhcp_parse(frame, (uint32_t)len, &m) != 0) continue;
            if (m.type == 2U && phase == 0) {        /* OFFER */
                memcpy(requested, m.yiaddr, 4);
                memcpy(server, m.server, 4);
                phase = 1;
                next_tx = now;                       /* 立刻发 REQUEST */
            } else if (m.type == 5U && phase == 1) { /* ACK */
                return nb_dhcp_finish(&m, deadline);
            } else if (m.type == 6U && phase == 1) { /* NAK */
                break;
            }
        }
    }
    return -1;
}

static void nb_dhcp_selftest(void)
{
    if (nb_dhcp_run() != 0) {
        /* 保留 net.c 的静态 10.0.2.15 配置，启动继续。 */
        kprintln("net: dhcp timeout, keep static");
    }
}

/* =============================================================================
 * DNS 解析（A 记录）
 * ============================================================================= */
static uint16_t nb_dns_id;
static uint16_t nb_dns_sport = 53000U;

static int nb_dns_build(uint8_t *buf, size_t cap, const char *name)
{
    uint32_t o = 12U;
    uint32_t total = 0;
    const char *p = name;

    if (cap < 32U || name == 0 || *name == '\0') return -1;
    memset(buf, 0, cap);
    nb_wr16(buf + 0, nb_dns_id);
    nb_wr16(buf + 2, 0x0100U);           /* RD */
    nb_wr16(buf + 4, 1U);                /* QDCOUNT */
    nb_wr16(buf + 6, 0U);
    nb_wr16(buf + 8, 0U);
    nb_wr16(buf + 10, 0U);

    while (*p != '\0') {
        const char *dot = p;
        uint32_t n;

        while (*dot != '\0' && *dot != '.') dot++;
        n = (uint32_t)(dot - p);
        if (n == 0U || n > 63U || o + 1U + n + 6U > cap) return -1;
        buf[o++] = (uint8_t)n;
        memcpy(buf + o, p, n);
        o += n;
        total += n + 1U;
        p = dot;
        if (*p == '.') p++;
    }
    if (total == 0U || total > 253U) return -1;
    buf[o++] = 0U;
    nb_wr16(buf + o, 1U); o += 2U;       /* A */
    nb_wr16(buf + o, 1U); o += 2U;       /* IN */
    return (int)o;
}

static int nb_dns_skip_name(const uint8_t *b, uint32_t len, uint32_t *off)
{
    uint32_t o = *off;
    uint32_t guard = 0;

    while (o < len && guard++ < 128U) {
        uint8_t c = b[o];

        if (c == 0U) {
            *off = o + 1U;
            return 0;
        }
        if ((c & 0xC0U) == 0xC0U) {      /* 压缩指针 */
            if (o + 1U >= len) return -1;
            *off = o + 2U;
            return 0;
        }
        if ((c & 0xC0U) != 0U) return -1;
        o += 1U + c;
    }
    return -1;
}

/* 返回 0 成功；-1 不是有效应答/没有 A 记录；-2 服务器返回错误 RCODE。 */
static int nb_dns_parse(const uint8_t *f, uint32_t len, uint16_t id,
                        uint16_t dport, uint8_t out[4])
{
    nb_udp_view_t v;
    const uint8_t *b;
    uint32_t qd;
    uint32_t an;
    uint32_t o;

    if (nb_parse_udp(f, len, &v) != 0) return -1;
    if (v.sport != 53U || v.dport != dport) return -1;
    b = v.payload;
    if (v.plen < 12U) return -1;
    if (nb_rd16(b + 0) != id) return -1;

    {
        uint16_t flags = nb_rd16(b + 2);

        if ((flags & 0x8000U) == 0U) return -1;          /* 不是响应 */
        if (((flags >> 11) & 0x0FU) != 0U) return -1;    /* opcode != QUERY */
        if ((flags & 0x000FU) != 0U) return -2;          /* RCODE != 0 */
    }

    qd = nb_rd16(b + 4);
    an = nb_rd16(b + 6);
    if (qd == 0U || an == 0U) return -1;

    o = 12U;
    for (uint32_t i = 0; i < qd; i++) {
        if (nb_dns_skip_name(b, v.plen, &o) != 0) return -1;
        if (o + 4U > v.plen) return -1;
        o += 4U;
    }
    for (uint32_t i = 0; i < an && i < 16U; i++) {
        uint16_t type;
        uint16_t class_;
        uint16_t rdlen;

        if (nb_dns_skip_name(b, v.plen, &o) != 0) return -1;
        if (o + 10U > v.plen) return -1;
        type = nb_rd16(b + o + 0);
        class_ = nb_rd16(b + o + 2);
        rdlen = nb_rd16(b + o + 8);
        o += 10U;
        if (o + rdlen > v.plen) return -1;
        if (type == 1U && class_ == 1U && rdlen == 4U) {
            memcpy(out, b + o, 4);
            return 0;
        }
        o += rdlen;
    }
    return -1;
}

/* 返回 0 成功，-2 服务器明确报错，-1 超时/无有效应答。 */
static int nb_dns_query(const char *name, uint8_t out[4], uint32_t budget_ms)
{
    uint8_t msg[300];
    uint8_t frame[NB_RX_MAX];
    uint8_t ip[4], gw[4], dns[4], mask[4], mac[6];
    uint32_t deadline;
    uint16_t sport;
    int n;

    net_cfg_get(ip, gw, dns, mask);
    (void)mask;
    if (nb_ip_zero_p(dns)) memcpy(dns, gw, 4);
    if (nb_ip_zero_p(dns)) return -1;

    if (nb_arp_resolve(dns, mac, NB_ARP_BUDGET_MS) != 0) return -1;

    nb_dns_id++;
    if (nb_dns_id == 0U) nb_dns_id = 1U;
    n = nb_dns_build(msg, sizeof(msg), name);
    if (n < 0) return -1;

    sport = nb_dns_sport++;
    if (nb_dns_sport >= 59000U) nb_dns_sport = 53000U;

    if (nb_send_udp(ip, dns, mac, sport, 53U, msg, (uint32_t)n) != 0) return -1;

    deadline = nb_now_ms() + budget_ms;
    for (uint32_t spins = 0; spins < NB_DNS_SPIN_LIMIT; spins++) {
        int len;

        if ((spins & 31U) == 0U && !nb_deadline_ok(deadline)) break;
        len = e1000_rx_poll(frame, sizeof(frame));
        if (len <= 0) continue;
        {
            int r = nb_dns_parse(frame, (uint32_t)len, nb_dns_id, sport, out);

            if (r == 0) return 0;
            if (r == -2) return -2;      /* 换下一个候选，不白等超时 */
        }
    }
    return -1;
}

static void nb_dns_selftest(void)
{
    static const char *const names[NB_DNS_CANDIDATES] = {
        "example.com", "www.example.com"
    };
    uint8_t ip[4];
    char ip_s[16];

    for (uint32_t i = 0; i < NB_DNS_CANDIDATES; i++) {
        memset(ip, 0, sizeof(ip));
        if (nb_dns_query(names[i], ip, NB_DNS_BUDGET_MS) == 0 &&
            !nb_ip_zero_p(ip)) {
            nb_ip_str(ip, ip_s);
            kprintf("net: dns ok %s=%s\n", names[i], ip_s);
            return;
        }
    }
    kprintln("net: dns timeout");
}

/* =============================================================================
 * 主动 ICMP echo（验证 TX + 收包匹配）
 * ============================================================================= */
static uint16_t nb_ping_ip_id;

/* 成功返回 0，rtt_x10 = 往返时间（0.1ms 为单位，向上取整）。 */
static int nb_ping_one(uint16_t seq, uint32_t *rtt_x10)
{
    uint8_t f[128];
    uint8_t frame[NB_RX_MAX];
    uint8_t self[4], gw[4], dns[4], mask[4], mac[6], smac[6];
    uint8_t *ip = f + 14;
    uint8_t *icmp = ip + 20;
    uint32_t start;
    uint32_t deadline;

    net_cfg_get(self, gw, dns, mask);
    (void)dns; (void)mask;
    if (nb_ip_zero_p(gw)) return -1;
    if (nb_arp_resolve(gw, mac, NB_ARP_BUDGET_MS) != 0) return -1;

    memset(f, 0, sizeof(f));
    memcpy(f, mac, 6);
    e1000_mac(smac);
    memcpy(f + 6, smac, 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    nb_wr16(ip + 2, 60U);                /* 20 IP + 8 ICMP + 32 payload */
    nb_wr16(ip + 4, ++nb_ping_ip_id);
    ip[8] = 64;
    ip[9] = 1;                           /* ICMP */
    memcpy(ip + 12, self, 4);
    memcpy(ip + 16, gw, 4);
    nb_wr16(ip + 10, nb_sum16(ip, 20U));

    icmp[0] = 8;                         /* echo request */
    icmp[1] = 0;
    nb_wr16(icmp + 2, 0);
    nb_wr16(icmp + 4, NB_PING_ID);
    nb_wr16(icmp + 6, seq);
    for (uint32_t i = 0; i < 32U; i++) icmp[8U + i] = (uint8_t)('a' + (i % 26U));
    nb_wr16(icmp + 2, nb_sum16(icmp, 40U));

    start = nb_rdtsc32();
    if (e1000_send_frame(f, 14U + 60U) != 0) return -1;

    deadline = nb_now_ms() + NB_PING_BUDGET_MS;
    for (uint32_t spins = 0; spins < NB_PING_SPIN_LIMIT; spins++) {
        int len;
        const uint8_t *rip;
        const uint8_t *ric;
        int ihl_i;
        uint32_t ihl;
        uint32_t tlen;

        if ((spins & 31U) == 0U && !nb_deadline_ok(deadline)) break;
        len = e1000_rx_poll(frame, sizeof(frame));
        if (len <= 0) continue;
        if ((uint32_t)len < 14U + 20U + 8U || nb_rd16(frame + 12) != 0x0800U) continue;
        rip = frame + 14;
        ihl_i = net_ipv4_validate(rip, (uint32_t)len - 14U);
        if (ihl_i < 0) continue;
        ihl = (uint32_t)ihl_i;
        if (rip[9] != 1U) continue;
        if (memcmp(rip + 12, gw, 4) != 0) continue;
        tlen = nb_rd16(rip + 2);
        if (tlen < ihl + 8U) continue;
        ric = rip + ihl;
        if (ric[0] != 0U || ric[1] != 0U) continue;
        if (nb_sum16(ric, tlen - ihl) != 0U) continue;        /* ICMP 校验和 */
        if (nb_rd16(ric + 4) != NB_PING_ID || nb_rd16(ric + 6) != seq) continue;

        {
            uint32_t d = nb_rdtsc32() - start;
            uint32_t per_100us = nb_tsc_per_ms / 10U;

            if (per_100us == 0U) per_100us = 1U;
            *rtt_x10 = d / per_100us;
            if (*rtt_x10 == 0U) *rtt_x10 = 1U;   /* 有应答就至少记 0.1ms */
            if (*rtt_x10 > 60000U) *rtt_x10 = 60000U;
        }
        return 0;
    }
    return -1;
}

static void nb_ping_selftest(void)
{
    uint32_t ok = 0;

    for (uint16_t seq = 1; seq <= NB_PING_COUNT; seq++) {
        uint32_t rtt_x10 = 0;

        if (nb_ping_one(seq, &rtt_x10) == 0) {
            kprintf("net: ping ok seq=%u rtt=%u.%ums\n",
                    (unsigned int)seq,
                    (unsigned int)(rtt_x10 / 10U), (unsigned int)(rtt_x10 % 10U));
            ok++;
        }
    }
    if (ok == 0U) kprintln("net: ping timeout");
}

/* =============================================================================
 * 入口
 * ============================================================================= */
void net_boot_selftest(void)
{
    static int done;

    if (done) return;
    done = 1;
    if (!e1000_present()) return;

    nb_time_init();
    nb_arp_seed();

    nb_dhcp_selftest();
    nb_dns_selftest();
    nb_ping_selftest();
}
