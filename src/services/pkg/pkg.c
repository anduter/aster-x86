/*
 * ng WP2-5: pkg 服务实现（L1，最小包目录 + manifest + SHA-256 校验）。
 *
 * 设计取舍：
 *   - 包目录就是 ramfs 上的普通目录，不引入新的存储格式/索引；
 *   - manifest 用严格 key=value 文本，字段少但校验完整（name/version/type/
 *     entry/sha256），未知/重复/缺失字段都拒绝；
 *   - pkg_list 只读目录，不解析 manifest；pkg_verify 才做字段 + 文件哈希；
 *   - 不做签名（D1），所以 verify 的语义是"自洽"而不是"可信"。
 */
#include "pkg.h"

/* L1 内部共享原语：SHA-256 实现在 auth 目录（见 sha256.h 头注释）。 */
#include "../auth/include/sha256.h"

#define PKG_SEED_NAME "hello"
#define PKG_SEED_VERSION "1.0.0"
#define PKG_SEED_TYPE "app"
#define PKG_SEED_ENTRY "bin/hello"
#define PKG_SEED_FILE "hello from ng pkg\n"

static struct pkg_stats g_stats;
static struct vfs_ctx *g_ctx;
static int g_ready;

/* ------------------------------------------------------------------------- */
/* 字符串小工具（无 libc）                                                    */
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

static void copy_n(char *dst, u32 cap, const char *src, u32 len)
{
    u32 i;

    if (dst == NULL || cap == 0) {
        return;
    }
    for (i = 0; i < len && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 追加字符串到一个以 NUL 结尾的缓冲；失败（不够放）返回 0。 */
static int append_str(char *buf, u32 *pos, u32 cap, const char *s)
{
    u32 i = 0;

    if (buf == NULL || pos == NULL || s == NULL) {
        return 0;
    }
    while (s[i] != '\0') {
        if (*pos + 1u >= cap) {
            return 0;
        }
        buf[*pos] = s[i];
        (*pos)++;
        i++;
    }
    buf[*pos] = '\0';
    return 1;
}

static int build_pkg_dir(char *out, u32 cap, const char *name)
{
    u32 pos = 0;

    if (!append_str(out, &pos, cap, PKG_ROOT) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, name)) {
        return PKG_ENAMETOOLONG;
    }
    return PKG_OK;
}

static int build_ver_dir(char *out, u32 cap, const char *name,
                         const char *version)
{
    u32 pos = 0;

    if (!append_str(out, &pos, cap, PKG_ROOT) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, name) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, version)) {
        return PKG_ENAMETOOLONG;
    }
    return PKG_OK;
}

static int build_file_path(char *out, u32 cap, const char *name,
                           const char *version, const char *rel)
{
    u32 pos = 0;

    if (!append_str(out, &pos, cap, PKG_ROOT) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, name) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, version) ||
        !append_str(out, &pos, cap, "/") ||
        !append_str(out, &pos, cap, rel)) {
        return PKG_ENAMETOOLONG;
    }
    return PKG_OK;
}

/* ------------------------------------------------------------------------- */
/* 字段校验                                                                  */
/* ------------------------------------------------------------------------- */

