/*
 * ng WP2-1: VFS 核心实现。
 *
 * 组成（从下往上）：
 *   1. 挂载表：最多 VFS_MOUNT_MAX 个 {path, root, ops, ctx}，按“最长路径前缀 +
 *      组件边界”选择挂载点；"/" 是根挂载。
 *   2. 路径词法层：vfs_normalize() 把 cwd/相对路径变成规范绝对路径（折叠
 *      '/'、去 "."、消 ".."、长度/组件上限），不碰任何 FS。
 *   3. 路径解析层：resolve_abs() 在挂载树里逐组件 lookup；遇到符号链接
 *      （中间组件，或跟随的末组件）就把“已解析前缀 + 目标 + 剩余部分”重新
 *      规范化后从头再来，深度超过 VFS_SYMLINK_MAX -> ELOOP。挂载点切换由
 *      每轮 find_mount() 自动完成。
 *   4. 节点层：vfs_stat / vfs_listdir / vfs_mkdir / vfs_unlink / vfs_rename /
 *      vfs_truncate / vfs_symlink / vfs_readlink。
 *   5. fd 层：per-AS 的 struct vfs_ctx，fd = (gen<<16)|slot；打开时对节点
 *      retain、关闭时 release（unlink 后最后一个 close 才真正回收）。
 *
 * 错误码一律是负 errno（vfs.h），不返回“-1 + 单独 errno 变量”；调用方
 * （syscall 胶水/host 单测）直接把它当返回值用。
 */
#include "vfs.h"

/* ---- 极简 freestanding 字符串/内存（不依赖 libc） ------------------------ */
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

static void str_copy(char *dst, const char *src, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

static void mem_copy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int name_eq(const char *a, const char *b, u32 b_len)
{
    u32 i;

    for (i = 0; i < b_len; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return 0;
        }
    }
    return a[b_len] == '\0';
}

/* abs 是否是 mount_path 的“子路径”（组件边界处才认）。 */
static int is_child_path(const char *mount_path, const char *abs)
{
    u32 plen = str_len(mount_path);
    u32 alen = str_len(abs);

    if (alen <= plen) {
        return 0;
    }
    if (!name_eq(mount_path, abs, plen)) {
        return 0;
    }
    return abs[plen] == '/';
}

/* ------------------------------------------------------------------------- */
/* 挂载表                                                                     */
/* ------------------------------------------------------------------------- */

struct vfs_mount {
    u32 used;
    u32 gen;
    char path[VFS_PATH_MAX];
    struct vfs_node *root;
    const struct vfs_fs_ops *ops;
    void *ctx;
};

static struct vfs_mount g_mounts[VFS_MOUNT_MAX];
static u32 g_mount_gen;

void vfs_reset(void)
{
    u32 i;

    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        g_mounts[i].used = 0;
        g_mounts[i].gen = 0;
        g_mounts[i].path[0] = '\0';
        g_mounts[i].root = NULL;
        g_mounts[i].ops = NULL;
        g_mounts[i].ctx = NULL;
    }
    g_mount_gen = 0;
}

u32 vfs_mount_count(void)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used != 0) {
            n++;
        }
    }
    return n;
}

const char *vfs_mount_path_at(u32 index)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used == 0) {
            continue;
        }
        if (n == index) {
            return g_mounts[i].path;
        }
        n++;
    }
    return NULL;
}

/* 精确匹配（挂载/卸载/挂载点占用检查用），返回下标或 -1。 */
static int find_mount_index(const char *abs)
{
    u32 i;
    u32 len = str_len(abs);

    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used == 0) {
            continue;
        }
        if (str_len(g_mounts[i].path) == len && name_eq(g_mounts[i].path, abs, len)) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * 最长前缀 + 组件边界匹配。"/" 匹配一切绝对路径；"/mnt" 匹配 "/mnt" 与
 * "/mnt/..."，但**不**匹配 "/mnt2"。*rest 跳过挂载点后的 '/'（指向相对部分）。
 */
