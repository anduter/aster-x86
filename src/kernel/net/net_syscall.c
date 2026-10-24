/* =============================================================================
 * net/net_syscall.c - 网络相关系统调用（UDP/Socket 组，M5）
 * ============================================================================= */
#include "../types.h"
#include "../proc/trap/interrupt.h"
#include "../proc/trap/syscall_internal.h"
#include "include/net.h"
#include "include/kernel.h"
#include "net_syscall.h"
#include "../proc/trap/syscall.h"

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

bool net_syscall_owns(uint32_t nr)
{
    return nr >= SYS_UDP_SEND && nr <= SYS_RECVFROM;
}

int net_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {    case SYS_UDP_SEND: {
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
             * 这样 sendto/recvfrom 天然不会收到别的测试/别的 socket 的回包。 */
            fd = fd_alloc_kind(p, FD_KIND_UDP, (uint32_t)net_udp_alloc_port(),
                               FD_RIGHT_READ | FD_RIGHT_WRITE);
        }
        ret = fd;
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

        if (fd == 0 || fd->kind != FD_KIND_UDP ||
            (fd->rights & FD_RIGHT_WRITE) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK ||
            req.len > sizeof(req.data)) {
            ret = -1;
            break;
        }
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

        if (fd == 0 || fd->kind != FD_KIND_UDP ||
            (fd->rights & FD_RIGHT_READ) == 0U ||
            copy_from_user(&req, (const void *)(uintptr_t)f->ecx, sizeof(req)) != E_OK) {
            ret = -1;
            break;
        }
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
        return -1000;
    }
    return ret;
}