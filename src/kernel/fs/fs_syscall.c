/* =============================================================================
 * fs/fs_syscall.c - 文件系统相关系统调用（M4）
 *
 * 从 proc/trap/syscall.c 拆出的 fs 组：open/fread/fwrite/close/list/drives/
 * mkdir/unlink/stat/chdir/chroot/getcwd/sync/time/fsck/chmod/chown。
 * 只搬代码不改逻辑；共享辅助（copy_user_string / make_abs_path / fd_*）仍由
 * proc/trap/syscall_util.c 提供。
 *
 * ABI v1.3（P1 身份）：所有判权都用"生效身份"——proc_t 的 euid/egid + 附加组
 * （proc_fill_cred），而不是 ruid/rgid。root 的 CAP_DAC_OVERRIDE 由 fs 层按
 * euid == 0 处理；拒绝访问会写一条审计（audit: DENY ...）。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/proc.h"
#include "include/security.h"
#include "fs_syscall.h"
#include "../proc/trap/syscall_internal.h"

/* 当前进程的生效凭据；p == 0（内核线程）按 root */
static void cur_cred(fs_cred_t *c)
{
    proc_t *p = proc_current();

    proc_fill_cred(p, c);
    c->umask = (p != 0) ? p->umask : 0U;
}

/* 拒绝访问的审计：只在确实"有权限位但不够"时打印，避免路径不存在也报 DENY */
static void audit_deny(const char *what, const char *path)
{
    char detail[120];
    acct_user_t me;
    const char *name = "?";

    if (proc_current() != 0 && acct_find_uid(proc_current()->euid, &me) == 0) {
        name = me.name;
    }
    ksnprintf(detail, sizeof(detail), "%s %s by %s", what, path, name);
    acct_audit("DENY", detail);
}

/* ---- 文件类系统调用的用户参数处理 ---- */

/* 把用户给的路径转成绝对路径：
 *   以 '/' 开头或含 ':'  -> 原样（绝对路径/指定驱动号）
 *   其它                -> 拼上当前进程的 cwd
 * （实现在 syscall_util.c 的 make_abs_path） */

/* SYS_CHROOT 的路径 -> proc_t.cwd 复制。
 * cwd 只有 192 字节，而用户路径可达 255 字节：旧实现的 strcpy 会写过
 * cwd 覆盖 user_brk/fds/caps。超长时一个字节都不写，返回 E_INVAL。
 * 做成非 static 以便 fs 审计自检直接用 canary 缓冲验证。 */
static int fs_chroot_cwd_copy(char *dst, uint32_t dst_size, const char *path)
{
    uint32_t n;

    if (dst == 0 || path == 0 || dst_size == 0U) return E_INVAL;
    n = (uint32_t)strlen(path);
    if (n + 1U > dst_size) return E_INVAL;
    memcpy(dst, path, n + 1U);
    return E_OK;
}

/* SYS_CHROOT 实际写 cwd 的入口（审计自检直接调用它）。 */
int fs_chroot_set_cwd(proc_t *p, const char *path)
{
    if (p == 0) return E_INVAL;
    return fs_chroot_cwd_copy(p->cwd, sizeof(p->cwd), path);
}

static int sys_open_user(uint32_t path_ptr, uint32_t flags)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();
    fs_cred_t cred;
    uint8_t be = FS_BE_RAMFS;
    int h;
    int fd;

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    /* "fat:..." 含 ':'，make_abs_path 会原样保留；
     * 具体路由（ramfs / FAT16）交给 fs 层，syscall 层不再判断文件系统类型。 */
    make_abs_path(path, sizeof(path), rel);

    cur_cred(&cred);
    h = fs_open_cred(path, flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT),
                     &cred, &be);
    if (h < 0) {
        /* 打开失败：区分"没权限"和"不存在"，前者进审计 */
        fs_stat_t probe;

        if (fs_stat_cred(path, &probe, &cred, 0) == E_OK) {
            audit_deny("open", path);
        }
        return -1;
    }

    fd = fd_alloc(p, (uint32_t)h, open_rights(flags));
    if (fd < 0) {
        fs_close_ex(be, h);
        return -1;
    }
    p->fds[fd].fs_be = be;
    return fd;
}

