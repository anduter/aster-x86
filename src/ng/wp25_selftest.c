/*
 * ng WP2-5: auth/log/pkg 内核自检。
 *
 * 证据行（run-ng 必须命中）：
 *   ng: auth users ok (2)
 *   ng: auth login root ok (uid=0)
 *   ng: auth login alice ok (uid=1000)
 *   ng: auth bad password rejected
 *   ng: log ring ok (bytes=...)
 *   ng: dmesg syscall ok
 *   ng: pkg list ok (n=1)
 *   ng: pkg verify ok
 *   ng: WP2-5 selftest PASS              （main.c）
 *
 * 31..35 的成功路径怎么测：把一页 PMM 以 U=1 临时映射到固定 VA，把用户
 * 字符串写到它的物理（恒等）地址，再把这些 VA 交给 syscall 胶水。这样
 * copyin/copyout 走的是**真实的 uaccess 页校验**（不是绕过），测试后
 * unmap + 归还。ring3 下的 31..35 由 hello.S 的 svc demo 再走一遍。
 */
#include "wp25.h"

#include "auth.h"
#include "fs_syscall.h"
#include "log.h"
#include "paging.h"
#include "pkg.h"
#include "pmm.h"
#include "serial.h"
#include "svc_syscall.h"
#include "types.h"
#include "vfs.h"

#define SVC_TEST_VA 0x60000000ULL

/* 用户页内的字符串/缓冲布局。 */
#define U_ROOT 0u
#define U_ROOT_PW 16u
#define U_ALICE 32u
#define U_ALICE_PW 48u
#define U_WRONG_PW 64u
#define U_TAG 80u
#define U_MSG 96u
#define U_HELLO 112u
#define U_NOPE 128u
#define U_BUF 256u
#define U_BUF_MAX 256u

static int g_fails;
static u64 g_phys;
static u8 *g_kbuf; /* 物理别名：SMAP 下 ring0 也能安全写用户页内容 */
static int g_mapped;

static void fail(const char *area, const char *what)
{
    g_fails++;
    serial_puts("ng: ");
    serial_puts(area);
    serial_puts(" FAIL (");
    serial_puts(what);
    serial_puts(")\r\n");
}

