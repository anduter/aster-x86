/*
 * ng WP2-7: parity 套件 —— 100 项断言的共享实现（kernel + host 同一份）。
 *
 * 本文件被两处 include：
 *   - src/ng/parity_selftest.c（内核自检；ATA 走真端口，显示/输入走真硬件）
 *   - tests/unit-ng/test_parity.c（host；ATA 走 fake 控制器，显示/输入跳过）
 *
 * 约定：
 *   - 只使用 L1/L0 的公开接口（vfs/ramfs/fat16/auth/log/pkg + shell 的 host
 *     测试钩子），不 include 任何内核内部头；因此同一份断言在两边语义一致。
 *   - 每项一个编号，返回 PARITY_PASS/PARITY_FAIL；失败原因写进 p_why，
 *     由 harness 打印（host/kernel 各自的输出函数）。
 *   - 用例自带清理：文件 unlink，目录不删（VFS 没有 rmdir），所以重复运行
 *     幂等；所有文件名带编号，避免互相干扰。
 *   - 不用 libc（内核 -ffreestanding）：字符串/数字都是本文件的小工具。
 *
 * harness 必须提供：
 *   struct vfs_ctx *parity_vfs_ctx(void);   已挂 ramfs 根、/parity 存在
 *   int parity_ata_case(int which);         0..4
 *   int parity_disp_case(int which);        0..4（仅内核）
 * shell.c 以 -DNG_HOST_TEST 编译（内核也编一份），提供 sh_test_* 钩子。
 */
#include "auth.h"
#include "blk.h"
#include "fat16.h"
#include "log.h"
#include "parity_cases.h"
#include "pkg.h"
#include "ramfs.h"
#include "types.h"
#include "vfs.h"

#define PARITY_PASS 0
#define PARITY_FAIL 1

extern struct vfs_ctx *parity_vfs_ctx(void);
extern int parity_ata_case(int which);
#if !defined(NG_PARITY_HOST)
extern int parity_disp_case(int which);
#endif

/* src/apps/shell/shell.c（-DNG_HOST_TEST）的测试钩子。 */
int sh_test_tokenize(char *line, int *argc, char *argv[8], char **redir);
int sh_test_resolve(const char *cwd, const char *in, char *out, u32 cap);

#define P_VFS_DIR "/parity/vfs"
#define P_RAM_DIR "/parity/ram"

#define P_SHELL_EINVAL (-22)

/* ------------------------------------------------------------------------- */
/* 失败原因                                                                  */
/* ------------------------------------------------------------------------- */

static char p_why[192];
static u32 p_why_len;

static void p_why_reset(void)
{
    p_why[0] = '\0';
    p_why_len = 0;
}

static void p_why_add(const char *s)
{
    if (s == 0) {
        return;
    }
    while (*s != '\0' && p_why_len + 1u < (u32)sizeof(p_why)) {
        p_why[p_why_len++] = *s;
    }
    p_why[p_why_len] = '\0';
}

static void p_why_int(i64 v)
{
    char tmp[24];
    u32 n = 0;
    u64 u;
    int neg = (v < 0);

    u = neg ? (u64)(-v) : (u64)v;
    if (u == 0u) {
        tmp[n++] = '0';
    }
    while (u != 0u && n < (u32)sizeof(tmp) - 1u) {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    if (neg && n < (u32)sizeof(tmp) - 1u) {
        tmp[n++] = '-';
    }
    while (n > 0u) {
        char c = tmp[--n];

        if (p_why_len + 1u < (u32)sizeof(p_why)) {
            p_why[p_why_len++] = c;
        }
    }
    p_why[p_why_len] = '\0';
}

#define PCHK(cond, what)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            p_why_add(what);                                                   \
            return PARITY_FAIL;                                                \
        }                                                                      \
    } while (0)

#define PEQ(got, want, what)                                                   \
    do {                                                                       \
        int p_g = (int)(got);                                                  \
        int p_w = (int)(want);                                                 \
        if (p_g != p_w) {                                                      \
            p_why_add(what);                                                   \
            p_why_add(" got=");                                                \
            p_why_int(p_g);                                                    \
            p_why_add(" want=");                                               \
            p_why_int(p_w);                                                    \
            return PARITY_FAIL;                                                \
        }                                                                      \
    } while (0)

#define PSTR(got, want, what)                                                  \
    do {                                                                       \
        if (!p_streq((got), (want))) {                                         \
            p_why_add(what);                                                   \
            p_why_add(" got=\"");                                              \
            p_why_add(got);                                                    \
            p_why_add("\" want=\"");                                           \
            p_why_add(want);                                                   \
            p_why_add("\"");                                                   \
            return PARITY_FAIL;                                                \
        }                                                                      \
    } while (0)

#define PMEM(got, want, n, what)                                               \
    do {                                                                       \
        if (!p_memeq((got), (want), (n))) {                                    \
            p_why_add(what);                                                   \
            return PARITY_FAIL;                                                \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */
/* 小工具                                                                    */
/* ------------------------------------------------------------------------- */

static u32 p_strlen(const char *s)
{
    u32 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int p_streq(const char *a, const char *b)
{
    u32 i = 0;

    if (a == 0 || b == 0) {
        return (a == b) ? 1 : 0;
    }
    while (a[i] != '\0' || b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void p_zero(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

static void p_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int p_memeq(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    u32 i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

/* 在 [hay,hay+hlen) 里找 NUL 结尾的 needle；返回 1/0。 */
static int p_has(const char *hay, u32 hlen, const char *needle)
{
    u32 nl = p_strlen(needle);
    u32 i;

    if (nl == 0u || nl > hlen) {
        return 0;
    }
    for (i = 0; i + nl <= hlen; i++) {
        if (p_memeq(hay + i, needle, nl)) {
            return 1;
        }
    }
    return 0;
}

static u32 p_count_byte(const char *buf, u32 len, char c)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < len; i++) {
        if (buf[i] == c) {
            n++;
        }
    }
    return n;
}

/* 建/写/关一个文件；返回 VFS_OK 或负 errno。 */
static int p_write_file(struct vfs_ctx *c, const char *path, const void *data,
                        u32 len)
{
    int fd;
    int n;
    int rc;

    fd = vfs_open(c, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644);
    if (fd <= 0) {
        return fd;
    }
    n = vfs_write(c, fd, data, (u64)len);
    rc = vfs_close(c, fd);
    if (n < 0) {
        return n;
    }
    if ((u32)n != len) {
        return VFS_ENOSPC;
    }
    return rc;
}

/* 读到 buf（最多 max 字节）；返回字节数或负 errno。 */
static int p_read_file(struct vfs_ctx *c, const char *path, void *buf, u32 max)
{
    int fd;
    int n;

    fd = vfs_open(c, path, VFS_O_RDONLY, 0);
    if (fd <= 0) {
        return fd;
    }
    n = vfs_read(c, fd, buf, (u64)max);
    (void)vfs_close(c, fd);
    return n;
}

static int p_mkdir_ok(struct vfs_ctx *c, const char *path)
{
    int rc = vfs_mkdir(c, path, 0755);

    return (rc == VFS_OK || rc == VFS_EEXIST) ? VFS_OK : rc;
}

static u8 p_buf[5120];
static u8 p_buf2[5120];

/* ------------------------------------------------------------------------- */
/* VFS（1..20）                                                              */
/* ------------------------------------------------------------------------- */

static int p_vfs_01(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/", "/", out, (u32)sizeof(out)), VFS_OK, "normalize rc");
    PSTR(out, "/", "normalize out");
    return PARITY_PASS;
}

static int p_vfs_02(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/", "//a//b/", out, (u32)sizeof(out)), VFS_OK,
        "normalize rc");
    PSTR(out, "/a/b", "normalize out");
    return PARITY_PASS;
}

static int p_vfs_03(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/x", ".", out, (u32)sizeof(out)), VFS_OK, "normalize rc");
    PSTR(out, "/x", "normalize out");
    return PARITY_PASS;
}

static int p_vfs_04(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/x/y", "..", out, (u32)sizeof(out)), VFS_OK,
        "normalize rc");
    PSTR(out, "/x", "normalize out");
    return PARITY_PASS;
}

static int p_vfs_05(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/x", "../..", out, (u32)sizeof(out)), VFS_OK,
        "normalize rc");
    PSTR(out, "/", "normalize out");
    return PARITY_PASS;
}

static int p_vfs_06(void)
{
    char out[VFS_PATH_MAX];

    PEQ(vfs_normalize("/", "", out, (u32)sizeof(out)), VFS_EINVAL, "empty path");
    PEQ(vfs_normalize("/", 0, out, (u32)sizeof(out)), VFS_EINVAL, "NULL path");
    PEQ(vfs_normalize("/", "/", 0, (u32)sizeof(out)), VFS_EINVAL, "NULL out");
    return PARITY_PASS;
}

static int p_vfs_07(void)
{
    char longpath[VFS_PATH_MAX + 64];
    char out[VFS_PATH_MAX];
    u32 i;

    for (i = 0; i < (u32)sizeof(longpath) - 1u; i++) {
        longpath[i] = 'p';
    }
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1u] = '\0';
    PEQ(vfs_normalize("/", longpath, out, (u32)sizeof(out)),
        VFS_ENAMETOOLONG, "long path");
    return PARITY_PASS;
}

static int p_vfs_08(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    char longpath[VFS_PATH_MAX + 64];
    u32 i;

    for (i = 0; i < (u32)sizeof(longpath) - 1u; i++) {
        longpath[i] = 'p';
    }
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1u] = '\0';
    PEQ(vfs_open(c, "", VFS_O_RDONLY, 0), VFS_EINVAL, "open empty");
    PEQ(vfs_open(c, longpath, VFS_O_RDONLY, 0), VFS_ENAMETOOLONG,
        "open long");
    return PARITY_PASS;
}

