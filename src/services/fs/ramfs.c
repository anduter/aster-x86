/*
 * ng WP2-1: ramfs 实现（内存节点 + 页链数据 + 目录项池）。
 *
 * 并发：ng 当前是单核、IF=0、显式调度点（协作式），ramfs 的每次调用都在
 * 内核态临界区内完成，所以不加锁；将来多核/抢占时在这里加 spinlock 即可，
 * 对外 API 不变。
 *
 * 名字：目录项名字最多 VFS_NAME_MAX-1 字节（含 NUL 的缓冲 VFS_NAME_MAX）；
 * 组件里不会出现 '/'（VFS 已按 '/' 切开，测试直接调 ramfs 也不会传）。
 *
 * 页链：node->first_page / page->next 都是“索引+1”，0 表示结束。读写按
 * off/4096 走链，文件 <= 256 页，最坏 256 次指针跳转，不值得再建页表。
 */
#include "ramfs.h"

static void mem_copy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void mem_zero(void *dst, u64 n)
{
    u8 *d = (u8 *)dst;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

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

static void str_copy_n(char *dst, const char *src, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

static u32 ifmt_of(u32 type)
{
    if (type == VFS_NODE_DIR) {
        return VFS_S_IFDIR;
    }
    if (type == VFS_NODE_SYMLINK) {
        return VFS_S_IFLNK;
    }
    return VFS_S_IFREG;
}

/* ------------------------------------------------------------------------- */
/* 节点 / 目录项 / 页链基础                                                   */
/* ------------------------------------------------------------------------- */

static struct ramfs_node *node_of(struct ramfs *fs, const struct vfs_node *vn)
{
    if (fs == NULL || vn == NULL || vn->fs_ctx != fs || vn->priv == NULL) {
        return NULL;
    }
    return (struct ramfs_node *)vn->priv;
}

static u32 node_index(struct ramfs *fs, const struct ramfs_node *n)
{
    return (u32)(n - fs->nodes);
}

static void fill_vnode(struct ramfs *fs, struct ramfs_node *n)
{
    n->vnode.type = n->type;
    n->vnode.mode = n->mode & 0777u;
    n->vnode.size = (n->type == VFS_NODE_SYMLINK) ? (u64)str_len(n->target)
                                                  : n->size;
    n->vnode.ino = n->ino;
    n->vnode.ops = ramfs_ops();
    n->vnode.fs_ctx = fs;
    n->vnode.priv = n;
}

/* 目录项按 parent 线性扫描；返回下标或 RAMFS_NO_ENT。 */
static u32 find_ent(struct ramfs *fs, u32 parent, const char *name, u32 len)
{
    u32 i;

    for (i = 0; i < RAMFS_MAX_ENTS; i++) {
        struct ramfs_ent *e = &fs->ents[i];

        if (e->used == 0 || e->parent != parent) {
            continue;
        }
        if (str_len(e->name) == len) {
            u32 k;
            u32 ok = 1;

            for (k = 0; k < len; k++) {
                if (e->name[k] != name[k]) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                return i;
            }
        }
    }
    return RAMFS_NO_ENT;
}

static u32 alloc_node(struct ramfs *fs)
{
    u32 i;

    for (i = 1; i < RAMFS_MAX_NODES; i++) {
        if (fs->nodes[i].used == 0) {
            return i;
        }
    }
    return RAMFS_NO_ENT;
}

static u32 alloc_ent(struct ramfs *fs)
{
    u32 i;

    for (i = 0; i < RAMFS_MAX_ENTS; i++) {
        if (fs->ents[i].used == 0) {
            return i;
        }
    }
    return RAMFS_NO_ENT;
}

static u32 page_index_at(struct ramfs *fs, u32 first, u32 index)
{
    u32 cur = first;
    u32 n = 0;

    while (cur != 0 && n < index) {
        cur = fs->pages[cur - 1].next;
        n++;
    }
    return cur; /* 0 = 链比 index 短 */
}

/* 释放页链上第 keep 页之后的全部页（keep=0 表示整条链）。 */
static void page_release_tail(struct ramfs *fs, u32 first, u32 keep)
{
    u32 cur = first;
    u32 n = 0;
    u32 prev = 0;

    while (cur != 0 && n < keep) {
        prev = cur;
        cur = fs->pages[cur - 1].next;
        n++;
    }
    if (prev != 0) {
        fs->pages[prev - 1].next = 0;
    }
    while (cur != 0) {
        u32 next = fs->pages[cur - 1].next;
        struct ramfs_page *p = &fs->pages[cur - 1];

        if (p->mem != NULL && fs->env.page_free != NULL) {
            fs->env.page_free(fs->env.ctx, p->mem);
        }
        p->mem = NULL;
        p->next = 0;
        p->used = 0;
        if (fs->st.pages_used > 0) {
            fs->st.pages_used--;
        }
        cur = next;
    }
}

static void node_free(struct ramfs *fs, struct ramfs_node *n)
{
    page_release_tail(fs, n->first_page, 0);
    n->used = 0;
    n->type = VFS_NODE_NONE;
    n->first_page = 0;
    n->page_count = 0;
    n->size = 0;
    n->open_refs = 0;
    n->deleted = 0;
    n->parent = RAMFS_ROOT_PARENT;
    n->target[0] = '\0';
    n->vnode.type = VFS_NODE_NONE;
    n->vnode.size = 0;
    if (fs->st.nodes_used > 0) {
        fs->st.nodes_used--;
    }
}

/*
 * 确保节点已有 need 页（只追加）。失败时回滚本次追加的页并把 first/count
 * 恢复原值；页池耗尽 -> ENOSPC，分配器失败 -> ENOMEM。
 */
static int ensure_pages(struct ramfs *fs, struct ramfs_node *n, u32 need)
{
    u32 old_count = n->page_count;
    u32 old_first = n->first_page;
    u32 last = 0;
    u32 i;

    if (need <= old_count) {
        return VFS_OK;
    }
    if (need > RAMFS_MAX_FILE_PAGES) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    if (old_count > 0) {
        last = page_index_at(fs, old_first, old_count - 1);
        if (last == 0) {
            return VFS_EINVAL;
        }
    }
    for (i = old_count; i < need; i++) {
        u32 pi = RAMFS_NO_ENT;
        struct ramfs_page *p;
        void *mem;
        u32 k;

        for (k = 0; k < RAMFS_MAX_DATA_PAGES; k++) {
            if (fs->pages[k].used == 0) {
                pi = k;
                break;
            }
        }
        if (pi == RAMFS_NO_ENT) {
            page_release_tail(fs, n->first_page, old_count);
            n->first_page = old_first;
            n->page_count = old_count;
            fs->st.enospc++;
            return VFS_ENOSPC;
        }
        mem = (fs->env.page_alloc != NULL) ? fs->env.page_alloc(fs->env.ctx)
                                           : NULL;
        if (mem == NULL) {
            page_release_tail(fs, n->first_page, old_count);
            n->first_page = old_first;
            n->page_count = old_count;
            return VFS_ENOMEM;
        }
        p = &fs->pages[pi];
        p->mem = mem;
        p->next = 0;
        p->used = 1;
        fs->st.pages_used++;
        if (fs->st.pages_used > fs->st.page_peak) {
            fs->st.page_peak = fs->st.pages_used;
        }
        /* last 用“索引+1”编码：0 才是“还没有任何页”，页 0 是合法页。 */
        if (last != 0) {
            fs->pages[last - 1].next = pi + 1;
        } else {
            n->first_page = pi + 1;
        }
        last = pi + 1;
    }
    n->page_count = need;
    return VFS_OK;
}

/* 在已有页范围内清零 [from,to)（调用方保证范围落在 page_count 内）。 */
static void zero_range(struct ramfs *fs, struct ramfs_node *n, u64 from, u64 to)
{
    while (from < to) {
        u32 idx = (u32)(from / 4096ULL);
        u32 inpage = (u32)(from % 4096ULL);
        u32 chain = page_index_at(fs, n->first_page, idx);
        u64 chunk = to - from;

        if (chain == 0) {
            return;
        }
        if (chunk > (u64)(4096u - inpage)) {
            chunk = (u64)(4096u - inpage);
        }
        mem_zero((u8 *)fs->pages[chain - 1].mem + inpage, chunk);
        from += chunk;
    }
}

/* ------------------------------------------------------------------------- */
/* ops：目录                                                                  */
/* ------------------------------------------------------------------------- */

static int ramfs_lookup(void *ctx, struct vfs_node *dir, const char *name,
                        u32 len, struct vfs_node **out)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *dn = node_of(fs, dir);
    u32 ei;
    struct ramfs_node *n;

    if (fs == NULL || dn == NULL || name == NULL || out == NULL) {
        return VFS_EINVAL;
    }
    if (dn->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    if (len == 0 || len >= VFS_NAME_MAX) {
        return VFS_ENOENT;
    }
    ei = find_ent(fs, node_index(fs, dn), name, len);
    if (ei == RAMFS_NO_ENT) {
        return VFS_ENOENT;
    }
    n = &fs->nodes[fs->ents[ei].node];
    if (n->used == 0) {
        return VFS_ENOENT;
    }
    fill_vnode(fs, n);
    *out = &n->vnode;
    return VFS_OK;
}

static int ramfs_create(void *ctx, struct vfs_node *dir, const char *name,
                        u32 len, u32 type, u32 mode, struct vfs_node **out)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *dn = node_of(fs, dir);
    u32 ni;
    u32 ei;
    struct ramfs_node *n;

    if (fs == NULL || dn == NULL || name == NULL || out == NULL) {
        return VFS_EINVAL;
    }
    if (dn->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    if (len == 0 || len >= VFS_NAME_MAX) {
        return VFS_ENAMETOOLONG;
    }
    if (type != VFS_NODE_FILE && type != VFS_NODE_DIR &&
        type != VFS_NODE_SYMLINK) {
        return VFS_EINVAL;
    }
    if (find_ent(fs, node_index(fs, dn), name, len) != RAMFS_NO_ENT) {
        return VFS_EEXIST;
    }
    ni = alloc_node(fs);
    if (ni == RAMFS_NO_ENT) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    ei = alloc_ent(fs);
    if (ei == RAMFS_NO_ENT) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    n = &fs->nodes[ni];
    n->used = 1;
    n->type = type;
    n->mode = mode & 0777u;
    n->parent = node_index(fs, dn);
    n->first_page = 0;
    n->page_count = 0;
    n->open_refs = 0;
    n->deleted = 0;
    n->size = 0;
    n->ino = ++fs->ino_seq;
    n->target[0] = '\0';
    fs->ents[ei].used = 1;
    fs->ents[ei].parent = node_index(fs, dn);
    fs->ents[ei].node = ni;
    str_copy_n(fs->ents[ei].name, name, len);
    fs->st.nodes_used++;
    fs->st.ents_used++;
    if (fs->st.nodes_used > fs->st.node_peak) {
        fs->st.node_peak = fs->st.nodes_used;
    }
    fs->st.creates++;
    fill_vnode(fs, n);
    *out = &n->vnode;
    return VFS_OK;
}

static int ramfs_unlink(void *ctx, struct vfs_node *dir, const char *name,
                        u32 len)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *dn = node_of(fs, dir);
    u32 ei;
    struct ramfs_node *n;

    if (fs == NULL || dn == NULL || name == NULL) {
        return VFS_EINVAL;
    }
    if (dn->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    ei = find_ent(fs, node_index(fs, dn), name, len);
    if (ei == RAMFS_NO_ENT) {
        return VFS_ENOENT;
    }
    n = &fs->nodes[fs->ents[ei].node];
    if (n->type == VFS_NODE_DIR) {
        return VFS_EISDIR;
    }
    fs->ents[ei].used = 0;
    fs->ents[ei].parent = RAMFS_ROOT_PARENT;
    fs->ents[ei].name[0] = '\0';
    fs->st.ents_used--;
    n->deleted = 1;
    n->parent = RAMFS_ROOT_PARENT;
    fs->st.deletes++;
    if (n->open_refs == 0) {
        node_free(fs, n);
    }
    return VFS_OK;
}

static int ramfs_rename(void *ctx, struct vfs_node *olddir, const char *oldname,
                        u32 oldlen, struct vfs_node *newdir,
                        const char *newname, u32 newlen)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *odn = node_of(fs, olddir);
    struct ramfs_node *ndn = node_of(fs, newdir);
    u32 ei;
    u32 odi;
    u32 ndi;

    if (fs == NULL || odn == NULL || ndn == NULL || oldname == NULL ||
        newname == NULL) {
        return VFS_EINVAL;
    }
    if (odn->type != VFS_NODE_DIR || ndn->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    if (newlen == 0 || newlen >= VFS_NAME_MAX) {
        return VFS_ENAMETOOLONG;
    }
    odi = node_index(fs, odn);
    ndi = node_index(fs, ndn);
    ei = find_ent(fs, odi, oldname, oldlen);
    if (ei == RAMFS_NO_ENT) {
        return VFS_ENOENT;
    }
    if (find_ent(fs, ndi, newname, newlen) != RAMFS_NO_ENT) {
        return VFS_EEXIST;
    }
    fs->ents[ei].parent = ndi;
    str_copy_n(fs->ents[ei].name, newname, newlen);
    fs->nodes[fs->ents[ei].node].parent = ndi;
    return VFS_OK;
}

static int ramfs_readdir(void *ctx, struct vfs_node *dir, u32 index, char *name,
                         u32 name_max, struct vfs_stat *st)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *dn = node_of(fs, dir);
    u32 i;
    u32 count = 0;

    if (fs == NULL || dn == NULL || name == NULL || st == NULL) {
        return VFS_EINVAL;
    }
    if (dn->type != VFS_NODE_DIR) {
        return VFS_ENOTDIR;
    }
    for (i = 0; i < RAMFS_MAX_ENTS; i++) {
        struct ramfs_ent *e = &fs->ents[i];
        struct ramfs_node *n;
        u32 nlen;

        if (e->used == 0 || e->parent != node_index(fs, dn)) {
            continue;
        }
        if (count == index) {
            n = &fs->nodes[e->node];
            nlen = str_len(e->name);
            if (nlen + 1u > name_max) {
                return VFS_ENAMETOOLONG;
            }
            str_copy_n(name, e->name, nlen);
            st->type = n->type;
            st->mode = ifmt_of(n->type) | (n->mode & 0777u);
            st->size = n->size;
            st->ino = n->ino;
            return 1;
        }
        count++;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* ops：文件数据                                                              */
/* ------------------------------------------------------------------------- */

static int ramfs_read(void *ctx, struct vfs_node *vn, u64 off, void *buf,
                      u64 len)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);
    u64 done = 0;

    if (fs == NULL || n == NULL || buf == NULL) {
        return VFS_EINVAL;
    }
    if (n->type != VFS_NODE_FILE) {
        return VFS_EISDIR;
    }
    if (off >= n->size) {
        return 0;
    }
    if (len > n->size - off) {
        len = n->size - off;
    }
    while (done < len) {
        u64 cur = off + done;
        u32 idx = (u32)(cur / 4096ULL);
        u32 inpage = (u32)(cur % 4096ULL);
        u32 chain = page_index_at(fs, n->first_page, idx);
        u64 chunk = len - done;

        if (chain == 0) {
            return VFS_EINVAL;
        }
        if (chunk > (u64)(4096u - inpage)) {
            chunk = (u64)(4096u - inpage);
        }
        mem_copy((u8 *)buf + done,
                 (const u8 *)fs->pages[chain - 1].mem + inpage, chunk);
        done += chunk;
    }
    fs->st.reads++;
    return (int)done;
}

static int ramfs_write(void *ctx, struct vfs_node *vn, u64 off, const void *buf,
                       u64 len)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);
    u64 end;
    u32 need;
    u64 done = 0;
    int rc;

    if (fs == NULL || n == NULL || buf == NULL) {
        return VFS_EINVAL;
    }
    if (n->type != VFS_NODE_FILE) {
        return VFS_EISDIR;
    }
    if (len == 0) {
        return 0;
    }
    if (off > RAMFS_MAX_FILE_BYTES ||
        len > RAMFS_MAX_FILE_BYTES - off) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    end = off + len;
    need = (u32)((end + 4095ULL) / 4096ULL);
    rc = ensure_pages(fs, n, need);
    if (rc != VFS_OK) {
        return rc;
    }
    while (done < len) {
        u64 cur = off + done;
        u32 idx = (u32)(cur / 4096ULL);
        u32 inpage = (u32)(cur % 4096ULL);
        u32 chain = page_index_at(fs, n->first_page, idx);
        u64 chunk = len - done;

        if (chain == 0) {
            return VFS_EINVAL;
        }
        if (chunk > (u64)(4096u - inpage)) {
            chunk = (u64)(4096u - inpage);
        }
        mem_copy((u8 *)fs->pages[chain - 1].mem + inpage,
                 (const u8 *)buf + done, chunk);
        done += chunk;
    }
    if (off > n->size) {
        /* 稀疏写留下的洞：新页由分配器清零，这里补旧页尾。 */
        zero_range(fs, n, n->size, off);
    }
    if (end > n->size) {
        n->size = end;
    }
    fill_vnode(fs, n);
    fs->st.writes++;
    return (int)done;
}