static int contains(const char *hay, const char *needle)
{
    u32 i;
    u32 j;

    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0'; j++) {
            if (hay[i + j] != needle[j]) {
                break;
            }
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void put_str(u32 off, const char *s)
{
    u32 i;

    for (i = 0; s[i] != '\0' && off + i < 4096u; i++) {
        g_kbuf[off + i] = (u8)s[i];
    }
    if (off + i < 4096u) {
        g_kbuf[off + i] = 0;
    }
}

static int map_user_scratch(void)
{
    g_phys = pmm_alloc_page();
    if (g_phys == 0) {
        return 0;
    }
    if (pg_map(SVC_TEST_VA, g_phys, PG_USER | PG_WRITE | PG_NX) != PG_OK) {
        pmm_free_page(g_phys);
        g_phys = 0;
        return 0;
    }
    g_kbuf = (u8 *)(usize)g_phys;
    {
        u32 i;

        for (i = 0; i < 4096u; i++) {
            g_kbuf[i] = 0;
        }
    }
    put_str(U_ROOT, "root");
    put_str(U_ROOT_PW, "root");
    put_str(U_ALICE, "alice");
    put_str(U_ALICE_PW, "alice");
    put_str(U_WRONG_PW, "nope");
    put_str(U_TAG, "ring");
    put_str(U_MSG, "from syscall log");
    put_str(U_HELLO, "hello");
    put_str(U_NOPE, "nope");
    g_mapped = 1;
    return 1;
}

static void unmap_user_scratch(void)
{
    if (g_mapped) {
        (void)pg_unmap(SVC_TEST_VA);
        g_mapped = 0;
    }
    if (g_phys != 0) {
        (void)pmm_free_page(g_phys);
        g_phys = 0;
    }
    g_kbuf = NULL;
}

/* ------------------------------------------------------------------------- */
/* 1. auth：种子 + 口令 + 锁定参数（L1 直接调用）                             */
/* ------------------------------------------------------------------------- */

static int check_auth(struct vfs_ctx *c)
{
    struct vfs_stat st;
    int fails = 0;
    int rc;

    rc = auth_init(c);
    if (rc != AUTH_OK) {
        fail("auth", "init");
        return 1;
    }
    if (auth_user_count() != 2) {
        fail("auth", "users");
        fails++;
    } else if (vfs_stat(c, "/etc/passwd", &st) != VFS_OK || st.size == 0) {
        fail("auth", "passwd file");
        fails++;
    } else {
        serial_puts("ng: auth users ok (2)\r\n");
    }

    if (auth_login("root", "root") != 0) {
        fail("auth", "login root");
        fails++;
    } else {
        serial_puts("ng: auth login root ok (uid=0)\r\n");
    }

    if (auth_login("alice", "alice") != 1000) {
        fail("auth", "login alice");
        fails++;
    } else {
        serial_puts("ng: auth login alice ok (uid=1000)\r\n");
    }

    if (auth_login("root", "wrong") != AUTH_EACCES ||
        auth_login("nobody", "alice") != AUTH_ENOENT ||
        auth_login(NULL, "x") != AUTH_EINVAL ||
        auth_login("root", NULL) != AUTH_EINVAL) {
        fail("auth", "bad password");
        fails++;
    } else {
        serial_puts("ng: auth bad password rejected\r\n");
    }

    return fails;
}

/* ------------------------------------------------------------------------- */
/* 2. log 环                                                                 */
/* ------------------------------------------------------------------------- */

static int check_log(void)
{
    char buf[512];
    u32 n;
    int fails = 0;

    if (log_write("wp25", "log ring test") <= 0) {
        fail("log", "write");
        return 1;
    }
    n = log_read(buf, (u32)sizeof(buf));
    if (n == 0 || !contains(buf, "wp25: log ring test")) {
        fail("log", "read");
        fails++;
    }
    if (log_write(NULL, "x") != LOG_EINVAL ||
        log_write("0123456789abcdef", "x") != LOG_EINVAL ||
        log_write("t", "") != LOG_EINVAL ||
        log_write("t", "bad\nline") != LOG_EINVAL) {
        fail("log", "validation");
        fails++;
    }
    if (fails == 0) {
        serial_puts("ng: log ring ok (bytes=");
        serial_putdec((u64)log_bytes_used());
        serial_puts(")\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 3. syscall 31/32/33：真实用户指针                                         */
/* ------------------------------------------------------------------------- */

static int check_svc_syscalls(void)
{
    char lbuf[SVC_LOG_SYS_READ_MAX];
    int fails = 0;
    int ok_auth = 1;
    int ok_log = 1;
    int rc;

    rc = ng_sys_login(SVC_TEST_VA + U_ROOT, SVC_TEST_VA + U_ROOT_PW);
    if (rc != 0) {
        ok_auth = 0;
    }
    rc = ng_sys_login(SVC_TEST_VA + U_ALICE, SVC_TEST_VA + U_ALICE_PW);
    if (rc != 1000) {
        ok_auth = 0;
    }
    if (ng_sys_login(SVC_TEST_VA + U_ROOT, SVC_TEST_VA + U_WRONG_PW) !=
        AUTH_EACCES) {
        ok_auth = 0;
    }
    if (ng_sys_login(0x1000ULL, SVC_TEST_VA + U_ROOT_PW) != VFS_EFAULT) {
        ok_auth = 0;
    }
    if (ng_sys_login(SVC_TEST_VA + U_ROOT, 0x1000ULL) != VFS_EFAULT) {
        ok_auth = 0;
    }

    if (ng_sys_log_write(SVC_TEST_VA + U_TAG, SVC_TEST_VA + U_MSG) != 0) {
        ok_log = 0;
    }
    rc = ng_sys_dmesg(SVC_TEST_VA + U_BUF, U_BUF_MAX);
    if (rc <= 0) {
        ok_log = 0;
    } else {
        lbuf[0] = 0;
        {
            u32 i;

            for (i = 0; i < U_BUF_MAX && g_kbuf[U_BUF + i] != 0; i++) {
                lbuf[i] = (char)g_kbuf[U_BUF + i];
            }
            lbuf[i] = 0;
        }
        if (!contains(lbuf, "ring: from syscall log")) {
            ok_log = 0;
        }
    }
    if (ng_sys_dmesg(0x1000ULL, 64) != VFS_EFAULT ||
        ng_sys_dmesg(SVC_TEST_VA + U_BUF, 0) != VFS_EINVAL) {
        ok_log = 0;
    }
    if (ng_sys_log_write(0x1000ULL, SVC_TEST_VA + U_MSG) != VFS_EFAULT) {
        ok_log = 0;
    }

    if (!ok_auth) {
        fail("auth", "syscall login");
        fails++;
    }
    if (!ok_log) {
        fail("log", "dmesg syscall");
        fails++;
    } else {
        serial_puts("ng: dmesg syscall ok\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 4. pkg：种子/列表/校验（经过 34/35）                                       */
/* ------------------------------------------------------------------------- */

static int check_pkg(void)
{
    char lbuf[U_BUF_MAX];
    int n;
    int fails = 0;

    if (pkg_init(fs_sys_ctx()) != PKG_OK || pkg_seed_count() != 1) {
        fail("pkg", "init");
        return 1;
    }

    n = ng_sys_pkg_list(SVC_TEST_VA + U_BUF, U_BUF_MAX);
    if (n != 1) {
        fail("pkg", "list");
        fails++;
    } else {
        u32 i;

        for (i = 0; i < U_BUF_MAX - 1u && g_kbuf[U_BUF + i] != 0; i++) {
            lbuf[i] = (char)g_kbuf[U_BUF + i];
        }
        lbuf[i] = 0;
        if (!contains(lbuf, "hello 1.0.0")) {
            fail("pkg", "list content");
            fails++;
        } else {
            serial_puts("ng: pkg list ok (n=1)\r\n");
        }
    }

    if (ng_sys_pkg_verify(SVC_TEST_VA + U_HELLO) != 0) {
        fail("pkg", "verify");
        fails++;
    } else if (ng_sys_pkg_verify(SVC_TEST_VA + U_NOPE) != PKG_ENOENT) {
        fail("pkg", "verify missing");
        fails++;
    } else {
        serial_puts("ng: pkg verify ok\r\n");
    }

    return fails;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                      */
/* ------------------------------------------------------------------------- */

int wp25_selftest(void)
{
    struct vfs_ctx *c = fs_sys_ctx();

    g_fails = 0;
    g_phys = 0;
    g_kbuf = NULL;
    g_mapped = 0;

    g_fails += check_auth(c);
    g_fails += check_log();

    if (!map_user_scratch()) {
        fail("log", "user page map");
        return g_fails + 1;
    }
    g_fails += check_svc_syscalls();
    g_fails += check_pkg();
    unmap_user_scratch();

    return g_fails;
}
