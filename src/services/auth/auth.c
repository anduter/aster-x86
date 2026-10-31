/*
 * ng WP2-5: auth 服务（L1，认证/账户最小集）。
 *
 * 范围（D0）：
 *   - 用户表：root(uid 0)、alice(uid 1000)；
 *   - 口令存储：每用户 16 字节 salt + SHA-256 迭代 4096 轮（自实现，见
 *     sha256.c），不存明文；
 *   - auth_init() 把用户记录种成 /etc/passwd 风格文本（ramfs），同时建立
 *     内存用户表作为登录判定源；
 *   - auth_login(name, pass) -> uid >= 0 / -ENOENT / -EACCES / -EINVAL；
 *     连续失败 AUTH_MAX_FAILS 次锁定（本 WP 只按次数，不接时钟）。
 *
 * 明确不做（按 WP2-5 边界）：
 *   - 不做 PAM/会话/令牌/多因素；不做 /etc/shadow 分离（D1 再拆）；
 *   - 不接随机数：salt 由 SHA-256("aster-ng-auth-salt:"+name) 前 16 字节
 *     派生，保证启动/单测可重现；D1 有熵源后换随机 salt 并迁移。
 *
 * 依赖：vfs.h + types.h + sha256.h（全部 L1/L0 公共头，不 include src/ng
 * 或 src/kernel 的内部头）。host 单测与内核编译同一份源码。
 */
#include "auth.h"

#include "sha256.h"
#include "vfs.h"

#define AUTH_SALT_PREFIX "aster-ng-auth-salt:"
#define AUTH_PASSWD_PATH "/etc/passwd"
#define AUTH_PASSWD_MAX 512u

static struct auth_user g_users[AUTH_MAX_USERS];
static struct auth_stats g_stats;
static int g_ready;

/* ------------------------------------------------------------------------- */
/* 小工具（无 libc）                                                          */
/* ------------------------------------------------------------------------- */