static int name_ok(const char *s)
{
    u32 n = str_len(s);
    u32 i;

    if (s == NULL || n == 0 || n >= PKG_NAME_MAX || s[0] == '.') {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int version_ok(const char *s)
{
    u32 n = str_len(s);
    u32 i;

    if (s == NULL || n == 0 || n >= PKG_VERSION_MAX || s[0] == '.') {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int type_ok(const char *s)
{
    return str_eq(s, "app") || str_eq(s, "service") || str_eq(s, "lib");
}

static int entry_ok(const char *s)
{
    u32 n = str_len(s);
    u32 i;
    u32 comp = 0;

    if (s == NULL || n == 0 || n >= PKG_ENTRY_MAX || s[0] == '/' ||
        s[n - 1u] == '/') {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (c == '/') {
            if (comp == 0) {
                return 0; /* 空组件 */
            }
            if (comp == 1 && s[i - 1u] == '.') {
                return 0; /* "." 组件 */
            }
            if (comp == 2 && s[i - 1u] == '.' && s[i - 2u] == '.') {
                return 0; /* ".." 组件 */
            }
            comp = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return 0;
        }
        comp++;
    }
    if (comp == 0) {
        return 0;
    }
    if (comp == 1 && s[n - 1u] == '.') {
        return 0;
    }
    if (comp == 2 && s[n - 1u] == '.' && s[n - 2u] == '.') {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* manifest 解析                                                             */
/* ------------------------------------------------------------------------- */

static int key_is(const char *key, u32 len, const char *lit)
{
    u32 i = 0;

    while (lit[i] != '\0') {
        if (i >= len || key[i] != lit[i]) {
            return 0;
        }
        i++;
    }
    return i == len;
}

static void trim_block(const char **ps, u32 *plen)
{
    const char *s = *ps;
    u32 len = *plen;

    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1u] == ' ' || s[len - 1u] == '\t')) {
        len--;
    }
    *ps = s;
    *plen = len;
}

int pkg_manifest_parse(const char *text, u32 len, struct pkg_manifest *out)
{
    u32 i = 0;

    if (text == NULL || out == NULL || len == 0 || len > PKG_MANIFEST_MAX) {
        return PKG_EINVAL;
    }
    {
        u8 *raw = (u8 *)out;
        u32 k;

        for (k = 0; k < (u32)sizeof(*out); k++) {
            raw[k] = 0;
        }
    }

    while (i < len) {
        u32 start = i;
        u32 end;
        u32 eq;
        const char *key;
        u32 key_len;
        const char *val;
        u32 val_len;

        while (i < len && text[i] != '\n') {
            i++;
        }
        end = i;
        if (i < len) {
            i++;
        }
        if (end > start && text[end - 1u] == '\r') {
            end--;
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            start++;
        }
        while (end > start && (text[end - 1u] == ' ' || text[end - 1u] == '\t')) {
            end--;
        }
        if (start == end || text[start] == '#') {
            continue;
        }

        eq = start;
        while (eq < end && text[eq] != '=') {
            eq++;
        }
        if (eq >= end) {
            return PKG_EINVAL;
        }
        key = text + start;
        key_len = eq - start;
        val = text + eq + 1u;
        val_len = end - (eq + 1u);
        trim_block(&key, &key_len);
        trim_block(&val, &val_len);
        if (key_len == 0 || val_len == 0) {
            return PKG_EINVAL;
        }

        if (key_is(key, key_len, "name")) {
            if ((out->fields & PKG_F_NAME) || val_len >= PKG_NAME_MAX) {
                return PKG_EINVAL;
            }
            copy_n(out->name, PKG_NAME_MAX, val, val_len);
            if (!name_ok(out->name)) {
                return PKG_EINVAL;
            }
            out->fields |= PKG_F_NAME;
        } else if (key_is(key, key_len, "version")) {
            if ((out->fields & PKG_F_VERSION) || val_len >= PKG_VERSION_MAX) {
                return PKG_EINVAL;
            }
            copy_n(out->version, PKG_VERSION_MAX, val, val_len);
            if (!version_ok(out->version)) {
                return PKG_EINVAL;
            }
            out->fields |= PKG_F_VERSION;
        } else if (key_is(key, key_len, "type")) {
            if ((out->fields & PKG_F_TYPE) || val_len >= PKG_TYPE_MAX) {
                return PKG_EINVAL;
            }
            copy_n(out->type, PKG_TYPE_MAX, val, val_len);
            if (!type_ok(out->type)) {
                return PKG_EINVAL;
            }
            out->fields |= PKG_F_TYPE;
        } else if (key_is(key, key_len, "entry")) {
            if ((out->fields & PKG_F_ENTRY) || val_len >= PKG_ENTRY_MAX) {
                return PKG_EINVAL;
            }
            copy_n(out->entry, PKG_ENTRY_MAX, val, val_len);
            if (!entry_ok(out->entry)) {
                return PKG_EINVAL;
            }
            out->fields |= PKG_F_ENTRY;
        } else if (key_is(key, key_len, "sha256")) {
            char hex[SHA256_HEX_MAX];

            if ((out->fields & PKG_F_SHA256) || val_len != SHA256_HEX_BYTES) {
                return PKG_EINVAL;
            }
            copy_n(hex, (u32)sizeof(hex), val, val_len);
            if (!sha256_hex_parse(hex, out->sha256)) {
                return PKG_EINVAL;
            }
            out->fields |= PKG_F_SHA256;
        } else {
            return PKG_EINVAL; /* D0 严格：未知字段不接受 */
        }
    }

    return (out->fields == PKG_F_ALL) ? PKG_OK : PKG_EINVAL;
}

/* ------------------------------------------------------------------------- */
/* 文件 IO                                                                   */
/* ------------------------------------------------------------------------- */

static int write_file(struct vfs_ctx *c, const char *path, const char *data,
                      u32 len)
{
    int fd;
    int rc;

    fd = vfs_open(c, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644);
    if (fd <= 0) {
        return (fd == 0) ? PKG_EINVAL : fd;
    }
    rc = vfs_write(c, fd, data, (u64)len);
    if (rc >= 0 && (u32)rc != len) {
        rc = PKG_ENOSPC;
    }
    if (rc >= 0) {
        rc = vfs_close(c, fd);
    } else {
        (void)vfs_close(c, fd);
    }
    return rc;
}

static int read_file(struct vfs_ctx *c, const char *path, char *buf, u32 max,
                     u32 *out_len)
{
    int fd;
    int n;

    fd = vfs_open(c, path, VFS_O_RDONLY, 0);
    if (fd <= 0) {
        return (fd == 0) ? PKG_EINVAL : fd;
    }
    n = vfs_read(c, fd, buf, (u64)max);
    (void)vfs_close(c, fd);
    if (n < 0) {
        return n;
    }
    *out_len = (u32)n;
    return PKG_OK;
}

static int hash_file(struct vfs_ctx *c, const char *path,
                     u8 out[SHA256_DIGEST_BYTES])
{
    struct sha256_ctx sc;
    char buf[256];
    int fd;
    int n;

    fd = vfs_open(c, path, VFS_O_RDONLY, 0);
    if (fd <= 0) {
        return (fd == 0) ? PKG_EINVAL : fd;
    }
    sha256_init(&sc);
    for (;;) {
        n = vfs_read(c, fd, buf, (u64)sizeof(buf));
        if (n < 0) {
            (void)vfs_close(c, fd);
            return n;
        }
        if (n == 0) {
            break;
        }
        sha256_update(&sc, buf, (u64)n);
    }
    (void)vfs_close(c, fd);
    sha256_final(&sc, out);
    g_stats.hash_calls++;
    return PKG_OK;
}

static int make_dir(struct vfs_ctx *c, const char *path)
{
    int rc = vfs_mkdir(c, path, 0755);

    if (rc == VFS_EEXIST) {
        return PKG_OK;
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* 种子包                                                                    */
/* ------------------------------------------------------------------------- */

static int seed_hello(struct vfs_ctx *c)
{
    static const char *dirs[] = {
        "/System",
        "/System/Packages",
        "/System/Packages/hello",
        "/System/Packages/hello/1.0.0",
        "/System/Packages/hello/1.0.0/bin"
    };
    char file_path[VFS_PATH_MAX];
    char manifest_path[VFS_PATH_MAX];
    char manifest[PKG_MANIFEST_MAX];
    char hex[SHA256_HEX_MAX];
    u8 digest[SHA256_DIGEST_BYTES];
    u32 pos = 0;
    u32 i;
    int rc;

    for (i = 0; i < (u32)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        rc = make_dir(c, dirs[i]);
        if (rc != PKG_OK) {
            return rc;
        }
    }

    if (build_file_path(file_path, (u32)sizeof(file_path), PKG_SEED_NAME,
                        PKG_SEED_VERSION, PKG_SEED_ENTRY) != PKG_OK) {
        return PKG_ENAMETOOLONG;
    }
    rc = write_file(c, file_path, PKG_SEED_FILE,
                    (u32)(sizeof(PKG_SEED_FILE) - 1u));
    if (rc != PKG_OK) {
        return rc;
    }

    sha256(PKG_SEED_FILE, (u64)(sizeof(PKG_SEED_FILE) - 1u), digest);
    g_stats.hash_calls++;
    sha256_hex(digest, hex);

    manifest[0] = '\0';
    if (!append_str(manifest, &pos, (u32)sizeof(manifest), "name=") ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), PKG_SEED_NAME) ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), "\nversion=") ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), PKG_SEED_VERSION) ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), "\ntype=") ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), PKG_SEED_TYPE) ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), "\nentry=") ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), PKG_SEED_ENTRY) ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), "\nsha256=") ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), hex) ||
        !append_str(manifest, &pos, (u32)sizeof(manifest), "\n")) {
        return PKG_ENOSPC;
    }

    if (build_file_path(manifest_path, (u32)sizeof(manifest_path), PKG_SEED_NAME,
                        PKG_SEED_VERSION, "manifest") != PKG_OK) {
        return PKG_ENAMETOOLONG;
    }
    rc = write_file(c, manifest_path, manifest, pos);
    if (rc != PKG_OK) {
        return rc;
    }

    g_stats.packages++;
    g_stats.seeds++;
    return PKG_OK;
}

