#ifndef CORE_NET_H
#define CORE_NET_H

/* 最小网络验证：ARP 解析网关 MAC，再发 ICMP echo 并收 reply。
 * 不实现完整协议栈、不启中断；仅轮询 RX。 */
void net_init(void);

#endif