static int find_mount(const char *abs, struct vfs_mount **out, const char **rest)
{
    u32 i;
    u32 alen = str_len(abs);
    int best = -1;
    u32 best_len = 0;

    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        u32 plen;

        if (g_mounts[i].used == 0) {
            continue;
        }
        plen = str_len(g_mounts[i].path);
        if (plen > alen) {
            continue;
        }
        if (plen != 1 || g_mounts[i].path[0] != '/') {
            if (plen == 0) {
                continue;
            }
            if (abs[plen] != '\0' && abs[plen] != '/') {
                continue;
            }
            if (plen > 0 && !name_eq(g_mounts[i].path, abs, plen)) {
                continue;
            }
        }
        if (best < 0 || plen > best_len) {
            best = (int)i;
            best_len = plen;
        }
    }
    if (best < 0) {
        return VFS_ENOENT;
    }
    *out = &g_mounts[best];
    *rest = abs + best_len;
    while (**rest == '/') {
        (*rest)++;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* 词法规范化                                                                 */
/* ------------------------------------------------------------------------- */

static void pop_component(char *out, u32 *pos)
{
    u32 p = *pos;

    while (p > 1 && out[p - 1] != '/') {
        p--;
    }
    if (p > 1) {
        p--;
    }
    while (p > 1 && out[p - 1] == '/') {
        p--;
    }
    out[p] = '\0';
    *pos = p;
}

int vfs_normalize(const char *cwd, const char *path, char *out, u32 out_size)
{
    char src[VFS_PATH_MAX * 2u];
    u32 n;
    u32 i = 0;
    u32 pos = 1;

    if (path == NULL || out == NULL || out_size < 2) {
        return VFS_EINVAL;
    }
    n = str_len(path);
    if (n == 0) {
        return VFS_EINVAL;
    }
    if (n >= VFS_PATH_MAX) {
        return VFS_ENAMETOOLONG;
    }
    if (cwd != NULL && cwd[0] == '/' && str_len(cwd) >= VFS_PATH_MAX) {
        return VFS_ENAMETOOLONG;
    }
    if (path[0] == '/') {
        str_copy(src, path, n);
    } else {
        const char *base = (cwd != NULL && cwd[0] == '/') ? cwd : "/";
        u32 blen = str_len(base);

        if (blen + 1u + n >= sizeof(src)) {
            return VFS_ENAMETOOLONG;
        }
        mem_copy(src, base, blen);
        if (blen > 1) {
            src[blen++] = '/';
        } else if (blen == 1 && base[0] == '/') {
            /* "/" + relative：直接接在根后面，不需要额外 '/'。 */
            blen = 1;
        }
        mem_copy(src + blen, path, n);
        src[blen + n] = '\0';
    }

    out[0] = '/';
    out[1] = '\0';
    while (src[i] != '\0') {
        u32 start;
        u32 clen;

        while (src[i] == '/') {
            i++;
        }
        if (src[i] == '\0') {
            break;
        }
        start = i;
        while (src[i] != '\0' && src[i] != '/') {
            i++;
        }
        clen = i - start;
        if (clen == 1 && src[start] == '.') {
            continue;
        }
        if (clen == 2 && src[start] == '.' && src[start + 1] == '.') {
            pop_component(out, &pos);
            continue;
        }
        if (clen >= VFS_NAME_MAX) {
            return VFS_ENAMETOOLONG;
        }
        if (pos + ((pos > 1) ? 1u : 0u) + clen + 1u > out_size) {
            return VFS_ENAMETOOLONG;
        }
        if (pos > 1) {
            out[pos++] = '/';
        }
        mem_copy(out + pos, src + start, clen);
        pos += clen;
        out[pos] = '\0';
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* 路径解析                                                                   */
/* ------------------------------------------------------------------------- */

static int append_combined(char *combined, u32 cap, const char *prefix,
                           u32 prefix_len, const char *target,
                           u32 target_len, const char *rest)
{
    u32 pos = 0;
    u32 rest_len = str_len(rest);

    if (prefix_len + 1u + target_len + 1u + rest_len + 1u > cap) {
        return VFS_ENAMETOOLONG;
    }
    mem_copy(combined + pos, prefix, prefix_len);
    pos += prefix_len;
    if (prefix_len == 0 || combined[pos - 1] != '/') {
        combined[pos++] = '/';
    }
    mem_copy(combined + pos, target, target_len);
    pos += target_len;
    if (rest_len > 0) {
        if (combined[pos - 1] != '/') {
            combined[pos++] = '/';
        }
        mem_copy(combined + pos, rest, rest_len);
        pos += rest_len;
    }
    combined[pos] = '\0';
    return VFS_OK;
}

/*
 * 逐组件解析规范绝对路径；符号链接通过“重建绝对路径 + 从头再来”处理，
 * 这样挂载点切换、"."/".." 折叠都只有一份实现（normalize）。
 */
static int resolve_abs(const char *abs_in, struct vfs_node **out)
{
    char abs[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];
    char combined[VFS_PATH_MAX * 3u];
    u32 in_len;
    u32 depth;

    if (abs_in == NULL || out == NULL) {
        return VFS_EINVAL;
    }
    in_len = str_len(abs_in);
    if (in_len == 0) {
        return VFS_EINVAL;
    }
    if (in_len >= VFS_PATH_MAX) {
        return VFS_ENAMETOOLONG;
    }
    str_copy(abs, abs_in, in_len);

    for (depth = 0; depth <= VFS_SYMLINK_MAX; depth++) {
        struct vfs_mount *m = NULL;
        const char *rest = NULL;
        struct vfs_node *cur;
        char cur_path[VFS_PATH_MAX];
        u32 cur_len;
        int restart = 0;
        int rc;

        rc = find_mount(abs, &m, &rest);
        if (rc != VFS_OK) {
            return rc;
        }
        if (m->ops == NULL || m->root == NULL) {
            return VFS_ENOENT;
        }
        cur = m->root;
        cur_len = str_len(m->path);
        str_copy(cur_path, m->path, cur_len);

        while (*rest != '\0') {
            const char *comp = rest;
            u32 clen = 0;
            u32 parent_len = cur_len;
            struct vfs_node *next = NULL;
            u32 tlen;

            while (comp[clen] != '\0' && comp[clen] != '/') {
                clen++;
            }
            if (clen == 0) {
                rest++;
                continue;
            }
            if (clen >= VFS_NAME_MAX) {
                return VFS_ENAMETOOLONG;
            }
            rest = comp + clen;
            while (*rest == '/') {
                rest++;
            }
            if (cur->type != VFS_NODE_DIR) {
                return VFS_ENOTDIR;
            }
            if (cur->ops == NULL || cur->ops->lookup == NULL) {
                return VFS_ENOTDIR;
            }
            rc = cur->ops->lookup(cur->fs_ctx, cur, comp, clen, &next);
            if (rc != VFS_OK) {
                return rc;
            }
            if (next == NULL) {
                return VFS_ENOENT;
            }
            if (parent_len + clen + 2u > VFS_PATH_MAX) {
                return VFS_ENAMETOOLONG;
            }
            if (parent_len > 1) {
                cur_path[parent_len++] = '/';
            }
            mem_copy(cur_path + parent_len, comp, clen);
            cur_len = parent_len + clen;
            cur_path[cur_len] = '\0';

            if (next->type == VFS_NODE_SYMLINK) {
                if (next->ops == NULL || next->ops->readlink == NULL) {
                    return VFS_EINVAL;
                }
                rc = next->ops->readlink(next->fs_ctx, next, target,
                                         (u32)sizeof(target));
                if (rc < 0) {
                    return rc;
                }
                if (rc == 0) {
                    return VFS_ENOENT;
                }
                tlen = (u32)rc;
                if (tlen >= VFS_PATH_MAX) {
                    return VFS_ENAMETOOLONG;
                }
                rc = append_combined(combined, (u32)sizeof(combined),
                                     (target[0] == '/') ? "" : cur_path,
                                     (target[0] == '/') ? 0u : parent_len,
                                     target, tlen, rest);
                if (rc != VFS_OK) {
                    return rc;
                }
                rc = vfs_normalize("/", combined, abs, (u32)sizeof(abs));
                if (rc != VFS_OK) {
                    return rc;
                }
                restart = 1;
                break;
            }
            cur = next;
        }
        if (restart) {
            continue;
        }
        *out = cur;
        return VFS_OK;
    }
    return VFS_ELOOP;
}

int vfs_resolve(struct vfs_ctx *c, const char *path, struct vfs_node **out)
{
    char abs[VFS_PATH_MAX];
    int rc;

    rc = vfs_normalize(c != NULL ? c->cwd : NULL, path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    return resolve_abs(abs, out);
}

/*
 * 解析“父目录 + 末组件”。创建/删除/改名/readlink 用：末组件可以还不存在，
 * 所以不能走 vfs_resolve()。路径规范化后末组件一定非空（末尾 '/' 已折叠、
 * "."/".." 已消掉），根本身（"/"）不能作为被操作对象 -> EINVAL。
 */
static int resolve_parent(struct vfs_ctx *c, const char *path,
                          struct vfs_node **dir, char *name, u32 name_size)
{
    char abs[VFS_PATH_MAX];
    char parent[VFS_PATH_MAX];
    u32 len;
    u32 slash;
    u32 nlen;
    int rc;

    rc = vfs_normalize(c != NULL ? c->cwd : NULL, path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    len = str_len(abs);
    if (len <= 1) {
        return VFS_EINVAL;
    }
    slash = len;
    while (slash > 0 && abs[slash - 1] != '/') {
        slash--;
    }
    if (slash == 0 || slash >= len) {
        return VFS_EINVAL;
    }
    nlen = len - slash;
    if (nlen == 0 || nlen >= VFS_NAME_MAX || nlen >= name_size) {
        return VFS_ENAMETOOLONG;
    }
    mem_copy(name, abs + slash, nlen);
    name[nlen] = '\0';
    if (slash == 1) {
        str_copy(parent, "/", 1);
    } else {
        mem_copy(parent, abs, slash - 1);
        parent[slash - 1] = '\0';
    }
    rc = resolve_abs(parent, dir);
    if (rc != VFS_OK) {
        return rc;
    }
    if (*dir == NULL || (*dir)->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* 权限 / 类型                                                                */
/* ------------------------------------------------------------------------- */

static u32 vfs_mode_of(u32 type, u32 perm)
{
    u32 ifmt = VFS_S_IFREG;

    if (type == VFS_NODE_DIR) {
        ifmt = VFS_S_IFDIR;
    } else if (type == VFS_NODE_SYMLINK) {
        ifmt = VFS_S_IFLNK;
    }
    return ifmt | (perm & 0777u);
}

static int perm_check(const struct vfs_ctx *c, const struct vfs_node *n,
                      int want_read, int want_write)
{
    u32 mode;

    if (c == NULL || c->uid == 0 || n == NULL) {
        return VFS_OK;
    }
    mode = n->mode & 0777u;
    if (want_read != 0 && (mode & 0400u) == 0) {
        return VFS_EPERM;
    }
    if (want_write != 0 && (mode & 0200u) == 0) {
        return VFS_EPERM;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* fd 表                                                                      */
/* ------------------------------------------------------------------------- */

#define VFS_GEN_MASK 0x7FFFu
#define VFS_SLOT_MASK 0xFFFFu

void vfs_ctx_init(struct vfs_ctx *c)
{
    u32 i;

    if (c == NULL) {
        return;
    }
    for (i = 0; i < VFS_FD_MAX; i++) {
        c->fds[i].offset = 0;
        c->fds[i].node = NULL;
        c->fds[i].gen = 0;
        c->fds[i].flags = 0;
        c->fds[i].used = 0;
    }
    str_copy(c->cwd, "/", 1);
    c->uid = 0;
    c->live = 0;
    c->peak = 0;
    c->opens = 0;
    c->closes = 0;
    c->bad_fd = 0;
    c->stale_fd = 0;
    c->full = 0;
    c->perm_denied = 0;
}

u32 vfs_fd_live(const struct vfs_ctx *c)
{
    u32 i;
    u32 n = 0;

    if (c == NULL) {
        return 0;
    }
    for (i = 0; i < VFS_FD_MAX; i++) {
        if (c->fds[i].used != 0) {
            n++;
        }
    }
    return n;
}

u32 vfs_fd_slot_of(int fd)
{
    return (u32)fd & VFS_SLOT_MASK;
}

u32 vfs_fd_gen_of(int fd)
{
    return ((u32)fd >> 16) & VFS_GEN_MASK;
}

static struct vfs_fd *fd_lookup(struct vfs_ctx *c, int fd)
{
    u32 slot;
    u32 gen;

    if (c == NULL || fd <= 0) {
        if (c != NULL) {
            c->bad_fd++;
        }
        return NULL;
    }
    slot = vfs_fd_slot_of(fd);
    gen = vfs_fd_gen_of(fd);
    if (slot >= VFS_FD_MAX || gen == 0) {
        c->bad_fd++;
        return NULL;
    }
    if (c->fds[slot].used == 0 || c->fds[slot].gen != gen) {
        c->bad_fd++;
        if (c->fds[slot].used != 0) {
            c->stale_fd++;
        }
        return NULL;
    }
    return &c->fds[slot];
}

static int fd_alloc(struct vfs_ctx *c, struct vfs_node *node, u32 flags,
                    int *out_fd)
{
    u32 i;

    for (i = 0; i < VFS_FD_MAX; i++) {
        u32 gen;

        if (c->fds[i].used != 0) {
            continue;
        }
        gen = (c->fds[i].gen + 1u) & VFS_GEN_MASK;
        if (gen == 0) {
            gen = 1;
        }
        c->fds[i].used = 1;
        c->fds[i].gen = gen;
        c->fds[i].flags = flags;
        c->fds[i].offset = 0;
        c->fds[i].node = node;
        c->live++;
        if (c->live > c->peak) {
            c->peak = c->live;
        }
        c->opens++;
        *out_fd = (int)((gen << 16) | i);
        return VFS_OK;
    }
    c->full++;
    return VFS_EMFILE;
}

/* ------------------------------------------------------------------------- */
/* 挂载 / 卸载                                                               */
/* ------------------------------------------------------------------------- */

int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *ctx,
              struct vfs_node *root)
{
    char abs[VFS_PATH_MAX];
    struct vfs_node *mountpoint = NULL;
    u32 i;
    int rc;

    if (ops == NULL || root == NULL) {
        return VFS_EINVAL;
    }
    rc = vfs_normalize("/", path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    if (find_mount_index(abs) >= 0) {
        return VFS_EEXIST;
    }
    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used == 0) {
            break;
        }
    }
    if (i >= VFS_MOUNT_MAX) {
        return VFS_ENOSPC;
    }
    if (str_len(abs) != 1) {
        rc = resolve_abs(abs, &mountpoint);
        if (rc != VFS_OK) {
            return rc;
        }
        if (mountpoint == NULL || mountpoint->type != VFS_NODE_DIR) {
            return VFS_ENOTDIR;
        }
    }
    root->ops = ops;
    root->fs_ctx = ctx;
    str_copy(g_mounts[i].path, abs, str_len(abs));
    g_mounts[i].used = 1;
    g_mounts[i].gen = ++g_mount_gen;
    g_mounts[i].root = root;
    g_mounts[i].ops = ops;
    g_mounts[i].ctx = ctx;
    return VFS_OK;
}

int vfs_umount(const char *path)
{
    char abs[VFS_PATH_MAX];
    u32 len;
    u32 i;
    int idx;
    int rc;

    rc = vfs_normalize("/", path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    len = str_len(abs);
    if (len == 1) {
        return VFS_EBUSY; /* 根挂载不允许卸载 */
    }
    idx = find_mount_index(abs);
    if (idx < 0) {
        return VFS_ENOENT;
    }
    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used == 0 || (int)i == idx) {
            continue;
        }
        if (is_child_path(abs, g_mounts[i].path)) {
            return VFS_EBUSY; /* 还有子挂载 */
        }
    }
    g_mounts[idx].used = 0;
    g_mounts[idx].root = NULL;
    g_mounts[idx].ops = NULL;
    g_mounts[idx].ctx = NULL;
    g_mounts[idx].path[0] = '\0';
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* 打开 / 读 / 写 / 关闭                                                     */
/* ------------------------------------------------------------------------- */

int vfs_open(struct vfs_ctx *c, const char *path, u32 flags, u32 mode)
{
    struct vfs_node *node = NULL;
    u32 acc;
    int created = 0;
    int rc;
    int fd = 0;

    if (c == NULL) {
        return VFS_EINVAL;
    }
    if ((flags & ~VFS_O_FLAGS_MASK) != 0) {
        return VFS_EINVAL;
    }
    acc = flags & VFS_O_ACCMODE;
    if (acc != VFS_O_RDONLY && acc != VFS_O_WRONLY && acc != VFS_O_RDWR) {
        return VFS_EINVAL;
    }
    rc = vfs_resolve(c, path, &node);
    if (rc == VFS_ENOENT && (flags & VFS_O_CREAT) != 0) {
        struct vfs_node *dir = NULL;
        char name[VFS_NAME_MAX];

        rc = resolve_parent(c, path, &dir, name, (u32)sizeof(name));
        if (rc != VFS_OK) {
            return rc;
        }
        if (dir->ops == NULL || dir->ops->create == NULL) {
            return VFS_EROFS;
        }
        rc = perm_check(c, dir, 0, 1);
        if (rc != VFS_OK) {
            c->perm_denied++;
            return rc;
        }
        rc = dir->ops->create(dir->fs_ctx, dir, name, str_len(name),
                              VFS_NODE_FILE,
                              (mode == 0) ? 0644u : (mode & 0777u), &node);
        if (rc != VFS_OK) {
            return rc;
        }
        created = 1;
    } else if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL || node->type == VFS_NODE_NONE) {
        return VFS_ENOENT;
    }
    if (created == 0 && (flags & VFS_O_CREAT) != 0 && (flags & VFS_O_EXCL) != 0) {
        return VFS_EEXIST;
    }
    if (node->type == VFS_NODE_DIR) {
        if (acc != VFS_O_RDONLY && (flags & VFS_O_DIRECTORY) == 0) {
            return VFS_EISDIR;
        }
        if (acc != VFS_O_RDONLY) {
            return VFS_EISDIR;
        }
        if ((flags & VFS_O_TRUNC) != 0) {
            return VFS_EISDIR;
        }
    } else {
        if ((flags & VFS_O_DIRECTORY) != 0) {
            return VFS_ENOTDIR;
        }
        if (node->type != VFS_NODE_FILE) {
            return VFS_EINVAL;
        }
    }
    /*
     * 新建的节点按“创建者立即拿到 fd”处理：只检查父目录的写权限（上面已做），
     * 不再用刚设的 mode 反查自己的读写权限——否则 open(O_CREAT|O_RDWR, 0444)
     * 会创建成功却返回 EPERM，留下一个孤儿文件。
     */
    if (created == 0) {
        rc = perm_check(c, node, (acc == VFS_O_RDONLY || acc == VFS_O_RDWR),
                        (acc == VFS_O_WRONLY || acc == VFS_O_RDWR));
        if (rc != VFS_OK) {
            c->perm_denied++;
            return rc;
        }
    }
    if ((flags & VFS_O_TRUNC) != 0 && node->type == VFS_NODE_FILE) {
        if (node->ops == NULL || node->ops->truncate == NULL) {
            return VFS_EINVAL;
        }
        rc = node->ops->truncate(node->fs_ctx, node, 0);
        if (rc != VFS_OK) {
            return rc;
        }
    }
    if (node->ops != NULL && node->ops->retain != NULL) {
        rc = node->ops->retain(node->fs_ctx, node);
        if (rc != VFS_OK) {
            return rc;
        }
    }
    rc = fd_alloc(c, node, flags, &fd);
    if (rc != VFS_OK) {
        if (node->ops != NULL && node->ops->release != NULL) {
            node->ops->release(node->fs_ctx, node);
        }
        return rc;
    }
    return fd;
}

int vfs_read(struct vfs_ctx *c, int fd, void *buf, u64 len)
{
    struct vfs_fd *f = fd_lookup(c, fd);
    u32 acc;
    int n;

    if (f == NULL) {
        return VFS_EBADF;
    }
    if (f->node->type == VFS_NODE_DIR) {
        return VFS_EISDIR;
    }
    if (f->node->type != VFS_NODE_FILE) {
        return VFS_EINVAL;
    }
    acc = f->flags & VFS_O_ACCMODE;
    if (acc == VFS_O_WRONLY) {
        return VFS_EBADF;
    }
    if (len == 0) {
        return 0;
    }
    if (buf == NULL) {
        return VFS_EFAULT;
    }
    if (f->node->ops == NULL || f->node->ops->read == NULL) {
        return VFS_EINVAL;
    }
    n = f->node->ops->read(f->node->fs_ctx, f->node, f->offset, buf, len);
    if (n < 0) {
        return n;
    }
    f->offset += (u64)n;
    return n;
}

int vfs_write(struct vfs_ctx *c, int fd, const void *buf, u64 len)
{
    struct vfs_fd *f = fd_lookup(c, fd);
    u32 acc;
    int n;

    if (f == NULL) {
        return VFS_EBADF;
    }
    if (f->node->type == VFS_NODE_DIR) {
        return VFS_EISDIR;
    }
    if (f->node->type != VFS_NODE_FILE) {
        return VFS_EINVAL;
    }
    acc = f->flags & VFS_O_ACCMODE;
    if (acc == VFS_O_RDONLY) {
        return VFS_EBADF;
    }
    if (len == 0) {
        return 0;
    }
    if (buf == NULL) {
        return VFS_EFAULT;
    }
    if (f->node->ops == NULL || f->node->ops->write == NULL) {
        return VFS_EINVAL;
    }
    if ((f->flags & VFS_O_APPEND) != 0) {
        f->offset = f->node->size;
    }
    n = f->node->ops->write(f->node->fs_ctx, f->node, f->offset, buf, len);
    if (n < 0) {
        return n;
    }
    f->offset += (u64)n;
    return n;
}

int vfs_close(struct vfs_ctx *c, int fd)
{
    struct vfs_fd *f = fd_lookup(c, fd);

    if (f == NULL) {
        return VFS_EBADF;
    }
    if (f->node != NULL && f->node->ops != NULL &&
        f->node->ops->release != NULL) {
        f->node->ops->release(f->node->fs_ctx, f->node);
    }
    f->used = 0;
    f->node = NULL;
    f->offset = 0;
    f->flags = 0;
    c->live--;
    c->closes++;
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* 元数据 / 目录 / 创建 / 删除 / 改名 / 链接                                 */
/* ------------------------------------------------------------------------- */

int vfs_stat(struct vfs_ctx *c, const char *path, struct vfs_stat *out)
{
    struct vfs_node *node = NULL;
    int rc;

    if (out == NULL) {
        return VFS_EFAULT;
    }
    rc = vfs_resolve(c, path, &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL) {
        return VFS_ENOENT;
    }
    out->type = node->type;
    out->mode = vfs_mode_of(node->type, node->mode);
    out->size = node->size;
    out->ino = node->ino;
    return VFS_OK;
}

int vfs_listdir(struct vfs_ctx *c, const char *path, struct vfs_dirent *ents,
                u32 max)
{
    struct vfs_node *node = NULL;
    u32 i;
    u32 count = 0;
    int rc;

    if (ents == NULL || max == 0) {
        return VFS_EINVAL;
    }
    rc = vfs_resolve(c, path, &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    if (node->ops == NULL || node->ops->readdir == NULL) {
        return VFS_ENOTDIR;
    }
    rc = perm_check(c, node, 1, 0);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    for (i = 0;; i++) {
        char name[VFS_NAME_MAX];
        struct vfs_stat st;
        u32 k;

        rc = node->ops->readdir(node->fs_ctx, node, i, name,
                                (u32)sizeof(name), &st);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        if (count >= max) {
            break; /* 最多填 max 条；返回值是填了几条 */
        }
        ents[count].type = st.type;
        ents[count].size = st.size;
        ents[count].name_len = str_len(name);
        for (k = 0; k < VFS_NAME_MAX; k++) {
            ents[count].name[k] = (k < ents[count].name_len) ? name[k] : '\0';
        }
        count++;
    }
    return (int)count;
}

int vfs_mkdir(struct vfs_ctx *c, const char *path, u32 mode)
{
    struct vfs_node *dir = NULL;
    struct vfs_node *existing = NULL;
    char name[VFS_NAME_MAX];
    int rc;

    rc = resolve_parent(c, path, &dir, name, (u32)sizeof(name));
    if (rc != VFS_OK) {
        return rc;
    }
    if (dir->ops == NULL || dir->ops->create == NULL) {
        return VFS_EROFS;
    }
    rc = dir->ops->lookup(dir->fs_ctx, dir, name, str_len(name), &existing);
    if (rc == VFS_OK && existing != NULL) {
        return VFS_EEXIST;
    }
    if (rc != VFS_OK && rc != VFS_ENOENT) {
        return rc;
    }
    rc = perm_check(c, dir, 0, 1);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    rc = dir->ops->create(dir->fs_ctx, dir, name, str_len(name), VFS_NODE_DIR,
                          (mode == 0) ? 0755u : (mode & 0777u), &existing);
    if (rc != VFS_OK) {
        return rc;
    }
    return VFS_OK;
}

int vfs_unlink(struct vfs_ctx *c, const char *path)
{
    struct vfs_node *dir = NULL;
    struct vfs_node *node = NULL;
    char name[VFS_NAME_MAX];
    int rc;
    char abs[VFS_PATH_MAX];
    u32 i;

    rc = resolve_parent(c, path, &dir, name, (u32)sizeof(name));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = dir->ops->lookup(dir->fs_ctx, dir, name, str_len(name), &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL) {
        return VFS_ENOENT;
    }
    if (node->type == VFS_NODE_DIR) {
        return VFS_EISDIR;
    }
    rc = vfs_normalize(c != NULL ? c->cwd : NULL, path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    for (i = 0; i < VFS_MOUNT_MAX; i++) {
        if (g_mounts[i].used != 0 && name_eq(g_mounts[i].path, abs, str_len(abs))) {
            return VFS_EBUSY;
        }
    }
    if (dir->ops == NULL || dir->ops->unlink == NULL) {
        return VFS_EROFS;
    }
    rc = perm_check(c, dir, 0, 1);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    return dir->ops->unlink(dir->fs_ctx, dir, name, str_len(name));
}

int vfs_rename(struct vfs_ctx *c, const char *oldpath, const char *newpath)
{
    struct vfs_node *olddir = NULL;
    struct vfs_node *newdir = NULL;
    struct vfs_node *node = NULL;
    struct vfs_node *existing = NULL;
    char oldname[VFS_NAME_MAX];
    char newname[VFS_NAME_MAX];
    int rc;

    rc = resolve_parent(c, oldpath, &olddir, oldname, (u32)sizeof(oldname));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = resolve_parent(c, newpath, &newdir, newname, (u32)sizeof(newname));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = olddir->ops->lookup(olddir->fs_ctx, olddir, oldname, str_len(oldname),
                             &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL) {
        return VFS_ENOENT;
    }
    if (node->type == VFS_NODE_DIR) {
        return VFS_EISDIR; /* 目录改名留给后续 WP */
    }
    rc = newdir->ops->lookup(newdir->fs_ctx, newdir, newname, str_len(newname),
                             &existing);
    if (rc == VFS_OK && existing != NULL) {
        return VFS_EEXIST;
    }
    if (olddir->ops == NULL || olddir->ops->rename == NULL) {
        return VFS_EROFS;
    }
    if (olddir != newdir) {
        return VFS_EINVAL; /* 本 WP 只支持同一目录内改名 */
    }
    rc = perm_check(c, olddir, 0, 1);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    return olddir->ops->rename(olddir->fs_ctx, olddir, oldname,
                               str_len(oldname), newdir, newname,
                               str_len(newname));
}

int vfs_truncate(struct vfs_ctx *c, const char *path, u64 size)
{
    struct vfs_node *node = NULL;
    int rc;

    rc = vfs_resolve(c, path, &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node->type == VFS_NODE_DIR) {
        return VFS_EISDIR;
    }
    if (node->type != VFS_NODE_FILE) {
        return VFS_EINVAL;
    }
    rc = perm_check(c, node, 0, 1);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    if (node->ops == NULL || node->ops->truncate == NULL) {
        return VFS_EINVAL;
    }
    return node->ops->truncate(node->fs_ctx, node, size);
}

int vfs_symlink(struct vfs_ctx *c, const char *path, const char *target)
{
    struct vfs_node *dir = NULL;
    struct vfs_node *node = NULL;
    char name[VFS_NAME_MAX];
    int rc;

    if (target == NULL || str_len(target) == 0 ||
        str_len(target) >= VFS_PATH_MAX) {
        return VFS_EINVAL;
    }
    rc = resolve_parent(c, path, &dir, name, (u32)sizeof(name));
    if (rc != VFS_OK) {
        return rc;
    }
    if (dir->ops == NULL || dir->ops->symlink == NULL) {
        return VFS_EROFS;
    }
    rc = dir->ops->lookup(dir->fs_ctx, dir, name, str_len(name), &node);
    if (rc == VFS_OK && node != NULL) {
        return VFS_EEXIST;
    }
    rc = perm_check(c, dir, 0, 1);
    if (rc != VFS_OK) {
        c->perm_denied++;
        return rc;
    }
    return dir->ops->symlink(dir->fs_ctx, dir, name, str_len(name), target);
}

int vfs_readlink(struct vfs_ctx *c, const char *path, char *buf, u32 max)
{
    struct vfs_node *dir = NULL;
    struct vfs_node *node = NULL;
    char name[VFS_NAME_MAX];
    int rc;

    if (buf == NULL || max == 0) {
        return VFS_EINVAL;
    }
    rc = resolve_parent(c, path, &dir, name, (u32)sizeof(name));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = dir->ops->lookup(dir->fs_ctx, dir, name, str_len(name), &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL) {
        return VFS_ENOENT;
    }
    if (node->type != VFS_NODE_SYMLINK) {
        return VFS_EINVAL;
    }
    if (node->ops == NULL || node->ops->readlink == NULL) {
        return VFS_EINVAL;
    }
    return node->ops->readlink(node->fs_ctx, node, buf, max);
}

int vfs_chdir(struct vfs_ctx *c, const char *path)
{
    struct vfs_node *node = NULL;
    char abs[VFS_PATH_MAX];
    int rc;

    if (c == NULL) {
        return VFS_EINVAL;
    }
    rc = vfs_normalize(c->cwd, path, abs, (u32)sizeof(abs));
    if (rc != VFS_OK) {
        return rc;
    }
    rc = resolve_abs(abs, &node);
    if (rc != VFS_OK) {
        return rc;
    }
    if (node == NULL || node->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    str_copy(c->cwd, abs, str_len(abs));
    return VFS_OK;
}
