/*
 * ng WP2-5 host 单测：pkg 清单解析 + 包目录/列表/SHA-256 校验。
 *
 * 覆盖：
 *   - pkg_init 种入 hello/1.0.0（manifest + bin/hello）；
 *   - pkg_list 的文本/计数/缓冲边界；
 *   - pkg_verify 的成功、ENOENT、字段不符、entry 非法、哈希不符；
 *   - manifest 纯解析：合法/CRLF/空格/注释、缺字段、重复、未知字段、
 *     非法 name/version/type/entry/sha256、超长/空文本。
 */
#include <stdio.h>
#include <string.h>

#include "fake_ng_env.h"
#include "pkg.h"
#include "ramfs.h"
#include "sha256.h"
#include "test_util.h"
#include "vfs.h"

#define SEED_FILE "hello from ng pkg\n"
#define SEED_DIR "/System/Packages/hello/1.0.0"
#define SEED_MANIFEST SEED_DIR "/manifest"
#define SEED_ENTRY SEED_DIR "/bin/hello"

static struct ramfs g_fs;
static struct fs_env g_env;
static struct vfs_ctx g_ctx;

static void fs_setup(void)
{
    vfs_reset();
    fake_fs_reset();
    fake_fs_env(&g_env);
    ramfs_init(&g_fs, &g_env);
    CHECK(vfs_mount("/", ramfs_ops(), &g_fs, ramfs_root(&g_fs)) == VFS_OK);
    vfs_ctx_init(&g_ctx);
}

static int write_path(const char *path, const char *data, u32 len)
{
    int fd = vfs_open(&g_ctx, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC,
                      0644);
    int rc;

    if (fd <= 0) {
        return 0;
    }
    rc = vfs_write(&g_ctx, fd, data, (u64)len);
    (void)vfs_close(&g_ctx, fd);
    return rc == (int)len;
}

static int read_path(const char *path, char *buf, u32 max)
{
    int fd = vfs_open(&g_ctx, path, VFS_O_RDONLY, 0);
    int n;

    if (fd <= 0) {
        return -1;
    }
    n = vfs_read(&g_ctx, fd, buf, (u64)max);
    (void)vfs_close(&g_ctx, fd);
    return n;
}

static void make_manifest(char *out, size_t cap, const char *name,
                          const char *version, const char *type,
                          const char *entry, const char *sha)
{
    (void)snprintf(out, cap,
                   "name=%s\nversion=%s\ntype=%s\nentry=%s\nsha256=%s\n", name,
                   version, type, entry, sha);
}

static const char *sha_abc(void)
{
    static char hex[SHA256_HEX_MAX];
    u8 d[SHA256_DIGEST_BYTES];

    sha256("abc", 3, d);
    sha256_hex(d, hex);
    return hex;
}

/* ------------------------------------------------------------------------- */
/* 1. 种子包 + 列表 + 校验                                                    */
/* ------------------------------------------------------------------------- */