static int p_vfs_09(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(vfs_open(c, P_VFS_DIR "/nope9", VFS_O_RDONLY, 0), VFS_ENOENT,
        "open missing");
    return PARITY_PASS;
}

static int p_vfs_10(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int fd;

    PEQ(vfs_open(c, P_VFS_DIR, VFS_O_RDWR, 0), VFS_EISDIR, "dir RDWR");
    fd = vfs_open(c, P_VFS_DIR, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
    PCHK(fd > 0, "dir RDONLY open");
    PEQ(vfs_close(c, fd), VFS_OK, "dir close");
    PCHK(p_write_file(c, P_VFS_DIR "/f10", "x", 1) == VFS_OK, "create f10");
    PEQ(vfs_open(c, P_VFS_DIR "/f10", VFS_O_RDONLY | VFS_O_DIRECTORY, 0),
        VFS_ENOTDIR, "file as dir");
    PEQ(vfs_unlink(c, P_VFS_DIR "/f10"), VFS_OK, "unlink f10");
    return PARITY_PASS;
}

static int p_vfs_11(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PCHK(p_write_file(c, P_VFS_DIR "/f11", "x", 1) == VFS_OK, "create f11");
    PEQ(vfs_open(c, P_VFS_DIR "/f11/x", VFS_O_RDONLY, 0), VFS_ENOTDIR,
        "file as component");
    PEQ(vfs_unlink(c, P_VFS_DIR "/f11"), VFS_OK, "unlink f11");
    return PARITY_PASS;
}

static int p_vfs_12(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(vfs_unlink(c, P_VFS_DIR), VFS_EISDIR, "unlink dir");
    PEQ(vfs_unlink(c, P_VFS_DIR "/nope12"), VFS_ENOENT, "unlink missing");
    return PARITY_PASS;
}

static int p_vfs_13(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(vfs_mkdir(c, P_VFS_DIR, 0755), VFS_EEXIST, "mkdir existing");
    PEQ(vfs_mkdir(c, "/parity/nope13/x", 0755), VFS_ENOENT, "missing parent");
    return PARITY_PASS;
}

static int p_vfs_14(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(vfs_read(c, -1, p_buf, 1), VFS_EBADF, "read(-1)");
    PEQ(vfs_read(c, 0, p_buf, 1), VFS_EBADF, "read(0)");
    PEQ(vfs_write(c, 0, p_buf, 1), VFS_EBADF, "write(0)");
    PEQ(vfs_close(c, 0x7FFF0000), VFS_EBADF, "close(bad gen)");
    return PARITY_PASS;
}

static int p_vfs_15(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int f1;
    int f2;

    PCHK(p_write_file(c, P_VFS_DIR "/f15", "ab", 2) == VFS_OK, "create f15");
    f1 = vfs_open(c, P_VFS_DIR "/f15", VFS_O_RDONLY, 0);
    PCHK(f1 > 0, "open f1");
    PEQ(vfs_close(c, f1), VFS_OK, "close f1");
    f2 = vfs_open(c, P_VFS_DIR "/f15", VFS_O_RDONLY, 0);
    PCHK(f2 > 0, "reopen f2");
    PCHK(vfs_fd_slot_of(f1) == vfs_fd_slot_of(f2), "slot reused");
    PEQ(vfs_read(c, f1, p_buf, 1), VFS_EBADF, "stale fd");
    PEQ(vfs_read(c, f2, p_buf, 1), 1, "fresh fd read");
    PEQ(vfs_close(c, f2), VFS_OK, "close f2");
    PEQ(vfs_unlink(c, P_VFS_DIR "/f15"), VFS_OK, "unlink f15");
    return PARITY_PASS;
}

static int p_vfs_16(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int fds[VFS_FD_MAX];
    u32 i;
    int rc = VFS_OK;

    PEQ(vfs_fd_live(c), 0, "fd table starts empty");
    PCHK(p_write_file(c, P_VFS_DIR "/f16", "x", 1) == VFS_OK, "create f16");
    for (i = 0; i < VFS_FD_MAX; i++) {
        fds[i] = vfs_open(c, P_VFS_DIR "/f16", VFS_O_RDONLY, 0);
        if (fds[i] <= 0) {
            rc = fds[i];
            break;
        }
    }
    PEQ(rc, VFS_OK, "fill fd table");
    if (i == VFS_FD_MAX) {
        PEQ(vfs_open(c, P_VFS_DIR "/f16", VFS_O_RDONLY, 0), VFS_EMFILE,
            "fd table full");
    }
    for (i = 0; i < VFS_FD_MAX; i++) {
        if (fds[i] > 0) {
            (void)vfs_close(c, fds[i]);
        }
    }
    PEQ(vfs_fd_live(c), 0, "fd table drained");
    PEQ(vfs_unlink(c, P_VFS_DIR "/f16"), VFS_OK, "unlink f16");
    return PARITY_PASS;
}

static int p_vfs_17(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    char target[VFS_PATH_MAX];
    int n;

    PCHK(p_write_file(c, P_VFS_DIR "/t17", "A", 1) == VFS_OK, "create t17");
    PEQ(vfs_symlink(c, P_VFS_DIR "/l17", "t17"), VFS_OK, "symlink");
    PEQ(vfs_stat(c, P_VFS_DIR "/l17", &st), VFS_OK, "stat through link");
    PEQ(st.type, VFS_NODE_FILE, "link target type");
    PEQ(st.size, 1, "link target size");
    n = vfs_readlink(c, P_VFS_DIR "/l17", target, (u32)sizeof(target));
    PEQ(n, 3, "readlink len");
    PSTR(target, "t17", "readlink text");
    PEQ(vfs_unlink(c, P_VFS_DIR "/l17"), VFS_OK, "unlink l17");
    PEQ(vfs_unlink(c, P_VFS_DIR "/t17"), VFS_OK, "unlink t17");
    return PARITY_PASS;
}

static int p_vfs_18(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;

    PEQ(vfs_symlink(c, P_VFS_DIR "/la18", "lb18"), VFS_OK, "symlink la");
    PEQ(vfs_symlink(c, P_VFS_DIR "/lb18", "la18"), VFS_OK, "symlink lb");
    PEQ(vfs_stat(c, P_VFS_DIR "/la18", &st), VFS_ELOOP, "loop detected");
    (void)vfs_unlink(c, P_VFS_DIR "/la18");
    (void)vfs_unlink(c, P_VFS_DIR "/lb18");
    return PARITY_PASS;
}

static int p_vfs_19(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int fd;

    fd = vfs_open(c, P_VFS_DIR "/ro19", VFS_O_CREAT | VFS_O_RDONLY, 0444);
    PCHK(fd > 0, "create 0444");
    PEQ(vfs_close(c, fd), VFS_OK, "close ro19 created");
    fd = vfs_open(c, P_VFS_DIR "/wo19", VFS_O_CREAT | VFS_O_WRONLY, 0200);
    PCHK(fd > 0, "create 0200");
    PEQ(vfs_close(c, fd), VFS_OK, "close wo19");
    c->uid = 1000u;
    PEQ(vfs_open(c, P_VFS_DIR "/ro19", VFS_O_WRONLY, 0), VFS_EPERM,
        "write 0444 as user");
    PEQ(vfs_open(c, P_VFS_DIR "/wo19", VFS_O_RDONLY, 0), VFS_EPERM,
        "read 0200 as user");
    fd = vfs_open(c, P_VFS_DIR "/ro19", VFS_O_RDONLY, 0);
    PCHK(fd > 0, "read 0444 as user");
    PEQ(vfs_close(c, fd), VFS_OK, "close ro19");
    fd = vfs_open(c, P_VFS_DIR "/wo19", VFS_O_WRONLY, 0);
    PCHK(fd > 0, "write 0200 as user");
    PEQ(vfs_close(c, fd), VFS_OK, "close wo19 again");
    c->uid = 0u;
    PEQ(vfs_unlink(c, P_VFS_DIR "/ro19"), VFS_OK, "unlink ro19");
    PEQ(vfs_unlink(c, P_VFS_DIR "/wo19"), VFS_OK, "unlink wo19");
    return PARITY_PASS;
}

static int p_vfs_20(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int fd;

    PEQ(vfs_chdir(c, P_VFS_DIR), VFS_OK, "chdir vfs dir");
    fd = vfs_open(c, "f20", VFS_O_CREAT | VFS_O_RDWR, 0644);
    PCHK(fd > 0, "relative create");
    PEQ(vfs_write(c, fd, "z", 1), 1, "relative write");
    PEQ(vfs_close(c, fd), VFS_OK, "close relative");
    PEQ(vfs_chdir(c, ".."), VFS_OK, "chdir ..");
    PSTR(c->cwd, "/parity", "cwd after ..");
    fd = vfs_open(c, P_VFS_DIR "/f20", VFS_O_RDONLY, 0);
    PCHK(fd > 0, "absolute open");
    PEQ(vfs_close(c, fd), VFS_OK, "close absolute");
    PEQ(vfs_chdir(c, "/"), VFS_OK, "chdir root");
    PSTR(c->cwd, "/", "cwd root");
    PEQ(vfs_unlink(c, P_VFS_DIR "/f20"), VFS_OK, "unlink f20");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* ramfs（21..35）                                                           */
/* ------------------------------------------------------------------------- */

static int p_ram_21(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;

    PEQ(p_write_file(c, P_RAM_DIR "/a21", "hello", 5), VFS_OK, "write a21");
    PEQ(vfs_stat(c, P_RAM_DIR "/a21", &st), VFS_OK, "stat a21");
    PEQ(st.type, VFS_NODE_FILE, "type");
    PEQ(st.size, 5, "size");
    PEQ(p_read_file(c, P_RAM_DIR "/a21", p_buf, 8), 5, "read len");
    PMEM(p_buf, "hello", 5, "read data");
    PEQ(vfs_unlink(c, P_RAM_DIR "/a21"), VFS_OK, "unlink");
    PEQ(vfs_stat(c, P_RAM_DIR "/a21", &st), VFS_ENOENT, "gone");
    return PARITY_PASS;
}

static int p_ram_22(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;
    u32 i;

    for (i = 0; i < 5000u; i++) {
        p_buf[i] = (u8)(i * 7u + 3u);
    }
    fd = vfs_open(c, P_RAM_DIR "/b22", VFS_O_CREAT | VFS_O_RDWR, 0644);
    PCHK(fd > 0, "create b22");
    PEQ(vfs_write(c, fd, p_buf, 4096), 4096, "write page1");
    PEQ(vfs_write(c, fd, p_buf + 4096, 904), 904, "write page2");
    PEQ(vfs_close(c, fd), VFS_OK, "close b22");
    PEQ(vfs_stat(c, P_RAM_DIR "/b22", &st), VFS_OK, "stat b22");
    PEQ(st.size, 5000, "size 5000");
    fd = vfs_open(c, P_RAM_DIR "/b22", VFS_O_RDONLY, 0);
    PCHK(fd > 0, "open b22");
    PEQ(vfs_read(c, fd, p_buf2, 4096), 4096, "read page1");
    PEQ(vfs_read(c, fd, p_buf2 + 4096, 904), 904, "read page2");
    PEQ(vfs_close(c, fd), VFS_OK, "close read b22");
    PMEM(p_buf2, p_buf, 5000, "cross-page content");
    PEQ(vfs_unlink(c, P_RAM_DIR "/b22"), VFS_OK, "unlink b22");
    return PARITY_PASS;
}

static int p_ram_23(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;

    PCHK(p_write_file(c, P_RAM_DIR "/c23", "abc", 3) == VFS_OK, "write abc");
    fd = vfs_open(c, P_RAM_DIR "/c23", VFS_O_WRONLY | VFS_O_APPEND, 0);
    PCHK(fd > 0, "open append");
    PEQ(vfs_write(c, fd, "de", 2), 2, "append de");
    PEQ(vfs_close(c, fd), VFS_OK, "close append");
    PEQ(vfs_stat(c, P_RAM_DIR "/c23", &st), VFS_OK, "stat");
    PEQ(st.size, 5, "size 5");
    PEQ(p_read_file(c, P_RAM_DIR "/c23", p_buf, 8), 5, "read 5");
    PMEM(p_buf, "abcde", 5, "append content");
    PEQ(vfs_unlink(c, P_RAM_DIR "/c23"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_24(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;

    PCHK(p_write_file(c, P_RAM_DIR "/d24", "xyz", 3) == VFS_OK, "write xyz");
    fd = vfs_open(c, P_RAM_DIR "/d24", VFS_O_RDWR | VFS_O_TRUNC, 0);
    PCHK(fd > 0, "open trunc");
    PEQ(vfs_stat(c, P_RAM_DIR "/d24", &st), VFS_OK, "stat empty");
    PEQ(st.size, 0, "truncated to 0");
    PEQ(vfs_write(c, fd, "Q", 1), 1, "write Q");
    PEQ(vfs_close(c, fd), VFS_OK, "close");
    PEQ(p_read_file(c, P_RAM_DIR "/d24", p_buf, 4), 1, "read 1");
    PMEM(p_buf, "Q", 1, "trunc content");
    PEQ(vfs_unlink(c, P_RAM_DIR "/d24"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_25(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PCHK(p_write_file(c, P_RAM_DIR "/e25", "x", 1) == VFS_OK, "create e25");
    PEQ(vfs_open(c, P_RAM_DIR "/e25", VFS_O_CREAT | VFS_O_EXCL | VFS_O_RDWR,
                 0644),
        VFS_EEXIST, "O_EXCL existing");
    PEQ(vfs_unlink(c, P_RAM_DIR "/e25"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_26(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;

    PCHK(p_write_file(c, P_RAM_DIR "/f26", p_buf, 0) == VFS_OK, "create f26");
    fd = vfs_open(c, P_RAM_DIR "/f26", VFS_O_WRONLY, 0);
    PCHK(fd > 0, "open f26");
    {
        u32 i;

        for (i = 0; i < 5000u; i++) {
            p_buf[i] = (u8)(i * 3u + 1u);
        }
    }
    PEQ(vfs_write(c, fd, p_buf, 4096), 4096, "write 4096");
    PEQ(vfs_write(c, fd, p_buf + 4096, 904), 904, "write 904");
    PEQ(vfs_close(c, fd), VFS_OK, "close f26");
    PEQ(vfs_truncate(c, P_RAM_DIR "/f26", 100), VFS_OK, "truncate 100");
    PEQ(vfs_stat(c, P_RAM_DIR "/f26", &st), VFS_OK, "stat f26");
    PEQ(st.size, 100, "size 100");
    PEQ(p_read_file(c, P_RAM_DIR "/f26", p_buf2, 200), 100, "read after shrink");
    PMEM(p_buf2, p_buf, 100, "prefix preserved");
    PEQ(vfs_unlink(c, P_RAM_DIR "/f26"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_27(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;
    u32 i;

    for (i = 0; i < 100u; i++) {
        p_buf[i] = 'x';
    }
    PCHK(p_write_file(c, P_RAM_DIR "/g27", p_buf, 100) == VFS_OK, "create g27");
    PEQ(vfs_truncate(c, P_RAM_DIR "/g27", 4097), VFS_OK, "truncate grow");
    PEQ(vfs_stat(c, P_RAM_DIR "/g27", &st), VFS_OK, "stat g27");
    PEQ(st.size, 4097, "size 4097");
    fd = vfs_open(c, P_RAM_DIR "/g27", VFS_O_RDONLY, 0);
    PCHK(fd > 0, "open g27");
    PEQ(vfs_read(c, fd, p_buf2, 4096), 4096, "read page1");
    PEQ(vfs_read(c, fd, p_buf2, 1), 1, "read tail byte");
    PEQ(p_buf2[0], 0, "tail zero");
    PEQ(vfs_close(c, fd), VFS_OK, "close g27");
    PEQ(vfs_unlink(c, P_RAM_DIR "/g27"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_28(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st1;
    struct vfs_stat st2;
    int fd;

    PCHK(p_write_file(c, P_RAM_DIR "/h28", "1", 1) == VFS_OK, "create h28");
    PEQ(vfs_stat(c, P_RAM_DIR "/h28", &st1), VFS_OK, "stat ino1");
    PEQ(vfs_unlink(c, P_RAM_DIR "/h28"), VFS_OK, "unlink h28");
    PEQ(vfs_stat(c, P_RAM_DIR "/h28", &st2), VFS_ENOENT, "hidden");
    fd = vfs_open(c, P_RAM_DIR "/h28", VFS_O_CREAT | VFS_O_RDWR, 0644);
    PCHK(fd > 0, "recreate h28");
    PEQ(vfs_close(c, fd), VFS_OK, "close recreate");
    PEQ(vfs_stat(c, P_RAM_DIR "/h28", &st2), VFS_OK, "stat ino2");
    PCHK(st2.ino != st1.ino, "new ino");
    PEQ(vfs_unlink(c, P_RAM_DIR "/h28"), VFS_OK, "unlink again");
    return PARITY_PASS;
}

static int p_ram_29(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_dirent ents[8];
    int n;
    int i;
    int saw_d2 = 0;
    int saw_x = 0;

    PEQ(p_mkdir_ok(c, P_RAM_DIR "/d29"), VFS_OK, "mkdir d29");
    PEQ(p_mkdir_ok(c, P_RAM_DIR "/d29/sub29"), VFS_OK, "mkdir sub29");
    PCHK(p_write_file(c, P_RAM_DIR "/d29/x29", "hi", 2) == VFS_OK,
         "create x29");
    n = vfs_listdir(c, P_RAM_DIR "/d29", ents, 8);
    PEQ(n, 2, "listdir count");
    for (i = 0; i < n; i++) {
        if (p_streq(ents[i].name, "sub29")) {
            PEQ(ents[i].type, VFS_NODE_DIR, "sub29 type");
            saw_d2 = 1;
        }
        if (p_streq(ents[i].name, "x29")) {
            PEQ(ents[i].type, VFS_NODE_FILE, "x29 type");
            PEQ(ents[i].size, 2, "x29 size");
            saw_x = 1;
        }
    }
    PCHK(saw_d2, "sub29 listed");
    PCHK(saw_x, "x29 listed");
    PEQ(vfs_unlink(c, P_RAM_DIR "/d29/x29"), VFS_OK, "unlink x29");
    return PARITY_PASS;
}

static int p_ram_30(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    char target[VFS_PATH_MAX];

    PCHK(p_write_file(c, P_RAM_DIR "/t30", "Z", 1) == VFS_OK, "create t30");
    PEQ(vfs_symlink(c, P_RAM_DIR "/l30", "t30"), VFS_OK, "symlink l30");
    PEQ(vfs_stat(c, P_RAM_DIR "/l30", &st), VFS_OK, "stat l30");
    PEQ(st.type, VFS_NODE_FILE, "l30 type");
    PEQ(st.size, 1, "l30 size");
    PEQ(vfs_readlink(c, P_RAM_DIR "/l30", target, (u32)sizeof(target)), 3,
        "readlink len");
    PSTR(target, "t30", "readlink text");
    PEQ(vfs_unlink(c, P_RAM_DIR "/l30"), VFS_OK, "unlink l30");
    PEQ(vfs_unlink(c, P_RAM_DIR "/t30"), VFS_OK, "unlink t30");
    return PARITY_PASS;
}

static int p_ram_31(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;

    PCHK(p_write_file(c, P_RAM_DIR "/r31", "abc", 3) == VFS_OK, "create r31");
    PEQ(vfs_rename(c, P_RAM_DIR "/r31", P_RAM_DIR "/r31b"), VFS_OK, "rename");
    PEQ(vfs_stat(c, P_RAM_DIR "/r31", &st), VFS_ENOENT, "old gone");
    PEQ(vfs_stat(c, P_RAM_DIR "/r31b", &st), VFS_OK, "new exists");
    PEQ(st.size, 3, "size kept");
    PEQ(p_read_file(c, P_RAM_DIR "/r31b", p_buf, 4), 3, "read renamed");
    PMEM(p_buf, "abc", 3, "content kept");
    PEQ(vfs_unlink(c, P_RAM_DIR "/r31b"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_32(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;

    PCHK(p_write_file(c, P_RAM_DIR "/ra32", "a", 1) == VFS_OK, "create ra32");
    PCHK(p_write_file(c, P_RAM_DIR "/rb32", "b", 1) == VFS_OK, "create rb32");
    PEQ(vfs_rename(c, P_RAM_DIR "/ra32", P_RAM_DIR "/rb32"), VFS_EEXIST,
        "rename onto existing");
    PEQ(vfs_stat(c, P_RAM_DIR "/ra32", &st), VFS_OK, "ra32 kept");
    PEQ(vfs_stat(c, P_RAM_DIR "/rb32", &st), VFS_OK, "rb32 kept");
    (void)vfs_unlink(c, P_RAM_DIR "/ra32");
    (void)vfs_unlink(c, P_RAM_DIR "/rb32");
    return PARITY_PASS;
}

static int p_ram_33(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;

    PCHK(p_write_file(c, P_RAM_DIR "/o33", "keep", 4) == VFS_OK, "create o33");
    fd = vfs_open(c, P_RAM_DIR "/o33", VFS_O_RDONLY, 0);
    PCHK(fd > 0, "open o33");
    PEQ(vfs_unlink(c, P_RAM_DIR "/o33"), VFS_OK, "unlink while open");
    PEQ(vfs_stat(c, P_RAM_DIR "/o33", &st), VFS_ENOENT, "hidden while open");
    PEQ(vfs_read(c, fd, p_buf, 8), 4, "read after unlink");
    PMEM(p_buf, "keep", 4, "data kept");
    PEQ(vfs_close(c, fd), VFS_OK, "close o33");
    return PARITY_PASS;
}

static int p_ram_34(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;

    PCHK(p_write_file(c, P_RAM_DIR "/big34", "x", 1) == VFS_OK, "create big34");
    PEQ(vfs_truncate(c, P_RAM_DIR "/big34",
                     (u64)RAMFS_MAX_FILE_BYTES + 1ULL),
        VFS_ENOSPC, "over 1 MiB");
    PEQ(vfs_stat(c, P_RAM_DIR "/big34", &st), VFS_OK, "stat big34");
    PEQ(st.size, 1, "size unchanged");
    PEQ(vfs_unlink(c, P_RAM_DIR "/big34"), VFS_OK, "unlink");
    return PARITY_PASS;
}

static int p_ram_35(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct vfs_stat st;
    int fd;

    fd = vfs_open(c, P_RAM_DIR "/z35", VFS_O_CREAT | VFS_O_RDWR, 0644);
    PCHK(fd > 0, "create z35");
    PEQ(vfs_close(c, fd), VFS_OK, "close empty");
    PEQ(vfs_stat(c, P_RAM_DIR "/z35", &st), VFS_OK, "stat empty");
    PEQ(st.size, 0, "size 0");
    PEQ(p_read_file(c, P_RAM_DIR "/z35", p_buf, 1), 0, "read empty EOF");
    PCHK(p_write_file(c, P_RAM_DIR "/z35", "x", 1) == VFS_OK, "write z35");
    PEQ(vfs_stat(c, P_RAM_DIR "/z35", &st), VFS_OK, "stat after write");
    PEQ(st.size, 1, "size 1");
    PEQ(vfs_unlink(c, P_RAM_DIR "/z35"), VFS_OK, "unlink");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* FAT16（36..55）：合成 64 扇区镜像（1 保留 + 2 FAT + 1 根目录 + 30 簇×1KiB） */
/* ------------------------------------------------------------------------- */

#define P_FAT_SECTORS 64u
#define P_FAT_IMAGE_BYTES (P_FAT_SECTORS * 512u)

static u8 p_fat_img[P_FAT_IMAGE_BYTES];

static void p_fat_u16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

static void p_fat_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void p_fat_entry_set(u32 cluster, u32 value)
{
    u32 f;

    for (f = 0; f < 2u; f++) {
        u8 *fat = p_fat_img + (1u + f) * 512u;

        p_fat_u16(fat + cluster * 2u, value);
    }
}

static void p_fat_bpb(u32 total)
{
    u8 *s = p_fat_img;

    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    p_copy(s + 3, "ASTER   ", 8);
    p_fat_u16(s + 11, 512);
    s[13] = 2; /* 2 sectors/cluster = 1024 B */
    p_fat_u16(s + 14, 1);
    s[16] = 2;
    p_fat_u16(s + 17, 16);
    p_fat_u16(s + 19, (u16)total);
    s[21] = 0xF8;
    p_fat_u16(s + 22, 1);
    p_fat_u16(s + 24, 18);
    p_fat_u16(s + 26, 2);
    s[38] = 0x29;
    p_fat_u32(s + 39, 0x4E473037u);
    p_copy(s + 43, "PARITYFAT  ", 11);
    p_copy(s + 54, "FAT16   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void p_fat_reset(void)
{
    p_zero(p_fat_img, P_FAT_IMAGE_BYTES);
    p_fat_bpb(P_FAT_SECTORS);
    p_fat_entry_set(0, 0xFFF8u);
    p_fat_entry_set(1, 0xFFFFu);
}

/* 根目录第 1 项 SYN.TXT（首簇 2、size 字节、FAT[2]=fat2），数据区写模式。 */
static void p_fat_put_syn(u32 fat2, u32 size)
{
    u8 entry[32];
    u32 i;

    p_fat_entry_set(2, fat2);
    p_zero(entry, 32);
    p_copy(entry, "SYN     TXT", 11);
    entry[11] = 0x20;
    p_fat_u16(entry + 26, 2);
    p_fat_u32(entry + 28, size);
    p_copy(p_fat_img + 3u * 512u, entry, 32);
    for (i = 0; i < 512u; i++) {
        p_fat_img[4u * 512u + i] = (u8)(i * 5u + 1u);
    }
}

static int p_fat_blk_read(void *ctx, u64 sector, void *buf, u32 count)
{
    (void)ctx;
    if (buf == 0 || count == 0u || sector + (u64)count > P_FAT_SECTORS) {
        return -1;
    }
    p_copy(buf, p_fat_img + sector * 512u, count * 512u);
    return 0;
}

static int p_fat_blk_write(void *ctx, u64 sector, const void *buf, u32 count)
{
    (void)ctx;
    if (buf == 0 || count == 0u || sector + (u64)count > P_FAT_SECTORS) {
        return -1;
    }
    p_copy(p_fat_img + sector * 512u, buf, count * 512u);
    return 0;
}

static void p_fat_env(struct blk_env *e)
{
    e->read = p_fat_blk_read;
    e->write = p_fat_blk_write;
    e->sectors = P_FAT_SECTORS;
    e->ctx = 0;
}

static int p_fat_boot(struct fat16 *fs)
{
    struct blk_env e;

    p_fat_env(&e);
    fat16_init(fs);
    return fat16_mount(fs, &e);
}

static const struct vfs_fs_ops *p_fat_ops(void)
{
    return fat16_ops();
}

static int p_fat36(void)
{
    struct fat16 fs;
    int rc;

    p_fat_reset();
    rc = p_fat_boot(&fs);
    PEQ(rc, VFS_OK, "mount");
    PEQ(fat16_cluster_count(&fs), 30, "cluster count");
    PEQ(fat16_bytes_per_cluster(&fs), 1024, "cluster bytes");
    PEQ(fat16_total_sectors(&fs), 64, "total sectors");
    PEQ(fat16_free_clusters(&fs), 30, "free clusters");
    return PARITY_PASS;
}

static int p_fat37(void)
{
    struct fat16 fs;

    p_fat_reset();
    p_fat_img[510] = 0;
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "bad signature");
    return PARITY_PASS;
}

static int p_fat38(void)
{
    struct fat16 fs;

    p_fat_reset();
    p_fat_u16(p_fat_img + 11, 1024);
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "bytes/sector");
    return PARITY_PASS;
}

static int p_fat39(void)
{
    struct fat16 fs;

    p_fat_reset();
    p_fat_img[13] = 0;
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "spc 0");
    p_fat_reset();
    p_fat_img[13] = 3;
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "spc non-power-of-two");
    return PARITY_PASS;
}

static int p_fat40(void)
{
    struct fat16 fs;

    p_fat_reset();
    p_fat_img[16] = 0;
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "fats 0");
    p_fat_reset();
    p_fat_img[16] = 5;
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "fats 5");
    return PARITY_PASS;
}

static int p_fat41(void)
{
    struct fat16 fs;

    p_fat_reset();
    p_fat_u16(p_fat_img + 19, 0);
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "total 0");
    p_fat_reset();
    p_fat_u16(p_fat_img + 19, 128);
    PEQ(p_fat_boot(&fs), VFS_EINVAL, "total > device");
    return PARITY_PASS;
}

static int p_fat42(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    u32 i;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PCHK(root != 0, "root");
    PEQ(p_fat_ops()->create(&fs, root, "A.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create A.TXT");
    for (i = 0; i < 700u; i++) {
        p_buf[i] = (u8)(i * 7u + 0x21u);
    }
    PEQ(p_fat_ops()->write(&fs, node, 0, p_buf, 700), 700, "write 700");
    PEQ(node->size, 700, "size 700");
    PEQ(p_fat_ops()->read(&fs, node, 0, p_buf2, 700), 700, "read 700");
    PMEM(p_buf2, p_buf, 700, "content");
    return PARITY_PASS;
}

static int p_fat43(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    u32 free0;
    u32 free1;
    u32 i;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    free0 = fat16_free_clusters(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "B.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create B.TXT");
    for (i = 0; i < 1500u; i++) {
        p_buf[i] = (u8)(i * 11u + 5u);
    }
    PEQ(p_fat_ops()->write(&fs, node, 0, p_buf, 1500), 1500, "write 1500");
    PEQ(node->size, 1500, "size 1500");
    PEQ(p_fat_ops()->read(&fs, node, 0, p_buf2, 1500), 1500, "read 1500");
    PMEM(p_buf2, p_buf, 1500, "content");
    free1 = fat16_free_clusters(&fs);
    PEQ(free0 - free1, 2, "two clusters allocated");
    return PARITY_PASS;
}

static int p_fat44(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    u32 i;
    u32 zero_ok = 1;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "C.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create C.TXT");
    for (i = 0; i < 1500u; i++) {
        p_buf[i] = (u8)(i * 13u + 7u);
    }
    PEQ(p_fat_ops()->write(&fs, node, 0, p_buf, 1500), 1500, "write 1500");
    PEQ(p_fat_ops()->truncate(&fs, node, 100), VFS_OK, "truncate 100");
    PEQ(node->size, 100, "size 100");
    PEQ(p_fat_ops()->read(&fs, node, 0, p_buf2, 200), 100, "read after shrink");
    PEQ(p_fat_ops()->truncate(&fs, node, 1100), VFS_OK, "truncate grow");
    PEQ(node->size, 1100, "size 1100");
    PEQ(p_fat_ops()->read(&fs, node, 100, p_buf2, 1000), 1000, "read grown");
    for (i = 0; i < 1000u; i++) {
        if (p_buf2[i] != 0) {
            zero_ok = 0;
            break;
        }
    }
    PCHK(zero_ok, "grown area zero");
    return PARITY_PASS;
}

static int p_fat45(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    struct vfs_node *out = 0;
    u32 base;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    base = fat16_free_clusters(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "D.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create D.TXT");
    PEQ(p_fat_ops()->write(&fs, node, 0, "0123456789", 10), 10, "write");
    PEQ(fat16_free_clusters(&fs), (int)base - 1, "one cluster used");
    PEQ(p_fat_ops()->unlink(&fs, root, "D.TXT", 5), VFS_OK, "unlink");
    PEQ(p_fat_ops()->lookup(&fs, root, "D.TXT", 5, &out), VFS_ENOENT,
        "lookup after unlink");
    PEQ(fat16_free_clusters(&fs), base, "clusters released");
    return PARITY_PASS;
}

static int p_fat46(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    struct vfs_node *out = 0;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "E.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create E.TXT");
    PEQ(p_fat_ops()->write(&fs, node, 0, "abc", 3), 3, "write abc");
    PEQ(p_fat_ops()->rename(&fs, root, "E.TXT", 5, root, "F.TXT", 5), VFS_OK,
        "rename");
    PEQ(p_fat_ops()->lookup(&fs, root, "E.TXT", 5, &out), VFS_ENOENT,
        "old gone");
    PEQ(p_fat_ops()->lookup(&fs, root, "F.TXT", 5, &out), VFS_OK, "new exists");
    PEQ(p_fat_ops()->read(&fs, out, 0, p_buf, 8), 3, "read renamed");
    PMEM(p_buf, "abc", 3, "content kept");
    return PARITY_PASS;
}

static int p_fat47(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "G.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create G.TXT");
    PEQ(p_fat_ops()->create(&fs, root, "H.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create H.TXT");
    PEQ(p_fat_ops()->rename(&fs, root, "G.TXT", 5, root, "H.TXT", 5), VFS_EEXIST,
        "rename onto existing");
    return PARITY_PASS;
}

static int p_fat48(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *dir = 0;
    struct vfs_node *file = 0;
    struct vfs_stat st;
    char name[FAT16_DISPLAY_MAX];

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "D", 1, VFS_NODE_DIR, 0755, &dir),
        VFS_OK, "create dir");
    PEQ(dir->type, VFS_NODE_DIR, "dir type");
    PEQ(p_fat_ops()->create(&fs, dir, "A.TXT", 5, VFS_NODE_FILE, 0644, &file),
        VFS_OK, "create sub file");
    PEQ(p_fat_ops()->readdir(&fs, dir, 0, name, (u32)sizeof(name), &st), 1,
        "subdir entry");
    PSTR(name, "A.TXT", "subdir name");
    PEQ(st.type, VFS_NODE_FILE, "subdir entry type");
    PEQ(st.size, 0, "subdir entry size");
    PEQ(p_fat_ops()->readdir(&fs, dir, 1, name, (u32)sizeof(name), &st), 0,
        "subdir end");
    PEQ(p_fat_ops()->readdir(&fs, root, 0, name, (u32)sizeof(name), &st), 1,
        "root entry");
    PSTR(name, "D", "root name");
    PEQ(st.type, VFS_NODE_DIR, "root entry type");
    PEQ(p_fat_ops()->readdir(&fs, root, 1, name, (u32)sizeof(name), &st), 0,
        "root end");
    return PARITY_PASS;
}

static int p_fat49(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *dir = 0;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "D", 1, VFS_NODE_DIR, 0755, &dir),
        VFS_OK, "create dir");
    PEQ(p_fat_ops()->unlink(&fs, root, "D", 1), VFS_EISDIR, "unlink dir");
    return PARITY_PASS;
}

static int p_fat50(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "ABCDEFGHI.TXT", 13, VFS_NODE_FILE, 0644,
                            &node),
        VFS_ENAMETOOLONG, "base 9");
    PEQ(p_fat_ops()->create(&fs, root, "A.TEXT", 6, VFS_NODE_FILE, 0644, &node),
        VFS_ENAMETOOLONG, "ext 4");
    PEQ(p_fat_ops()->create(&fs, root, "A:B.TXT", 7, VFS_NODE_FILE, 0644, &node),
        VFS_EINVAL, "bad char");
    PEQ(p_fat_ops()->create(&fs, root, "", 0, VFS_NODE_FILE, 0644, &node),
        VFS_EINVAL, "empty name");
    return PARITY_PASS;
}

static int p_fat_chain(u32 fat2)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;

    p_fat_reset();
    p_fat_put_syn(fat2, 2048);
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->lookup(&fs, root, "SYN.TXT", 7, &node), VFS_OK,
        "lookup SYN.TXT");
    PEQ(p_fat_ops()->read(&fs, node, 0, p_buf, 1024), 1024, "read first cluster");
    PEQ(p_fat_ops()->read(&fs, node, 1024, p_buf, 512), VFS_EIO,
        "read past chain");
    return PARITY_PASS;
}

static int p_fat51(void)
{
    return p_fat_chain(2);
}

static int p_fat52(void)
{
    return p_fat_chain(0xFFF8u);
}

static int p_fat53(void)
{
    return p_fat_chain(0xFFF7u);
}

static int p_fat54(void)
{
    return p_fat_chain(0x7FFFu);
}

static int p_fat55(void)
{
    struct fat16 fs;
    struct vfs_node *root;
    struct vfs_node *node = 0;
    struct vfs_node *out = 0;
    struct vfs_stat st;
    char name[FAT16_DISPLAY_MAX];

    p_fat_reset();
    PEQ(p_fat_boot(&fs), VFS_OK, "mount");
    root = fat16_root(&fs);
    PEQ(p_fat_ops()->create(&fs, root, "A.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_OK, "create A.TXT");
    PEQ(p_fat_ops()->create(&fs, root, "A.TXT", 5, VFS_NODE_FILE, 0644, &node),
        VFS_EEXIST, "duplicate create");
    PEQ(p_fat_ops()->lookup(&fs, root, "NOPE.TXT", 8, &out), VFS_ENOENT,
        "lookup missing");
    PEQ(p_fat_ops()->readdir(&fs, root, 0, name, (u32)sizeof(name), &st), 1,
        "entry 0");
    PSTR(name, "A.TXT", "entry name");
    PEQ(p_fat_ops()->readdir(&fs, root, 1, name, (u32)sizeof(name), &st), 0,
        "readdir end");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* auth（61..70）                                                            */
/* ------------------------------------------------------------------------- */

static int p_auth_61(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct auth_user *u;

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_user_count(), 2, "user count");
    u = auth_user_at(0);
    PCHK(u != 0, "user 0");
    PSTR(u->name, "root", "user 0 name");
    PEQ(u->uid, 0, "root uid");
    u = auth_user_at(1);
    PCHK(u != 0, "user 1");
    PSTR(u->name, "alice", "user 1 name");
    PEQ(u->uid, 1000, "alice uid");
    PCHK(auth_user_at(2) == 0, "user 2 null");
    return PARITY_PASS;
}

static int p_auth_62(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_login("root", "root"), 0, "root login");
    return PARITY_PASS;
}

static int p_auth_63(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_login("alice", "alice"), 1000, "alice login");
    return PARITY_PASS;
}

static int p_auth_64(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct auth_stats *st;

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_login("alice", "wrong"), AUTH_EACCES, "wrong password");
    st = auth_stat();
    PCHK(st != 0 && st->failures >= 1u, "failure counted");
    return PARITY_PASS;
}

static int p_auth_65(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_login("bob", "x"), AUTH_ENOENT, "unknown user");
    return PARITY_PASS;
}

static int p_auth_66(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    PEQ(auth_login("", "x"), AUTH_EINVAL, "empty name");
    PEQ(auth_login("root", ""), AUTH_EINVAL, "empty pass");
    return PARITY_PASS;
}

static int p_auth_67(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int n;

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    n = p_read_file(c, "/etc/passwd", (char *)p_buf, 512);
    PCHK(n > 0, "read /etc/passwd");
    PCHK(p_has((const char *)p_buf, (u32)n, "root:0:"), "root record");
    PCHK(p_has((const char *)p_buf, (u32)n, "alice:1000:"), "alice record");
    PEQ(p_count_byte((const char *)p_buf, (u32)n, '\n'), 2, "two lines");
    return PARITY_PASS;
}

static int p_auth_68(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct auth_user *u;
    u8 digest[AUTH_HASH_BYTES];

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    u = auth_user_at(0);
    PCHK(u != 0, "root user");
    PEQ(u->iterations, (int)AUTH_ITERATIONS, "iterations");
    PEQ(auth_hash_password("root", u->salt, AUTH_ITERATIONS, digest), AUTH_OK,
        "hash");
    PMEM(digest, u->hash, AUTH_HASH_BYTES, "hash matches stored");
    PEQ(auth_hash_password("root", u->salt, AUTH_ITERATIONS, digest), AUTH_OK,
        "hash again");
    PMEM(digest, u->hash, AUTH_HASH_BYTES, "hash deterministic");
    return PARITY_PASS;
}

static int p_auth_69(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct auth_user *u;
    u8 salt[AUTH_SALT_BYTES];
    u8 digest[AUTH_HASH_BYTES];

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    u = auth_user_at(0);
    PCHK(u != 0, "root user");
    p_copy(salt, u->salt, AUTH_SALT_BYTES);
    salt[0] = (u8)(salt[0] ^ 0x5Au);
    PEQ(auth_hash_password("root", salt, AUTH_ITERATIONS, digest), AUTH_OK,
        "hash other salt");
    PCHK(!p_memeq(digest, u->hash, AUTH_HASH_BYTES), "salt changes digest");
    PEQ(auth_hash_password("root", u->salt, 0, digest), AUTH_EINVAL,
        "iterations 0");
    return PARITY_PASS;
}

static int p_auth_70(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct auth_user *u;
    const struct auth_stats *st;
    u32 i;
    int locked_ok = 1;

    PEQ(auth_init(c), AUTH_OK, "auth_init");
    for (i = 0; i < AUTH_MAX_FAILS; i++) {
        if (auth_login("alice", "bad") != AUTH_EACCES) {
            locked_ok = 0;
            break;
        }
    }
    PCHK(locked_ok, "failed attempts rejected");
    PEQ(auth_login("alice", "alice"), AUTH_EACCES, "locked rejects correct pass");
    u = auth_user_at(1);
    PCHK(u != 0 && u->locked == 1u, "user locked");
    st = auth_stat();
    PCHK(st != 0 && st->locks == 1u, "lock counted");
    /* 重新 init 必须解锁（ring3 demo 还要登录 alice；也是回归点）。 */
    PEQ(auth_init(c), AUTH_OK, "re-init");
    PEQ(auth_login("alice", "alice"), 1000, "unlocked after re-init");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* log（71..75）                                                             */
/* ------------------------------------------------------------------------- */

static int p_log_71(void)
{
    char buf[64];
    int n;

    PEQ(log_init(), 0, "log_init");
    PEQ(log_write("t", "m"), 5, "write t: m");
    PEQ(log_write("w", "abc"), 7, "write w: abc");
    PEQ(log_line_count(), 2, "line count");
    PEQ(log_bytes_used(), 12, "bytes used");
    n = (int)log_read(buf, (u32)sizeof(buf));
    PEQ(n, 12, "read len");
    PMEM(buf, "t: m\nw: abc\n", 12, "read content");
    PEQ(log_capacity(), (int)LOG_CAPACITY, "capacity");
    return PARITY_PASS;
}

static int p_log_72(void)
{
    char tag15[16];
    char tag16[17];
    char msg127[128];
    char msg128[129];
    char bad[8];
    u32 i;

    for (i = 0; i < 15u; i++) {
        tag15[i] = 'a';
    }
    tag15[15] = '\0';
    for (i = 0; i < 16u; i++) {
        tag16[i] = 'b';
    }
    tag16[16] = '\0';
    for (i = 0; i < 127u; i++) {
        msg127[i] = 'c';
    }
    msg127[127] = '\0';
    for (i = 0; i < 128u; i++) {
        msg128[i] = 'd';
    }
    msg128[128] = '\0';
    p_zero(bad, (u32)sizeof(bad));
    bad[0] = 'a';
    bad[1] = 0x01;

    PEQ(log_init(), 0, "log_init");
    PEQ(log_write("", "x"), LOG_EINVAL, "empty tag");
    PEQ(log_write(tag16, "x"), LOG_EINVAL, "tag 16");
    PEQ(log_write("a:b", "x"), LOG_EINVAL, "tag colon");
    PEQ(log_write("t", ""), LOG_EINVAL, "empty msg");
    PEQ(log_write("t", msg128), LOG_EINVAL, "msg 128");
    PEQ(log_write("t", bad), LOG_EINVAL, "msg control char");
    PCHK(log_write(tag15, msg127) > 0, "boundary tag/msg accepted");
    PCHK(log_line_count() >= 1u, "boundary line stored");
    return PARITY_PASS;
}

static int p_log_73(void)
{
    char buf[64];
    int n;

    PEQ(log_init(), 0, "log_init");
    PEQ(log_write("t", "abcdef"), 10, "write line");
    n = (int)log_read(buf, 4);
    PEQ(n, 4, "read max 4");
    PMEM(buf, "def\n", 4, "tail bytes");
    n = (int)log_read(buf, 10);
    PEQ(n, 10, "read whole line");
    PMEM(buf, "t: abcdef\n", 10, "whole line");
    return PARITY_PASS;
}

static int p_log_74(void)
{
    char msg[121];
    char buf[128];
    u32 i;
    int n;

    for (i = 0; i < 120u; i++) {
        msg[i] = 'x';
    }
    msg[120] = '\0';
    PEQ(log_init(), 0, "log_init");
    for (i = 0; i < 600u; i++) {
        if (log_write("p", msg) <= 0) {
            p_why_add("log_write failed at ");
            p_why_int((i64)i);
            return PARITY_FAIL;
        }
    }
    PCHK(log_dropped_lines() > 0u, "oldest lines dropped");
    PCHK(log_bytes_used() <= LOG_CAPACITY, "within capacity");
    n = (int)log_read(buf, (u32)sizeof(buf));
    PEQ(n, 124, "latest line len");
    PEQ(buf[123], '\n', "ends with newline");
    return PARITY_PASS;
}

static int p_log_75(void)
{
    char buf[16];

    PEQ(log_init(), 0, "log_init");
    PEQ(log_line_count(), 0, "empty count");
    PEQ((int)log_read(buf, (u32)sizeof(buf)), 0, "empty read");
    PEQ(log_write("z", "q"), 5, "write one");
    PEQ(log_line_count(), 1, "one line");
    PEQ(log_init(), 0, "reset");
    PEQ(log_line_count(), 0, "reset count");
    PEQ((int)log_bytes_used(), 0, "reset bytes");
    PEQ((int)log_read(buf, (u32)sizeof(buf)), 0, "reset read");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* pkg（76..85）                                                             */
/* ------------------------------------------------------------------------- */

static int p_pkg_76(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    const struct pkg_stats *st;

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    PEQ(pkg_seed_count(), 1, "seed count");
    st = pkg_stat();
    PCHK(st != 0 && st->packages == 1u, "package stat");
    return PARITY_PASS;
}

static int p_pkg_77(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    char buf[256];
    int n;

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    n = pkg_list(buf, (u32)sizeof(buf));
    PEQ(n, 1, "list count");
    PSTR(buf, "hello 1.0.0\n", "list text");
    PEQ(pkg_list(buf, 4), PKG_ENOSPC, "small buffer");
    return PARITY_PASS;
}

static int p_pkg_78(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    PEQ(pkg_verify("hello"), PKG_OK, "verify hello");
    return PARITY_PASS;
}

static int p_pkg_79(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    PEQ(pkg_verify("nope"), PKG_ENOENT, "verify unknown");
    return PARITY_PASS;
}

static const char p_manifest_ok[] =
    "name=hello\n"
    "version=1.0.0\n"
    "type=app\n"
    "entry=bin/hello\n"
    "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";

static int p_pkg_80(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    struct pkg_manifest m;

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    PEQ(pkg_manifest_parse(p_manifest_ok, (u32)(sizeof(p_manifest_ok) - 1u), &m),
        PKG_OK, "parse valid");
    PEQ(m.fields, (int)PKG_F_ALL, "all fields");
    PSTR(m.name, "hello", "name");
    PSTR(m.version, "1.0.0", "version");
    PSTR(m.type, "app", "type");
    PSTR(m.entry, "bin/hello", "entry");
    return PARITY_PASS;
}

static int p_pkg_81(void)
{
    static const char text[] = "name=hello\nversion=1.0.0\ntype=app\n"
                               "entry=bin/hello\n";
    struct pkg_manifest m;

    PEQ(pkg_manifest_parse(text, (u32)(sizeof(text) - 1u), &m), PKG_EINVAL,
        "missing sha256");
    return PARITY_PASS;
}

static int p_pkg_82(void)
{
    static const char text[] =
        "name=hello\nversion=1.0.0\ntype=app\nentry=bin/hello\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "license=MIT\n";
    struct pkg_manifest m;

    PEQ(pkg_manifest_parse(text, (u32)(sizeof(text) - 1u), &m), PKG_EINVAL,
        "unknown key");
    return PARITY_PASS;
}

static int p_pkg_83(void)
{
    static const char text[] =
        "name=hello\nname=again\nversion=1.0.0\ntype=app\nentry=bin/hello\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    struct pkg_manifest m;

    PEQ(pkg_manifest_parse(text, (u32)(sizeof(text) - 1u), &m), PKG_EINVAL,
        "duplicate key");
    return PARITY_PASS;
}

static int p_pkg_84(void)
{
    static const char short_hex[] =
        "name=hello\nversion=1.0.0\ntype=app\nentry=bin/hello\n"
        "sha256=0123456789abcdef\n";
    static const char bad_hex[] =
        "name=hello\nversion=1.0.0\ntype=app\nentry=bin/hello\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\n";
    struct pkg_manifest m;

    PEQ(pkg_manifest_parse(short_hex, (u32)(sizeof(short_hex) - 1u), &m),
        PKG_EINVAL, "short sha256");
    PEQ(pkg_manifest_parse(bad_hex, (u32)(sizeof(bad_hex) - 1u), &m),
        PKG_EINVAL, "non-hex sha256");
    return PARITY_PASS;
}

static int p_pkg_85(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    static const char entry_path[] =
        "/System/Packages/hello/1.0.0/bin/hello";
    static const char seed_file[] = "hello from ng pkg\n";

    PEQ(pkg_init(c), PKG_OK, "pkg_init");
    PEQ(p_write_file(c, entry_path, "X", 1), VFS_OK, "tamper entry");
    PEQ(pkg_verify("hello"), PKG_EIO, "tamper detected");
    PEQ(p_write_file(c, entry_path, seed_file, (u32)(sizeof(seed_file) - 1u)),
        VFS_OK, "restore entry");
    PEQ(pkg_verify("hello"), PKG_OK, "verify after restore");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* shell（86..95）：src/apps/shell/shell.c 的 tokenizer / cwd 解析            */
/* ------------------------------------------------------------------------- */

static int p_shell_86(void)
{
    char line[] = "echo hi";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line, &argc, argv, &redir), 0, "tokenize rc");
    PEQ(argc, 2, "argc");
    PSTR(argv[0], "echo", "argv0");
    PSTR(argv[1], "hi", "argv1");
    PCHK(redir == 0, "no redir");
    return PARITY_PASS;
}

static int p_shell_87(void)
{
    char line[] = "echo hi > /tmp/s.txt";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line, &argc, argv, &redir), 0, "tokenize rc");
    PEQ(argc, 2, "argc");
    PCHK(redir != 0, "redir present");
    PSTR(redir, "/tmp/s.txt", "redir path");
    return PARITY_PASS;
}

static int p_shell_88(void)
{
    char line[] = "echo hi >/tmp/s.txt";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line, &argc, argv, &redir), 0, "tokenize rc");
    PEQ(argc, 2, "argc");
    PCHK(redir != 0, "redir present");
    PSTR(redir, "/tmp/s.txt", "redir path");
    return PARITY_PASS;
}

static int p_shell_89(void)
{
    char line[] = "  ls   /tmp  ";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line, &argc, argv, &redir), 0, "tokenize rc");
    PEQ(argc, 2, "argc");
    PSTR(argv[0], "ls", "argv0");
    PSTR(argv[1], "/tmp", "argv1");
    PCHK(redir == 0, "no redir");
    return PARITY_PASS;
}

