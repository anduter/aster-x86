/* =============================================================================
 * svc/svc_syscall.c - 服务/能力相关系统调用（M6）
 *
 * 从 proc/trap/syscall.c 拆出的 svc 组：pipe / dup2 / cap_drop / cap_send /
 * input_set。只搬代码不改逻辑。
 * ============================================================================= */
#include "../types.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "svc_syscall.h"
#include "../proc/proc.h"
#include "../proc/trap/interrupt.h"
#include "../proc/trap/syscall.h"
#include "../proc/trap/syscall_internal.h"
#include "../proc/trap/uaccess.h"

bool svc_syscall_owns(uint32_t nr)
{
    return nr == SYS_PIPE || nr == SYS_DUP2 ||
           nr == SYS_CAP_DROP || nr == SYS_CAP_SEND ||
           nr == SYS_INPUT_SET;
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
            if (rfd >= 0) memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
            if (wfd >= 0) memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
            ret = -1;
            break;
        }

        fds[0] = rfd;
        fds[1] = wfd;
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, fds, sizeof(fds)) == E_OK) ? 0 : -1;
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
    default:
        return -1000;
    }
    return ret;
}