static int sys_fread_user(int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);
    char buf[128];
    int n;

    if (f == 0) return -1;
    if ((f->rights & FD_RIGHT_READ) == 0U) return -1;
    if (len > (uint32_t)sizeof(buf)) len = (uint32_t)sizeof(buf);

    if (f->kind == FD_KIND_CONSOLE) {
        /* stdin：按字节阻塞读，直到读到数据或缓冲区满 */
        uint32_t got = 0;
        while (got < len) {
            int c = sys_getchar_block();
            if (c < 0) {
                /* 一个字节都没读到：把 -EINTR 原样返回；已有数据先交付 */
                if (got == 0U && c == -EINTR) return c;
                break;
            }
            buf[got++] = (char)c;
            if (ubuf == 0U) break;
        }
        if (got == 0U) return 0;
        if (copy_to_user((void *)(uintptr_t)ubuf, buf, got) != E_OK) return -1;
        return (int)got;
    }

    if (f->kind == FD_KIND_PIPE_R) {
        n = pipe_read((int)f->handle, buf, len);
        if (n < 0) return n;
        if (copy_to_user((void *)(uintptr_t)ubuf, buf, (uint32_t)n) != E_OK) return -1;
        return n;
    }

    n = fs_read_ex(f->fs_be, (int)f->handle, buf, len);
    if (n < 0) return n;
    if (copy_to_user((void *)(uintptr_t)ubuf, buf, (uint32_t)n) != E_OK) return -1;
    return n;
}

static int sys_fwrite_user(int fd, uint32_t ubuf, uint32_t len)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);
    char buf[128];
    uint32_t done = 0;

    if (f == 0) return -1;
    if ((f->rights & FD_RIGHT_WRITE) == 0U) return -1;

    /* 分块搬运：用户一次可能写几百字节（例如整段帮助文本） */
    while (done < len) {
        uint32_t n = len - done;

        if (n > (uint32_t)sizeof(buf)) n = (uint32_t)sizeof(buf);

        if (copy_from_user(buf, (const void *)(uintptr_t)(ubuf + done), n) != E_OK) {
            return -1;
        }

        if (f->kind == FD_KIND_CONSOLE) {
            /* 整段持 console 锁写：逐字符 console_putc 会让另一颗 CPU 的
             * 日志行插进用户输出中间（fork/sectest 的假失败来源）。 */
            console_write_n_atomic(buf, n);
        } else if (f->kind == FD_KIND_PIPE_W) {
            int w = pipe_write((int)f->handle, buf, n);
            if (w < 0) return w;                 /* -EINTR：不吞信号 */
            if (w != (int)n) return -1;
        } else {
            if (fs_write_ex(f->fs_be, (int)f->handle, buf, n) != (int)n) return -1;
        }
        done += n;
    }

    return (int)done;
}

static int sys_close_user(int fd)
{
    proc_t *p = proc_current();
    proc_fd_t *f = fd_get(p, fd);

    if (f == 0 || fd < 3) return -1;

    if (f->kind == FD_KIND_FILE) {
        fs_close_ex(f->fs_be, (int)f->handle);
    } else if (f->kind == FD_KIND_PIPE_W) {
        pipe_unref_write((int)f->handle);
    } else if (f->kind == FD_KIND_PIPE_R) {
        pipe_unref_read((int)f->handle);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

static int sys_list_user(uint32_t path_ptr, uint32_t index, uint32_t ubuf)
{
    char rel[256];
    char path[256];
    char name[FS_NAME_MAX];
    fs_stat_t st;
    fs_cred_t cred;
    uint32_t n;

    if (path_ptr == 0U) {
        /* 没有路径参数：模块层，列出驱动器 */
        path[0] = '\0';
    } else {
        if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) {
            return -1;
        }
        make_abs_path(path, sizeof(path), rel);
    }

    cur_cred(&cred);
    if (fs_listdir_cred(path, index, name, sizeof(name), &st, &cred, 0) != E_OK) {
        return -1;
    }

    n = (uint32_t)strlen(name) + 1U;
    if (copy_to_user((void *)(uintptr_t)ubuf, name, n) != E_OK) return -1;
    return 0;
}

/* 把用户路径转成绝对路径并执行一个 fs 操作 */
static int sys_mkdir_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    fs_cred_t cred;

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    cur_cred(&cred);
    if (fs_mkdir_cred(path, &cred, 0) != E_OK) {
        audit_deny("mkdir", path);
        return -1;
    }
    return 0;
}

static int sys_unlink_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    fs_cred_t cred;

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    cur_cred(&cred);
    if (fs_unlink_cred(path, &cred, 0) != E_OK) {
        audit_deny("unlink", path);
        return -1;
    }
    return 0;
}