/* ------------------------------------------------------------------------- */
/* 对外 API                                                                  */
/* ------------------------------------------------------------------------- */

int pkg_init(struct vfs_ctx *c)
{
    u32 i;
    int rc;

    g_ready = 0;
    g_ctx = NULL;
    for (i = 0; i < (u32)sizeof(g_stats); i++) {
        ((u8 *)&g_stats)[i] = 0;
    }
    if (c == NULL) {
        return PKG_EINVAL;
    }
    g_ctx = c;
    rc = seed_hello(c);
    if (rc != PKG_OK) {
        g_ctx = NULL;
        return rc;
    }
    g_ready = 1;
    return PKG_OK;
}

int pkg_list(char *buf, u32 max)
{
    struct vfs_dirent pkgs[PKG_MAX_PACKAGES];
    char tmp[PKG_LIST_MAX];
    u32 pos = 0;
    int n;
    int i;
    u32 count = 0;

    if (g_ready == 0 || g_ctx == NULL || buf == NULL || max == 0) {
        return PKG_EINVAL;
    }

    n = vfs_listdir(g_ctx, PKG_ROOT, pkgs, PKG_MAX_PACKAGES);
    if (n < 0) {
        return n;
    }
    for (i = 0; i < n; i++) {
        struct vfs_dirent vers[PKG_MAX_VERSIONS];
        char base[VFS_PATH_MAX];
        int m;
        int j;

        if (pkgs[i].type != VFS_NODE_DIR) {
            continue;
        }
        if (build_pkg_dir(base, (u32)sizeof(base), pkgs[i].name) != PKG_OK) {
            return PKG_ENAMETOOLONG;
        }
        m = vfs_listdir(g_ctx, base, vers, PKG_MAX_VERSIONS);
        if (m < 0) {
            return m;
        }
        for (j = 0; j < m; j++) {
            if (vers[j].type != VFS_NODE_DIR) {
                continue;
            }
            if (!append_str(tmp, &pos, (u32)sizeof(tmp), pkgs[i].name) ||
                !append_str(tmp, &pos, (u32)sizeof(tmp), " ") ||
                !append_str(tmp, &pos, (u32)sizeof(tmp), vers[j].name) ||
                !append_str(tmp, &pos, (u32)sizeof(tmp), "\n")) {
                return PKG_ENOSPC;
            }
            count++;
        }
    }

    if (pos + 1u > max) {
        return PKG_ENOSPC;
    }
    for (i = 0; i < (int)pos; i++) {
        buf[i] = tmp[i];
    }
    buf[pos] = '\0';
    g_stats.lists++;
    return (int)count;
}

