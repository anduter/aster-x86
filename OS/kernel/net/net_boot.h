/* =============================================================================
 * net/net_boot.h - 启动期网络自检（DHCP / DNS / 主动 ICMP）
 *
 * 这些接口只在启动阶段使用一次：net_init() 末尾调用 net_boot_selftest()，
 * 依次跑 DHCP 客户端、DNS A 记录解析、向网关主动 ping。每一步都有毫秒级
 * 上界，失败只降级（保留静态配置），绝不阻塞启动。
 *
 * 这里同时声明 net.c 暴露给自检的内部配置访问函数；它们不是 syscall，
 * 也不出现在用户态 ABI 里。
 * ============================================================================= */
#ifndef CORE_NET_BOOT_H
#define CORE_NET_BOOT_H

#include "../include/types.h"

/* 启动期自检入口；只在第一次调用时执行，重复调用直接返回。 */
void net_boot_selftest(void);

/* ---- net.c 的内部配置状态（DHCP 成功后就地更新） ---- */
/* 任一出参可以为 NULL；没有配置的地址返回 0.0.0.0。 */
void net_cfg_get(uint8_t ip[4], uint8_t gw[4], uint8_t dns[4], uint8_t mask[4]);
/* 应用一份新配置；ip 非法返回 -1。网关变化时清掉缓存的网关 MAC。 */
int  net_cfg_set(const uint8_t ip[4], const uint8_t gw[4],
                 const uint8_t mask[4], const uint8_t dns[4]);
/* net.c 维护的网关 MAC（ARP 自检/自检模块解析后写入）。 */
void net_gw_mac_get(uint8_t mac[6]);
void net_gw_mac_set(const uint8_t mac[6]);

#endif /* CORE_NET_BOOT_H */
