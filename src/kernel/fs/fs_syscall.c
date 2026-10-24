/* =============================================================================
 * fs/fs_syscall.c - 文件系统相关系统调用（M4）
 *
 * 从 proc/trap/syscall.c 拆出的 fs 组：open/fread/fwrite/close/list/drives/
 * mkdir/unlink/stat/chdir/chroot/getcwd/sync/time/fsck/chmod/chown。
 * 只搬代码不改逻辑；共享辅助（copy_user_string / make_abs_path / fd_*）仍由
 * proc/trap/syscall_util.c 提供。
 * ============================================================================= */
#include "../types.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/proc.h"
#include "fs_syscall.h"
#include "../proc/trap/syscall_internal.h"

/* ---- 文件类系统调用的用户参数处理 ---- */

/* 把用户给的路径转成绝对路径：
 *   以 '/' 开头或含 ':'  -> 原样（绝对路径/指定驱动号）
 *   其它                -> 拼上当前进程的 cwd
 * （实现在 syscall_util.c 的 make_abs_path） */

static int sys_open_user(uint32_t path_ptr, uint32_t flags)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();
    uint8_t be = FS_BE_RAMFS;
    int h;
    int fd;

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    /* "fat:..." 含 ':'，make_abs_path 会原样保留；
     * 具体路由（ramfs / FAT16）交给 fs 层，syscall 层不再判断文件系统类型。 */
    make_abs_path(path, sizeof(path), rel);

    h = fs_open_ex(path, flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT),
                   (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, &be);
    if (h < 0) return -1;

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
            if (c < 0) break;
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
            if (pipe_write((int)f->handle, buf, n) != (int)n) return -1;
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

    if (fs_listdir_ex(path, index, name, sizeof(name), &st, 0) != 0) return -1;

    n = (uint32_t)strlen(name) + 1U;
    if (copy_to_user((void *)(uintptr_t)ubuf, name, n) != E_OK) return -1;
    return 0;
}

/* 把用户路径转成绝对路径并执行一个 fs 操作 */
static int sys_mkdir_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    return (fs_mkdir_ex(path, (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, 0) == 0) ? 0 : -1;
}

static int sys_unlink_user(uint32_t path_ptr)
{
    char rel[256];
    char path[256];
    proc_t *p = proc_current();

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    return (fs_unlink_ex(path, (p != 0) ? p->uid : 0U, (p != 0) ? p->gid : 0U, 0) == 0) ? 0 : -1;
}

/* 同卷 rename/move：ebx=旧路径, ecx=新路径。
 * 两个路径都按当前进程的 cwd 规范化；具体路由/判权在 fs_rename_as 里。 */
static int sys_rename_user(uint32_t old_ptr, uint32_t new_ptr)
{
    char oldrel[256];
    char newrel[256];
    char oldpath[256];
    char newpath[256];
    proc_t *p = proc_current();

    if (copy_user_string(oldrel, sizeof(oldrel), old_ptr) != E_OK) return -1;
    if (copy_user_string(newrel, sizeof(newrel), new_ptr) != E_OK) return -1;

    make_abs_path(oldpath, sizeof(oldpath), oldrel);
    make_abs_path(newpath, sizeof(newpath), newrel);

    return (fs_rename_as(oldpath, newpath,
                         (p != 0) ? p->uid : 0U,
                         (p != 0) ? p->gid : 0U) == E_OK) ? 0 : -1;
}

static int sys_stat_user(uint32_t path_ptr, uint32_t ubuf)
{
    char rel[256];
    char path[256];
    fs_stat_t st;
    uint32_t out[6];

    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;

    make_abs_path(path, sizeof(path), rel);
    if (fs_stat_ex(path, &st, 0) != 0) return -1;

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
    proc_t *p = proc_current();

    if (p == 0) return -1;
    if (copy_user_string(rel, sizeof(rel), path_ptr) != E_OK) return -1;
    make_abs_path(path, sizeof(path), rel);

    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_DIR) return -1;

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

        if (p == 0 || copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) {
            ret = -1;
            break;
        }
        make_abs_path(path, sizeof(path), rel);
        if (path[0] == '\0' || fs_stat(path, &st) != 0 || st.type != FS_TYPE_DIR) {
            ret = -1;
            break;
        }
        if (proc_root_set(p, path) != 0) { ret = -1; break; }
        strcpy(p->cwd, path);
        ret = 0;
        break;
    }
    case SYS_GETCWD:
        ret = sys_getcwd_user(f->ebx, f->ecx);
        break;
    case SYS_SYNC:
        ret = fs_sync_backing();
        break;
    case SYS_TIME: {
        /* ebx=用户缓冲, ecx=长度：写入格式化的 RTC 时间 */
        char tbuf[24];
        int n = rtc_format(rtc_now(), tbuf, sizeof(tbuf));

        if (n <= 0) { ret = -1; break; }
        ret = (copy_to_user((void *)(uintptr_t)f->ebx, tbuf, (uint32_t)n + 1U) == E_OK) ? 0 : -1;
        break;
    }
    case SYS_FSCK: {
        /* ebx=用户缓冲, ecx=长度：做一致性检查并返回错误数 */
        char report[96];
        int errs = fs_check(report, sizeof(report));

        if (f->ebx != 0U) {
            (void)copy_to_user((void *)(uintptr_t)f->ebx, report,
                               (uint32_t)strlen(report) + 1U);
        }
        ret = errs;
        break;
    }
    case SYS_CHMOD: {
        /* ebx=路径, ecx=新 mode（低 9 位） */
        char rel[256];
        char path[256];

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        ret = (fs_chmod(path, f->ecx, (proc_current() != 0) ? proc_current()->uid : 0) == E_OK)
              ? 0 : -1;
        break;
    }
    case SYS_CHOWN: {
        /* ebx=路径, ecx=uid, edx=gid（只有 root 能调用） */
        char rel[256];
        char path[256];
        proc_t *p = proc_current();

        if (copy_user_string(rel, sizeof(rel), f->ebx) != E_OK) { ret = -1; break; }
        make_abs_path(path, sizeof(path), rel);
        ret = (fs_chown_as(path, f->ecx, f->edx,
                           (p != 0) ? p->uid : 0U) == E_OK) ? 0 : -1;
        break;
    }
    default:
        return -1000;
    }
    return ret;
}
