/*
 * ng WP2-1: VFS syscall 胶水（ABI 1.0 草案，调用号 20..27）。
 *
 * 这一层只做三件事：
 *   1. 用户指针 -> 内核缓冲（路径 copyin_str、读缓冲 copyin、写/结构体
 *      copyout），全部经 uaccess.c，坏指针返回 EFAULT 而不是 #PF；
 *   2. 把 syscall 参数（rdi/rsi/rdx/r10）转成 vfs_*() 的参数；
 *   3. 维护一份 per-AS 的 vfs_ctx（本 WP 只有单 AS，内核里是静态一份）。
 *
 * 调用号（syscall.h）：
 *   20 open(path, flags, mode)  -> fd / -err
 *   21 read(fd, buf, len)       -> 字节数 / -err
 *   22 write(fd, buf, len)      -> 字节数 / -err
 *   23 close(fd)                -> 0 / -err
 *   24 stat(path, struct vfs_stat*) -> 0 / -err
 *   25 listdir(path, struct vfs_dirent*, max) -> 条数 / -err
 *   26 mkdir(path, mode)        -> 0 / -err
 *   27 unlink(path)             -> 0 / -err
 *
 * 缓冲上限 FS_SYS_IO_MAX（512）与 FS_SYS_LISTDIR_MAX（8）：内核侧用静态/
 * 栈上的 bounce buffer，不做“用户页直接读写”，也不按 len 分配内核内存。
 * 超限返回 EINVAL，由调用方降成多次调用（本 WP 的 demo/自检都很小）。
 */
#include "fs_syscall.h"

#include "uaccess.h"
#include "vfs.h"

static struct ramfs g_ram;
static struct vfs_ctx g_ctx;
static int g_ready;

struct vfs_ctx *fs_sys_ctx(void)
{
    return &g_ctx;
}

/* 用户路径 -> 内核缓冲；返回 VFS_OK / EFAULT / ENAMETOOLONG。 */
static int read_user_path(u64 upath, char *buf, u32 cap)
{
    int n;

    if (upath == 0 || buf == NULL || cap < 2) {
        return VFS_EFAULT;
    }
    n = copyin_str(buf, upath, (u64)cap);
    if (n < 0) {
        return VFS_EFAULT;
    }
    if ((u32)n >= cap - 1u) {
        return VFS_ENAMETOOLONG;
    }
    return VFS_OK;
}

int fs_sys_init(const struct fs_env *env)
{
    struct vfs_node *root;
    int rc;

    g_ready = 0;
    if (env == NULL || env->page_alloc == NULL || env->page_free == NULL) {
        return VFS_EINVAL;
    }
    vfs_reset();
    ramfs_init(&g_ram, env);
    root = ramfs_root(&g_ram);
    rc = vfs_mount("/", ramfs_ops(), &g_ram, root);
    if (rc != VFS_OK) {
        return rc;
    }
    vfs_ctx_init(&g_ctx);
    rc = vfs_mkdir(&g_ctx, "/tmp", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    g_ready = 1;
    return VFS_OK;
}

int fs_sys_open(u64 upath, u32 flags, u32 mode)
{
    char path[VFS_PATH_MAX];
    int rc;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    rc = read_user_path(upath, path, (u32)sizeof(path));
    if (rc != VFS_OK) {
        return rc;
    }
    return vfs_open(&g_ctx, path, flags, mode);
}

int fs_sys_read(u32 fd, u64 ubuf, u64 len)
{
    char buf[FS_SYS_IO_MAX];
    int n;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    if (len == 0 || len > (u64)FS_SYS_IO_MAX) {
        return VFS_EINVAL;
    }
    if (uaccess_range_ok(ubuf, len, 1) == 0) {
        return VFS_EFAULT;
    }
    n = vfs_read(&g_ctx, (int)fd, buf, len);
    if (n < 0) {
        return n;
    }
    if (n > 0 && copyout(ubuf, buf, (u64)n) != UACCESS_OK) {
        return VFS_EFAULT;
    }
    return n;
}

int fs_sys_write(u32 fd, u64 ubuf, u64 len)
{
    char buf[FS_SYS_IO_MAX];

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    if (len == 0 || len > (u64)FS_SYS_IO_MAX) {
        return VFS_EINVAL;
    }
    if (copyin(buf, ubuf, len) != UACCESS_OK) {
        return VFS_EFAULT;
    }
    return vfs_write(&g_ctx, (int)fd, buf, len);
}

int fs_sys_close(u32 fd)
{
    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    return vfs_close(&g_ctx, (int)fd);
}

int fs_sys_stat(u64 upath, u64 ustat)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat st;
    int rc;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    if (ustat == 0) {
        return VFS_EFAULT;
    }
    rc = read_user_path(upath, path, (u32)sizeof(path));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = vfs_stat(&g_ctx, path, &st);
    if (rc != VFS_OK) {
        return rc;
    }
    if (copyout(ustat, &st, (u64)sizeof(st)) != UACCESS_OK) {
        return VFS_EFAULT;
    }
    return VFS_OK;
}