/* 同卷 rename/move：ebx=旧路径, ecx=新路径。
 * 两个路径都按当前进程的 cwd 规范化；具体路由/判权在 fs_rename_as 里。 */
static int sys_rename_user(uint32_t old_ptr, uint32_t new_ptr)
{
    char oldrel[256];
    char newrel[256];
    char oldpath[256];
    char newpath[256];
    fs_cred_t cred;

    if (copy_user_string(oldrel, sizeof(oldrel), old_ptr) != E_OK) return -1;
    if (copy_user_string(newrel, sizeof(newrel), new_ptr) != E_OK) return -1;

    make_abs_path(oldpath, sizeof(oldpath), oldrel);
    make_abs_path(newpath, sizeof(newpath), newrel);

    cur_cred(&cred);
    if (fs_rename_cred(oldpath, newpath, &cred) != E_OK) {
        audit_deny("rename", oldpath);
        return -1;
    }
    return 0;
}

static int sys_stat_user(uint32_t path_ptr, uint32_t ubuf)
{
    char rel[256];
    char path[256];
    fs_stat_t st;
    fs_cred_t cred;
    uint32_t out[6];

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    cur_cred(&cred);
    if (fs_stat_cred(path, &st, &cred, 0) != E_OK) return -1;

    out[0] = (uint32_t)st.type;
    out[1] = st.size;
    out[2] = st.mtime;
    out[3] = st.mode;
    out[4] = st.uid;
    out[5] = st.gid;
    return (copy_to_user((void *)(uintptr_t)ubuf, out, sizeof(out)) == E_OK) ? 0 : -1;
}

/* 规范化绝对路径：去掉结尾的 '/'、'/.'、'/..'（供 cd 存 cwd 用） */
static void normalize_abs(char *p)
{
    for (;;) {
        uint32_t n = (uint32_t)strlen(p);

        if (n > 0U && p[n - 1U] == '/') {
            p[n - 1U] = '\0';
            continue;
        }
        if (n >= 2U && p[n - 1U] == '.' && p[n - 2U] == '/') {
            p[n - 2U] = '\0';
            continue;
        }
        if (n >= 3U && p[n - 1U] == '.' && p[n - 2U] == '.' && p[n - 3U] == '/') {
            p[n - 3U] = '\0';
            n = (uint32_t)strlen(p);
            while (n > 0U && p[n - 1U] != '/') {
                p[--n] = '\0';
            }
            continue;
        }
        break;
    }
}

static int sys_chdir_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    fs_stat_t st;
    fs_cred_t cred;
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;
    make_abs_path(path, sizeof(path), rel);

    cur_cred(&cred);
    if (fs_stat_cred(path, &st, &cred, 0) != E_OK || st.type != FS_TYPE_DIR) return -1;

    normalize_abs(path);
    if (path[0] == '\0') return -1;

    strncpy(p->cwd, path, sizeof(p->cwd) - 1U);
    p->cwd[sizeof(p->cwd) - 1U] = '\0';
    return 0;
}

static int sys_getcwd_user(uint32_t ubuf, uint32_t max)
{
    proc_t *p = proc_current();
    uint32_t n;

    if (p == 0 || max == 0U) return -1;

    n = (uint32_t)strlen(p->cwd) + 1U;
    if (n > max) n = max;

    if (copy_to_user((void *)(uintptr_t)ubuf, p->cwd, n) != E_OK) return -1;
    return (int)(n - 1U);
}

/* 取第 index 个盘符名；FAT16 交换盘排在最后 */
static int sys_drives_user(uint32_t index, uint32_t ubuf, uint32_t max)
{
    char name[FS_DRIVE_MAX];
    uint32_t n;
    uint32_t fs_count = fs_volume_count();

    if (index < fs_count) {
        if (fs_volume_name(index, name, sizeof(name)) != E_OK) return -1;
    } else if (index == fs_count && fat16_mounted()) {
        strncpy(name, fat16_drive(), sizeof(name) - 1U);
        name[sizeof(name) - 1U] = '\0';
    } else {
        return -1;
    }

    n = (uint32_t)strlen(name) + 1U;
    if (n > max) n = max;
    if (copy_to_user((void *)(uintptr_t)ubuf, name, n) != E_OK) return -1;
    return 0;
}

