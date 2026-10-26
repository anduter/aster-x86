/* =============================================================================
 * net/net_syscall.c - 网络相关系统调用（UDP/Socket + TCP listen/accept）
 *
 * TCP 服务端（ABI v1.4 预留号）：
 *   SYS_SOCKET(52)   ebx=AF_INET, ecx=SOCK_STREAM(1) -> fd（TCP socket）
 *   SYS_LISTEN(78)   ebx=port, ecx=backlog            -> 0 / -1
 *   SYS_ACCEPT(79)   ebx=sock(fd 或监听端口), ecx=&addr -> 连接 fd / -1
 *   SYS_SENDTO(53)   ebx=fd, ecx=&udp_req             -> 写入字节数 / -1
 *   SYS_RECVFROM(54) ebx=fd, ecx=&udp_req             -> 读出字节数 / -1
 *   SYS_CLOSE(9)     由 fs 组处理（清 fd 表项）；网络侧在下一个网络 syscall
 *                    入口的 net_tcp_sock_gc() 里回收 socket/连接。
 *
 * 语义细节（与 docs/claims/net.md 一致）：
 *   - SYS_LISTEN 绑定“当前进程最近分配且尚未绑定的 TCP socket”；没有这样的
 *     socket 时建一个内核持有的监听 socket（此时用端口号也能 SYS_ACCEPT）。
 *   - 重复 bind / 端口处于 TIME_WAIT -> -1；backlog 截断到 [1,4]。
 *   - SYS_ACCEPT 有界阻塞（2s），只接受三次握手已完成的连接；addr 布局
 *     { uint8_t ip[4]; uint16_t port; }（6 字节，与 NET_TCP_ADDR_LEN 一致）。
 *   - SYS_SENDTO/SYS_RECVFROM 对 TCP fd 复用 udp_req 的 data/len 字段
 *     （单次 <= 64 字节，ABI 结构冻结不改）。
 * ============================================================================= */
#include "../include/types.h"
#include "include/proc.h"
#include "include/net.h"
#include "include/kernel.h"
#include "net_internal.h"
#include "net_syscall.h"
#include "../proc/trap/syscall_internal.h"

/* SYS_ACCEPT 的有界等待（ticks，100Hz）：2s */
#define NET_TCP_ACCEPT_WAIT 200U
/* SYS_RECVFROM 对 TCP fd 的有界等待：1s */
#define NET_TCP_RECV_WAIT   100U

static int udp_recv_yield(uint16_t local_port, uint8_t ip[4], uint16_t *port,
                          void *buf, uint32_t len, int allow_loopback)
{
    int n = -1;

    for (int i = 0; i < 40; i++) {
        n = net_udp_recv_port(local_port, ip, port, buf, len, 20000U, 0);
        if (n >= 0) break;
        proc_sleep_ticks(1U);
    }

    /* 宿主始终没回显：最后允许一次本地回环兜底（会打印日志） */
    if (n < 0 && allow_loopback) {
        n = net_udp_recv_port(local_port, ip, port, buf, len, 1U, 1);
    }
    return n;
}

/* 从 fd->handle 解出 TCP 监听 socket 号；不是 TCP fd / 不是监听 socket 返回 -1 */
static int tcp_listen_sock_of_fd(proc_t *p, int fd)
{
    proc_fd_t *f = fd_get(p, fd);
    int sock;

    if (f == 0 || f->kind != FD_KIND_TCP) return -1;
    if ((f->handle & NET_TCP_HDL_TAG_MASK) != NET_TCP_HDL_LISTEN) return -1;
    sock = (int)(f->handle & NET_TCP_HDL_ID_MASK);
    return net_tcp_sock_listening(sock) ? sock : -1;
}

/* 已接受连接的 fd -> conn id */
static int tcp_conn_of_fd(proc_t *p, int fd)
{
    proc_fd_t *f = fd_get(p, fd);

    if (f == 0 || f->kind != FD_KIND_TCP) return -1;
    if ((f->handle & NET_TCP_HDL_TAG_MASK) != NET_TCP_HDL_CONN) return -1;
    return (int)(f->handle & NET_TCP_HDL_ID_MASK);
}