static int ramfs_truncate(void *ctx, struct vfs_node *vn, u64 size)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);
    u32 need;
    u64 old_size;
    u32 old_count;

    if (fs == NULL || n == NULL) {
        return VFS_EINVAL;
    }
    if (n->type != VFS_NODE_FILE) {
        return VFS_EISDIR;
    }
    if (size > RAMFS_MAX_FILE_BYTES) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    old_size = n->size;
    old_count = n->page_count;
    need = (u32)((size + 4095ULL) / 4096ULL);
    if (size > old_size) {
        int rc = ensure_pages(fs, n, need);

        if (rc != VFS_OK) {
            return rc;
        }
        if (old_size < size) {
            u64 hi = (u64)old_count * 4096ULL;

            if (hi > size) {
                hi = size;
            }
            if (hi > old_size) {
                zero_range(fs, n, old_size, hi);
            }
        }
    } else if (size < old_size) {
        u64 hi = (u64)need * 4096ULL;

        if (hi > old_size) {
            hi = old_size;
        }
        if (hi > size) {
            zero_range(fs, n, size, hi);
        }
        page_release_tail(fs, n->first_page, need);
        n->page_count = need;
        if (need == 0) {
            n->first_page = 0;
        }
    }
    n->size = size;
    fill_vnode(fs, n);
    fs->st.truncates++;
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* ops：符号链接 / 引用                                                       */
/* ------------------------------------------------------------------------- */

