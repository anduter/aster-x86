#ifndef CORE_NET_H
#define CORE_NET_H

#include "../include/types.h"

/* 启动 ARP/ICMP/UDP 自检。 */
void net_init(void);

/* 旧 SYS_UDP_SEND/RECV 与内核启动自检使用的固定本地端口（可由 -EchoPort 覆盖）。 */
#ifndef NET_ECHO_PORT
#define NET_ECHO_PORT 12345U
#endif
#define NET_UDP_LOCAL_PORT NET_ECHO_PORT

/* 用户态 UDP：目的 IP 4 字节、目的端口、payload；返回 0 / -1。 */
int net_udp_send_to(const uint8_t dst_ip[4], uint16_t dst_port,
                    const void *data, uint32_t len);
/* 指定源端口的发送（socket fd 的本地端口即 fd->handle）；返回 0 / -1。 */
int net_udp_send_from(uint16_t src_port, const uint8_t dst_ip[4], uint16_t dst_port,
                      const void *data, uint32_t len);
/* 给用户态 socket 分配一个本地端口（避开自检端口）。 */
uint16_t net_udp_alloc_port(void);
/* 轮询收一个 UDP；返回 payload 长度，src_ip/src_port 可为 NULL。 */
int net_udp_recv(uint8_t src_ip[4], uint16_t *src_port,
                 void *buf, uint32_t max_len, uint32_t spins);
/* 只接收“目的端口 = local_port”的 UDP。allow_loopback 非 0 时，收包窗口
 * 用尽后会拿最近一次发送的报文做一次本地回环兜底（宿主没有回显端口时用）。 */
int net_udp_recv_port(uint16_t local_port, uint8_t src_ip[4], uint16_t *src_port,
                      void *buf, uint32_t max_len, uint32_t spins, int allow_loopback);

/* 最小 TCP echo client 自检（SYN/SYN-ACK/ACK/data）。 */
int net_tcp_echo_test(void);

#endif
