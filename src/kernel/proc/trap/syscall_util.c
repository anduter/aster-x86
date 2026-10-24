/* =============================================================================
 * proc/trap/syscall_util.c - syscall 层共享辅助（用户指针/路径/fd 分配）
 * ============================================================================= */
#include "../../types.h"
#include "interrupt.h"
#include "syscall_internal.h"
#include "uaccess.h"
#include "../../core/error.h"
#include "include/fs.h"
#include "../proc.h"
#include "../proc_internal.h"
#include "../../lib/string.h"

#define USER_ADDR_LIMIT 0xC0000000U
#define USER_LEN_LIMIT  (64U * 1024U)

proc_fd_t *fd_get(proc_t *p, int fd)
{
    if (p == 0 || fd < 0 || fd >= PROC_FD_MAX) return 0;
    if (!p->fds[fd].used) return 0;
    return &p->fds[fd];
}
bool user_range_ok(uint32_t ptr, uint32_t len)
{
    if (len > USER_LEN_LIMIT) return false;
    if (ptr >= USER_ADDR_LIMIT) return false;
    if (len > USER_ADDR_LIMIT - ptr) return false;
    return true;
}

int copy_user_string(char *dst, uint32_t dst_size, uint32_t uptr)
{
    uint32_t i;

    if (dst_size == 0U) return E_INVAL;

    for (i = 0; i + 1U < dst_size; i++) {
        char c;
        if (copy_from_user(&c, (const void *)(uintptr_t)(uptr + i), 1) != E_OK) {
            return E_INVAL;
        }
        dst[i] = c;
        if (c == '\0') return E_OK;
    }
    dst[dst_size - 1U] = '\0';
    return E_INVAL;                    /* 路径太长 */
}

int path_has_parent(const char *p)
{
    for (; p[0] != '\0' && p[1] != '\0'; p++) {
        if (p[0] == '.' && p[1] == '.') return 1;
    }
    return 0;
}

void make_abs_path(char *out, uint32_t out_max, const char *in)
{
    proc_t *p = proc_current();
    const char *cwd = (p != 0 && p->cwd[0] != '\0') ? p->cwd : "system:";
    const char *root = (p != 0) ? proc_root_of(p) : "";
    uint32_t n = 0;
    uint32_t i;

    if (root[0] != '\0' && path_has_parent(in)) {
        out[0] = '\0';
        return;
    }

    if (in[0] == '/') {
        if (root[0] == '\0') {
            strncpy(out, in, out_max - 1U);
            out[out_max - 1U] = '\0';
            return;
        }
        for (i = 0; root[i] != '\0' && n + 1U < out_max; i++) out[n++] = root[i];
        while (n > 0U && out[n - 1U] == '/') n--;
        if (n + 1U < out_max) out[n++] = '/';
        for (i = 1; in[i] != '\0' && n + 1U < out_max; i++) out[n++] = in[i];
        out[n] = '\0';
        return;
    }

    if (strchr(in, ':') != 0) {
        if (root[0] != '\0') {
            uint32_t rl = (uint32_t)strlen(root);
            if (rl > 0U && strncmp(in, root, rl) != 0) {
                out[0] = '\0';
                return;
            }
        }
        strncpy(out, in, out_max - 1U);
        out[out_max - 1U] = '\0';
        return;
    }

    if (in[0] == '\0') {
        strncpy(out, cwd, out_max - 1U);
        out[out_max - 1U] = '\0';
        return;
    }

    for (i = 0; cwd[i] != '\0' && n + 1U < out_max; i++) out[n++] = cwd[i];
    if (n > 0U && out[n - 1U] != '/' && n + 1U < out_max) out[n++] = '/';
    for (i = 0; in[i] != '\0' && n + 1U < out_max; i++) out[n++] = in[i];
    out[n] = '\0';
}

int fd_alloc_kind(proc_t *p, uint8_t kind, uint32_t handle, uint32_t rights)
{
    for (int i = 3; i < PROC_FD_MAX; i++) {
        if (!p->fds[i].used) {
            p->fds[i].used = true;
            p->fds[i].kind = kind;
            p->fds[i].handle = handle;
            p->fds[i].rights = rights;
            return i;
        }
    }
    return -1;
}

uint32_t open_rights(uint32_t flags)
{
    uint32_t rights = 0;

    if ((flags & FS_OPEN_READ) != 0U) rights |= FD_RIGHT_READ;
    if ((flags & FS_OPEN_WRITE) != 0U) rights |= FD_RIGHT_WRITE;
    return (rights != 0U) ? rights : FD_RIGHT_READ;
}

int fd_alloc(proc_t *p, uint32_t handle, uint32_t rights)
{
    return fd_alloc_kind(p, FD_KIND_FILE, handle, rights);
}

/* 阻塞读一个字符：没有键就睡在键盘通道上，由 IRQ1 唤醒。
 * fs 组的 stdin 读（fread）与 misc 组的 SYS_GETCHAR 共用。 */
int sys_getchar_block(void)
{
    for (;;) {
        unsigned int flags;
        int c;

        __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
        c = keyboard_getchar();
        if (c >= 0) {
            __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
            return c;
        }
        proc_block(PROC_WAIT_KBD);
        __asm__ volatile ("pushl %0; popfl" : : "r"(flags));
    }
}