static u32 str_len(const char *s)
{
    u32 n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b)
{
    u32 i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    for (i = 0;; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
}

static void str_copy(char *dst, const char *src, u32 cap)
{
    u32 i = 0;

    if (dst == NULL || cap == 0) {
        return;
    }
    if (src != NULL) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int byte_eq(const u8 *a, const u8 *b, u32 n)
{
    u8 diff = 0;
    u32 i;

    for (i = 0; i < n; i++) {
        diff |= (u8)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* 任意长度字节 -> 小写 hex（out 至少 n*2+1）。 */
static void hex_encode(const u8 *in, u32 n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    u32 i;

    for (i = 0; i < n; i++) {
        out[i * 2u] = digits[(in[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = digits[in[i] & 0xFu];
    }
    out[n * 2u] = '\0';
}

/* 追加文本；返回新的结尾偏移，超出 cap 返回 0。 */
static u32 append_str(char *buf, u32 pos, u32 cap, const char *text)
{
    u32 i = 0;

    if (text == NULL) {
        return 0;
    }
    while (text[i] != '\0') {
        if (pos + 1u >= cap) {
            return 0;
        }
        buf[pos++] = text[i++];
    }
    buf[pos] = '\0';
    return pos;
}

static u32 append_u32(char *buf, u32 pos, u32 cap, u32 value)
{
    char tmp[11];
    u32 n = 0;
    u32 i;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < 10u) {
            tmp[n++] = (char)('0' + (int)(value % 10u));
            value /= 10u;
        }
    }
    for (i = n; i > 0; i--) {
        if (pos + 1u >= cap) {
            return 0;
        }
        buf[pos++] = tmp[i - 1u];
    }
    buf[pos] = '\0';
    return pos;
}

/* ------------------------------------------------------------------------- */
/* 口令哈希                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * FIPS 风格迭代：h0 = SHA256(salt || pass)，h(i) = SHA256(h(i-1) || salt)。
 * 迭代次数由调用方给出（用户记录里存了 iterations，便于以后升级参数）。
 */
int auth_hash_password(const char *pass, const u8 salt[AUTH_SALT_BYTES],
                       u32 iterations, u8 out[AUTH_HASH_BYTES])
{
    struct sha256_ctx ctx;
    u8 tmp[AUTH_HASH_BYTES];
    u32 i;

    if (pass == NULL || salt == NULL || out == NULL || iterations < 1u) {
        return AUTH_EINVAL;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, salt, AUTH_SALT_BYTES);
    sha256_update(&ctx, pass, (u64)str_len(pass));
    sha256_final(&ctx, tmp);

    for (i = 1; i < iterations; i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, tmp, AUTH_HASH_BYTES);
        sha256_update(&ctx, salt, AUTH_SALT_BYTES);
        sha256_final(&ctx, tmp);
    }
    for (i = 0; i < AUTH_HASH_BYTES; i++) {
        out[i] = tmp[i];
    }
    return AUTH_OK;
}

/*
 * D0 的 salt 来源：SHA-256("aster-ng-auth-salt:"+name) 的前 16 字节。
 * 每用户不同、启动可重现；不是密码学安全的随机源（D1 换熵源）。
 */
static void derive_salt(const char *name, u8 salt[AUTH_SALT_BYTES])
{
    char seed[AUTH_NAME_MAX + 32u];
    u8 digest[AUTH_HASH_BYTES];
    u32 pos = 0;

    pos = append_str(seed, pos, (u32)sizeof(seed), AUTH_SALT_PREFIX);
    if (pos != 0) {
        pos = append_str(seed, pos, (u32)sizeof(seed), name);
    }
    if (pos == 0) {
        /* 名字过长（理论上不会到这里）时退回固定前缀。 */
        str_copy(seed, AUTH_SALT_PREFIX, (u32)sizeof(seed));
        pos = str_len(seed);
    }
    sha256(seed, (u64)pos, digest);
    {
        u32 i;

        for (i = 0; i < AUTH_SALT_BYTES; i++) {
            salt[i] = digest[i];
        }
    }
}

static int name_len_ok(const char *name)
{
    u32 n = str_len(name);

    return n > 0u && n < AUTH_NAME_MAX;
}

static int pass_len_ok(const char *pass)
{
    u32 n = str_len(pass);

    return n > 0u && n < AUTH_PASS_MAX;
}

/* ------------------------------------------------------------------------- */
/* 用户表                                                                    */
/* ------------------------------------------------------------------------- */

static struct auth_user *find_user(const char *name)
{
    u32 i;

    for (i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].used != 0 && str_eq(g_users[i].name, name)) {
            return &g_users[i];
        }
    }
    return NULL;
}

static int seed_user(const char *name, u32 uid, const char *pass)
{
    struct auth_user *u = NULL;
    u32 i;

    for (i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].used == 0) {
            u = &g_users[i];
            break;
        }
    }
    if (u == NULL) {
        return AUTH_ENOSPC;
    }
    u->used = 1;
    u->uid = uid;
    u->iterations = AUTH_ITERATIONS;
    u->failed = 0;
    u->locked = 0;
    u->logins = 0;
    str_copy(u->name, name, AUTH_NAME_MAX);
    derive_salt(name, u->salt);
    if (auth_hash_password(pass, u->salt, u->iterations, u->hash) != AUTH_OK) {
        u->used = 0;
        return AUTH_EINVAL;
    }
    g_stats.hash_calls++;
    g_stats.users++;
    return AUTH_OK;
}

/* ------------------------------------------------------------------------- */
/* /etc/passwd 种子                                                          */
/* ------------------------------------------------------------------------- */

static u32 format_record(char *buf, u32 pos, u32 cap,
                         const struct auth_user *u)
{
    char salt_hex[AUTH_SALT_BYTES * 2u + 1u];
    char hash_hex[AUTH_HASH_BYTES * 2u + 1u];

    hex_encode(u->salt, AUTH_SALT_BYTES, salt_hex);
    hex_encode(u->hash, AUTH_HASH_BYTES, hash_hex);

    pos = append_str(buf, pos, cap, u->name);
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, ":");
    if (pos == 0) {
        return 0;
    }
    pos = append_u32(buf, pos, cap, u->uid);
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, ":");
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, salt_hex);
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, ":");
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, hash_hex);
    if (pos == 0) {
        return 0;
    }
    pos = append_str(buf, pos, cap, ":");
    if (pos == 0) {
        return 0;
    }
    pos = append_u32(buf, pos, cap, u->iterations);
    if (pos == 0) {
        return 0;
    }
    return append_str(buf, pos, cap, "\n");
}

