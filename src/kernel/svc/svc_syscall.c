/* =============================================================================
 * svc/svc_syscall.c - 服务/能力相关系统调用（M6 + P1 IPC）
 *
 * svc 组：pipe / dup2 / cap_drop / cap_send / input_set，以及 ABI 1.5 预留给
 * 三层架构的 80..84（PORT_CREATE/SEND/RECV/REPLY/CAP_GRANT）。
 * 未实现的编号由分发器统一返回 -1，本文件不认领。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/proc.h"
#include "svc_syscall.h"
#include "ipc.h"
#include "../proc/trap/syscall_internal.h"

/* IPC 消息最多 4144B；syscall 在拷贝到内核缓冲后调用核心，核心不再碰用户指针。
 * 缓冲放在独立 helper 的栈帧里，避免 svc_syscall() 为所有调用预留 8KB。 */
static int svc_ipc_send(uint32_t port, uint32_t uptr, uint32_t len)
{
    uint8_t msg[IPC_MSG_MAX];
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (len < IPC_HDR_SIZE || len > IPC_MSG_MAX) return -1;
    if (copy_from_user(msg, (const void *)(uintptr_t)uptr, len) != E_OK) {
        return -1;
    }
    return ipc_port_send(port, msg, len, p);
}

static int svc_ipc_reply(uint32_t ticket, uint32_t uptr, uint32_t len)
{
    uint8_t msg[IPC_MSG_MAX];
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (len < IPC_HDR_SIZE || len > IPC_MSG_MAX) return -1;
    if (copy_from_user(msg, (const void *)(uintptr_t)uptr, len) != E_OK) {
        return -1;
    }
    return ipc_port_reply(ticket, msg, len, p);
}

static int svc_ipc_recv(uint32_t port, uint32_t uptr, uint32_t maxlen)
{
    uint8_t buf[IPC_MSG_MAX];
    proc_t *p = proc_current();
    uint32_t n;
    int r;

    if (p == 0) return -1;
    if (maxlen == 0U || maxlen > IPC_MSG_MAX) return -1;
    if (!user_range_ok(uptr, maxlen)) return -1;     /* 睡前先验缓冲 */
    r = ipc_port_recv(port, buf, maxlen, p);
    if (r < 0) return r;

    /* r 是消息真实长度；用户缓冲小时只回填 maxlen，仍返回真实长度。 */
    n = (uint32_t)r;
    if (n > maxlen) n = maxlen;
    if (copy_to_user((void *)(uintptr_t)uptr, buf, n) != E_OK) return -1;
    return r;
}

bool svc_syscall_owns(uint32_t nr)
{
    return nr == SYS_PIPE || nr == SYS_DUP2 ||
           nr == SYS_CAP_DROP || nr == SYS_CAP_SEND ||
           nr == SYS_INPUT_SET ||
           nr == SYS_PORT_CREATE || nr == SYS_PORT_SEND ||
           nr == SYS_PORT_RECV || nr == SYS_PORT_REPLY ||
           nr == SYS_CAP_GRANT;
}