/* 找当前进程“已分配、还没绑定端口”的 TCP socket（SYS_LISTEN 用） */
static int tcp_unbound_sock_of_proc(proc_t *p)
{
    for (int i = 3; i < PROC_FD_MAX; i++) {
        proc_fd_t *f = &p->fds[i];
        int sock;

        if (!f->used || f->kind != FD_KIND_TCP) continue;
        if ((f->handle & NET_TCP_HDL_TAG_MASK) != NET_TCP_HDL_LISTEN) continue;
        sock = (int)(f->handle & NET_TCP_HDL_ID_MASK);
        if (net_tcp_sock_port(sock) == 0 && net_tcp_sock_listening(sock) == 0) return sock;
    }
    return -1;
}

/* SYS_ACCEPT 的 sock 参数：优先按 fd（TCP 监听 socket）解释，其次按监听端口 */
static int tcp_accept_sock_of_arg(proc_t *p, uint32_t arg)
{
    int sock = tcp_listen_sock_of_fd(p, (int)arg);

    if (sock >= 0) return sock;
    if (arg == 0U || arg > 65535U) return -1;
    return net_tcp_sock_of_port((uint16_t)arg);
}

static int tcp_sys_listen(proc_t *p, uint32_t port_arg, uint32_t backlog_arg)
{
    int sock;

    if (p == 0 || port_arg == 0U || port_arg > 65535U) return -1;

    net_tcp_sock_gc();

    /* 绑定本进程已分配但未绑定的 TCP socket；没有就建一个进程持有的监听 socket */
    sock = tcp_unbound_sock_of_proc(p);
    if (sock < 0) {
        /* audit-net：兜底 listener 以前用 net_tcp_socket(0)（owner_pid=0），
         * GC 永远不回收 -> 普通进程可以反复 SYS_LISTEN 永久占满 8 个 socket
         * 槽和任意端口（拒绝服务）。现在记在调用者 pid 名下、每进程最多 1 个，
         * 进程退出时由 net_tcp_sock_gc() 回收。 */
        if (net_tcp_pid_orphan_listeners((int)p->pid) != 0) return -1;
        sock = net_tcp_socket((int)p->pid);
        if (sock < 0) return -1;
        if (net_tcp_listen(sock, (uint16_t)port_arg, backlog_arg) != 0) {
            (void)net_tcp_sock_close(sock);
            return -1;
        }
        return 0;
    }
    return (net_tcp_listen(sock, (uint16_t)port_arg, backlog_arg) == 0) ? 0 : -1;
}

static int tcp_sys_accept(proc_t *p, uint32_t sock_arg, uint32_t addr_ptr)
{
    uint8_t peer_ip[4];
    uint16_t peer_port = 0;
    int sock;
    int conn;
    int fd;

    if (p == 0) return -1;

    net_tcp_sock_gc();

    sock = tcp_accept_sock_of_arg(p, sock_arg);
    if (sock < 0) return -1;

    conn = net_tcp_accept(sock, peer_ip, &peer_port, NET_TCP_ACCEPT_WAIT);
    if (conn < 0) return -1;

    fd = fd_alloc_kind(p, FD_KIND_TCP, NET_TCP_HDL_CONN | (uint32_t)conn,
                       FD_RIGHT_READ | FD_RIGHT_WRITE);
    if (fd < 0) {
        (void)net_tcp_conn_close(conn);
        return -1;
    }
    net_tcp_conn_owner(conn, (int)p->pid, fd);

    if (addr_ptr != 0U) {
        struct {
            uint8_t  ip[4];
            uint16_t port;
        } addr;

        memcpy(addr.ip, peer_ip, 4);
        addr.port = peer_port;
        if (copy_to_user((void *)(uintptr_t)addr_ptr, &addr, sizeof(addr)) != E_OK) {
            memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
            (void)net_tcp_conn_close(conn);
            return -1;
        }
    }
    return fd;
}

bool net_syscall_owns(uint32_t nr)
{
    return (nr >= SYS_UDP_SEND && nr <= SYS_RECVFROM) ||
           nr == SYS_LISTEN || nr == SYS_ACCEPT;
}