static int p_shell_90(void)
{
    char line[] = "help";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line, &argc, argv, &redir), 0, "tokenize rc");
    PEQ(argc, 1, "argc");
    PSTR(argv[0], "help", "argv0");
    return PARITY_PASS;
}

static int p_shell_91(void)
{
    char line1[] = ">> x";
    char line2[] = "echo a > b > c";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line1, &argc, argv, &redir), -4, "leading >>");
    PEQ(sh_test_tokenize(line2, &argc, argv, &redir), -4, "double redirect");
    return PARITY_PASS;
}

static int p_shell_92(void)
{
    char line1[] = "echo hi >";
    char line2[] = ">";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line1, &argc, argv, &redir), -3, "missing redir path");
    PEQ(sh_test_tokenize(line2, &argc, argv, &redir), -3, "only redir");
    return PARITY_PASS;
}

static int p_shell_93(void)
{
    char line1[] = "";
    char line2[] = "   ";
    char line3[] = "a b c d e f g h i";
    char *argv[8];
    char *redir;
    int argc;

    PEQ(sh_test_tokenize(line1, &argc, argv, &redir), -2, "empty line");
    PEQ(sh_test_tokenize(line2, &argc, argv, &redir), -2, "blank line");
    PEQ(sh_test_tokenize(line3, &argc, argv, &redir), -1, "too many args");
    return PARITY_PASS;
}

