/*
 * ng WP2-1: VFS/ramfs 内核自检。
 *
 * 证据行（run-ng 必须命中）：
 *   ng: vfs mount ok (root=ramfs mounts=2)     根挂载 + /mnt 子挂载/卸载
 *   ng: ramfs write/read ok (bytes=4196)       跨页写、读回、截断、删除
 *   ng: fd bad rejected (n=8)                  非法 fd/代际/目录误用/坏指针
 *   ng: vfs path edge ok (n=12)                "."/".."/"//"/长度/ENOTDIR/链接
 *   ng: fs syscall user ptr EFAULT ok          20..27 的路径指针走 uaccess
 *   ng: WP2-1 selftest PASS                    （main.c）
 *
 * 每个负向用例都断言**具体错误码**；只统计“拒绝”不够。
 * 数据页来自 PMM（恒等映射，物理地址可直接当指针）；fs_env 是 L1 与 L0 的
 * 唯一接口，src/services/fs 不包含任何内核内部头。
 */
#include "fs_selftest.h"

#include "fs_syscall.h"
#include "pmm.h"
#include "ramfs.h"
#include "serial.h"
#include "types.h"
#include "vfs.h"

static struct ramfs g_mnt;
static u8 g_buf[4096 * 3];
static u8 g_rbuf[4096 * 3];
static int g_fails;

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

static void *sel_page_alloc(void *ctx)
{
    u64 phys;

    (void)ctx;
    phys = pmm_alloc_page();
    return (phys != 0) ? (void *)(usize)phys : NULL; /* 恒等映射 */
}

static void sel_page_free(void *ctx, void *page)
{
    (void)ctx;
    if (page != NULL) {
        (void)pmm_free_page((u64)(usize)page);
    }
}

static void fill_pattern(u8 *buf, u32 n, u8 seed)
{
    u32 i;

    for (i = 0; i < n; i++) {
        buf[i] = (u8)(seed + (u8)(i * 7u));
    }
}