static int seed_passwd_file(struct vfs_ctx *c)
{
    char text[AUTH_PASSWD_MAX];
    u32 pos = 0;
    u32 i;
    int rc;
    int fd;

    if (c == NULL) {
        return AUTH_EINVAL;
    }

    text[0] = '\0';
    for (i = 0; i < AUTH_MAX_USERS; i++) {
        if (g_users[i].used == 0) {
            continue;
        }
        pos = format_record(text, pos, (u32)sizeof(text), &g_users[i]);
        if (pos == 0) {
            return AUTH_ENOSPC;
        }
    }

    rc = vfs_mkdir(c, "/etc", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    fd = vfs_open(c, AUTH_PASSWD_PATH, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC,
                  0600);
    if (fd <= 0) {
        return (fd == 0) ? AUTH_EINVAL : fd;
    }
    rc = vfs_write(c, fd, text, (u64)pos);
    if (rc >= 0 && (u32)rc != pos) {
        rc = AUTH_ENOSPC;
    }
    if (rc >= 0) {
        rc = vfs_close(c, fd);
    } else {
        (void)vfs_close(c, fd);
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* 对外 API                                                                  */
/* ------------------------------------------------------------------------- */

int auth_init(struct vfs_ctx *c)
{
    int rc;
    u32 i;
    u8 *raw = (u8 *)g_users;

    g_ready = 0;
    for (i = 0; i < (u32)sizeof(g_users); i++) {
        raw[i] = 0;
    }
    for (i = 0; i < (u32)sizeof(g_stats); i++) {
        ((u8 *)&g_stats)[i] = 0;
    }
    g_stats.iterations = AUTH_ITERATIONS;

    rc = seed_user("root", 0, "root");
    if (rc != AUTH_OK) {
        return rc;
    }
    rc = seed_user("alice", 1000, "alice");
    if (rc != AUTH_OK) {
        return rc;
    }

    rc = seed_passwd_file(c);
    if (rc != VFS_OK) {
        return rc;
    }
    g_ready = 1;
    return AUTH_OK;
}

int auth_login(const char *name, const char *pass)
{
    struct auth_user *u;
    u8 digest[AUTH_HASH_BYTES];
    int rc;

    if (g_ready == 0) {
        return AUTH_EINVAL;
    }
    if (!name_len_ok(name) || !pass_len_ok(pass)) {
        return AUTH_EINVAL;
    }
    u = find_user(name);
    if (u == NULL) {
        g_stats.unknown++;
        return AUTH_ENOENT;
    }
    if (u->locked != 0) {
        g_stats.locked_rejects++;
        return AUTH_EACCES;
    }

    rc = auth_hash_password(pass, u->salt, u->iterations, digest);
    if (rc != AUTH_OK) {
        return rc;
    }
    g_stats.hash_calls++;

    if (!byte_eq(digest, u->hash, AUTH_HASH_BYTES)) {
        g_stats.failures++;
        u->failed++;
        if (u->failed >= AUTH_MAX_FAILS) {
            u->locked = 1;
            g_stats.locks++;
        }
        return AUTH_EACCES;
    }

    u->failed = 0;
    u->logins++;
    g_stats.logins++;
    return (int)u->uid;
}

u32 auth_user_count(void)
{
    return g_stats.users;
}

const struct auth_user *auth_user_at(u32 index)
{
    if (index >= AUTH_MAX_USERS || g_users[index].used == 0) {
        return NULL;
    }
    return &g_users[index];
}

const struct auth_stats *auth_stat(void)
{
    return &g_stats;
}