static int p_shell_94(void)
{
    char out[VFS_PATH_MAX];

    PEQ(sh_test_resolve("/a/b", "../c", out, (u32)sizeof(out)), 0, "resolve 1");
    PSTR(out, "/a/c", "resolve 1 out");
    PEQ(sh_test_resolve("/", "x", out, (u32)sizeof(out)), 0, "resolve 2");
    PSTR(out, "/x", "resolve 2 out");
    PEQ(sh_test_resolve("/a/b", "c/d", out, (u32)sizeof(out)), 0, "resolve 3");
    PSTR(out, "/a/b/c/d", "resolve 3 out");
    return PARITY_PASS;
}

static int p_shell_95(void)
{
    char out[VFS_PATH_MAX];

    PEQ(sh_test_resolve("/", "", out, (u32)sizeof(out)), P_SHELL_EINVAL,
        "empty input");
    PEQ(sh_test_resolve("/", "..", out, (u32)sizeof(out)), 0, ".. at root");
    PSTR(out, "/", ".. at root out");
    PEQ(sh_test_resolve("/", "//a///b/", out, (u32)sizeof(out)), 0,
        "collapse slashes");
    PSTR(out, "/a/b", "collapse out");
    return PARITY_PASS;
}

/* ------------------------------------------------------------------------- */
/* 分发                                                                      */
/* ------------------------------------------------------------------------- */

