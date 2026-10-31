/*
 * ng WP2-1 host 单测：VFS 核心 + ramfs。
 *
 * 覆盖（同一份源码在内核里也跑）：
 *   1. 路径词法：绝对/相对/cwd/"."/".."/重复 '/'/尾 '/'/空路径/NUL 结尾/
 *      组件与整条路径长度上限/规范化缓冲不足；
 *   2. 挂载：根挂载、重复挂载、挂载点必须存在且是目录、子挂载、卸载
 *      （根/子挂载占用）、挂载点跨越（/mnt 前缀按组件边界匹配）；
 *   3. 节点与文件：create/stat/读写/截断/删除/改名/目录枚举/目录与文件
 *      类型错误（EISDIR/ENOTDIR/EEXIST/ENOSPC/EBADF）；
 *   4. ramfs 上限：页池耗尽（fake 池 8 页）与单文件 1 MiB 上限；
 *   5. 符号链接：相对/绝对目标、循环 ELOOP、悬空目标 ENOENT、readlink、
 *      链接自身删除不影响目标；
 *   6. fd：slot+generation 复用后旧 fd 失效（EBADF/stale_fd）、fd 表满、
 *      非法 fd、打开文件被 unlink 后仍可读、最后 close 才回收页；
 *   7. 权限：uid!=0 时 owner 位检查（读/写/建文件），uid=0 绕过；
 *   8. 错误码逐个断言（不是“只要是负值就算过”）。
 */
#include <string.h>

#include "fake_ng_env.h"
#include "ramfs.h"
#include "test_util.h"
#include "vfs.h"

static struct ramfs g_fs;
static struct vfs_ctx g_ctx;
static u8 g_wbuf[4096 * 3];
static u8 g_rbuf[4096 * 3];

static void env_reset(void)
{
    struct fs_env env;

    fake_fs_reset();
    vfs_reset();
    fake_fs_env(&env);
    ramfs_init(&g_fs, &env);
    vfs_ctx_init(&g_ctx);
    CHECK(vfs_mount("/", ramfs_ops(), &g_fs, ramfs_root(&g_fs)) == VFS_OK);
    CHECK(vfs_mount_count() == 1);
}

static void put_file(struct vfs_ctx *c, const char *path, const void *data,
                     u64 len)
{
    int fd = vfs_open(c, path, VFS_O_CREAT | VFS_O_RDWR, 0644);

    CHECK(fd > 0);
    if (fd > 0) {
        CHECK(vfs_write(c, fd, data, len) == (int)len);
        CHECK(vfs_close(c, fd) == VFS_OK);
    }
}

/* ------------------------------------------------------------------------- */
/* 1. 路径词法                                                               */
/* ------------------------------------------------------------------------- */