int fs_sys_listdir(u64 upath, u64 uents, u32 max)
{
    char path[VFS_PATH_MAX];
    struct vfs_dirent ents[FS_SYS_LISTDIR_MAX];
    int n;
    int rc;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    if (uents == 0 || max == 0) {
        return VFS_EFAULT;
    }
    if (max > (u32)FS_SYS_LISTDIR_MAX) {
        max = (u32)FS_SYS_LISTDIR_MAX;
    }
    if (uaccess_range_ok(uents, (u64)max * (u64)sizeof(struct vfs_dirent), 1) ==
        0) {
        return VFS_EFAULT;
    }
    rc = read_user_path(upath, path, (u32)sizeof(path));
    if (rc != VFS_OK) {
        return rc;
    }
    n = vfs_listdir(&g_ctx, path, ents, max);
    if (n < 0) {
        return n;
    }
    if (n > 0 &&
        copyout(uents, ents, (u64)n * (u64)sizeof(struct vfs_dirent)) !=
            UACCESS_OK) {
        return VFS_EFAULT;
    }
    return n;
}

int fs_sys_mkdir(u64 upath, u32 mode)
{
    char path[VFS_PATH_MAX];
    int rc;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    rc = read_user_path(upath, path, (u32)sizeof(path));
    if (rc != VFS_OK) {
        return rc;
    }
    return vfs_mkdir(&g_ctx, path, mode);
}

int fs_sys_unlink(u64 upath)
{
    char path[VFS_PATH_MAX];
    int rc;

    if (g_ready == 0) {
        return VFS_EINVAL;
    }
    rc = read_user_path(upath, path, (u32)sizeof(path));
    if (rc != VFS_OK) {
        return rc;
    }
    return vfs_unlink(&g_ctx, path);
}

/* ------------------------------------------------------------------------- */
/* ng WP3-0：内核侧文件接口（spawn 读 ELF / 启动时种 /bin）                  */
/* ------------------------------------------------------------------------- */
/*
 * 这两个接口只给内核用（参数是内核指针 + 绝对路径），不经过 uaccess：
 *   - seed：把嵌入在内核 .rodata 的用户 ELF 种子进 ramfs；
 *   - read：spawn 从 VFS 读出 ELF 镜像（ramfs 和 FAT16 都经过同一条 VFS 路径）。
 * 单次 read/write 直接交给 vfs_*()，长度上限由调用方（PROC_ELF_MAX）控制。
 */
int fs_sys_seed_file(const char *path, const void *data, u64 size)
{
    int fd;
    int n;

    if (g_ready == 0 || path == NULL || data == NULL || size == 0) {
        return VFS_EINVAL;
    }
    fd = vfs_open(&g_ctx, path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (fd < 0) {
        return fd;
    }
    n = vfs_write(&g_ctx, fd, data, size);
    (void)vfs_close(&g_ctx, fd);
    return (n == (int)size) ? VFS_OK : ((n < 0) ? n : VFS_EIO);
}

int fs_sys_read_file(const char *path, void *buf, u64 cap, u64 *out_size)
{
    int fd;
    int n;
    u64 total = 0;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (g_ready == 0 || path == NULL || buf == NULL || cap == 0) {
        return VFS_EINVAL;
    }
    fd = vfs_open(&g_ctx, path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    while (total < cap) {
        u64 want = cap - total;

        if (want > FS_SYS_IO_MAX) {
            want = FS_SYS_IO_MAX;
        }
        n = vfs_read(&g_ctx, fd, (u8 *)buf + total, want);
        if (n < 0) {
            (void)vfs_close(&g_ctx, fd);
            return n;
        }
        if (n == 0) {
            break;
        }
        total += (u64)n;
    }
    (void)vfs_close(&g_ctx, fd);
    if (out_size != NULL) {
        *out_size = total;
    }
    return VFS_OK;
}
