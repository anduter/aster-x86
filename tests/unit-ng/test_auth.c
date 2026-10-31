/*
 * ng WP2-5 host 单测：SHA-256 向量/迭代 + auth 用户表/登录/锁定。
 *
 * 覆盖：
 *   - FIPS 180-4 向量（空串、abc、56/112 字节、1,000,000 个 'a'）；
 *   - 流式 update 分块与一次性哈希一致；hex 编解码边界；
 *   - auth_init 种入 root/alice + /etc/passwd 文本字段（salt/hash/iterations）；
 *   - 登录成功/口令错/未知用户/参数非法/EFAULT 之外的错误码；
 *   - 连续失败 5 次锁定、重新 auth_init 解锁、锁定不影响其他用户。
 */
#include <string.h>

#include "auth.h"
#include "fake_ng_env.h"
#include "ramfs.h"
#include "sha256.h"
#include "test_util.h"
#include "vfs.h"

static struct ramfs g_fs;
static struct fs_env g_env;
static struct vfs_ctx g_ctx;
static char g_file[512];

static void fs_setup(void)
{
    vfs_reset();
    fake_fs_reset();
    fake_fs_env(&g_env);
    ramfs_init(&g_fs, &g_env);
    CHECK(vfs_mount("/", ramfs_ops(), &g_fs, ramfs_root(&g_fs)) == VFS_OK);
    vfs_ctx_init(&g_ctx);
}

static void digest_hex(const char *msg, char *hex)
{
    u8 d[SHA256_DIGEST_BYTES];

    sha256(msg, (u64)strlen(msg), d);
    sha256_hex(d, hex);
}

/* ------------------------------------------------------------------------- */
/* 1. SHA-256 向量                                                           */
/* ------------------------------------------------------------------------- */

static void t_sha_vectors(void)
{
    char hex[SHA256_HEX_MAX];
    u8 d[SHA256_DIGEST_BYTES];
    struct sha256_ctx ctx;
    u32 i;

    sha256("", 0, d);
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    sha256("abc", 3, d);
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    digest_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    digest_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
               "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
               hex);
    CHECK(strcmp(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") == 0);

    /* 1,000,000 个 'a'：分块 update，覆盖多块/长度编码。 */
    sha256_init(&ctx);
    for (i = 0; i < 1000u; i++) {
        char chunk[1000];

        memset(chunk, 'a', sizeof(chunk));
        sha256_update(&ctx, chunk, (u64)sizeof(chunk));
    }
    sha256_final(&ctx, d);
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void t_sha_stream_and_hex(void)
{
    u8 one[SHA256_DIGEST_BYTES];
    u8 split[SHA256_DIGEST_BYTES];
    struct sha256_ctx ctx;
    char hex[SHA256_HEX_MAX];
    u8 parsed[SHA256_DIGEST_BYTES];

    sha256("hello world", 11, one);
    sha256_init(&ctx);
    sha256_update(&ctx, "hello ", 6);
    sha256_update(&ctx, "world", 5);
    sha256_final(&ctx, split);
    CHECK(memcmp(one, split, sizeof(one)) == 0);

    /* 跨 64 字节块边界的长度（63/64/65 字节）都稳定。 */
    {
        char data[65];

        memset(data, 'x', sizeof(data));
        sha256(data, 63, one);
        sha256(data, 64, split);
        CHECK(memcmp(one, split, sizeof(one)) != 0);
        sha256(data, 65, split);
        CHECK(memcmp(one, split, sizeof(one)) != 0);
    }

    sha256("abc", 3, one);
    sha256_hex(one, hex);
    CHECK(strlen(hex) == 64);
    CHECK(sha256_hex_valid(hex) == 1);
    CHECK(sha256_hex_parse(hex, parsed) == 1);
    CHECK(memcmp(one, parsed, sizeof(one)) == 0);

    CHECK(sha256_hex_valid("") == 0);
    CHECK(sha256_hex_valid("zz") == 0);
    CHECK(sha256_hex_valid("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0") == 0);
    CHECK(sha256_hex_parse("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
                           parsed) == 1);
    CHECK(memcmp(one, parsed, sizeof(one)) == 0);
    CHECK(sha256_hex_parse(NULL, parsed) == 0);
}

/* ------------------------------------------------------------------------- */
/* 2. auth 种子 + /etc/passwd                                                */
/* ------------------------------------------------------------------------- */

static int read_passwd(void)
{
    struct vfs_stat st;
    int fd;
    int n;

    if (vfs_stat(&g_ctx, "/etc/passwd", &st) != VFS_OK || st.size == 0 ||
        st.size >= sizeof(g_file)) {
        return 0;
    }
    fd = vfs_open(&g_ctx, "/etc/passwd", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        return 0;
    }
    n = vfs_read(&g_ctx, fd, g_file, sizeof(g_file) - 1u);
    (void)vfs_close(&g_ctx, fd);
    if (n <= 0) {
        return 0;
    }
    g_file[n] = '\0';
    return 1;
}

static int count_lines(const char *text)
{
    int n = 0;
    u32 i;

    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            n++;
        }
    }
    return n;
}