static void t_pkg_seed(void)
{
    struct pkg_manifest man;
    char buf[128];
    char hex[SHA256_HEX_MAX];
    char text[PKG_MANIFEST_MAX];
    u8 d[SHA256_DIGEST_BYTES];
    int n;

    fs_setup();
    CHECK(pkg_init(&g_ctx) == PKG_OK);
    CHECK(pkg_seed_count() == 1);
    CHECK(pkg_stat()->packages == 1);
    CHECK(pkg_stat()->seeds == 1);

    /* pkg_list 文本：13 字节含 NUL，12 字节数据。 */
    memset(buf, 0, sizeof(buf));
    n = pkg_list(buf, 13);
    CHECK(n == 1);
    CHECK(strcmp(buf, "hello 1.0.0\n") == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(pkg_list(buf, 12) == PKG_ENOSPC);
    CHECK(pkg_list(buf, 13) == 1);
    CHECK(pkg_list(NULL, 10) == PKG_EINVAL);
    CHECK(pkg_list(buf, 0) == PKG_EINVAL);

    CHECK(pkg_verify("hello") == PKG_OK);
    CHECK(pkg_stat()->verifies == 1);
    CHECK(pkg_verify("nope") == PKG_ENOENT);
    CHECK(pkg_stat()->failures == 1);
    CHECK(pkg_verify("../etc") == PKG_EINVAL);
    CHECK(pkg_verify("") == PKG_EINVAL);
    CHECK(pkg_verify(NULL) == PKG_EINVAL);

    /* manifest 字段与文件内容哈希一致。 */
    memset(text, 0, sizeof(text));
    n = read_path(SEED_MANIFEST, text, sizeof(text) - 1u);
    CHECK(n > 0);
    CHECK(pkg_manifest_parse(text, (u32)n, &man) == PKG_OK);
    CHECK(man.fields == PKG_F_ALL);
    CHECK(strcmp(man.name, "hello") == 0);
    CHECK(strcmp(man.version, "1.0.0") == 0);
    CHECK(strcmp(man.type, "app") == 0);
    CHECK(strcmp(man.entry, "bin/hello") == 0);

    sha256(SEED_FILE, (u64)(sizeof(SEED_FILE) - 1u), d);
    sha256_hex(d, hex);
    CHECK(memcmp(hex,
                 text + strlen("name=hello\nversion=1.0.0\ntype=app\n"
                               "entry=bin/hello\nsha256="),
                 64) == 0);
    CHECK(memcmp(man.sha256, d, SHA256_DIGEST_BYTES) == 0);

    memset(buf, 0, sizeof(buf));
    n = read_path(SEED_ENTRY, buf, sizeof(buf) - 1u);
    CHECK(n == (int)(sizeof(SEED_FILE) - 1u));
    CHECK(memcmp(buf, SEED_FILE, (size_t)n) == 0);
}

/* ------------------------------------------------------------------------- */
/* 2. manifest 解析                                                          */
/* ------------------------------------------------------------------------- */

static void t_manifest_valid(void)
{
    struct pkg_manifest man;
    char text[PKG_MANIFEST_MAX];
    char crlf[PKG_MANIFEST_MAX];
    char spaced[PKG_MANIFEST_MAX];

    make_manifest(text, sizeof(text), "demo", "2.0.1", "service", "bin/demo",
                  sha_abc());
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_OK);
    CHECK(man.fields == PKG_F_ALL);
    CHECK(strcmp(man.name, "demo") == 0);
    CHECK(strcmp(man.version, "2.0.1") == 0);
    CHECK(strcmp(man.type, "service") == 0);
    CHECK(strcmp(man.entry, "bin/demo") == 0);
    {
        u8 d[SHA256_DIGEST_BYTES];

        sha256("abc", 3, d);
        CHECK(memcmp(man.sha256, d, SHA256_DIGEST_BYTES) == 0);
    }

    /* CRLF + 注释 + 空行。 */
    (void)snprintf(crlf, sizeof(crlf),
                   "# comment\r\n\r\nname=demo\r\nversion=1.0\r\ntype=lib\r\n"
                   "entry=lib/demo.so\r\nsha256=%s\r\n",
                   sha_abc());
    CHECK(pkg_manifest_parse(crlf, (u32)strlen(crlf), &man) == PKG_OK);
    CHECK(strcmp(man.entry, "lib/demo.so") == 0);

    /* key/value 周围空白允许。 */
    (void)snprintf(spaced, sizeof(spaced),
                   " name = demo \n version = 1.0 \n type = app \n"
                   " entry = bin/demo \n sha256 = %s \n",
                   sha_abc());
    CHECK(pkg_manifest_parse(spaced, (u32)strlen(spaced), &man) == PKG_OK);
    CHECK(strcmp(man.name, "demo") == 0);
}

static void t_manifest_invalid(void)
{
    struct pkg_manifest man;
    char text[PKG_MANIFEST_MAX];
    char bad[PKG_MANIFEST_MAX];
    char bad_sha[65];
    const char *good = sha_abc();

    memset(bad_sha, 'z', 64);
    bad_sha[64] = '\0';

    CHECK(pkg_manifest_parse(NULL, 10, &man) == PKG_EINVAL);
    CHECK(pkg_manifest_parse("name=x", 0, &man) == PKG_EINVAL);
    CHECK(pkg_manifest_parse(text, PKG_MANIFEST_MAX + 1u, &man) == PKG_EINVAL);

    /* 缺字段：逐个省略。 */
    (void)snprintf(text, sizeof(text), "version=1.0\ntype=app\nentry=bin/x\nsha256=%s\n", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    (void)snprintf(text, sizeof(text), "name=x\ntype=app\nentry=bin/x\nsha256=%s\n", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    (void)snprintf(text, sizeof(text), "name=x\nversion=1.0\nentry=bin/x\nsha256=%s\n", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    (void)snprintf(text, sizeof(text), "name=x\nversion=1.0\ntype=app\nsha256=%s\n", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    (void)snprintf(text, sizeof(text), "name=x\nversion=1.0\ntype=app\nentry=bin/x\n");
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);

    /* 重复 / 未知字段 / 没有 '='。 */
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "bin/demo", good);
    (void)snprintf(bad, sizeof(bad), "%sname=demo\n", text);
    CHECK(pkg_manifest_parse(bad, (u32)strlen(bad), &man) == PKG_EINVAL);
    (void)snprintf(bad, sizeof(bad), "%sextra=1\n", text);
    CHECK(pkg_manifest_parse(bad, (u32)strlen(bad), &man) == PKG_EINVAL);
    (void)snprintf(bad, sizeof(bad), "%snoequals\n", text);
    CHECK(pkg_manifest_parse(bad, (u32)strlen(bad), &man) == PKG_EINVAL);

    /* 非法字段值。 */
    make_manifest(text, sizeof(text), "Demo", "1.0", "app", "bin/demo", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), ".hidden", "1.0", "app", "bin/demo", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "a/b", "1.0", "app", "bin/demo", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1/0", "app", "bin/demo", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "driver", "bin/demo", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "../x", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "/abs", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "a//b", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "a/./b", good);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "bin/demo",
                  bad_sha);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
    make_manifest(text, sizeof(text), "demo", "1.0", "app", "bin/demo",
                  "abcdef");
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_EINVAL);
}