int pkg_verify(const char *name)
{
    struct vfs_dirent vers[PKG_MAX_VERSIONS];
    struct pkg_manifest man;
    char base[VFS_PATH_MAX];
    char manifest_path[VFS_PATH_MAX];
    char entry_path[VFS_PATH_MAX];
    char text[PKG_MANIFEST_MAX];
    char version[PKG_VERSION_MAX];
    u8 digest[SHA256_DIGEST_BYTES];
    u32 text_len = 0;
    int n;
    int i;
    int rc;

    if (g_ready == 0 || g_ctx == NULL || !name_ok(name)) {
        return PKG_EINVAL;
    }

    if (build_pkg_dir(base, (u32)sizeof(base), name) != PKG_OK) {
        return PKG_ENAMETOOLONG;
    }
    n = vfs_listdir(g_ctx, base, vers, PKG_MAX_VERSIONS);
    if (n < 0) {
        g_stats.failures++;
        return n;
    }
    version[0] = '\0';
    for (i = 0; i < n; i++) {
        if (vers[i].type == VFS_NODE_DIR) {
            copy_n(version, PKG_VERSION_MAX, vers[i].name, vers[i].name_len);
            break;
        }
    }
    if (version[0] == '\0') {
        g_stats.failures++;
        return PKG_ENOENT;
    }

    if (build_ver_dir(manifest_path, (u32)sizeof(manifest_path), name, version) !=
        PKG_OK) {
        return PKG_ENAMETOOLONG;
    }
    {
        u32 pos = str_len(manifest_path);

        if (!append_str(manifest_path, &pos, (u32)sizeof(manifest_path),
                        "/manifest")) {
            return PKG_ENAMETOOLONG;
        }
    }
    rc = read_file(g_ctx, manifest_path, text, (u32)sizeof(text), &text_len);
    if (rc != PKG_OK) {
        return rc;
    }
    rc = pkg_manifest_parse(text, text_len, &man);
    if (rc != PKG_OK) {
        g_stats.failures++;
        return rc;
    }
    if (!str_eq(man.name, name) || !str_eq(man.version, version)) {
        g_stats.failures++;
        return PKG_EINVAL;
    }
    if (build_file_path(entry_path, (u32)sizeof(entry_path), name, version,
                        man.entry) != PKG_OK) {
        return PKG_ENAMETOOLONG;
    }
    rc = hash_file(g_ctx, entry_path, digest);
    if (rc != PKG_OK) {
        g_stats.failures++;
        return rc;
    }

    {
        u8 diff = 0;
        u32 k;

        for (k = 0; k < SHA256_DIGEST_BYTES; k++) {
            diff |= (u8)(digest[k] ^ man.sha256[k]);
        }
        if (diff != 0) {
            g_stats.failures++;
            return PKG_EIO;
        }
    }
    g_stats.verifies++;
    return PKG_OK;
}

u32 pkg_seed_count(void)
{
    return g_stats.packages;
}

const struct pkg_stats *pkg_stat(void)
{
    return &g_stats;
}