int net_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    net_tcp_sock_gc();

    switch (f->eax) {
    case SYS_UDP_SEND: {
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;

        if (copy_from_user(&req, (const void *)(uintptr_t)f->ebx, sizeof(req)) != E_OK ||
            req.len > sizeof(req.data)) {
            ret = -1;
            break;
        }
        ret = net_udp_send_to(req.ip, req.port, req.data, req.len);
        break;
    }
    case SYS_UDP_RECV: {
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;
        int n;

        if (copy_from_user(&req, (const void *)(uintptr_t)f->ebx, sizeof(req)) != E_OK) {
            ret = -1;
            break;
        }
        /* 旧路径固定用 12345 端口发/收；只收发给该端口的回包，且宿主
         * 完全没回显时允许一次本地回环兜底，保证 ring3 自检可判定。 */
        n = udp_recv_yield(NET_UDP_LOCAL_PORT, req.ip, &req.port,
                           req.data, sizeof(req.data), 1);
        if (n < 0) { ret = -1; break; }
        req.len = (uint16_t)n;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, &req, sizeof(req)) == E_OK) ? n : -1;
        break;
    }
    case SYS_SOCKET: {
        proc_t *p = proc_current();
        int fd = -1;

        if (p != 0) {
            /* socket 的本地端口就是 fd->handle：每个 socket 用独立端口，
             * 这样 sendto/recvfrom 天然不会收到别的测试/别的 socket 的回包。
             * type 1 = SOCK_STREAM -> TCP socket（listen/accept 用）；
             * 其它（ABI 既有调用都传 2 = SOCK_DGRAM）走 UDP。 */
            if (f->ecx == 1U) {
                int sock = net_tcp_socket((int)p->pid);

                if (sock >= 0) {
                    fd = fd_alloc_kind(p, FD_KIND_TCP,
                                       NET_TCP_HDL_LISTEN | (uint32_t)sock,
                                       FD_RIGHT_READ | FD_RIGHT_WRITE);
                    if (fd >= 0) {
                        net_tcp_sock_owner(sock, (int)p->pid, fd);
                    } else {
                        (void)net_tcp_sock_close(sock);
                    }
                }
            } else {
                fd = fd_alloc_kind(p, FD_KIND_UDP, (uint32_t)net_udp_alloc_port(),
                                   FD_RIGHT_READ | FD_RIGHT_WRITE);
            }
        }
        ret = fd;
        break;
    }
    case SYS_LISTEN: {
        ret = tcp_sys_listen(proc_current(), f->ebx, f->ecx);
        break;
    }
    case SYS_ACCEPT: {
        ret = tcp_sys_accept(proc_current(), f->ebx, f->ecx);
        break;
    }
    case SYS_SENDTO: {
        proc_t *p = proc_current();
        proc_fd_t *fd = fd_get(p, (int)f->ebx);
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;

        if (fd == 0 || (fd->rights & FD_RIGHT_WRITE) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK ||
            req.len > sizeof(req.data)) {
            ret = -1;
            break;
        }
        if (fd->kind == FD_KIND_TCP) {
            int conn = tcp_conn_of_fd(p, (int)f->ebx);

            ret = (conn >= 0) ? net_tcp_conn_write(conn, req.data, req.len) : -1;
            break;
        }
        if (fd->kind != FD_KIND_UDP) { ret = -1; break; }
        ret = net_udp_send_from((uint16_t)fd->handle, req.ip, req.port,
                                req.data, req.len);
        break;
    }
    case SYS_RECVFROM: {
        proc_t *p = proc_current();
        proc_fd_t *fd = fd_get(p, (int)f->ebx);
        struct {
            uint8_t  ip[4];
            uint16_t port;
            uint16_t len;
            uint8_t  data[64];
        } req;
        int n;

        if (fd == 0 || (fd->rights & FD_RIGHT_READ) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK) {
            ret = -1;
            break;
        }
        if (fd->kind == FD_KIND_TCP) {
            int conn = tcp_conn_of_fd(p, (int)f->ebx);

            if (conn < 0) { ret = -1; break; }
            n = net_tcp_conn_read(conn, req.data, sizeof(req.data), NET_TCP_RECV_WAIT);
            if (n <= 0) { ret = -1; break; }     /* 超时 / 对端已关 */
            req.len = (uint16_t)n;
            ret = (copy_to_user((void *)(uintptr_t)f->ecx, &req, sizeof(req)) == E_OK) ? n : -1;
            break;
        }
        if (fd->kind != FD_KIND_UDP) { ret = -1; break; }
        /* 只收“目的端口 = socket 本地端口(fd->handle)”的报文；与 SYS_UDP_*
         * 的固定 12345 端口彼此隔离，不会互相吃回包。 */
        n = udp_recv_yield((uint16_t)fd->handle, req.ip, &req.port,
                           req.data, sizeof(req.data), 1);
        if (n < 0) { ret = -1; break; }
        req.len = (uint16_t)n;
        ret = (copy_to_user((void *)(uintptr_t)f->ecx, &req, sizeof(req)) == E_OK) ? n : -1;
        break;
    }
    default:
        /* 不是网络组：不吞掉别人的调用号（返回哨兵值，由分发层继续） */
        return -1000;
    }
    return ret;
}