/* ------------------------------------------------------------------------- */
/* 3. pkg_verify 的完整性路径                                                */
/* ------------------------------------------------------------------------- */

static void t_pkg_integrity(void)
{
    char text[PKG_MANIFEST_MAX];
    char good_manifest[PKG_MANIFEST_MAX];
    struct pkg_manifest man;
    const char *good = sha_abc();

    fs_setup();
    CHECK(pkg_init(&g_ctx) == PKG_OK);
    CHECK(pkg_verify("hello") == PKG_OK);

    /* 文件被改：哈希不符 -> EIO。 */
    CHECK(write_path(SEED_ENTRY, "tampered", 8) == 1);
    CHECK(pkg_verify("hello") == PKG_EIO);
    CHECK(pkg_stat()->failures >= 1);

    /* 重新种入恢复。 */
    CHECK(pkg_init(&g_ctx) == PKG_OK);
    CHECK(pkg_verify("hello") == PKG_OK);

    /* manifest 的 sha256 字段合法但与文件不符 -> EIO。 */
    memset(text, 0, sizeof(text));
    CHECK(read_path(SEED_MANIFEST, text, sizeof(text) - 1u) > 0);
    CHECK(pkg_manifest_parse(text, (u32)strlen(text), &man) == PKG_OK);
    make_manifest(good_manifest, sizeof(good_manifest), "hello", "1.0.0", "app",
                  "bin/hello", good);
    CHECK(write_path(SEED_MANIFEST, good_manifest,
                     (u32)strlen(good_manifest)) == 1);
    CHECK(pkg_verify("hello") == PKG_EIO);

    /* 清单里的 name 与目录名不一致 -> EINVAL。 */
    make_manifest(text, sizeof(text), "other", "1.0.0", "app", "bin/hello",
                  good);
    CHECK(write_path(SEED_MANIFEST, text, (u32)strlen(text)) == 1);
    CHECK(pkg_verify("hello") == PKG_EINVAL);

    /* entry 越界 -> 清单解析拒绝。 */
    make_manifest(text, sizeof(text), "hello", "1.0.0", "app", "../bin/x",
                  good);
    CHECK(write_path(SEED_MANIFEST, text, (u32)strlen(text)) == 1);
    CHECK(pkg_verify("hello") == PKG_EINVAL);

    /* 重新种入恢复，再确认 hello 可用。 */
    CHECK(pkg_init(&g_ctx) == PKG_OK);
    CHECK(pkg_verify("hello") == PKG_OK);
}

/* ------------------------------------------------------------------------- */
/* 4. 第二个包：列表/计数/精确缓冲边界                                        */
/* ------------------------------------------------------------------------- */

static void t_pkg_second(void)
{
    char manifest[PKG_MANIFEST_MAX];
    char buf[128];
    char hex[SHA256_HEX_MAX];
    u8 d[SHA256_DIGEST_BYTES];
    int n;

    fs_setup();
    CHECK(pkg_init(&g_ctx) == PKG_OK);
    CHECK(vfs_mkdir(&g_ctx, "/System/Packages/beta", 0755) == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "/System/Packages/beta/2.0.0", 0755) == VFS_OK);
    CHECK(vfs_mkdir(&g_ctx, "/System/Packages/beta/2.0.0/bin", 0755) == VFS_OK);
    CHECK(write_path("/System/Packages/beta/2.0.0/bin/beta", "beta pkg\n", 9) ==
          1);
    sha256("beta pkg\n", 9, d);
    sha256_hex(d, hex);
    make_manifest(manifest, sizeof(manifest), "beta", "2.0.0", "app", "bin/beta",
                  hex);
    CHECK(write_path("/System/Packages/beta/2.0.0/manifest", manifest,
                     (u32)strlen(manifest)) == 1);

    memset(buf, 0, sizeof(buf));
    n = pkg_list(buf, sizeof(buf));
    CHECK(n == 2);
    CHECK(strstr(buf, "hello 1.0.0\n") != NULL);
    CHECK(strstr(buf, "beta 2.0.0\n") != NULL);

    /* 文本 24 字节（含 NUL）：23 字节缓冲 -> ENOSPC，24 -> 2。 */
    CHECK(pkg_list(buf, 23) == PKG_ENOSPC);
    CHECK(pkg_list(buf, 24) == 2);

    CHECK(pkg_verify("beta") == PKG_OK);
    CHECK(pkg_verify("hello") == PKG_OK);
    CHECK(pkg_verify("Beta") == PKG_EINVAL);
}

int test_pkg(void)
{
    t_pkg_seed();
    t_manifest_valid();
    t_manifest_invalid();
    t_pkg_integrity();
    t_pkg_second();
    return 0;
}
