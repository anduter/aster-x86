#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

/* =============================================================================
 * net/net_internal.h - net 模块内部共享的解析校验原语（只给 net 模块自己用）
 *
 * 这些函数不导出到 include/net.h：它们只负责"一个帧能不能进协议栈"的
 * 静态校验，调用者都是 net.c / net_boot.c / tcp.c 自己的解析入口。
 *
 * 背景（audit-net，见 docs/audit/net.md）：
 *   - 所有 IPv4 收包入口都必须过 net_ipv4_validate()：校验首部校验和、
 *     总长/帧长一致性，并拒绝分片（本栈没有重组，把片当完整报文解析会
 *     直接把半截 payload 交给上层）；
 *   - UDP 入口必须过 net_udp_validate()：声明长度必须整段落在帧内，
 *     非 0 校验和必须正确（0 在 IPv4 下表示"未计算"，仍然允许）。
 * ========================================================================== */

#include "../include/types.h"

/* 校验 IPv4 首部（ip 指向首部起点，avail = 从 ip 到帧尾的字节数）。
 * 成功返回 IHL（20..60），失败返回 -1（调用者丢弃整帧）。 */
int net_ipv4_validate(const uint8_t *ip, uint32_t avail);

/* 校验 UDP 头（ip 指向 IPv4 首部，udp 指向 UDP 首部，avail = udp 到帧尾字节数）。
 * 成功返回 0 并写入声明的 UDP 长度（含 8 字节头）；失败返回 -1。 */
int net_udp_validate(const uint8_t *ip, const uint8_t *udp, uint32_t avail,
                     uint32_t *ulen);

/* IPv4 首部校验和（16 位反码和；返回 0 表示首部完好）。 */
uint16_t net_ip_checksum(const uint8_t *ip, uint32_t ihl);

/* 某进程持有的"没有 fd 的监听 socket"数量（tcp.c 实现；限 1 个/进程防 DoS）。 */
int net_tcp_pid_orphan_listeners(int pid);

/* net_syscall.c 的 SYS_LISTEN 兜底 listener 归属/上限负向用例，返回 0 = 全过。 */
int net_syscall_audit_selftest(void);

/* net_boot.c 的 DNS 事务 ID 熵用例（防 off-path 伪造响应），返回 0 = 全过。 */
int nb_dns_audit_selftest(void);

/* net.c 的畸形包负向用例（ARP 欺骗 / UDP 校验和 / 分片 / 长度），返回 0 = 全过。 */
int net_audit_selftest(void);

#endif /* NET_INTERNAL_H */
