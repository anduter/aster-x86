#ifndef CORE_NET_H
#define CORE_NET_H

#include "../types.h"

/* 启动 ARP/ICMP/UDP 自检。 */
void net_init(void);

/* 用户态 UDP：目的 IP 4 字节、目的端口、payload；返回 0 / -1。 */
int net_udp_send_to(const uint8_t dst_ip[4], uint16_t dst_port,
                    const void *data, uint32_t len);
/* 轮询收一个 UDP；返回 payload 长度，src_ip/src_port 可为 NULL。 */
int net_udp_recv(uint8_t src_ip[4], uint16_t *src_port,
                 void *buf, uint32_t max_len, uint32_t spins);

#endif