static int ramfs_symlink(void *ctx, struct vfs_node *dir, const char *name,
                         u32 len, const char *target)
{
    struct vfs_node *vn = NULL;
    struct ramfs_node *n;
    u32 tlen;
    int rc;

    if (target == NULL) {
        return VFS_EINVAL;
    }
    tlen = str_len(target);
    if (tlen == 0 || tlen >= VFS_PATH_MAX) {
        return VFS_ENAMETOOLONG;
    }
    rc = ramfs_create(ctx, dir, name, len, VFS_NODE_SYMLINK, 0777u, &vn);
    if (rc != VFS_OK) {
        return rc;
    }
    n = node_of((struct ramfs *)ctx, vn);
    if (n == NULL) {
        return VFS_EINVAL;
    }
    str_copy_n(n->target, target, tlen);
    fill_vnode((struct ramfs *)ctx, n);
    return VFS_OK;
}

static int ramfs_readlink(void *ctx, struct vfs_node *vn, char *buf, u32 max)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);
    u32 len;

    if (fs == NULL || n == NULL || buf == NULL || max == 0) {
        return VFS_EINVAL;
    }
    if (n->type != VFS_NODE_SYMLINK) {
        return VFS_EINVAL;
    }
    len = str_len(n->target);
    if (len + 1u > max) {
        return VFS_ENAMETOOLONG;
    }
    str_copy_n(buf, n->target, len);
    return (int)len;
}