/* =============================================================================
 * V6 回归：SYS_LISTEN 兜底 listener 的归属 / 每进程上限（audit-net）
 *
 * 旧实现用 net_tcp_socket(0) 建兜底 listener（owner_pid=0），GC 永远跳过它，
 * 于是普通进程可以反复 SYS_LISTEN 永久占住端口与 socket 槽。这里断言两件事：
 *   1. 兜底 listener 记在当前进程 pid 名下、且没有 fd（GC 才能按"进程退出"回收）；
 *   2. 每进程最多 1 个这种 listener，第二个端口必须被拒。
 * 期望串口输出：net: audit listen socket owned by pid ok
 * ========================================================================== */
#define NET_AUDIT_ORPHAN_PORT  15074U
#define NET_AUDIT_SECOND_PORT  15076U

int net_syscall_audit_selftest(void)
{
    proc_t *p = proc_current();
    interrupt_frame_t f;
    int pid;
    int s;

    if (p == 0) return 0;
    pid = (int)p->pid;

    /* 前置条件：本线程没有可绑定的 TCP fd、也没有遗留的兜底 listener。
     * 不满足就跳过并打印原因，避免"静默通过"。 */
    if (tcp_unbound_sock_of_proc(p) >= 0 || net_tcp_pid_orphan_listeners(pid) != 0) {
        kprintln("net: audit listen socket SKIPPED (precondition)");
        return 0;
    }

    memset(&f, 0, sizeof(f));
    f.eax = (unsigned int)SYS_LISTEN;
    f.ebx = NET_AUDIT_ORPHAN_PORT;
    f.ecx = 2U;
    if (net_syscall(&f) != 0) {
        kprintln("net: audit listen socket owned by pid FAILED (listen)");
        return -1;
    }
    if (net_tcp_pid_orphan_listeners(pid) != 1) {
        kprintln("net: audit listen socket owned by pid FAILED (owner)");
        s = net_tcp_sock_of_port(NET_AUDIT_ORPHAN_PORT);
        if (s >= 0) (void)net_tcp_sock_close(s);
        return -1;
    }

    f.ebx = NET_AUDIT_SECOND_PORT;
    if (net_syscall(&f) != -1) {
        kprintln("net: audit listen socket owned by pid FAILED (no per-pid cap)");
        s = net_tcp_sock_of_port(NET_AUDIT_ORPHAN_PORT);
        if (s >= 0) (void)net_tcp_sock_close(s);
        s = net_tcp_sock_of_port(NET_AUDIT_SECOND_PORT);
        if (s >= 0) (void)net_tcp_sock_close(s);
        return -1;
    }

    s = net_tcp_sock_of_port(NET_AUDIT_ORPHAN_PORT);
    if (s >= 0) (void)net_tcp_sock_close(s);
    kprintln("net: audit listen socket owned by pid ok");
    return 0;
}