int svc_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_INPUT_SET: {
        char buf[128];
        if (f->ecx >= (uint32_t)sizeof(buf)) {
            ret = -1;
            break;
        }
        if (copy_from_user(buf, (const void *)(uintptr_t)f->ebx, f->ecx) != E_OK) {
            ret = -1;
            break;
        }
        buf[f->ecx] = '\0';
        console_input_set(buf);     /* 只画在底部输入框，不滚动输出区 */
        ret = 0;
        break;
    }
    case SYS_PIPE: {
        proc_t *p = proc_current();
        int idx, rfd, wfd;
        int fds[2];

        if (p == 0) { ret = -1; break; }
        idx = pipe_create();
        if (idx < 0) { ret = -1; break; }

        rfd = fd_alloc_kind(p, FD_KIND_PIPE_R, (uint32_t)idx, FD_RIGHT_READ);
        wfd = fd_alloc_kind(p, FD_KIND_PIPE_W, (uint32_t)idx, FD_RIGHT_WRITE);
        if (rfd < 0 || wfd < 0) {
            /* pipe_create() 预置了 1 读 + 1 写引用；失败时两个 fd 都不要了，
             * 必须把两侧引用都还回去，否则槽位永久泄漏。 */
            if (rfd >= 0) memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
            if (wfd >= 0) memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
            pipe_unref_read(idx);
            pipe_unref_write(idx);
            ret = -1;
            break;
        }

        fds[0] = rfd;
        fds[1] = wfd;
        if (copy_to_user((void *)(uintptr_t)f->ebx, fds, sizeof(fds)) != E_OK) {
            /* 用户缓冲坏了：把刚分配的两端还回去，不能留下不可见的 fd/管道。 */
            memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
            memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
            pipe_unref_read(idx);
            pipe_unref_write(idx);
            ret = -1;
            break;
        }
        ret = 0;
        break;
    }
    case SYS_DUP2: {
        proc_t *p = proc_current();
        proc_fd_t *old;
        int oldfd = (int)f->ebx;
        int newfd = (int)f->ecx;

        if (p == 0) { ret = -1; break; }
        old = fd_get(p, oldfd);
        if (old == 0 || newfd < 0 || newfd >= PROC_FD_MAX) { ret = -1; break; }
        if (newfd == oldfd) { ret = newfd; break; }

        /* 先释放 newfd 原有的内容 */
        if (p->fds[newfd].used) {
            if (p->fds[newfd].kind == FD_KIND_FILE) {
                fs_close_ex(p->fds[newfd].fs_be, (int)p->fds[newfd].handle);
            } else if (p->fds[newfd].kind == FD_KIND_PIPE_W) {
                pipe_unref_write((int)p->fds[newfd].handle);
            } else if (p->fds[newfd].kind == FD_KIND_PIPE_R) {
                pipe_unref_read((int)p->fds[newfd].handle);
            }
        }

        p->fds[newfd] = *old;
        if (p->fds[newfd].kind == FD_KIND_PIPE_W) {
            pipe_ref_write((int)p->fds[newfd].handle);
        } else if (p->fds[newfd].kind == FD_KIND_PIPE_R) {
            /* 读端也是共享资源：dup 后两份 fd 各自持有一个读引用，close 一份
             * 不会把管道槽位释放掉（否则另一份会读到 -1 或复用后的别家数据）。 */
            pipe_ref_read((int)p->fds[newfd].handle);
        } else if (p->fds[newfd].kind == FD_KIND_FILE) {
            if (fs_ref_ex(p->fds[newfd].fs_be, (int)p->fds[newfd].handle) != E_OK) {
                /* 该后端不支持句柄共享（如 FAT16）：拒绝 dup2，避免双重关闭 */
                memset(&p->fds[newfd], 0, sizeof(p->fds[newfd]));
                ret = -1;
                break;
            }
        }
        ret = newfd;
        break;
    }
    case SYS_CAP_DROP: {
        /* 显式撤销 fd 的读/写 capability；之后同一 fd 的对应操作立即失败。 */
        proc_fd_t *fd = fd_get(proc_current(), (int)f->ebx);
        uint32_t drop = f->ecx & (FD_RIGHT_READ | FD_RIGHT_WRITE);

        if (fd == 0) { ret = -1; break; }
        fd->rights &= ~drop;
        ret = 0;
        break;
    }
    case SYS_CAP_SEND: {
        /* 把当前进程的文件 fd capability 发送给目标进程（source 保留）。 */
        proc_t *p = proc_current();
        proc_t *target = proc_find(f->ebx);
        proc_fd_t *src = fd_get(p, (int)f->ecx);
        uint32_t rights;
        int tfd;

        if (p == 0 || target == 0 || target == p || src == 0 ||
            src->kind != FD_KIND_FILE) {
            ret = -1;
            break;
        }
        rights = (f->edx != 0U) ? f->edx : src->rights;
        rights &= src->rights;
        if (rights == 0U) { ret = -1; break; }

        tfd = fd_alloc_kind(target, FD_KIND_FILE, src->handle, rights);
        if (tfd < 0) { ret = -1; break; }
        if (fs_ref_ex(src->fs_be, (int)src->handle) != E_OK) {
            memset(&target->fds[tfd], 0, sizeof(target->fds[tfd]));
            ret = -1;
            break;
        }
        target->fds[tfd].fs_be = src->fs_be;
        ret = tfd;
        break;
    }
    case SYS_PORT_CREATE: {
        /* flags 只有 bit0（私有）；保留位传上来直接拒绝。 */
        if ((f->ebx & ~(uint32_t)IPC_FLAG_PRIVATE) != 0U) { ret = -1; break; }
        ret = ipc_port_create(f->ebx, proc_current());
        break;
    }
    case SYS_PORT_SEND:
        ret = svc_ipc_send(f->ebx, f->ecx, f->edx);
        break;
    case SYS_PORT_RECV:
        ret = svc_ipc_recv(f->ebx, f->ecx, f->edx);
        break;
    case SYS_PORT_REPLY:
        ret = svc_ipc_reply(f->ebx, f->ecx, f->edx);
        break;
    case SYS_CAP_GRANT: {
        /* abi.h 注释只列 ebx/ecx/edx；rights 走 esi（0 = 默认 RW），
         * 见 docs/claims/ipc.md“设计问题 #3”。 */
        ret = ipc_cap_grant(f->ebx, f->ecx, f->edx, f->esi, proc_current());
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