/* 硬链接：old 必须是普通文件、同卷；权限/存在性由 fs_link_cred 判。 */
static int sys_link_user(uint32_t old_ptr, uint32_t new_ptr)
{
    char oldrel[FS_PATH_MAX];
    char newrel[FS_PATH_MAX];
    char oldpath[FS_PATH_MAX];
    char newpath[FS_PATH_MAX];
    fs_cred_t cred;

    if (copy_user_string(oldrel, sizeof(oldrel), old_ptr) != E_OK) return -1;
    if (copy_user_string(newrel, sizeof(newrel), new_ptr) != E_OK) return -1;
    make_abs_path(oldpath, sizeof(oldpath), oldrel);
    make_abs_path(newpath, sizeof(newpath), newrel);
    cur_cred(&cred);
    return (fs_link_cred(oldpath, newpath, &cred) == E_OK) ? 0 : -1;
}

/* 符号链接：目标文本原样保存（不做 cwd 规范化），链接路径按 cwd 规范化。 */
static int sys_symlink_user(uint32_t target_ptr, uint32_t link_ptr)
{
    char target[FS_PATH_MAX];
    char linkrel[FS_PATH_MAX];
    char linkpath[FS_PATH_MAX];
    fs_cred_t cred;

    if (copy_user_string(target, sizeof(target), target_ptr) != E_OK) return -1;
    if (copy_user_string(linkrel, sizeof(linkrel), link_ptr) != E_OK) return -1;
    make_abs_path(linkpath, sizeof(linkpath), linkrel);
    cur_cred(&cred);
    return (fs_symlink_cred(target, linkpath, &cred) == E_OK) ? 0 : -1;
}