static int ramfs_retain(void *ctx, struct vfs_node *vn)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);

    if (n == NULL) {
        return VFS_EINVAL;
    }
    n->open_refs++;
    return VFS_OK;
}

static void ramfs_release(void *ctx, struct vfs_node *vn)
{
    struct ramfs *fs = (struct ramfs *)ctx;
    struct ramfs_node *n = node_of(fs, vn);

    if (n == NULL) {
        return;
    }
    if (n->open_refs > 0) {
        n->open_refs--;
    }
    if (n->deleted != 0 && n->open_refs == 0) {
        node_free(fs, n);
    }
}

static const struct vfs_fs_ops g_ramfs_ops = {
    ramfs_lookup,
    ramfs_create,
    ramfs_unlink,
    ramfs_rename,
    ramfs_readdir,
    ramfs_read,
    ramfs_write,
    ramfs_truncate,
    ramfs_readlink,
    ramfs_symlink,
    ramfs_retain,
    ramfs_release,
};

/* ------------------------------------------------------------------------- */
/* 对外                                                                       */
/* ------------------------------------------------------------------------- */

const struct vfs_fs_ops *ramfs_ops(void)
{
    return &g_ramfs_ops;
}

void ramfs_init(struct ramfs *fs, const struct fs_env *env)
{
    u32 i;
    u8 *raw = (u8 *)fs;

    if (fs == NULL) {
        return;
    }
    for (i = 0; i < (u32)sizeof(*fs); i++) {
        raw[i] = 0;
    }
    if (env != NULL) {
        fs->env = *env;
    }
    fs->ino_seq = 1;
    fs->nodes[0].used = 1;
    fs->nodes[0].type = VFS_NODE_DIR;
    fs->nodes[0].mode = 0755u;
    fs->nodes[0].parent = RAMFS_ROOT_PARENT;
    fs->nodes[0].ino = 1;
    fs->nodes[0].target[0] = '\0';
    fs->st.nodes_used = 1;
    fs->st.node_peak = 1;
    fill_vnode(fs, &fs->nodes[0]);
}

struct vfs_node *ramfs_root(struct ramfs *fs)
{
    if (fs == NULL) {
        return NULL;
    }
    fill_vnode(fs, &fs->nodes[0]);
    return &fs->nodes[0].vnode;
}

const struct ramfs_stats *ramfs_stat(const struct ramfs *fs)
{
    return (fs != NULL) ? &fs->st : NULL;
}