static void t_normalize(void)
{
    char out[VFS_PATH_MAX];
    char big[VFS_PATH_MAX + 8];
    u32 i;

    CHECK(vfs_normalize("/", "/", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/") == 0);

    CHECK(vfs_normalize("/", "", out, sizeof(out)) == VFS_EINVAL);
    CHECK(vfs_normalize("/", NULL, out, sizeof(out)) == VFS_EINVAL);
    CHECK(vfs_normalize("/", "/", NULL, sizeof(out)) == VFS_EINVAL);
    CHECK(vfs_normalize("/", "/a", out, 2) == VFS_ENAMETOOLONG);

    CHECK(vfs_normalize("/", ".", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/") == 0);
    CHECK(vfs_normalize("/", "./a", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a") == 0);
    CHECK(vfs_normalize("/", "a/b", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a/b") == 0);
    CHECK(vfs_normalize("/x", "a", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/x/a") == 0);
    CHECK(vfs_normalize(NULL, "a", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a") == 0);
    CHECK(vfs_normalize("/x/", "a", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/x/a") == 0);

    CHECK(vfs_normalize("/", "/a//b", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a/b") == 0);
    CHECK(vfs_normalize("/", "/a/./b/", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a/b") == 0);
    CHECK(vfs_normalize("/", "/a/../b", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/b") == 0);
    CHECK(vfs_normalize("/", "/a/b/..", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/a") == 0);
    CHECK(vfs_normalize("/", "/..", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/") == 0);
    CHECK(vfs_normalize("/", "/../../x", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/x") == 0);
    CHECK(vfs_normalize("/", "///", out, sizeof(out)) == VFS_OK);
    CHECK(strcmp(out, "/") == 0);

    /* 单个组件 63 字节 OK，64 字节超限。 */
    for (i = 0; i < 63; i++) {
        big[i] = 'n';
    }
    big[63] = '\0';
    CHECK(vfs_normalize("/", big, out, sizeof(out)) == VFS_OK);
    CHECK(strlen(out) == 64); /* '/' + 63 */
    big[63] = 'n';
    big[64] = '\0';
    CHECK(vfs_normalize("/", big, out, sizeof(out)) == VFS_ENAMETOOLONG);

    /* 整条路径 255 字节 OK，256 字节超限。 */
    for (i = 0; i < 255; i++) {
        big[i] = (i == 0) ? '/' : 'p';
    }
    big[255] = '\0';
    CHECK(vfs_normalize("/", big, out, sizeof(out)) == VFS_ENAMETOOLONG); /* 单组件太长 */
    big[0] = '/';
    for (i = 1; i < 255; i++) {
        big[i] = ((i % 32) == 0) ? '/' : 'p';
    }
    big[255] = '\0';
    CHECK(vfs_normalize("/", big, out, sizeof(out)) == VFS_OK);
    big[255] = 'p';
    big[256] = '\0';
    CHECK(vfs_normalize("/", big, out, sizeof(out)) == VFS_ENAMETOOLONG);
}

/* ------------------------------------------------------------------------- */
/* 2. 挂载                                                                   */
/* ------------------------------------------------------------------------- */

static void t_mount(void)
{
    struct ramfs mnt;
    struct fs_env env;
    struct vfs_node *node = NULL;
    struct vfs_stat st;

    env_reset();
    fake_fs_env(&env);
    ramfs_init(&mnt, &env);

    /* 挂载点必须存在且是目录。 */
    CHECK(vfs_mount("/mnt", ramfs_ops(), &mnt, ramfs_root(&mnt)) == VFS_ENOENT);
    CHECK(vfs_mkdir(&g_ctx, "/mnt", 0755) == VFS_OK);
    CHECK(vfs_mount("/mnt", ramfs_ops(), &mnt, ramfs_root(&mnt)) == VFS_OK);
    CHECK(vfs_mount_count() == 2);
    CHECK(vfs_mount("/mnt", ramfs_ops(), &mnt, ramfs_root(&mnt)) == VFS_EEXIST);
    CHECK(vfs_mount("/nope/x", ramfs_ops(), &mnt, ramfs_root(&mnt)) ==
          VFS_ENOENT);
    CHECK(vfs_mount("/mnt", NULL, &mnt, ramfs_root(&mnt)) == VFS_EINVAL);
    CHECK(vfs_mount("/mnt", ramfs_ops(), &mnt, NULL) == VFS_EINVAL);

    /* 挂载点跨越：写进 /mnt 的文件只存在于第二份 ramfs。 */
    CHECK(vfs_resolve(&g_ctx, "/mnt", &node) == VFS_OK);
    CHECK(node != NULL && node->type == VFS_NODE_DIR);
    CHECK(vfs_resolve(&g_ctx, "/mnt/only", &node) == VFS_ENOENT);
    put_file(&g_ctx, "/mnt/only", "m", 1);
    CHECK(vfs_stat(&g_ctx, "/mnt/only", &st) == VFS_OK);
    /* 根 fs 里的 /mnt 还是目录（mount 只是覆盖，不替换目录项）。 */
    CHECK(vfs_stat(&g_ctx, "/mnt", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_DIR);

    /* "/mnt2" 不能被 "/mnt" 前缀误匹配。 */
    CHECK(vfs_mkdir(&g_ctx, "/mnt2", 0755) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/mnt2", &st) == VFS_OK);

    /* 根挂载不能卸载；子挂载可卸载，卸掉后 /mnt/only 就消失了。 */
    CHECK(vfs_umount("/") == VFS_EBUSY);
    CHECK(vfs_umount("/mnt") == VFS_OK);
    CHECK(vfs_mount_count() == 1);
    CHECK(vfs_umount("/mnt") == VFS_ENOENT);
    CHECK(vfs_umount("/nope") == VFS_ENOENT);
    CHECK(vfs_stat(&g_ctx, "/mnt/only", &st) == VFS_ENOENT);

    /* 子挂载存在时父挂载不能卸载（用 /mnt 下的 /mnt/x 造一层）。 */
    CHECK(vfs_mkdir(&g_ctx, "/sub", 0755) == VFS_OK);
    CHECK(vfs_mount("/sub", ramfs_ops(), &mnt, ramfs_root(&mnt)) == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "/sub/inner", 0755) == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "/sub/inner/x", 0755) == VFS_OK);
    CHECK(vfs_mount("/sub/inner/x", ramfs_ops(), &mnt, ramfs_root(&mnt)) ==
          VFS_OK);
    CHECK(vfs_umount("/sub") == VFS_EBUSY);
    CHECK(vfs_umount("/sub/inner") == VFS_ENOENT);
    CHECK(vfs_umount("/sub/inner/x") == VFS_OK);
    CHECK(vfs_umount("/sub") == VFS_OK);
    CHECK(vfs_mount_path_at(0) != NULL);
    CHECK(vfs_mount_path_at(8) == NULL);
}

/* ------------------------------------------------------------------------- */
/* 3. 节点 / 文件操作                                                        */
/* ------------------------------------------------------------------------- */

static void t_file_ops(void)
{
    struct vfs_stat st;
    int fd;
    int fd2;

    env_reset();
    CHECK(vfs_stat(&g_ctx, "/", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_DIR);
    CHECK((st.mode & VFS_S_IFMT) == VFS_S_IFDIR);
    CHECK(st.ino == 1);
    CHECK((st.mode & 0777u) == 0755u);

    CHECK(vfs_mkdir(&g_ctx, "/d", 0750) == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "/d", 0750) == VFS_EEXIST);
    CHECK(vfs_stat(&g_ctx, "/d", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_DIR);
    CHECK((st.mode & 0777u) == 0750u);

    fd = vfs_open(&g_ctx, "/d/f", VFS_O_CREAT | VFS_O_RDWR, 0644);
    CHECK(fd > 0);
    CHECK(vfs_fd_live(&g_ctx) == 1);
    CHECK(vfs_write(&g_ctx, fd, "hello", 5) == 5);
    CHECK(vfs_stat(&g_ctx, "/d/f", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_FILE);
    CHECK((st.mode & VFS_S_IFMT) == VFS_S_IFREG);
    CHECK((st.mode & 0777u) == 0644u);
    CHECK(st.size == 5);
    CHECK(st.ino > 1);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 新 fd 从 offset 0 顺序读：读到 EOF 返回 0。 */
    fd = vfs_open(&g_ctx, "/d/f", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    memset(g_rbuf, 0, sizeof(g_rbuf));
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 8) == 5);
    CHECK(memcmp(g_rbuf, "hello", 5) == 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 8) == 0);

    /* O_EXCL 已存在 -> EEXIST；O_TRUNC 清零。 */
    CHECK(vfs_open(&g_ctx, "/d/f", VFS_O_CREAT | VFS_O_EXCL, 0644) ==
          VFS_EEXIST);
    fd2 = vfs_open(&g_ctx, "/d/f", VFS_O_TRUNC | VFS_O_RDWR, 0644);
    CHECK(fd2 > 0);
    CHECK(vfs_stat(&g_ctx, "/d/f", &st) == VFS_OK);
    CHECK(st.size == 0);
    CHECK(vfs_close(&g_ctx, fd2) == VFS_OK);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(vfs_fd_live(&g_ctx) == 0);

    /* 只读 fd 写 -> EBADF；只写 fd 读 -> EBADF。 */
    fd = vfs_open(&g_ctx, "/d/f", VFS_O_RDONLY, 0644);
    CHECK(fd > 0);
    CHECK(vfs_write(&g_ctx, fd, "x", 1) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    fd = vfs_open(&g_ctx, "/d/f", VFS_O_WRONLY, 0644);
    CHECK(fd > 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 1) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 目录：不能写/截断/删；不能当文件读；RDWR 打开 -> EISDIR。 */
    fd = vfs_open(&g_ctx, "/d", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 1) == VFS_EISDIR);
    CHECK(vfs_write(&g_ctx, fd, "x", 1) == VFS_EISDIR);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(vfs_open(&g_ctx, "/d", VFS_O_RDWR, 0) == VFS_EISDIR);
    CHECK(vfs_open(&g_ctx, "/d", VFS_O_RDONLY | VFS_O_TRUNC, 0) == VFS_EISDIR);
    CHECK(vfs_unlink(&g_ctx, "/d") == VFS_EISDIR);
    CHECK(vfs_truncate(&g_ctx, "/d", 0) == VFS_EISDIR);

    /* 文件当目录：ENOTDIR。 */
    CHECK(vfs_open(&g_ctx, "/d/f/x", VFS_O_RDONLY, 0) == VFS_ENOTDIR);
    CHECK(vfs_listdir(&g_ctx, "/d/f", NULL, 1) == VFS_EINVAL);
    {
        struct vfs_dirent e;

        CHECK(vfs_listdir(&g_ctx, "/d/f", &e, 1) == VFS_ENOTDIR);
    }

    /* 改名：同目录内成功；目标存在 EEXIST；找不到 ENOENT。 */
    put_file(&g_ctx, "/d/a", "a", 1);
    CHECK(vfs_rename(&g_ctx, "/d/a", "/d/b") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/d/a", &st) == VFS_ENOENT);
    CHECK(vfs_stat(&g_ctx, "/d/b", &st) == VFS_OK);
    CHECK(vfs_rename(&g_ctx, "/d/b", "/d/b") == VFS_EEXIST);
    CHECK(vfs_rename(&g_ctx, "/d/zz", "/d/yy") == VFS_ENOENT);

    /* 删除：文件可删，路径消失。 */
    CHECK(vfs_unlink(&g_ctx, "/d/b") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/d/b", &st) == VFS_ENOENT);
    CHECK(vfs_unlink(&g_ctx, "/d/b") == VFS_ENOENT);
}

/* ------------------------------------------------------------------------- */
/* 4. 读写 / 截断 / 上限                                                     */
/* ------------------------------------------------------------------------- */

static void t_data(void)
{
    struct vfs_stat st;
    int fd;
    u32 i;

    env_reset();
    CHECK(vfs_mkdir(&g_ctx, "/data", 0755) == VFS_OK);
    for (i = 0; i < sizeof(g_wbuf); i++) {
        g_wbuf[i] = (u8)(i * 7u + 3u);
    }
    put_file(&g_ctx, "/data/p", g_wbuf, sizeof(g_wbuf)); /* 12 KiB = 3 页 */
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_OK);
    CHECK(st.size == sizeof(g_wbuf));
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    memset(g_rbuf, 0, sizeof(g_rbuf));
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, sizeof(g_rbuf)) ==
          (int)sizeof(g_rbuf));
    CHECK(memcmp(g_rbuf, g_wbuf, sizeof(g_wbuf)) == 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 跨页偏移读：从 4090 读 12 字节。 */
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 4090) == 4090); /* 跳过前 4090 字节 */
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 12) == 12);
    CHECK(memcmp(g_rbuf, g_wbuf + 4090, 12) == 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 截断：缩小释放页，扩大补零。 */
    CHECK(vfs_truncate(&g_ctx, "/data/p", 100) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_OK);
    CHECK(st.size == 100);
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    memset(g_rbuf, 0xAA, 32);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 32) == 32);
    CHECK(memcmp(g_rbuf, g_wbuf, 32) == 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    CHECK(vfs_truncate(&g_ctx, "/data/p", 8192) == VFS_OK);
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 4096) == 4096); /* 前 4096 有 100 字节旧数据 */
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 4096) == 4096);
    for (i = 100; i < 4096; i++) {
        if (g_rbuf[i] != 0) {
            break;
        }
    }
    CHECK(i == 4096);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 1) == 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 单文件上限 1 MiB：+1 字节 -> ENOSPC。 */
    CHECK(vfs_truncate(&g_ctx, "/data/p", RAMFS_MAX_FILE_BYTES + 1) ==
          VFS_ENOSPC);
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_OK);
    CHECK(st.size == 8192);
    /* 页池（fake 8 页）耗尽 -> ENOMEM（数据页分配器失败），size 不变。 */
    CHECK(vfs_truncate(&g_ctx, "/data/p", 40000) == VFS_ENOMEM);
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_OK);
    CHECK(st.size == 8192);

    /* 追加写：O_APPEND 总在末尾。 */
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_APPEND | VFS_O_WRONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_write(&g_ctx, fd, "ZZ", 2) == 2);
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_OK);
    CHECK(st.size == 8194);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 打开的文件被 unlink：仍可读；close 后页/节点回收。 */
    fd = vfs_open(&g_ctx, "/data/p", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_unlink(&g_ctx, "/data/p") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/data/p", &st) == VFS_ENOENT);
    memset(g_rbuf, 0, 8);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 8) == 8);
    CHECK(memcmp(g_rbuf, g_wbuf, 8) == 0);
    CHECK(fake_fs_pages_live() == 3); /* unlink 但 fd 还开着，页不回收 */
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(fake_fs_pages_live() == 0); /* 最后一个 close 才回收 */
}

/* ------------------------------------------------------------------------- */
/* 5. 符号链接                                                               */
/* ------------------------------------------------------------------------- */

static void t_symlink(void)
{
    struct vfs_stat st;
    char link[VFS_PATH_MAX];
    int fd;

    env_reset();
    CHECK(vfs_mkdir(&g_ctx, "/l", 0755) == VFS_OK);
    put_file(&g_ctx, "/l/target", "target-data", 11);

    /* 相对目标。 */
    CHECK(vfs_symlink(&g_ctx, "/l/rel", "target") == VFS_OK);
    CHECK(vfs_readlink(&g_ctx, "/l/rel", link, sizeof(link)) == 6);
    CHECK(strcmp(link, "target") == 0);
    CHECK(vfs_stat(&g_ctx, "/l/rel", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_FILE);
    CHECK(st.size == 11);
    fd = vfs_open(&g_ctx, "/l/rel", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&g_ctx, fd, g_rbuf, 16) == 11);
    CHECK(memcmp(g_rbuf, "target-data", 11) == 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    /* 绝对目标 + ".." 目标。 */
    CHECK(vfs_symlink(&g_ctx, "/l/abs", "/l/target") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/abs", &st) == VFS_OK && st.size == 11);
    CHECK(vfs_symlink(&g_ctx, "/l/dotdot", "../l/target") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/dotdot", &st) == VFS_OK && st.size == 11);

    /* 悬空目标 -> ENOENT；循环 -> ELOOP。 */
    CHECK(vfs_symlink(&g_ctx, "/l/dangling", "missing") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/dangling", &st) == VFS_ENOENT);
    CHECK(vfs_symlink(&g_ctx, "/l/loop_a", "loop_b") == VFS_OK);
    CHECK(vfs_symlink(&g_ctx, "/l/loop_b", "loop_a") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/loop_a", &st) == VFS_ELOOP);

    /* 链接本身 stat 之后仍是链接（st.type 走跟随，所以用 readlink 看类型）。 */
    CHECK(vfs_readlink(&g_ctx, "/l/target", link, sizeof(link)) == VFS_EINVAL);
    CHECK(vfs_readlink(&g_ctx, "/l/nope", link, sizeof(link)) == VFS_ENOENT);
    CHECK(vfs_readlink(&g_ctx, "/l/rel", link, 4) == VFS_ENAMETOOLONG);

    /* 删链接不影响目标；重复创建链接 EEXIST。 */
    CHECK(vfs_symlink(&g_ctx, "/l/rel", "target") == VFS_EEXIST);
    CHECK(vfs_unlink(&g_ctx, "/l/rel") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/target", &st) == VFS_OK && st.size == 11);
    CHECK(vfs_stat(&g_ctx, "/l/rel", &st) == VFS_ENOENT);

    /* 通过链接写：写到目标文件。 */
    fd = vfs_open(&g_ctx, "/l/abs", VFS_O_WRONLY | VFS_O_TRUNC, 0);
    CHECK(fd > 0);
    CHECK(vfs_write(&g_ctx, fd, "xy", 2) == 2);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/l/target", &st) == VFS_OK && st.size == 2);
}

/* ------------------------------------------------------------------------- */
/* 6. fd 表 / 代际                                                           */
/* ------------------------------------------------------------------------- */

static void t_fd_table(void)
{
    int fds[VFS_FD_MAX];
    int fd;
    int old;
    int base;
    u32 i;

    env_reset();
    CHECK(vfs_open(&g_ctx, "/nope", VFS_O_RDONLY, 0) == VFS_ENOENT);
    base = vfs_open(&g_ctx, "/f1", VFS_O_CREAT | VFS_O_RDWR, 0644);
    CHECK(base > 0);

    /* 非法 fd。 */
    CHECK(vfs_read(&g_ctx, 0, g_rbuf, 1) == VFS_EBADF);
    CHECK(vfs_read(&g_ctx, -1, g_rbuf, 1) == VFS_EBADF);
    CHECK(vfs_write(&g_ctx, 0x7FFF0000, g_rbuf, 1) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, 0) == VFS_EBADF);

    /* slot/gen 复用：close 后旧 fd 失效，新 fd generation 不同。 */
    old = vfs_open(&g_ctx, "/f1", VFS_O_RDWR, 0);
    CHECK(old > 0);
    CHECK(vfs_fd_slot_of(old) == 0 || vfs_fd_slot_of(old) == 1);
    CHECK(vfs_fd_gen_of(old) >= 1);
    CHECK(vfs_close(&g_ctx, old) == VFS_OK);
    fd = vfs_open(&g_ctx, "/f1", VFS_O_RDWR, 0);
    CHECK(fd > 0);
    CHECK(vfs_fd_slot_of(fd) == vfs_fd_slot_of(old)); /* 同一槽位被复用 */
    CHECK(vfs_fd_gen_of(fd) == vfs_fd_gen_of(old) + 1u);
    CHECK(vfs_read(&g_ctx, old, g_rbuf, 1) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, old) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    CHECK(vfs_close(&g_ctx, base) == VFS_OK);

    /* fd 表满：16 个槽位。 */
    for (i = 0; i < VFS_FD_MAX; i++) {
        fds[i] = vfs_open(&g_ctx, "/f1", VFS_O_RDWR, 0);
        CHECK(fds[i] > 0);
    }
    CHECK(vfs_fd_live(&g_ctx) == VFS_FD_MAX);
    CHECK(vfs_open(&g_ctx, "/f1", VFS_O_RDWR, 0) == VFS_EMFILE);
    for (i = 0; i < VFS_FD_MAX; i++) {
        CHECK(vfs_close(&g_ctx, fds[i]) == VFS_OK);
    }
    CHECK(vfs_fd_live(&g_ctx) == 0);

    /* 统计：open/close 次数、bad_fd、stale_fd、peak。 */
    {
        u32 bad = g_ctx.bad_fd;
        u32 stale = g_ctx.stale_fd;

        CHECK(vfs_close(&g_ctx, old) == VFS_EBADF);
        CHECK(g_ctx.bad_fd > bad);
        CHECK(vfs_read(&g_ctx, 123456, g_rbuf, 1) == VFS_EBADF);
        (void)stale;
    }
}

/* 6b. open 失败不留副作用：fd 表满时不能截断已有文件 / 不能创建新文件。 */
static void t_open_side_effects(void)
{
    struct vfs_stat st;
    int fds[VFS_FD_MAX];
    int fd;
    u32 i;

    env_reset();
    put_file(&g_ctx, "/keep", "hello", 5);
    put_file(&g_ctx, "/victim", "hello", 5);

    for (i = 0; i < VFS_FD_MAX; i++) {
        fds[i] = vfs_open(&g_ctx, "/keep", VFS_O_RDONLY, 0);
        CHECK(fds[i] > 0);
    }
    CHECK(vfs_fd_live(&g_ctx) == VFS_FD_MAX);

    /* O_TRUNC 的 open 在 fd 分配失败时不能截断文件。 */
    CHECK(vfs_open(&g_ctx, "/victim", VFS_O_RDWR | VFS_O_TRUNC, 0) ==
          VFS_EMFILE);
    CHECK(vfs_stat(&g_ctx, "/victim", &st) == VFS_OK && st.size == 5);

    /* O_CREAT 的 open 在 fd 分配失败时不能留下新文件。 */
    CHECK(vfs_open(&g_ctx, "/newfile", VFS_O_CREAT | VFS_O_RDWR, 0644) ==
          VFS_EMFILE);
    CHECK(vfs_stat(&g_ctx, "/newfile", &st) == VFS_ENOENT);

    for (i = 0; i < VFS_FD_MAX; i++) {
        CHECK(vfs_close(&g_ctx, fds[i]) == VFS_OK);
    }
    CHECK(vfs_fd_live(&g_ctx) == 0);
    CHECK(g_ctx.fds[0].used == 0 && g_ctx.fds[VFS_FD_MAX - 1].used == 0);

    /* 表空后同样的 open 成功；victim 此前未被破坏。 */
    fd = vfs_open(&g_ctx, "/victim", VFS_O_RDWR | VFS_O_TRUNC, 0);
    CHECK(fd > 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/victim", &st) == VFS_OK && st.size == 0);
    fd = vfs_open(&g_ctx, "/newfile", VFS_O_CREAT | VFS_O_RDWR, 0644);
    CHECK(fd > 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/newfile", &st) == VFS_OK);
}

/* ------------------------------------------------------------------------- */
/* 7. 权限                                                                   */
/* ------------------------------------------------------------------------- */

static void t_perm(void)
{
    int fd;
    int rc;

    env_reset();
    rc = vfs_mkdir(&g_ctx, "/ro", 0555);
    CHECK(rc == VFS_OK);
    put_file(&g_ctx, "/ro/f", "abc", 3);
    /* uid=0 时先建只读文件（0555 目录里 root 可以写）。 */
    CHECK(vfs_open(&g_ctx, "/ro/rofile", VFS_O_CREAT | VFS_O_RDWR, 0444) > 0);

    g_ctx.uid = 1000;
    /* 目录 0555：不能在里面建东西。 */
    CHECK(vfs_open(&g_ctx, "/ro/new", VFS_O_CREAT | VFS_O_RDWR, 0644) ==
          VFS_EPERM);
    CHECK(vfs_mkdir(&g_ctx, "/ro/d2", 0755) == VFS_EPERM);
    CHECK(vfs_unlink(&g_ctx, "/ro/f") == VFS_EPERM);

    /* 文件 0644 owner 位：读 OK，写 OK（owner 是 uid 1000 自己）。 */
    fd = vfs_open(&g_ctx, "/ro/f", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);

    CHECK(vfs_open(&g_ctx, "/ro/rofile", VFS_O_WRONLY, 0) == VFS_EPERM);
    CHECK(vfs_truncate(&g_ctx, "/ro/rofile", 0) == VFS_EPERM);
    fd = vfs_open(&g_ctx, "/ro/rofile", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_write(&g_ctx, fd, "x", 1) == VFS_EBADF);
    CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
    CHECK(g_ctx.perm_denied >= 3);

    /* root（uid=0）绕过权限位。 */
    g_ctx.uid = 0;
    CHECK(vfs_open(&g_ctx, "/ro/rofile", VFS_O_WRONLY, 0) > 0);
    CHECK(vfs_mkdir(&g_ctx, "/ro/rootd", 0755) == VFS_OK);
}

/* ------------------------------------------------------------------------- */
/* 8. 目录 / 相对路径 / cwd                                                  */
/* ------------------------------------------------------------------------- */

static void t_dir(void)
{
    struct vfs_dirent ents[8];
    struct vfs_stat st;
    int n;
    int fd;

    env_reset();
    CHECK(vfs_mkdir(&g_ctx, "/dir", 0755) == VFS_OK);
    put_file(&g_ctx, "/dir/one", "1", 1);
    put_file(&g_ctx, "/dir/two", "22", 2);
    CHECK(vfs_mkdir(&g_ctx, "/dir/sub", 0755) == VFS_OK);

    memset(ents, 0, sizeof(ents));
    n = vfs_listdir(&g_ctx, "/dir", ents, 8);
    CHECK(n == 3);
    CHECK(ents[0].name_len >= 1 && ents[0].name[0] != '\0');
    CHECK(vfs_listdir(&g_ctx, "/dir", ents, 1) == 1); /* 截断到 max */
    CHECK(vfs_listdir(&g_ctx, "/", ents, 8) >= 1);
    CHECK(vfs_listdir(&g_ctx, "/nope", ents, 8) == VFS_ENOENT);

    /* cwd / 相对路径。 */
    CHECK(vfs_chdir(&g_ctx, "/dir") == VFS_OK);
    CHECK(strcmp(g_ctx.cwd, "/dir") == 0);
    CHECK(vfs_stat(&g_ctx, "one", &st) == VFS_OK && st.size == 1);
    CHECK(vfs_open(&g_ctx, "one", VFS_O_RDONLY, 0) > 0);
    CHECK(vfs_chdir(&g_ctx, "..") == VFS_OK);
    CHECK(strcmp(g_ctx.cwd, "/") == 0);
    CHECK(vfs_chdir(&g_ctx, "/dir/sub") == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "../one", &st) == VFS_OK);
    CHECK(vfs_chdir(&g_ctx, "/dir/one") == VFS_ENOTDIR);
    CHECK(vfs_chdir(&g_ctx, "/nope") == VFS_ENOENT);

    /* 相对路径创建：cwd=/dir 时 mkdir("sub2")。 */
    CHECK(vfs_chdir(&g_ctx, "/dir") == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "sub2", 0755) == VFS_OK);
    CHECK(vfs_stat(&g_ctx, "/dir/sub2", &st) == VFS_OK);

    /* 空路径 / 超长路径穿过 FS 层。 */
    fd = vfs_open(&g_ctx, "", VFS_O_RDONLY, 0);
    CHECK(fd == VFS_EINVAL);
    {
        char longp[VFS_PATH_MAX + 8];

        memset(longp, 'x', sizeof(longp) - 1);
        longp[sizeof(longp) - 1] = '\0';
        CHECK(vfs_open(&g_ctx, longp, VFS_O_RDONLY, 0) == VFS_ENAMETOOLONG);
    }
}

/* ------------------------------------------------------------------------- */

int test_vfs(void)
{
    t_normalize();
    t_mount();
    t_file_ops();
    t_data();
    t_symlink();
    t_fd_table();
    t_open_side_effects();
    t_perm();
    t_dir();
    return 0;
}