static int sys_readlink_user(uint32_t path_ptr, uint32_t ubuf, uint32_t max)
{
    char rel[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    char target[FS_PATH_MAX];
    fs_cred_t cred;
    int n;

    if (max == 0U || max >= FS_PATH_MAX) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;
    make_abs_path(path, sizeof(path), rel);
    cur_cred(&cred);
    n = fs_readlink_cred(path, target, sizeof(target), &cred);
    if (n < 0) return -1;
    if ((uint32_t)n >= max) n = (int)max - 1;
    if (copy_to_user((void *)(uintptr_t)ubuf, target, (uint32_t)n + 1U) != E_OK) return -1;
    return n;
}

/* lstat：不跟随末段（符号链接本身），输出布局和 SYS_STAT 一样 */
static int sys_lstat_user(uint32_t path_ptr, uint32_t ubuf)
{
    char rel[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    fs_stat_t st;
    fs_cred_t cred;
    uint32_t out[6];

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;
    make_abs_path(path, sizeof(path), rel);
    cur_cred(&cred);
    if (fs_lstat_cred(path, &st, &cred) != E_OK) return -1;

    out[0] = (uint32_t)st.type;
    out[1] = st.size;
    out[2] = st.mtime;
    out[3] = st.mode;
    out[4] = st.uid;
    out[5] = st.gid;
    return (copy_to_user((void *)(uintptr_t)ubuf, out, sizeof(out)) == E_OK) ? 0 : -1;
}
bool fs_syscall_owns(uint32_t nr)
{
    switch (nr) {
    case SYS_OPEN:
    case SYS_FREAD:
    case SYS_FWRITE:
    case SYS_CLOSE:
    case SYS_LIST:
    case SYS_DRIVES:
    case SYS_MKDIR:
    case SYS_UNLINK:
    case SYS_RENAME:
    case SYS_STAT:
    case SYS_CHDIR:
    case SYS_CHROOT:
    case SYS_GETCWD:
    case SYS_SYNC:
    case SYS_TIME:
    case SYS_FSCK:
    case SYS_CHMOD:
    case SYS_CHOWN:
    case SYS_LINK:
    case SYS_SYMLINK:
    case SYS_READLINK:
    case SYS_LSTAT:
        return true;
    default:
        return false;
    }
}

int fs_syscall(interrupt_frame_t *f)
{
    int ret = 0;

    switch (f->eax) {
    case SYS_OPEN:
        ret = sys_open_user(f->ebx, f->ecx);
        break;
    case SYS_FREAD:
        ret = sys_fread_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_FWRITE:
        ret = sys_fwrite_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_CLOSE:
        ret = sys_close_user(f->ebx);
        break;
    case SYS_LIST:
        ret = sys_list_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_DRIVES:
        ret = sys_drives_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_MKDIR:
        ret = sys_mkdir_user(f->ebx);
        break;
    case SYS_UNLINK:
        ret = sys_unlink_user(f->ebx);
        break;
    case SYS_RENAME:
        ret = sys_rename_user(f->ebx, f->ecx);
        break;
    case SYS_LINK:
        ret = sys_link_user(f->ebx, f->ecx);
        break;
    case SYS_SYMLINK:
        ret = sys_symlink_user(f->ebx, f->ecx);
        break;
    case SYS_READLINK:
        ret = sys_readlink_user(f->ebx, f->ecx, f->edx);
        break;
    case SYS_LSTAT:
        ret = sys_lstat_user(f->ebx, f->ecx);
        break;
    case SYS_STAT:
        ret = sys_stat_user(f->ebx, f->ecx);
        break;
    case SYS_CHDIR:
        ret = sys_chdir_user(f->ebx);
        break;
    case SYS_CHROOT: {
        proc_t *p = proc_current();
        char rel[FS_PATH_MAX];
        char path[FS_PATH_MAX];
        fs_stat_t st;
        fs_cred_t cred;

        if (p == 0 || copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) {
            ret = -1;
            break;
        }
        make_abs_path(path, sizeof(path), rel);
        cur_cred(&cred);
        if (path[0] == '\0' ||
            fs_stat_cred(path, &st, &cred, 0) != E_OK || st.type != FS_TYPE_DIR) {
            ret = -1;
            break;
        }
        /* cwd 只有 192B：先按长度拒绝，绝不先改 root 再截断 cwd
         * （两者不一致会被用来越过 chroot）。 */
        if (strlen(path) + 1U > sizeof(p->cwd)) { ret = -1; break; }
        if (proc_root_set(p, path) != 0) { ret = -1; break; }
        if (fs_chroot_set_cwd(p, path) != E_OK) { ret = -1; break; }
        ret = 0;
        break;
    }
    case SYS_GETCWD:
        ret = sys_getcwd_user(f->ebx, f->ecx);
        break;
    case SYS_SYNC:
        ret = fs_sync_backing();
        /* 同步时把审计环刷进 /var/log/audit.log（审计本身不碰文件系统） */
        (void)acct_audit_flush();
        break;
    case SYS_TIME: {
        /* ebx=用户缓冲, ecx=长度：写入格式化的 RTC 时间。
         * 旧实现完全不看 ecx，给 4B 缓冲也会写 20B（写穿用户缓冲）。 */
        char tbuf[24];
        uint32_t max = f->ecx;
        uint32_t ncopy;
        int n = rtc_format(rtc_now(), tbuf, sizeof(tbuf));

        if (n <= 0 || max == 0U) { ret = -1; break; }
        ncopy = (uint32_t)n + 1U;
        if (ncopy > max) {
            ncopy = max;                /* 截断，但最后一个字节保持 NUL */
            tbuf[ncopy - 1U] = '\0';
        }
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, tbuf, ncopy) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_FSCK: {
        /* ebx=用户缓冲, ecx=长度：做一致性检查并返回错误数。
         * 坏用户指针必须返回 -1：旧实现忽略 copy_to_user 返回值，用户拿到
         * "错误数"但缓冲根本没写（且可能写进内核地址）。 */
        char report[96];
        int errs;

        if (f->ebx != 0U && !user_range_ok(f->ebx, sizeof(report))) {
            ret = -1;
            break;
        }
        errs = fs_check(report, sizeof(report));
        if (f->ebx != 0U &&
            copy_to_user((void *)(uintptr_t)f->ebx, report,
                         (uint32_t)strlen(report) + 1U) != E_OK) {
            ret = -1;
            break;
        }
        ret = errs;
        break;
    }
    case SYS_CHMOD: {
        /* ebx=路径, ecx=新 mode（低 12 位：含 setuid/setgid/sticky） */
        char rel[256];
        char path[256];
        fs_cred_t cred;

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        cur_cred(&cred);
        if (fs_chmod_cred(path, f->ecx, &cred) != E_OK) {
            audit_deny("chmod", path);
            ret = -1;
            break;
        }
        acct_audit("CHMOD", path);
        ret = 0;
        break;
    }
    case SYS_CHOWN: {
        /* ebx=路径, ecx=uid, edx=gid（只有 root 能调用） */
        char rel[256];
        char path[256];
        fs_cred_t cred;

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        cur_cred(&cred);
        if (fs_chown_cred(path, f->ecx, f->edx, &cred) != E_OK) {
            audit_deny("chown", path);
            ret = -1;
            break;
        }
        acct_audit("CHOWN", path);
        ret = 0;
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
