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

/* ---- 阶段 4c：tcp.c 用的发送/收包原语 ---- */
/* 以太网+IPv4 封装并发送（目的 MAC 用网关 MAC）；返回 0 / -1。
 * 目的 IP 就是本机地址时走“本地回环”：帧直接进软件 RX 队列，不经网卡
 * （QEMU slirp 不转发 127.0.0.1，listen/accept 自检靠这条路径）。 */
int net_ip_send(const uint8_t dst_ip[4], uint8_t proto,
                const uint8_t *payload, uint32_t len);
/* 从网卡收一帧（非阻塞）；先吐本地回环队列，再轮询 e1000；返回长度 / <=0 */
int net_rx_frame(uint8_t *buf, uint32_t max);
const uint8_t *net_local_ip_ptr(void);
const uint8_t *net_gw_ip_ptr(void);

/* 最小 TCP echo client 自检（SYN/SYN-ACK/ACK/data）。 */
int net_tcp_echo_test(void);
/* 阶段 4c：TCP 健壮性自检（RTO/重传/乱序/FIN/TIME_WAIT/校验和/大传输） */
int net_tcp_robust_selftest(void);
/* 阶段 4d：TCP 服务端 listen/accept 自检（本机回环 + backlog/TIME_WAIT 负向用例） */
int net_tcp_listen_selftest(void);

/* =============================================================================
 * TCP 服务端：listen/accept（legacy ABI：SYS_LISTEN=78 / SYS_ACCEPT=79）
 *
 * 语义（与 docs/claims/net.md 一致）：
 *   - net_tcp_socket() 分配一个 socket 槽；用户态 fd->handle 就是这个槽号，
 *     pid 只用于“进程退出/fd 关闭后回收”，pid=0 表示内核自检持有；
 *   - net_tcp_listen(sock, port, backlog) 绑定并监听。端口已被监听、已被某个
 *     连接占用、或连接处于 TIME_WAIT 时返回 -1（重复 bind 拒绝）；
 *     backlog 截断到 [1, NET_TCP_BACKLOG_MAX]；
 *   - net_tcp_accept(sock, ip, port, timeout_ticks) 有界阻塞等待“三次握手
 *     已完成”的连接，返回连接句柄（>=0）；超时/无监听返回 -1。半开连接
 *     （SYN 已收、ACK 未到）只占 SYN 队列，不会被 accept 取走；
 *   - 连接句柄支持 net_tcp_conn_read/write/close：read 有界超时，
 *     close 主动发 FIN 并进入 TIME_WAIT（2*MSL 简化值），窗口内端口仍被
 *     占用，过期后释放；
 *   - 关闭监听 socket 时端口立即释放，未取走的半开/已完成连接一并丢弃。
 *
 * fd 归连接/监听所有：SYS_CLOSE 只清 fd 表项，net_tcp_sock_gc() 在每次网络
 * syscall 入口把“fd 已经不在了”的 socket/连接回收掉（进程退出同理）。
 * ========================================================================== */
#define NET_TCP_BACKLOG_MAX 4U
#define NET_TCP_ADDR_LEN    6U   /* uint8_t ip[4] + uint16_t port（用户可见布局） */

/* 网络模块自有的 fd 类型：proc/proc.h 的 FD_KIND_* 序列（0..5）之后追加，
 * proc.h 由 proc 会话维护，这里只声明编号，不动它的枚举。 */
#define FD_KIND_TCP 6

/* fd->handle 的编码（用户不可见，net 模块内部 + 文档用）：
 *   监听 socket：NET_TCP_HDL_LISTEN | sock_id
 *   已接受连接：NET_TCP_HDL_CONN   | conn_id
 * SYS_ACCEPT 的 ebx 既接受“监听 fd”，也接受“监听端口”。 */
#define NET_TCP_HDL_LISTEN   0x1000U
#define NET_TCP_HDL_CONN     0x2000U
#define NET_TCP_HDL_TAG_MASK 0xF000U
#define NET_TCP_HDL_ID_MASK  0x0FFFU

int net_tcp_socket(int pid);
int net_tcp_sock_close(int sock);
int net_tcp_sock_conn(int sock);
int net_tcp_sock_port(int sock);
int net_tcp_sock_listening(int sock);
int net_tcp_sock_of_port(uint16_t port);
void net_tcp_sock_owner(int sock, int pid, int fd);
void net_tcp_sock_gc(void);

int net_tcp_listen(int sock, uint16_t port, uint32_t backlog);
/* 便捷入口（内核自检用，pid=0）：返回 socket 槽号 / -1。 */
int net_tcp_listen_port(uint16_t port, uint32_t backlog);
int net_tcp_accept(int sock, uint8_t peer_ip[4], uint16_t *peer_port,
                   uint32_t timeout_ticks);
int net_tcp_accept_port(uint16_t port, uint8_t peer_ip[4], uint16_t *peer_port,
                        uint32_t timeout_ticks);
int net_tcp_conn_read(int conn, void *buf, uint32_t max_len, uint32_t timeout_ticks);
int net_tcp_conn_write(int conn, const void *buf, uint32_t len);
int net_tcp_conn_close(int conn);
int net_tcp_conn_port(int conn);
/* 记录连接句柄的属主 fd（GC 用：fd 关闭/进程退出后回收连接） */
void net_tcp_conn_owner(int conn, int pid, int fd);

/* 本地回环：把一帧以太网帧塞进 RX 队列（源/宿都是本机）。返回 0 / -1。
 * 自检用它扮演“外部对端”，走完整的 parse/校验和/分发路径。 */
int net_loopback_push(const uint8_t *frame, uint32_t len);

#endif