static int same_bytes(const u8 *a, const u8 *b, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* 1. 挂载                                                                   */
/* ------------------------------------------------------------------------- */

static int check_mount(struct vfs_ctx *c)
{
    struct fs_env env;
    struct vfs_node *root = NULL;
    struct vfs_stat st;
    int rc;

    env.page_alloc = sel_page_alloc;
    env.page_free = sel_page_free;
    env.ctx = NULL;
    ramfs_init(&g_mnt, &env);

    if (vfs_mount_count() != 1 || vfs_mount_path_at(0) == NULL ||
        vfs_mount_path_at(0)[0] != '/' || vfs_mount_path_at(0)[1] != '\0') {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_resolve(c, "/", &root) != VFS_OK || root == NULL ||
        root->type != VFS_NODE_DIR) {
        fail_item("vfs mount");
        return 1;
    }

    rc = vfs_mkdir(c, "/mnt", 0755);
    if (rc != VFS_OK) {
        fail_item("vfs mount");
        return 1;
    }
    rc = vfs_mount("/mnt", ramfs_ops(), &g_mnt, ramfs_root(&g_mnt));
    if (rc != VFS_OK || vfs_mount_count() != 2) {
        fail_item("vfs mount");
        return 1;
    }
    /* 挂进 /mnt 的文件必须只在第二份 ramfs 里可见。 */
    {
        int fd = vfs_open(c, "/mnt/x", VFS_O_CREAT | VFS_O_RDWR, 0644);

        if (fd <= 0 || vfs_write(c, fd, "m", 1) != 1 ||
            vfs_close(c, fd) != VFS_OK) {
            fail_item("vfs mount");
            return 1;
        }
    }
    if (vfs_stat(c, "/mnt/x", &st) != VFS_OK || st.size != 1) {
        fail_item("vfs mount");
        return 1;
    }
    /* 重复挂载/坏挂载点/卸载规则。 */
    if (vfs_mount("/mnt", ramfs_ops(), &g_mnt, ramfs_root(&g_mnt)) !=
        VFS_EEXIST) {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_mount("/nope", ramfs_ops(), &g_mnt, ramfs_root(&g_mnt)) !=
        VFS_ENOENT) {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_umount("/") != VFS_EBUSY) {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_umount("/mnt") != VFS_OK || vfs_mount_count() != 1) {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_stat(c, "/mnt/x", &st) != VFS_ENOENT) {
        fail_item("vfs mount");
        return 1;
    }
    if (vfs_umount("/mnt") != VFS_ENOENT) {
        fail_item("vfs mount");
        return 1;
    }
    serial_puts("ng: vfs mount ok (root=ramfs mounts=2)\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 2. ramfs 写/读/截断/删除                                                  */
/* ------------------------------------------------------------------------- */

static int check_ramfs(struct vfs_ctx *c)
{
    struct vfs_stat st;
    int fd;
    u32 total = 0;

    if (vfs_mkdir(c, "/selftest", 0755) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }
    fill_pattern(g_buf, (u32)sizeof(g_buf), 0x31);
    fd = vfs_open(c, "/selftest/f", VFS_O_CREAT | VFS_O_RDWR, 0644);
    if (fd <= 0) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_write(c, fd, g_buf, 4096) != 4096) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_write(c, fd, g_buf, 100) != 100) {
        fail_item("ramfs write/read");
        return 1;
    }
    total = 4196;
    if (vfs_stat(c, "/selftest/f", &st) != VFS_OK || st.size != total) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_close(c, fd) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }

    /* 读回：跨第 1/2 页边界。 */
    fd = vfs_open(c, "/selftest/f", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("ramfs write/read");
        return 1;
    }
    for (u32 i = 0; i < 4096u; i++) {
        g_rbuf[i] = 0;
    }
    if (vfs_read(c, fd, g_rbuf, 4096) != 4096) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (!same_bytes(g_rbuf, g_buf, 4096)) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_read(c, fd, g_rbuf, 100) != 100) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (!same_bytes(g_rbuf, g_buf, 100)) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_close(c, fd) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }

    /* 截断到 100 字节：读 200 只回 100；再扩到 8192 尾部补零。 */
    if (vfs_truncate(c, "/selftest/f", 100) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }
    fd = vfs_open(c, "/selftest/f", VFS_O_RDONLY, 0);
    if (fd <= 0 || vfs_read(c, fd, g_rbuf, 200) != 100 ||
        vfs_close(c, fd) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_truncate(c, "/selftest/f", 8192) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }
    fd = vfs_open(c, "/selftest/f", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("ramfs write/read");
        return 1;
    }
    if (vfs_read(c, fd, g_rbuf, 4096) != 4096 || vfs_read(c, fd, g_rbuf, 4096) != 4096) {
        fail_item("ramfs write/read");
        return 1;
    }
    /* 第 2 页整页应为 0（扩展部分）。 */
    {
        u32 i;
        int zero = 1;

        for (i = 0; i < 4096u; i++) {
            if (g_rbuf[i] != 0) {
                zero = 0;
                break;
            }
        }
        if (!zero) {
            fail_item("ramfs write/read");
            return 1;
        }
    }
    if (vfs_close(c, fd) != VFS_OK) {
        fail_item("ramfs write/read");
        return 1;
    }

    /* 单文件上限 1 MiB。 */
    if (vfs_truncate(c, "/selftest/f", RAMFS_MAX_FILE_BYTES + 1) !=
        VFS_ENOSPC) {
        fail_item("ramfs write/read");
        return 1;
    }
    /* 删除后不可见。 */
    if (vfs_unlink(c, "/selftest/f") != VFS_OK ||
        vfs_stat(c, "/selftest/f", &st) != VFS_ENOENT) {
        fail_item("ramfs write/read");
        return 1;
    }
    serial_puts("ng: ramfs write/read ok (bytes=");
    serial_putdec(total);
    serial_puts(" limit=1MiB)\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 3. fd / 类型错误 / 用户指针                                                */
/* ------------------------------------------------------------------------- */

static int check_fd_rejects(struct vfs_ctx *c)
{
    struct vfs_stat st;
    struct vfs_dirent ent;
    int fd;
    int old;
    int n = 0;

    fd = vfs_open(c, "/selftest/f2", VFS_O_CREAT | VFS_O_RDWR, 0644);
    if (fd <= 0) {
        fail_item("fd bad");
        return 1;
    }
    old = fd;
    if (vfs_close(c, fd) != VFS_OK) {
        fail_item("fd bad");
        return 1;
    }
    /* 1..4：旧 fd / 0 / -1 / 越界 generation 全部 EBADF。 */
    if (vfs_read(c, old, g_rbuf, 1) == VFS_EBADF) {
        n++;
    }
    if (vfs_read(c, 0, g_rbuf, 1) == VFS_EBADF) {
        n++;
    }
    if (vfs_write(c, -1, g_rbuf, 1) == VFS_EBADF) {
        n++;
    }
    if (vfs_close(c, 0x7FFF0000) == VFS_EBADF) {
        n++;
    }

    /* 5..6：目录不能写/RDWR 打开。 */
    if (vfs_open(c, "/selftest", VFS_O_RDWR, 0) == VFS_EISDIR) {
        n++;
    }
    fd = vfs_open(c, "/selftest", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    if (fd > 0) {
        if (vfs_write(c, fd, "x", 1) == VFS_EISDIR) {
            n++;
        }
        (void)vfs_close(c, fd);
    }

    /* 7..8：目录 unlink；文件当目录 listdir。 */
    if (vfs_unlink(c, "/selftest") == VFS_EISDIR) {
        n++;
    }
    if (vfs_listdir(c, "/selftest/f2", &ent, 1) == VFS_ENOTDIR) {
        n++;
    }

    /* 9..10：坏用户指针（当前 CR3 是内核表，用户页校验必然失败）。 */
    if (fs_sys_open(0x1000ULL, VFS_O_RDONLY, 0) == VFS_EFAULT) {
        n++;
    }
    if (fs_sys_stat(0x1000ULL, 0x2000ULL) == VFS_EFAULT) {
        n++;
    }
    /* 11：不存在的路径。 */
    if (vfs_open(c, "/selftest/nope", VFS_O_RDONLY, 0) == VFS_ENOENT) {
        n++;
    }

    (void)vfs_unlink(c, "/selftest/f2");
    (void)vfs_stat(c, "/selftest/f2", &st);
    if (n != 11) {
        serial_puts("ng: fd bad count=");
        serial_putdec((u64)n);
        serial_puts("\r\n");
        fail_item("fd bad");
        return 1;
    }
    serial_puts("ng: fd bad rejected (n=");
    serial_putdec((u64)n);
    serial_puts(")\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 4. 路径边界 / 链接                                                        */
/* ------------------------------------------------------------------------- */

static int check_path_edges(struct vfs_ctx *c)
{
    struct vfs_stat st;
    char longpath[VFS_PATH_MAX + 8];
    int fd;
    int n = 0;
    u32 i;

    if (vfs_mkdir(c, "/edge", 0755) != VFS_OK) {
        fail_item("vfs path edge");
        return 1;
    }
    fd = vfs_open(c, "/edge/a", VFS_O_CREAT | VFS_O_RDWR, 0644);
    if (fd <= 0 || vfs_write(c, fd, "A", 1) != 1) {
        fail_item("vfs path edge");
        return 1;
    }
    (void)vfs_close(c, fd);

    /* 1..4："." / "//" / ".." / 尾 '/'。 */
    fd = vfs_open(c, "/edge/./a", VFS_O_RDONLY, 0);
    n += (fd > 0) ? 1 : 0;
    (void)vfs_close(c, fd);
    fd = vfs_open(c, "//edge//a", VFS_O_RDONLY, 0);
    n += (fd > 0) ? 1 : 0;
    (void)vfs_close(c, fd);
    fd = vfs_open(c, "/edge/../edge/a", VFS_O_RDONLY, 0);
    n += (fd > 0) ? 1 : 0;
    (void)vfs_close(c, fd);
    fd = vfs_open(c, "/edge/a/", VFS_O_RDONLY, 0);
    n += (fd > 0) ? 1 : 0;
    (void)vfs_close(c, fd);

    /* 5..6：空路径 / 超长路径 / 单组件超长。 */
    if (vfs_open(c, "", VFS_O_RDONLY, 0) == VFS_EINVAL) {
        n++;
    }
    for (i = 0; i < sizeof(longpath) - 1; i++) {
        longpath[i] = 'p';
    }
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';
    if (vfs_open(c, longpath, VFS_O_RDONLY, 0) == VFS_ENAMETOOLONG) {
        n++;
    }

    /* 7：文件当目录。 */
    if (vfs_open(c, "/edge/a/x", VFS_O_RDONLY, 0) == VFS_ENOTDIR) {
        n++;
    }

    /* 8..9：相对路径 + cwd。 */
    if (vfs_chdir(c, "/edge") == VFS_OK) {
        fd = vfs_open(c, "a", VFS_O_RDONLY, 0);
        n += (fd > 0) ? 1 : 0;
        (void)vfs_close(c, fd);
        if (vfs_chdir(c, "..") == VFS_OK && c->cwd[0] == '/' &&
            c->cwd[1] == '\0') {
            n++;
        }
    }

    /* 10..12：相对/绝对符号链接 + 循环 ELOOP。 */
    if (vfs_symlink(c, "/edge/rel", "a") == VFS_OK &&
        vfs_stat(c, "/edge/rel", &st) == VFS_OK && st.size == 1) {
        n++;
    }
    if (vfs_symlink(c, "/edge/abs", "/edge/a") == VFS_OK &&
        vfs_stat(c, "/edge/abs", &st) == VFS_OK && st.size == 1) {
        n++;
    }
    if (vfs_symlink(c, "/edge/la", "lb") == VFS_OK &&
        vfs_symlink(c, "/edge/lb", "la") == VFS_OK &&
        vfs_stat(c, "/edge/la", &st) == VFS_ELOOP) {
        n++;
    }

    /* 清理，免得 /edge 里留一堆东西（demo 只看 /tmp）。 */
    (void)vfs_unlink(c, "/edge/rel");
    (void)vfs_unlink(c, "/edge/abs");
    (void)vfs_unlink(c, "/edge/la");
    (void)vfs_unlink(c, "/edge/lb");
    (void)vfs_unlink(c, "/edge/a");

    if (n != 12) {
        serial_puts("ng: vfs path edge count=");
        serial_putdec((u64)n);
        serial_puts("\r\n");
        fail_item("vfs path edge");
        return 1;
    }
    serial_puts("ng: vfs path edge ok (n=");
    serial_putdec((u64)n);
    serial_puts(")\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */

int fs_selftest(void)
{
    struct fs_env env;
    struct vfs_ctx *c;
    int rc;

    g_fails = 0;
    env.page_alloc = sel_page_alloc;
    env.page_free = sel_page_free;
    env.ctx = NULL;
    rc = fs_sys_init(&env);
    if (rc != VFS_OK) {
        serial_puts("ng: fs init rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("vfs mount");
        return g_fails;
    }
    c = fs_sys_ctx();

    g_fails += check_mount(c);
    g_fails += check_ramfs(c);
    g_fails += check_fd_rejects(c);
    g_fails += check_path_edges(c);

    /* 20..27 的路径指针经 uaccess：内核 CR3 下一定是 EFAULT。 */
    if (fs_sys_mkdir(0x1000ULL, 0755) != VFS_EFAULT ||
        fs_sys_unlink(0x1000ULL) != VFS_EFAULT ||
        fs_sys_listdir(0x1000ULL, 0x2000ULL, 1) != VFS_EFAULT) {
        fail_item("fs syscall user ptr");
    } else {
        serial_puts("ng: fs syscall user ptr EFAULT ok\r\n");
    }

    /* /tmp 留给 ring3 demo；这里只确认它存在且是目录。 */
    {
        struct vfs_stat st;

        if (vfs_stat(c, "/tmp", &st) != VFS_OK || st.type != VFS_NODE_DIR) {
            fail_item("vfs mount");
        }
    }
    return g_fails;
}
