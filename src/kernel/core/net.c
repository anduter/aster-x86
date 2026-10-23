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

/* ---- UDP 回显：宿主侧需要有一个回显端口（tools/test_udp.ps1 会起） ---- */
#define NET_UDP_PORT 12345U

static const char net_udp_payload[] = "aster-udp-echo";

static void net_udp_send(void)
{
    uint8_t f[64];
    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint32_t plen = (uint32_t)(sizeof(net_udp_payload) - 1U);

    /* 没解析到网关 MAC 就发不出去 */
    if ((gw_mac[0] | gw_mac[1] | gw_mac[2] | gw_mac[3] | gw_mac[4] | gw_mac[5]) == 0) return;

    memset(f, 0, sizeof(f));
    memcpy(f, gw_mac, 6);
    e1000_mac(f + 6);
    f[12] = 0x08; f[13] = 0x00;

    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20U + 8U + plen));
    wr16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = 17;                          /* UDP */
    memcpy(ip + 12, local_ip, 4);
    memcpy(ip + 16, gw_ip, 4);
    wr16(ip + 10, checksum(ip, 20));

    wr16(udp + 0, NET_UDP_PORT);         /* 源端口 = 目的端口，方便回显 */
    wr16(udp + 2, NET_UDP_PORT);
    wr16(udp + 4, (uint16_t)(8U + plen));
    wr16(udp + 6, 0);                    /* 校验和 0：IPv4 允许不校验 */
    memcpy(udp + 8, net_udp_payload, plen);

    (void)e1000_send_frame(f, 14U + 20U + 8U + plen);
}

static int net_parse_udp(const uint8_t *f, uint32_t len)
{
    const uint8_t *ip;
    const uint8_t *udp;
    uint32_t ihl;
    uint32_t plen = (uint32_t)(sizeof(net_udp_payload) - 1U);

    if (len < 14U + 20U + 8U || rd16(f + 12) != 0x0800U) return 0;
    ip = f + 14;
    if ((ip[0] >> 4) != 4U || ip[9] != 17U) return 0;
    ihl = (uint32_t)(ip[0] & 0x0FU) * 4U;
    if (ihl < 20U || 14U + ihl + 8U + plen > len) return 0;
    udp = ip + ihl;
    if (rd16(udp + 0) != NET_UDP_PORT || rd16(udp + 2) != NET_UDP_PORT) return 0;
    if (memcmp(udp + 8, net_udp_payload, plen) != 0) return 0;
    kprintln("net: UDP echo reply");
    return 1;
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
    net_udp_send();
    for (volatile uint32_t n = 0; n < 30000000U; n++) {
        int len = e1000_rx_poll(buf, sizeof(buf));
        if (len > 0 && net_parse_udp(buf, (uint32_t)len)) return;
    }
    kprintln("net: UDP echo timeout (no host echo port)");
}