static int p_shared_case(int id)
{
    switch (id) {
    case 1: return p_vfs_01();
    case 2: return p_vfs_02();
    case 3: return p_vfs_03();
    case 4: return p_vfs_04();
    case 5: return p_vfs_05();
    case 6: return p_vfs_06();
    case 7: return p_vfs_07();
    case 8: return p_vfs_08();
    case 9: return p_vfs_09();
    case 10: return p_vfs_10();
    case 11: return p_vfs_11();
    case 12: return p_vfs_12();
    case 13: return p_vfs_13();
    case 14: return p_vfs_14();
    case 15: return p_vfs_15();
    case 16: return p_vfs_16();
    case 17: return p_vfs_17();
    case 18: return p_vfs_18();
    case 19: return p_vfs_19();
    case 20: return p_vfs_20();
    case 21: return p_ram_21();
    case 22: return p_ram_22();
    case 23: return p_ram_23();
    case 24: return p_ram_24();
    case 25: return p_ram_25();
    case 26: return p_ram_26();
    case 27: return p_ram_27();
    case 28: return p_ram_28();
    case 29: return p_ram_29();
    case 30: return p_ram_30();
    case 31: return p_ram_31();
    case 32: return p_ram_32();
    case 33: return p_ram_33();
    case 34: return p_ram_34();
    case 35: return p_ram_35();
    case 36: return p_fat36();
    case 37: return p_fat37();
    case 38: return p_fat38();
    case 39: return p_fat39();
    case 40: return p_fat40();
    case 41: return p_fat41();
    case 42: return p_fat42();
    case 43: return p_fat43();
    case 44: return p_fat44();
    case 45: return p_fat45();
    case 46: return p_fat46();
    case 47: return p_fat47();
    case 48: return p_fat48();
    case 49: return p_fat49();
    case 50: return p_fat50();
    case 51: return p_fat51();
    case 52: return p_fat52();
    case 53: return p_fat53();
    case 54: return p_fat54();
    case 55: return p_fat55();
    case 61: return p_auth_61();
    case 62: return p_auth_62();
    case 63: return p_auth_63();
    case 64: return p_auth_64();
    case 65: return p_auth_65();
    case 66: return p_auth_66();
    case 67: return p_auth_67();
    case 68: return p_auth_68();
    case 69: return p_auth_69();
    case 70: return p_auth_70();
    case 71: return p_log_71();
    case 72: return p_log_72();
    case 73: return p_log_73();
    case 74: return p_log_74();
    case 75: return p_log_75();
    case 76: return p_pkg_76();
    case 77: return p_pkg_77();
    case 78: return p_pkg_78();
    case 79: return p_pkg_79();
    case 80: return p_pkg_80();
    case 81: return p_pkg_81();
    case 82: return p_pkg_82();
    case 83: return p_pkg_83();
    case 84: return p_pkg_84();
    case 85: return p_pkg_85();
    case 86: return p_shell_86();
    case 87: return p_shell_87();
    case 88: return p_shell_88();
    case 89: return p_shell_89();
    case 90: return p_shell_90();
    case 91: return p_shell_91();
    case 92: return p_shell_92();
    case 93: return p_shell_93();
    case 94: return p_shell_94();
    case 95: return p_shell_95();
    default:
        p_why_add("no shared body for id ");
        p_why_int((i64)id);
        return PARITY_FAIL;
    }
}

/* harness 调这个跑一项；disp/ata 由各自实现接管。 */
static int parity_run_one(int id)
{
    p_why_reset();
    if (id >= 56 && id <= 60) {
        return parity_ata_case(id - 56);
    }
#if !defined(NG_PARITY_HOST)
    if (id >= PARITY_DISP_FIRST && id <= PARITY_TOTAL) {
        return parity_disp_case(id - PARITY_DISP_FIRST);
    }
#endif
    return p_shared_case(id);
}

/* harness 读失败原因（没有失败时是空串）。 */
static const char *parity_why(void)
{
    return p_why;
}