static void t_auth_init(void)
{
    const struct auth_user *u;
    const struct auth_stats *st;

    fs_setup();
    CHECK(auth_init(&g_ctx) == AUTH_OK);
    CHECK(auth_user_count() == 2);
    CHECK(auth_stat()->users == 2);
    CHECK(auth_stat()->iterations == 4096);

    u = auth_user_at(0);
    CHECK(u != NULL);
    CHECK(u != NULL && strcmp(u->name, "root") == 0);
    CHECK(u != NULL && u->uid == 0);
    CHECK(u != NULL && u->iterations == 4096);
    CHECK(u != NULL && u->locked == 0);
    u = auth_user_at(1);
    CHECK(u != NULL && strcmp(u->name, "alice") == 0);
    CHECK(u != NULL && u->uid == 1000);
    CHECK(auth_user_at(2) == NULL);
    CHECK(auth_user_at(AUTH_MAX_USERS) == NULL);

    CHECK(read_passwd() == 1);
    CHECK(count_lines(g_file) == 2);
    CHECK(strstr(g_file, "root:0:") != NULL);
    CHECK(strstr(g_file, "alice:1000:") != NULL);
    CHECK(strstr(g_file, ":4096\n") != NULL);

    /* 每条记录 5 个字段：name:uid:salt(32):hash(64):iterations。 */
    {
        char *line = g_file;
        int fields = 0;
        char *p;

        for (p = line; *p != '\0' && *p != '\n'; p++) {
            if (*p == ':') {
                fields++;
            }
        }
        CHECK(fields == 4);
        CHECK(p - line > 100);
        CHECK(p - line < 160);
    }

    st = auth_stat();
    CHECK(st != NULL && st->hash_calls >= 2);
}

static void t_auth_hash_params(void)
{
    const struct auth_user *root;
    u8 recomputed[AUTH_HASH_BYTES];
    u8 other[AUTH_HASH_BYTES];
    u8 salt[AUTH_SALT_BYTES];

    fs_setup();
    CHECK(auth_init(&g_ctx) == AUTH_OK);
    root = auth_user_at(0);
    CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    CHECK(root->salt[0] != 0 || root->salt[1] != 0);
    CHECK(root->hash[0] != 0 || root->hash[1] != 0);
    CHECK(auth_hash_password("root", root->salt, root->iterations,
                             recomputed) == AUTH_OK);
    CHECK(memcmp(recomputed, root->hash, AUTH_HASH_BYTES) == 0);

    CHECK(auth_hash_password("root", root->salt, 1u, other) == AUTH_OK);
    CHECK(memcmp(other, root->hash, AUTH_HASH_BYTES) != 0);
    CHECK(auth_hash_password("alice", root->salt, root->iterations, other) ==
          AUTH_OK);
    CHECK(memcmp(other, root->hash, AUTH_HASH_BYTES) != 0);

    memset(salt, 0x5A, sizeof(salt));
    CHECK(auth_hash_password("root", salt, 1000u, other) == AUTH_OK);
    CHECK(auth_hash_password("root", salt, 1000u, recomputed) == AUTH_OK);
    CHECK(memcmp(other, recomputed, AUTH_HASH_BYTES) == 0);
    CHECK(auth_hash_password("root", salt, 0u, other) == AUTH_EINVAL);
    CHECK(auth_hash_password(NULL, salt, 1000u, other) == AUTH_EINVAL);
    CHECK(auth_hash_password("root", NULL, 1000u, other) == AUTH_EINVAL);
}

/* ------------------------------------------------------------------------- */
/* 3. 登录与锁定                                                             */
/* ------------------------------------------------------------------------- */

static void t_auth_login(void)
{
    const struct auth_stats *st;

    fs_setup();
    CHECK(auth_init(&g_ctx) == AUTH_OK);
    CHECK(auth_login("root", "root") == 0);
    CHECK(auth_login("alice", "alice") == 1000);
    CHECK(auth_stat()->logins == 2);
    CHECK(auth_login("root", "wrong") == AUTH_EACCES);
    CHECK(auth_stat()->failures == 1);
    CHECK(auth_login("nobody", "root") == AUTH_ENOENT);
    CHECK(auth_stat()->unknown == 1);
    CHECK(auth_login("", "root") == AUTH_EINVAL);
    CHECK(auth_login("root", "") == AUTH_EINVAL);
    CHECK(auth_login(NULL, "root") == AUTH_EINVAL);
    CHECK(auth_login("root", NULL) == AUTH_EINVAL);

    /* 成功登录清零该用户的失败计数。 */
    CHECK(auth_login("alice", "bad") == AUTH_EACCES);
    CHECK(auth_login("alice", "alice") == 1000);
    CHECK(auth_user_at(1) != NULL && auth_user_at(1)->failed == 0);

    st = auth_stat();
    CHECK(st != NULL && st->locks == 0);
}

static void t_auth_lock(void)
{
    const struct auth_user *root;
    u32 i;

    fs_setup();
    CHECK(auth_init(&g_ctx) == AUTH_OK);
    for (i = 0; i < AUTH_MAX_FAILS; i++) {
        CHECK(auth_login("root", "wrong") == AUTH_EACCES);
    }
    root = auth_user_at(0);
    CHECK(root != NULL && root->failed == AUTH_MAX_FAILS);
    CHECK(root != NULL && root->locked == 1);
    CHECK(auth_stat()->locks == 1);
    CHECK(auth_login("root", "root") == AUTH_EACCES);
    CHECK(auth_stat()->locked_rejects >= 1);
    CHECK(auth_login("alice", "alice") == 1000);

    /* 重新 auth_init 清空锁定状态。 */
    CHECK(auth_init(&g_ctx) == AUTH_OK);
    root = auth_user_at(0);
    CHECK(root != NULL && root->locked == 0);
    CHECK(auth_login("root", "root") == 0);
    CHECK(auth_stat()->users == 2);
}

/* ------------------------------------------------------------------------- */

int test_auth(void)
{
    t_sha_vectors();
    t_sha_stream_and_hex();
    t_auth_init();
    t_auth_hash_params();
    t_auth_login();
    t_auth_lock();
    return 0;
}
