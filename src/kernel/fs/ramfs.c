/* =============================================================================
 * ramfs.c - 内存文件系统后端 + 简易 VFS（支持盘符/卷）
 *
 * 结构：
 *   - 卷表：每个卷有自己的根目录和一个最长 32 字节的盘符名（"C"、"D" …）；
 *   - 节点池（文件/目录共用），目录用 parent / first_child / next_sibling 组成树；
 *   - 路径解析：先看有没有 "X:" 盘符前缀，再逐段下钻，支持 "/"、"."、".."；
 *   - 打开文件返回 OBJ_FILE 对象句柄，读写必须带正确类型的句柄；
 *   - 数据直接放在节点里（每文件 FS_FILE_MAX 字节），所以整卷都在内存中。
 * ============================================================================= */
#include "fs.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "include/dev.h"
#include "include/kernel.h"
#include "include/kernel.h"

/* =============================================================================
 * SMP 串行化
 *
 * 节点池、打开句柄表、卷表都是全局共享状态，BSP/AP 上的用户进程可能同时
 * 调 fs_*。这里用一把全局 fs_lock：先把所有公开入口改名成 *_impl（宏替换，
 * 函数体不用动），文件末尾再生成加锁的包装函数。
 *
 * 为什么不在每个函数体里加锁：公开函数有多条 return 路径，逐条补解锁容易漏；
 * 包装函数只有一条出口，语义上更接近"整个文件系统操作是原子的"。
 * 锁序：fs_lock -> obj_lock（fs_open/fs_close 内部会调对象层），没有反向路径。
 * ============================================================================= */
static spinlock_t fs_lock = SPINLOCK_INIT;

#define fs_mount        fs_mount_impl
#define fs_open         fs_open_impl
#define fs_read         fs_read_impl
#define fs_write        fs_write_impl
#define fs_close        fs_close_impl
#define fs_ref          fs_ref_impl
#define fs_stat         fs_stat_impl
#define fs_mkdir        fs_mkdir_impl
#define fs_unlink       fs_unlink_impl
#define fs_chmod        fs_chmod_impl
#define fs_chown        fs_chown_impl
#define fs_access_as    fs_access_as_impl
#define fs_listdir      fs_listdir_impl
#define fs_format_disk  fs_format_disk_impl
#define fs_sync         fs_sync_impl
#define fs_mount_disk   fs_mount_disk_impl
#define fs_sync_backing fs_sync_backing_impl
#define fs_check        fs_check_impl

/* 公开名已被宏改名为 *_impl；内部调用发生在定义之前，先声明。
 * ABI v1.3 起 impl 接收 fs_cred_t（生效身份 + 附加组 + umask）；
 * 公开的 (uid, gid) 旧入口是"没有附加组、umask=0"的薄包装。 */
int fs_mount_impl(const char *name);
static uint32_t fs_open_impl(const char *path, uint32_t flags, const fs_cred_t *c);
int fs_read_impl(uint32_t handle, void *buf, uint32_t len);
int fs_write_impl(uint32_t handle, const void *buf, uint32_t len);
int fs_close_impl(uint32_t handle);
int fs_ref_impl(uint32_t handle);
int fs_stat_impl(const char *path, fs_stat_t *out);
int fs_mkdir_impl(const char *path, uint32_t uid, uint32_t gid);
int fs_unlink_impl(const char *path, uint32_t uid, uint32_t gid);
int fs_chmod_impl(const char *path, uint32_t mode, const fs_cred_t *c);
int fs_chown_impl(const char *path, uint32_t uid, uint32_t gid, const fs_cred_t *c);
int fs_access_as_impl(const char *path, uint32_t want, uint32_t uid, uint32_t gid);
int fs_access_cred_impl(const char *path, uint32_t want, const fs_cred_t *c);
int fs_stat_cred_impl(const char *path, fs_stat_t *out, const fs_cred_t *c);
int fs_mkdir_cred_impl(const char *path, const fs_cred_t *c);
int fs_unlink_cred_impl(const char *path, const fs_cred_t *c);
int fs_listdir_cred_impl(const char *path, uint32_t index, char *name, uint32_t name_max,
                         fs_stat_t *out, const fs_cred_t *c);
int fs_listdir_impl(const char *path, uint32_t index, char *name,
                    uint32_t name_max, fs_stat_t *out);
int fs_format_disk_impl(const char *blk_name);
int fs_sync_impl(const char *blk_name);
int fs_mount_disk_impl(const char *blk_name);
int fs_sync_backing_impl(void);
int fs_check_impl(char *report, uint32_t max);

/* 权限模型的内部辅助（定义在下面的“文件操作”一节）：路径解析要用到，
 * 这里先声明（fs_node_t 在下面才 typedef，所以放在节点定义之后）。 */

/* 宏会把下面的公开名展开成 *_impl：fs_mount 在 fs_init 里被提前调用。 */
int      fs_mount(const char *name);

/* ---- 节点 ---- */
typedef struct fs_node {
    fs_type_t type;
    char      name[FS_NAME_MAX];
    struct fs_node *parent;
    struct fs_node *first_child;
    struct fs_node *next_sibling;
    uint32_t  size;
    uint32_t  mtime;        /* 最后修改时间（epoch 秒） */
    uint32_t  mode;         /* 权限位（rwxrwxrwx + setuid/setgid/sticky） */
    uint32_t  uid;          /* 属主 */
    uint32_t  gid;          /* 属组 */
    uint8_t   data[FS_FILE_MAX];
} fs_node_t;

static uint32_t perm_bits(const fs_node_t *n, const fs_cred_t *c);
static bool     may_access(const fs_node_t *n, uint32_t want, const fs_cred_t *c);
static bool     may_unlink_in(const fs_node_t *dir, const fs_node_t *node, const fs_cred_t *c);

/* ---- 卷（盘符） ---- */
typedef struct {
    bool      used;
    char      name[FS_DRIVE_MAX];
    fs_node_t *root;
} fs_volume_t;

/* ---- 打开的文件句柄 ---- */
typedef struct {
    bool      used;
    fs_node_t *node;
    uint32_t  offset;
    uint32_t  flags;
    uint32_t  refs;         /* dup2/fork 会增加引用，最后一个 close 才真正释放 */
} fs_file_t;

static fs_node_t nodes[FS_MAX_NODES];
static fs_file_t handles[FS_MAX_OPEN];
static fs_volume_t volumes[FS_MAX_VOLUMES];

/* ==================== 内部辅助 ==================== */

static fs_node_t *node_alloc(void)
{
    for (uint32_t i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].type == FS_TYPE_NONE) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            return &nodes[i];
        }
    }
    return 0;
}

static bool name_eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static fs_node_t *find_child(fs_node_t *dir, const char *name)
{
    if (dir == 0 || dir->type != FS_TYPE_DIR) return 0;

    for (fs_node_t *c = dir->first_child; c != 0; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return 0;
}

static void link_child(fs_node_t *dir, fs_node_t *child)
{
    child->parent = dir;
    child->next_sibling = dir->first_child;
    dir->first_child = child;
}

static void unlink_child(fs_node_t *dir, fs_node_t *child)
{
    fs_node_t **pp = &dir->first_child;

    while (*pp != 0) {
        if (*pp == child) {
            *pp = child->next_sibling;
            child->parent = 0;
            child->next_sibling = 0;
            return;
        }
        pp = &(*pp)->next_sibling;
    }
}

/* ---- 路径解析：驱动器（卷）各自为根，顶层是卷表本身 ---- */

static fs_volume_t *volume_by_name(const char *name)
{
    for (uint32_t i = 0; i < FS_MAX_VOLUMES; i++) {
        if (volumes[i].used && name_eq_ci(volumes[i].name, name)) {
            return &volumes[i];
        }
    }
    return 0;
}

static fs_volume_t *default_volume(void)
{
    return volumes[0].used ? &volumes[0] : 0;
}

/* 解析 "驱动器:路径"：
 *   "system:/bin/shell" -> 卷 system，路径 "bin/shell"
 *   "system:"           -> 卷 system，路径 ""（卷根）
 *   "/docs/a.txt"       -> 默认卷，路径 "docs/a.txt"
 *   ""                  -> 0（模块层，用于列出驱动器） */
static fs_volume_t *path_volume(const char *path, const char **rel)
{
    const char *colon;
    char name[FS_DRIVE_MAX];
    uint32_t n;

    if (path == 0) return 0;
    if (path[0] == '\0') return 0;              /* 模块层 */

    colon = strchr(path, ':');
    if (colon != 0) {
        n = (uint32_t)(colon - path);
        if (n == 0U || n >= FS_DRIVE_MAX) return 0;
        for (uint32_t i = 0; i < n; i++) name[i] = path[i];
        name[n] = '\0';

        *rel = colon + 1;
        while (**rel == '/') (*rel)++;
        return volume_by_name(name);
    }

    *rel = path;
    while (**rel == '/') (*rel)++;
    return default_volume();
}

/* 在指定卷里逐段解析相对路径；空路径返回卷根。
 *
 * ABI v1.3 起按生效身份检查"沿途目录的 x 位"：访问 system:/a/b/c 时，
 * a、b 每一层都要对当前 cred 有 x（否则 E_ACCES），c 由调用方按读/写判权。
 * 这是 chroot/路径穿越防护的基础：没有 x 就进不去，也不需要额外过滤 ".."。
 *
 * why（可为 0）：0 = 成功；1 = 不存在；2 = 中间目录缺 x（E_ACCES）。
 * 传 c == 0 表示内核内部 root 访问（跳过沿途检查）。 */
static fs_node_t *walk_rel_ex(fs_volume_t *vol, const char *rel,
                             const fs_cred_t *c, int *why)
{
    char comp[FS_NAME_MAX];
    const char *p = (rel != 0) ? rel : "";
    fs_node_t *cur = (vol != 0) ? vol->root : 0;

    if (why != 0) *why = 1;

    while (cur != 0 && *p != '\0') {
        uint32_t n = 0;

        while (*p == '/') p++;
        if (*p == '\0') break;

        while (*p != '\0' && *p != '/' && n + 1U < FS_NAME_MAX) {
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        if (cur->type != FS_TYPE_DIR) return 0;

        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (cur->parent != 0) cur = cur->parent;
            continue;
        }

        /* 进入 cur 之前要求 x（root 跳过） */
        if (!may_access(cur, FS_ACCESS_EXEC, c)) {
            if (why != 0) *why = 2;
            return 0;
        }

        cur = find_child(cur, comp);
    }

    if (cur != 0 && why != 0) *why = 0;
    return cur;
}

/* 把路径拆成“卷 + 父目录 + 末段名字”，用于创建/删除。
 * c 非 0 时按生效身份检查沿途 x（父目录自己的 w+x 由调用方判）。 */
static int split_parent_ex(const char *path, fs_volume_t **vol_out,
                           fs_node_t **dir_out, char *name_out,
                           const fs_cred_t *c, int *why)
{
    char buf[FS_PATH_MAX];
    fs_volume_t *vol;
    const char *rel;
    uint32_t n, slash;

    if (why != 0) *why = 1;
    if (path == 0 || path[0] == '\0') return E_INVAL;

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    if (strlen(rel) >= sizeof(buf)) return E_INVAL;
    strcpy(buf, rel);

    n = (uint32_t)strlen(buf);
    while (n > 0U && buf[n - 1U] == '/') buf[--n] = '\0';
    if (n == 0U) return E_INVAL;                /* 卷根不能建/删 */

    slash = n;
    while (slash > 0U && buf[slash - 1U] != '/') slash--;

    if (slash == 0U) {
        /* 卷根下的名字 */
        if (strlen(buf) >= FS_NAME_MAX) return E_INVAL;
        strcpy(name_out, buf);
        *dir_out = vol->root;
        *vol_out = vol;
        if (why != 0) *why = 0;
        return E_OK;
    }

    buf[slash - 1U] = '\0';
    if (strlen(&buf[slash]) >= FS_NAME_MAX) return E_INVAL;
    strcpy(name_out, &buf[slash]);

    if (strcmp(name_out, ".") == 0 || strcmp(name_out, "..") == 0) return E_INVAL;
    if (strchr(name_out, ':') != 0) return E_INVAL;

    *dir_out = walk_rel_ex(vol, buf, c, why);
    *vol_out = vol;
    if (*dir_out == 0 || (*dir_out)->type != FS_TYPE_DIR) return E_NOENT;
    return E_OK;
}

static fs_file_t *handle_from(uint32_t handle)
{
    void *data = 0;

    if (obj_deref(handle, OBJ_FILE, &data) != E_OK) return 0;
    return (fs_file_t *)data;
}

/* 节点是否还有打开的句柄。删除一个仍被引用的节点会把槽位清零，
 * node_alloc() 随后可能把同一槽位复用给新文件——旧句柄就会静默读写到
 * 别人的数据（跨文件数据泄漏/状态损坏）。所以删除前必须拒绝。 */
static bool node_is_open(const fs_node_t *node)
{
    for (uint32_t i = 0; i < FS_MAX_OPEN; i++) {
        if (handles[i].used && handles[i].node == node) return true;
    }
    return false;
}

/* ==================== 卷管理 ==================== */

void fs_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(handles, 0, sizeof(handles));
    memset(volumes, 0, sizeof(volumes));

    /* 顶层就是卷表本身：每个卷的根节点就是驱动器根，没有虚拟的 "/" */
    if (fs_mount("system") != E_OK) {
        /* 连默认卷都建不出来，文件系统不可用 */
    }
}

int fs_mount(const char *name)
{
    fs_volume_t *vol = 0;
    fs_node_t *root;
    uint32_t len;

    if (name == 0) return E_INVAL;
    len = (uint32_t)strlen(name);
    if (len == 0U || len >= FS_DRIVE_MAX) return E_INVAL;
    if (strchr(name, ':') != 0 || strchr(name, '/') != 0) return E_INVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return E_INVAL;
    if (volume_by_name(name) != 0) return E_EXIST;

    for (uint32_t i = 0; i < FS_MAX_VOLUMES; i++) {
        if (!volumes[i].used) {
            vol = &volumes[i];
            break;
        }
    }
    if (vol == 0) return E_FULL;

    root = node_alloc();
    if (root == 0) return E_FULL;

    /* 驱动器根节点，名字就是驱动号 */
    root->type = FS_TYPE_DIR;
    strcpy(root->name, name);
    root->mtime = rtc_now();
    root->mode = FS_MODE_DIR_DEFAULT;
    root->uid = 0;
    root->gid = 0;

    memset(vol, 0, sizeof(*vol));
    vol->used = true;
    strcpy(vol->name, name);
    vol->root = root;
    return E_OK;
}

uint32_t fs_volume_count(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < FS_MAX_VOLUMES; i++) {
        if (volumes[i].used) n++;
    }
    return n;
}

int fs_volume_name(uint32_t index, char *buf, uint32_t name_max)
{
    uint32_t seen = 0;

    if (buf == 0 || name_max == 0U) return E_INVAL;

    for (uint32_t i = 0; i < FS_MAX_VOLUMES; i++) {
        if (!volumes[i].used) continue;
        if (seen == index) {
            strncpy(buf, volumes[i].name, name_max - 1U);
            buf[name_max - 1U] = '\0';
            return E_OK;
        }
        seen++;
    }
    return E_NOENT;
}

/* ==================== 文件操作 ==================== */

/* ---- 权限模型（ABI v1.3：生效身份 + 附加组） ----
 * 判定顺序与 Linux 一致：euid == 属主 -> user 位；否则 egid 或任一附加组
 * == 属组 -> group 位；否则 other 位。euid == 0 保留 CAP_DAC_OVERRIDE。 */

/* 按“属主 / 属组 / 其他人”取出 3 位权限 */
static uint32_t perm_bits(const fs_node_t *n, const fs_cred_t *c)
{
    if (c != 0 && c->euid == n->uid) return (n->mode >> 6) & 7U;
    if (c != 0 && fs_cred_in_group(c, n->gid)) return (n->mode >> 3) & 7U;
    return n->mode & 7U;
}

static bool may_access(const fs_node_t *n, uint32_t want, const fs_cred_t *c)
{
    if (n == 0) return false;
    if (fs_cred_is_root(c)) return true;        /* root 绕过权限 */
    if (want == 0U) return true;
    return (perm_bits(n, c) & want) == want;
}

/* sticky 目录（/tmp 0755+01000）的删除规则：只有文件属主、目录属主或 root
 * 能删除/改名目录里的项。没有 sticky 时只看目录的 w+x（由调用方先判）。 */
static bool may_unlink_in(const fs_node_t *dir, const fs_node_t *node, const fs_cred_t *c)
{
    if (fs_cred_is_root(c)) return true;
    if ((dir->mode & FS_MODE_STICKY) == 0U) return true;
    if (node->uid == c->euid) return true;
    if (dir->uid == c->euid) return true;
    return false;
}

void fs_cred_from_ids(fs_cred_t *out, uint32_t uid, uint32_t gid)
{
    if (out == 0) return;
    memset(out, 0, sizeof(*out));
    out->euid = uid;
    out->egid = gid;
    out->ngroups = 0;
    out->umask = 0U;
}

bool fs_cred_in_group(const fs_cred_t *c, uint32_t gid)
{
    if (c == 0) return false;
    if (c->egid == gid) return true;
    for (uint32_t i = 0; i < c->ngroups && i < FS_CRED_GROUPS_MAX; i++) {
        if (c->groups[i] == gid) return true;
    }
    return false;
}

bool fs_cred_is_root(const fs_cred_t *c)
{
    return (c == 0) || (c->euid == 0U);
}

/* 建文件/目录时请求的权限位与 umask：
 *   mode & ~umask，且 setuid/setgid 位不会被 umask 清掉（Linux 语义） */
static uint32_t new_mode_umasked(uint32_t base, const fs_cred_t *c)
{
    uint32_t mask = (c != 0) ? (c->umask & 0777U) : 0U;

    return (base & ~mask) & FS_MODE_ALL;
}

static int access_impl(const char *path, uint32_t want, const fs_cred_t *c)
{
    fs_volume_t *vol;
    const char *rel;
    fs_node_t *n;
    int why = 0;

    if (path == 0) return E_INVAL;

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    n = walk_rel_ex(vol, rel, c, &why);
    if (n == 0) return (why == 2) ? E_ACCES : E_NOENT;

    return may_access(n, want, c) ? E_OK : E_ACCES;
}

int fs_access_as_impl(const char *path, uint32_t want, uint32_t uid, uint32_t gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    return access_impl(path, want, &c);
}

int fs_access_cred_impl(const char *path, uint32_t want, const fs_cred_t *c)
{
    return access_impl(path, want, c);
}

int fs_chmod_impl(const char *path, uint32_t mode, const fs_cred_t *c)
{
    if (sip_guard_write(path) != E_OK) return E_ACCES;
    fs_volume_t *vol;
    const char *rel;
    fs_node_t *n;
    uint32_t why = 0;

    (void)why;
    if (path == 0) return E_INVAL;

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    n = walk_rel_ex(vol, rel, c, 0);
    if (n == 0) return E_NOENT;

    /* 只有 root 或属主能改权限 */
    if (!fs_cred_is_root(c) && (c == 0 || c->euid != n->uid)) return E_ACCES;

    /* 保留 setuid/setgid/sticky（P2）；只有 root 能设置 setuid/setgid 位，
     * 属主 chmod 时静默丢弃这两位（Linux 语义：非 root 的 chmod 会清 04000/02000
     * 当文件属主不是自己……这里简化为：非 root 只能改低 9 位）。 */
    if (fs_cred_is_root(c)) {
        n->mode = mode & FS_MODE_ALL;
    } else {
        n->mode = (n->mode & FS_MODE_SPECIAL) | (mode & FS_MODE_PERM);
        if (n->uid != c->euid) n->mode &= ~FS_MODE_SPECIAL;
    }
    n->mtime = rtc_now();
    return E_OK;
}

int fs_chown_impl(const char *path, uint32_t uid, uint32_t gid, const fs_cred_t *c)
{
    if (sip_guard_write(path) != E_OK) return E_ACCES;
    fs_volume_t *vol;
    const char *rel;
    fs_node_t *n;

    if (path == 0) return E_INVAL;

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    n = walk_rel_ex(vol, rel, c, 0);
    if (n == 0) return E_NOENT;

    if (!fs_cred_is_root(c)) return E_ACCES;    /* 只有 root 能改属主 */

    n->uid = uid;
    n->gid = gid;
    /* chown 清 setuid/setgid（P2）：避免普通用户借 root 的 chown 保留提权位。
     * 内核对 chown 请求也遵循这个规则（Linux 的 chown(2) 语义）。 */
    n->mode &= ~(FS_MODE_SETUID | FS_MODE_SETGID);
    return E_OK;
}

static uint32_t fs_open_impl(const char *path, uint32_t flags, const fs_cred_t *c)
{
    if ((flags & (FS_OPEN_WRITE | FS_OPEN_CREAT)) != 0U &&
        sip_guard_write(path) != E_OK) {
        return 0;
    }
    fs_volume_t *vol = 0;
    const char *rel;
    fs_node_t *node;
    fs_node_t *dir = 0;
    char name[FS_NAME_MAX];
    int why = 0;

    if (path == 0 || path[0] == '\0') return 0;

    vol = path_volume(path, &rel);
    if (vol == 0) return 0;

    node = walk_rel_ex(vol, rel, c, &why);

    if (node == 0) {
        /* 不存在：只有带 FS_OPEN_CREAT 才创建普通文件 */
        if ((flags & FS_OPEN_CREAT) == 0U) return 0;
        if (why == 2) return 0;                 /* 沿途目录缺 x */
        if (split_parent_ex(path, &vol, &dir, name, c, &why) != E_OK) return 0;

        /* 在目录里创建文件需要该目录的写+执行权限 */
        if (!may_access(dir, FS_ACCESS_WRITE | FS_ACCESS_EXEC, c)) {
            return 0;
        }

        node = node_alloc();
        if (node == 0) return 0;

        node->type = FS_TYPE_FILE;
        strcpy(node->name, name);
        node->size = 0;
        node->mode = new_mode_umasked(FS_MODE_FILE_DEFAULT, c);
        node->uid = (c != 0) ? c->euid : 0U;
        node->gid = (c != 0) ? c->egid : 0U;
        node->mtime = rtc_now();
        link_child(dir, node);
    } else if (node->type == FS_TYPE_DIR) {
        return 0;                       /* 不支持打开目录 */
    } else {
        /* 已存在的文件：按打开标志检查读写权限 */
        uint32_t want = 0;

        if ((flags & FS_OPEN_READ) != 0U) want |= FS_ACCESS_READ;
        if ((flags & FS_OPEN_WRITE) != 0U) want |= FS_ACCESS_WRITE;

        if (!may_access(node, want, c)) return 0;
    }

    for (uint32_t i = 0; i < FS_MAX_OPEN; i++) {
        if (!handles[i].used) {
            handles[i].used = true;
            handles[i].node = node;
            handles[i].offset = 0;
            handles[i].flags = flags;
            handles[i].refs = 1;

            uint32_t h = obj_create(OBJ_FILE, &handles[i]);
            if (h == 0) {
                handles[i].used = false;
                return 0;
            }
            return h;
        }
    }
    return 0;                           /* 打开的文件太多 */
}

int fs_read(uint32_t handle, void *buf, uint32_t len)
{
    fs_file_t *f = handle_from(handle);
    uint32_t n;

    if (f == 0 || buf == 0) return E_TYPE;
    if ((f->flags & FS_OPEN_READ) == 0U) return E_INVAL;

    /* 偏移超出节点当前大小说明节点被截断/回收过：无符号相减会下溢成巨大值，
     * 直接拒绝，避免越界读（正常路径 offset 永远 <= size）。 */
    if (f->offset > f->node->size) return E_INVAL;

    n = f->node->size - f->offset;
    if (n > len) n = len;

    memcpy(buf, f->node->data + f->offset, n);
    f->offset += n;
    return (int)n;
}

int fs_write(uint32_t handle, const void *buf, uint32_t len)
{
    fs_file_t *f = handle_from(handle);
    uint32_t space;

    if (f == 0 || buf == 0) return E_TYPE;
    if ((f->flags & FS_OPEN_WRITE) == 0U) return E_INVAL;

    /* 超过单文件上限时整次写入失败，一个字节都不写。
     * 不能"截断后返回短计数"：内核侧调用点大多把"返回值 == len"当成功，
     * 截断写入会让文件停在半截状态；而返回 0 又和"成功写了 0 字节"混淆。
     * 也不允许先写满再让下一次失败——那样原文件内容已被部分覆盖。 */
    space = (f->offset < FS_FILE_MAX) ? (FS_FILE_MAX - f->offset) : 0U;
    if (len > space) return E_NOSPC;

    memcpy(f->node->data + f->offset, buf, len);
    f->offset += len;
    if (f->offset > f->node->size) {
        f->node->size = f->offset;
    }
    if (len != 0U) {
        f->node->mtime = rtc_now();     /* 记录最后修改时间 */
    }
    return (int)len;
}

int fs_close(uint32_t handle)
{
    fs_file_t *f = handle_from(handle);

    if (f == 0) return E_TYPE;

    if (f->refs > 1U) {
        int r = obj_close(handle, OBJ_FILE);
        if (r == E_OK) f->refs--;
        return r;                       /* 还有别的 fd 引用它 */
    }

    f->refs = 0;
    f->used = false;
    f->node = 0;
    return obj_close(handle, OBJ_FILE);
}

int fs_ref(uint32_t handle)
{
    fs_file_t *f = handle_from(handle);
    int r;

    if (f == 0) return E_TYPE;
    r = obj_ref(handle, OBJ_FILE);
    if (r != E_OK) return r;
    f->refs++;
    return E_OK;
}

static int stat_impl(const char *path, fs_stat_t *out, const fs_cred_t *c)
{
    fs_volume_t *vol;
    const char *rel;
    fs_node_t *n;
    int why = 0;

    if (path == 0) return E_INVAL;

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    n = walk_rel_ex(vol, rel, c, &why);
    if (n == 0) return (why == 2) ? E_ACCES : E_NOENT;
    if (out != 0) {
        out->type = n->type;
        out->size = (n->type == FS_TYPE_FILE) ? n->size : 0U;
        out->mtime = n->mtime;
        out->mode = n->mode;
        out->uid = n->uid;
        out->gid = n->gid;
    }
    return E_OK;
}

int fs_stat(const char *path, fs_stat_t *out)
{
    return stat_impl(path, out, 0);
}

int fs_stat_cred_impl(const char *path, fs_stat_t *out, const fs_cred_t *c)
{
    return stat_impl(path, out, c);
}

static int mkdir_impl(const char *path, const fs_cred_t *c)
{
    if (sip_guard_write(path) != E_OK) return E_ACCES;
    fs_volume_t *vol = 0;
    fs_node_t *dir = 0;
    fs_node_t *node;
    char name[FS_NAME_MAX];
    int why = 0;
    int r = split_parent_ex(path, &vol, &dir, name, c, &why);

    if (r != E_OK) return (why == 2) ? E_ACCES : r;
    if (!may_access(dir, FS_ACCESS_WRITE | FS_ACCESS_EXEC, c)) {
        return E_ACCES;
    }
    if (find_child(dir, name) != 0) return E_EXIST;

    node = node_alloc();
    if (node == 0) return E_FULL;

    node->type = FS_TYPE_DIR;
    strcpy(node->name, name);
    node->mode = new_mode_umasked(FS_MODE_DIR_DEFAULT, c);
    node->uid = (c != 0) ? c->euid : 0U;
    node->gid = (c != 0) ? c->egid : 0U;
    node->mtime = rtc_now();
    link_child(dir, node);
    return E_OK;
}

int fs_mkdir(const char *path, uint32_t uid, uint32_t gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    return mkdir_impl(path, &c);
}

int fs_mkdir_cred_impl(const char *path, const fs_cred_t *c)
{
    return mkdir_impl(path, c);
}

static int unlink_impl(const char *path, const fs_cred_t *c)
{
    if (sip_guard_write(path) != E_OK) return E_ACCES;
    fs_volume_t *vol = 0;
    fs_node_t *dir = 0;
    fs_node_t *node;
    char name[FS_NAME_MAX];
    int why = 0;
    int r = split_parent_ex(path, &vol, &dir, name, c, &why);

    if (r != E_OK) return (why == 2) ? E_ACCES : r;
    if (!may_access(dir, FS_ACCESS_WRITE | FS_ACCESS_EXEC, c)) {
        return E_ACCES;
    }

    node = find_child(dir, name);
    if (node == 0) return E_NOENT;
    if (!may_unlink_in(dir, node, c)) return E_ACCES;   /* sticky 目录 */
    if (node_is_open(node)) return E_BUSY;
    if (node->type == FS_TYPE_DIR && node->first_child != 0) return E_NOSPC;

    unlink_child(dir, node);
    memset(node, 0, sizeof(*node));
    return E_OK;
}

int fs_unlink(const char *path, uint32_t uid, uint32_t gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    return unlink_impl(path, &c);
}

int fs_unlink_cred_impl(const char *path, const fs_cred_t *c)
{
    return unlink_impl(path, c);
}

static int listdir_impl(const char *path, uint32_t index,
                        char *name, uint32_t name_max, fs_stat_t *out,
                        const fs_cred_t *c)
{
    fs_volume_t *vol;
    const char *rel;
    fs_node_t *dir;
    fs_node_t *c_;
    uint32_t i = 0;
    int why = 0;

    if (path == 0) return E_INVAL;

    /* 空路径或 "/"：模块层，列出所有驱动器 */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        uint32_t seen = 0;

        for (uint32_t v = 0; v < FS_MAX_VOLUMES; v++) {
            if (!volumes[v].used) continue;
            if (seen == index) {
                if (name != 0 && name_max > 0U) {
                    strncpy(name, volumes[v].name, name_max - 1U);
                    name[name_max - 1U] = '\0';
                }
                if (out != 0) {
                    out->type = FS_TYPE_DIR;
                    out->size = 0U;
                }
                return E_OK;
            }
            seen++;
        }
        return E_NOENT;
    }

    vol = path_volume(path, &rel);
    if (vol == 0) return E_NOENT;

    dir = walk_rel_ex(vol, rel, c, &why);
    if (dir == 0) return (why == 2) ? E_ACCES : E_NOENT;
    if (dir->type != FS_TYPE_DIR) return E_NOENT;
    if (!may_access(dir, FS_ACCESS_READ | FS_ACCESS_EXEC, c)) return E_ACCES;

    for (c_ = dir->first_child; c_ != 0; c_ = c_->next_sibling, i++) {
        if (i == index) {
            if (name != 0 && name_max > 0U) {
                strncpy(name, c_->name, name_max - 1U);
                name[name_max - 1U] = '\0';
            }
            if (out != 0) {
                out->type = c_->type;
                out->size = (c_->type == FS_TYPE_FILE) ? c_->size : 0U;
                out->mtime = c_->mtime;
                out->mode = c_->mode;
                out->uid = c_->uid;
                out->gid = c_->gid;
            }
            return E_OK;
        }
    }
    return E_NOENT;
}

int fs_listdir(const char *path, uint32_t index,
               char *name, uint32_t name_max, fs_stat_t *out)
{
    return listdir_impl(path, index, name, name_max, out, 0);
}

int fs_listdir_cred_impl(const char *path, uint32_t index, char *name, uint32_t name_max,
                         fs_stat_t *out, const fs_cred_t *c)
{
    return listdir_impl(path, index, name, name_max, out, c);
}

/* ==================== rename / move ==================== */

/* 目标目录是否位于 node 的子树里（含 node 自己）：防止把目录移进自己的内部，
 * 那会形成环（父链走不到卷根），之后的 fsck/序列化都会卡住。 */
static bool dir_is_inside(fs_node_t *dir, fs_node_t *node)
{
    uint32_t guard = 0;

    while (dir != 0 && guard++ < FS_MAX_NODES) {
        if (dir == node) return true;
        dir = dir->parent;
    }
    return false;
}

/* ramfs 的 rename 实现（调用方已持 fs_lock）。
 *
 * 覆盖策略（与 POSIX rename 接近，但显式写死）：
 *   - 目标不存在：直接改名/移动；
 *   - 目标是文件、源也是文件：允许替换；目标仍被打开则 E_BUSY
 *     （槽位会被 memset 复用，旧句柄会读写到别人的数据）；
 *   - 目标是空目录、源也是目录：允许替换；
 *   - 目标是非空目录、或源/目标类型不一致：E_EXIST；
 *   - 把目录移动到自己的子树里：E_INVAL；
 *   - 跨卷（system: -> data:）：E_INVAL，VFS 层也会先拒一次。
 * 源文件即使被打开也允许改名：句柄指向节点而不是名字，改名后照样读写。 */
static int ramfs_rename_impl(const char *oldrel, const char *newrel,
                             const fs_cred_t *c)
{
    /* 源和目标任一在受保护目录都拒绝（rename 会同时改两边目录项） */
    if (sip_guard_write(oldrel) != E_OK || sip_guard_write(newrel) != E_OK) {
        return E_ACCES;
    }
    fs_volume_t *ovol = 0;
    fs_volume_t *nvol = 0;
    fs_node_t *odir = 0;
    fs_node_t *ndir = 0;
    fs_node_t *node;
    fs_node_t *target;
    char oname[FS_NAME_MAX];
    char nname[FS_NAME_MAX];
    int why = 0;
    int r;

    r = split_parent_ex(oldrel, &ovol, &odir, oname, c, &why);
    if (r != E_OK) return E_NOENT;              /* 父路径不合法/不存在 */

    node = find_child(odir, oname);
    if (node == 0) return E_NOENT;

    r = split_parent_ex(newrel, &nvol, &ndir, nname, c, &why);
    if (r != E_OK) return E_NOENT;
    if (nvol != ovol) return E_INVAL;           /* 跨卷 */

    if (!may_access(odir, FS_ACCESS_WRITE | FS_ACCESS_EXEC, c)) return E_ACCES;
    if (!may_access(ndir, FS_ACCESS_WRITE | FS_ACCESS_EXEC, c)) return E_ACCES;
    /* sticky 源目录：非属主不能把别人的文件搬走 */
    if (!may_unlink_in(odir, node, c)) return E_ACCES;

    if (node->type == FS_TYPE_DIR && dir_is_inside(ndir, node)) return E_INVAL;

    target = find_child(ndir, nname);
    if (target == node) return E_OK;            /* 同一节点：幂等，不改名 */

    if (target != 0) {
        if (target->type == FS_TYPE_DIR) {
            if (node->type != FS_TYPE_DIR) return E_EXIST;   /* 文件不能顶掉目录 */
            if (target->first_child != 0) return E_EXIST;    /* 非空目录 */
        } else if (node->type == FS_TYPE_DIR) {
            return E_EXIST;                                  /* 目录不能顶掉文件 */
        }
        if (target->type == FS_TYPE_FILE && node_is_open(target)) return E_BUSY;
        if (!may_unlink_in(ndir, target, c)) return E_ACCES;  /* sticky 目标目录 */

        unlink_child(ndir, target);
        memset(target, 0, sizeof(*target));
    }

    unlink_child(odir, node);
    strncpy(node->name, nname, FS_NAME_MAX - 1U);
    node->name[FS_NAME_MAX - 1U] = '\0';
    link_child(ndir, node);

    odir->mtime = rtc_now();
    ndir->mtime = rtc_now();
    return E_OK;
}

/* 加锁包装：和公开 fs_* 一样，整次 rename 是原子的 */
static int ramfs_rename_locked(const char *oldrel, const char *newrel,
                               const fs_cred_t *c)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = ramfs_rename_impl(oldrel, newrel, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

/* VFS 的 fs_rename_cred 路由到这里（ramfs 的 rel 就是原始路径） */
int ramfs_rename_cred(const char *oldrel, const char *newrel, const fs_cred_t *c)
{
    return ramfs_rename_locked(oldrel, newrel, c);
}

/* ==================== 持久化：磁盘格式 ==================== */

/* 当前用于持久化的块设备名（ata0 / ram0），由启动流程设置 */
static char backing_name[FS_DRIVE_MAX];

void fs_set_backing(const char *name)
{
    if (name == 0) {
        backing_name[0] = '\0';
        return;
    }
    strncpy(backing_name, name, FS_DRIVE_MAX - 1U);
    backing_name[FS_DRIVE_MAX - 1U] = '\0';
}

const char *fs_backing(void)
{
    return backing_name;
}

int fs_sync_backing(void)
{
    if (backing_name[0] == '\0') return E_NOENT;
    return fs_sync(backing_name);
}

/* ==================== 一致性检查（fsck） ==================== */

/* 检查：类型合法、大小不越界、孩子指针的 parent 回指正确、没有环。
 * 返回错误数；report 里写一行摘要。 */
/* 已用节点数（诊断用） */
unsigned int fs_node_count(void)
{
    unsigned int n = 0;

    for (uint32_t i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].type != FS_TYPE_NONE) n++;
    }
    return n;
}int fs_check(char *report, uint32_t max)
{    uint32_t node_count = 0, file_count = 0, dir_count = 0, errors = 0;

    for (uint32_t i = 0; i < FS_MAX_NODES; i++) {
        fs_node_t *n = &nodes[i];

        if (n->type == FS_TYPE_NONE) continue;
        node_count++;

        if (n->type == FS_TYPE_FILE) {
            file_count++;
            if (n->size > FS_FILE_MAX) errors++;
        } else if (n->type == FS_TYPE_DIR) {
            dir_count++;
            if (n->size != 0U) errors++;
        } else {
            errors++;
        }

        /* 子节点必须回指自己 */
        for (fs_node_t *c = n->first_child; c != 0; c = c->next_sibling) {
            if (c->parent != n) errors++;
            if (c == n) errors++;
        }

        /* 向上走，最多 64 层；超过说明有环或父链损坏 */
        {
            fs_node_t *p = n->parent;
            uint32_t depth = 0;
            while (p != 0 && depth < 64U) { p = p->parent; depth++; }
            if (depth >= 64U) errors++;
        }
    }

    if (report != 0 && max > 0U) {
        char tmp[96];
        ksnprintf(tmp, sizeof(tmp), "nodes=%u files=%u dirs=%u errors=%u",
                  (unsigned int)node_count, (unsigned int)file_count,
                  (unsigned int)dir_count, (unsigned int)errors);
        strncpy(report, tmp, max - 1U);
        report[max - 1U] = '\0';
    }
    return (int)errors;
}

/* 磁盘布局（扇区 512 字节）：
 *   0        超级块
 *   1..12    inode 表（64 × 92 字节）
 *   13..511  数据区
 */
#define DISK_MAGIC       "XFS1"
#define DISK_VERSION     4U
#define DISK_INODE_MAX   64U
#define DISK_INODE_LBA   1U
#define DISK_INODE_SECT  14U   /* 64 × 108 = 6912 字节 → 14 扇区 */
#define DISK_DATA_LBA    (DISK_INODE_LBA + DISK_INODE_SECT)

typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t inode_count;
    uint32_t inode_size;
    uint32_t inode_lba;
    uint32_t data_lba;
    uint32_t volume_count;
    char     volumes[FS_MAX_VOLUMES][FS_DRIVE_MAX];
} disk_super_t;

typedef struct {
    uint32_t used;
    uint32_t type;
    uint32_t parent;      /* 父 inode 下标；卷根为 0xFFFFFFFF */
    uint32_t size;
    uint32_t data_lba;
    uint32_t volume;
    uint32_t mtime;       /* 最后修改时间（epoch 秒） */
    uint32_t mode;        /* 权限位 */
    uint32_t uid;         /* 属主 */
    uint32_t gid;         /* 属组 */
    char     name[FS_NAME_MAX];
    uint8_t  pad[3];      /* 对齐到 4 字节：40 + 65 + 3 = 108 */
} disk_inode_t;

static disk_inode_t dump_inodes[DISK_INODE_MAX];
static uint32_t     dump_count;

/* 深度优先遍历：父节点一定先于子节点编号 */
static int dump_tree(fs_node_t *n, uint32_t parent, uint32_t vol,
                     block_device_t *blk, uint32_t *lba)
{
    disk_inode_t *in;
    uint32_t idx;

    if (dump_count >= DISK_INODE_MAX) return E_FULL;

    idx = dump_count++;
    in = &dump_inodes[idx];
    memset(in, 0, sizeof(*in));
    in->used = 1U;
    in->type = (uint32_t)n->type;
    in->parent = parent;
    in->size = n->size;
    in->volume = vol;
    in->mtime = n->mtime;
    in->mode = n->mode;
    in->uid = n->uid;
    in->gid = n->gid;
    strncpy(in->name, n->name, FS_NAME_MAX - 1U);
    in->name[FS_NAME_MAX - 1U] = '\0';

    if (n->type == FS_TYPE_FILE && n->size > 0U) {
        uint32_t sectors = (n->size + BLK_SECTOR - 1U) / BLK_SECTOR;
        static uint8_t buf[BLK_SECTOR];

        if (*lba + sectors > blk->sector_count) return E_NOSPC;
        in->data_lba = *lba;

        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t off = s * BLK_SECTOR;
            uint32_t nbytes = n->size - off;
            if (nbytes > BLK_SECTOR) nbytes = BLK_SECTOR;
            memset(buf, 0, sizeof(buf));
            memcpy(buf, n->data + off, nbytes);
            if (blk_write(blk, *lba + s, buf) != E_OK) return E_INVAL;
        }
        *lba += sectors;
    }

    for (fs_node_t *c = n->first_child; c != 0; c = c->next_sibling) {
        int r = dump_tree(c, idx, vol, blk, lba);
        if (r != E_OK) return r;
    }
    return E_OK;
}

int fs_format_disk(const char *blk_name)
{
    block_device_t *blk = blk_lookup(blk_name);
    disk_super_t super;
    uint8_t zero[BLK_SECTOR];

    if (blk == 0) return E_NOENT;
    /* 设备至少要能放下超级块 + inode 表 + 一个数据扇区，否则格式不完整。 */
    if (blk->sector_count < DISK_DATA_LBA + 1U) return E_NOSPC;

    memset(&super, 0, sizeof(super));
    super.magic[0] = 'X'; super.magic[1] = 'F'; super.magic[2] = 'S'; super.magic[3] = '1';
    super.version = DISK_VERSION;
    super.sector_size = BLK_SECTOR;
    super.sector_count = blk->sector_count;
    super.inode_count = DISK_INODE_MAX;
    super.inode_size = (uint32_t)sizeof(disk_inode_t);
    super.inode_lba = DISK_INODE_LBA;
    super.data_lba = DISK_DATA_LBA;
    super.volume_count = 0U;

    if (blk_write(blk, 0U, &super) != E_OK) return E_INVAL;

    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < DISK_INODE_SECT; s++) {
        if (blk_write(blk, DISK_INODE_LBA + s, zero) != E_OK) return E_INVAL;
    }
    return E_OK;
}

int fs_sync(const char *blk_name)
{
    block_device_t *blk = blk_lookup(blk_name);
    static disk_super_t super;
    static uint8_t inode_buf[DISK_INODE_SECT * BLK_SECTOR];
    uint32_t data_lba = DISK_DATA_LBA;

    if (blk == 0) return E_NOENT;
    if (blk->sector_count < DISK_DATA_LBA + 1U) return E_NOSPC;

    dump_count = 0;
    memset(dump_inodes, 0, sizeof(dump_inodes));

    /* 每个驱动器根各序列化一遍（卷根 parent = 0xFFFFFFFF） */
    for (uint32_t v = 0; v < FS_MAX_VOLUMES; v++) {
        if (!volumes[v].used) continue;
        int r = dump_tree(volumes[v].root, 0xFFFFFFFFU, v, blk, &data_lba);
        if (r != E_OK) return r;
    }

    memset(&super, 0, sizeof(super));
    super.magic[0] = 'X'; super.magic[1] = 'F'; super.magic[2] = 'S'; super.magic[3] = '1';
    super.version = DISK_VERSION;
    super.sector_size = BLK_SECTOR;
    super.sector_count = blk->sector_count;
    super.inode_count = DISK_INODE_MAX;
    super.inode_size = (uint32_t)sizeof(disk_inode_t);
    super.inode_lba = DISK_INODE_LBA;
    super.data_lba = DISK_DATA_LBA;
    super.volume_count = fs_volume_count();
    for (uint32_t v = 0; v < FS_MAX_VOLUMES; v++) {
        if (volumes[v].used) {
            strncpy(super.volumes[v], volumes[v].name, FS_DRIVE_MAX - 1U);
        }
    }

    memset(inode_buf, 0, sizeof(inode_buf));
    memcpy(inode_buf, dump_inodes, dump_count * sizeof(disk_inode_t));
    for (uint32_t s = 0; s < DISK_INODE_SECT; s++) {
        if (blk_write(blk, DISK_INODE_LBA + s, inode_buf + s * BLK_SECTOR) != E_OK) {
            return E_INVAL;
        }
    }

    return blk_write(blk, 0U, &super);
}

/* 从盘恢复失败时把内存树清回一致的空状态。
 * 恢复是先清空再逐条建节点，中途失败会留下"半个文件树"：卷根已建立、
 * 部分子节点缺失、by_index 里还有悬空父指针；之后再 open/stat 会把它当
 * 正常卷使用，表现为偶发 E_NOENT 或访问到不该存在的目录。
 * 这里选择"清空"而不是恢复旧树：旧树已被覆盖，清空至少是自洽状态。 */
static int mount_disk_fail(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(volumes, 0, sizeof(volumes));
    memset(handles, 0, sizeof(handles));
    return E_INVAL;
}

int fs_mount_disk(const char *blk_name)
{
    block_device_t *blk = blk_lookup(blk_name);
    static disk_super_t super;
    static uint8_t inode_buf[DISK_INODE_SECT * BLK_SECTOR];
    static fs_node_t *by_index[DISK_INODE_MAX];
    disk_inode_t *inodes;

    if (blk == 0) return E_NOENT;
    if (blk_read(blk, 0U, &super) != E_OK) return E_INVAL;
    if (super.magic[0] != 'X' || super.magic[1] != 'F' ||
        super.magic[2] != 'S' || super.magic[3] != '1') {
        return E_INVAL;               /* 不是本文件系统的盘 */
    }
    if (super.inode_count > DISK_INODE_MAX ||
        super.inode_size != sizeof(disk_inode_t)) {
        return E_INVAL;
    }

    /* 几何字段必须自洽且落在设备范围内。否则按 data_lba 读会越界（块层会拒绝），
     * 但那时内存树已经清空、节点建了一半；先在"还没动内存状态"时拒绝。 */
    if (blk->sector_count < DISK_DATA_LBA ||
        super.inode_lba != DISK_INODE_LBA ||
        super.inode_lba + DISK_INODE_SECT > blk->sector_count ||
        super.data_lba < DISK_DATA_LBA ||
        super.data_lba >= blk->sector_count) {
        return E_INVAL;
    }

    /* 清空内存里的树和句柄，准备从盘恢复 */
    memset(nodes, 0, sizeof(nodes));
    memset(volumes, 0, sizeof(volumes));
    memset(handles, 0, sizeof(handles));
    memset(by_index, 0, sizeof(by_index));

    memset(inode_buf, 0, sizeof(inode_buf));
    for (uint32_t s = 0; s < DISK_INODE_SECT; s++) {
        if (blk_read(blk, super.inode_lba + s, inode_buf + s * BLK_SECTOR) != E_OK) {
            return mount_disk_fail();
        }
    }
    inodes = (disk_inode_t *)inode_buf;

    for (uint32_t i = 0; i < super.inode_count; i++) {
        disk_inode_t *in = &inodes[i];
        fs_node_t *n;

        if (in->used == 0U) continue;

        n = node_alloc();
        if (n == 0) {
            (void)mount_disk_fail();
            return E_FULL;
        }

        n->type = (fs_type_t)in->type;
        n->size = in->size;
        n->mtime = in->mtime;
        n->mode = in->mode;
        n->uid = in->uid;
        n->gid = in->gid;
        strncpy(n->name, in->name, FS_NAME_MAX - 1U);
        n->name[FS_NAME_MAX - 1U] = '\0';

        if (n->type == FS_TYPE_FILE && n->size > 0U) {
            uint32_t sectors = (n->size + BLK_SECTOR - 1U) / BLK_SECTOR;
            for (uint32_t s = 0; s < sectors; s++) {
                uint8_t buf[BLK_SECTOR];
                uint32_t off = s * BLK_SECTOR;
                uint32_t nbytes = n->size - off;
                if (nbytes > BLK_SECTOR) nbytes = BLK_SECTOR;
                if (blk_read(blk, in->data_lba + s, buf) != E_OK) {
                    return mount_disk_fail();
                }
                memcpy(n->data + off, buf, nbytes);
            }
        }

        by_index[i] = n;

        if (in->parent == 0xFFFFFFFFU) {
            /* 卷根：用盘上记录的卷号重建卷表 */
            if (in->volume < FS_MAX_VOLUMES) {
                volumes[in->volume].used = true;
                strncpy(volumes[in->volume].name, super.volumes[in->volume],
                        FS_DRIVE_MAX - 1U);
                volumes[in->volume].name[FS_DRIVE_MAX - 1U] = '\0';
                volumes[in->volume].root = n;
            }
        } else if (in->parent < DISK_INODE_MAX && by_index[in->parent] != 0) {
            link_child(by_index[in->parent], n);
        } else {
            return mount_disk_fail();     /* 父节点不存在的孤儿：整次挂载失败 */
        }
    }

    return E_OK;
}

/* =============================================================================
 * 加锁包装：从这里开始恢复公开符号名，所有外部调用者看到的都是"原子"的 fs_*
 * ============================================================================= */
#undef fs_mount
#undef fs_open
#undef fs_read
#undef fs_write
#undef fs_close
#undef fs_ref
#undef fs_stat
#undef fs_mkdir
#undef fs_unlink
#undef fs_chmod
#undef fs_chown
#undef fs_access_as
#undef fs_listdir
#undef fs_format_disk
#undef fs_sync
#undef fs_mount_disk
#undef fs_sync_backing
#undef fs_check

int fs_mount(const char *name)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_mount_impl(name);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

uint32_t fs_open_as(const char *path, uint32_t flags, uint32_t uid, uint32_t gid)
{
    fs_cred_t c;
    uint32_t fl, h;

    fs_cred_from_ids(&c, uid, gid);
    fl = spin_lock_irqsave(&fs_lock);
    h = fs_open_impl(path, flags, &c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return h;
}

/* ABI v1.3：用户态入口，按调用进程的 euid/egid/附加组判权 */
/* 返回后端句柄（>=0），失败 -1：FAT16 等后端的合法句柄可以是 0，
 * 所以失败必须用 -1 表示，不能用 0。 */
int ramfs_open_cred(const char *path, uint32_t flags, const fs_cred_t *c, uint8_t *be)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    uint32_t h = fs_open_impl(path, flags, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    if (h == 0U) return -1;
    if (be != 0) *be = FS_BE_RAMFS;
    return (int)h;
}

/* 内核内部调用（加载 ELF、安装 shell 等）按 root 身份打开 */
uint32_t fs_open(const char *path, uint32_t flags)
{
    return fs_open_as(path, flags, 0U, 0U);
}

int fs_read(uint32_t handle, void *buf, uint32_t len)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_read_impl(handle, buf, len);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_write(uint32_t handle, const void *buf, uint32_t len)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_write_impl(handle, buf, len);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_close(uint32_t handle)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_close_impl(handle);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_ref(uint32_t handle)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_ref_impl(handle);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_stat(const char *path, fs_stat_t *out)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_stat_impl(path, out);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int ramfs_stat_cred(const char *path, fs_stat_t *out, const fs_cred_t *c, uint8_t *be)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_stat_cred_impl(path, out, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    if (r == E_OK && be != 0) *be = FS_BE_RAMFS;
    return r;
}

int fs_mkdir_as(const char *path, uint32_t uid, uint32_t gid)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_mkdir_impl(path, uid, gid);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_mkdir(const char *path)
{
    return fs_mkdir_as(path, 0U, 0U);
}

int ramfs_mkdir_cred(const char *path, const fs_cred_t *c, uint8_t *be)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_mkdir_cred_impl(path, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    if (r == E_OK && be != 0) *be = FS_BE_RAMFS;
    return r;
}

int fs_unlink_as(const char *path, uint32_t uid, uint32_t gid)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_unlink_impl(path, uid, gid);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_unlink(const char *path)
{
    return fs_unlink_as(path, 0U, 0U);
}

int ramfs_unlink_cred(const char *path, const fs_cred_t *c, uint8_t *be)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_unlink_cred_impl(path, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    if (r == E_OK && be != 0) *be = FS_BE_RAMFS;
    return r;
}

int ramfs_chmod_cred(const char *path, uint32_t mode, const fs_cred_t *c)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_chmod_impl(path, mode, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int ramfs_chown_cred(const char *path, uint32_t uid, uint32_t gid, const fs_cred_t *c)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_chown_impl(path, uid, gid, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int ramfs_access_cred(const char *path, uint32_t want, const fs_cred_t *c)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_access_cred_impl(path, want, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int ramfs_listdir_cred(const char *path, uint32_t index, char *name, uint32_t name_max,
                    fs_stat_t *out, const fs_cred_t *c, uint8_t *be)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_listdir_cred_impl(path, index, name, name_max, out, c);
    spin_unlock_irqrestore(&fs_lock, fl);
    if (r == E_OK && be != 0) *be = FS_BE_RAMFS;
    return r;
}

int fs_chmod(const char *path, uint32_t mode, uint32_t uid)
{
    fs_cred_t c;
    uint32_t fl;
    int r;

    fs_cred_from_ids(&c, uid, 0U);
    fl = spin_lock_irqsave(&fs_lock);
    r = fs_chmod_impl(path, mode, &c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_chown_as(const char *path, uint32_t uid, uint32_t gid, uint32_t caller_uid)
{
    fs_cred_t c;
    uint32_t fl;
    int r;

    fs_cred_from_ids(&c, caller_uid, 0U);
    fl = spin_lock_irqsave(&fs_lock);
    r = fs_chown_impl(path, uid, gid, &c);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    return fs_chown_as(path, uid, gid, 0U);
}

int fs_access_as(const char *path, uint32_t want, uint32_t uid, uint32_t gid)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_access_as_impl(path, want, uid, gid);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_listdir(const char *path, uint32_t index,
               char *name, uint32_t name_max, fs_stat_t *out)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_listdir_impl(path, index, name, name_max, out);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_format_disk(const char *blk_name)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_format_disk_impl(blk_name);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_sync(const char *blk_name)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_sync_impl(blk_name);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_mount_disk(const char *blk_name)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_mount_disk_impl(blk_name);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_sync_backing(void)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_sync_backing_impl();
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

int fs_check(char *report, uint32_t max)
{
    uint32_t fl = spin_lock_irqsave(&fs_lock);
    int r = fs_check_impl(report, max);
    spin_unlock_irqrestore(&fs_lock, fl);
    return r;
}

/* =============================================================================
 * 后端适配器：把现有 fs_* 包装成 fs_backend_t 的 ops
 * ============================================================================= */

/* ramfs 接管所有没被别的后端 probe 走的路径（含 "system:/..." 这类卷前缀） */
static int ramfs_probe(const char *path, const char **rel)
{
    if (path == 0) return 0;
    if (rel != 0) *rel = path;
    return 1;
}

static int ramfs_be_open(const char *rel, uint32_t flags, uint32_t uid, uint32_t gid)
{
    uint32_t h = fs_open_as(rel, flags, uid, gid);

    return (h == 0U) ? -1 : (int)h;
}

static int ramfs_be_read(int h, void *buf, uint32_t len)
{
    return fs_read((uint32_t)h, buf, len);
}

static int ramfs_be_write(int h, const void *buf, uint32_t len)
{
    return fs_write((uint32_t)h, buf, len);
}

static int ramfs_be_close(int h) { return fs_close((uint32_t)h); }
static int ramfs_be_ref(int h)   { return fs_ref((uint32_t)h); }

static int ramfs_be_stat(const char *rel, fs_stat_t *out)
{
    return (fs_stat(rel, out) == E_OK) ? 0 : -1;
}

static int ramfs_be_listdir(const char *rel, uint32_t index, char *name,
                            uint32_t name_max, fs_stat_t *out)
{
    return (fs_listdir(rel, index, name, name_max, out) == E_OK) ? 0 : -1;
}

static int ramfs_be_mkdir(const char *rel, uint32_t uid, uint32_t gid)
{
    return (fs_mkdir_as(rel, uid, gid) == E_OK) ? 0 : -1;
}

static int ramfs_be_unlink(const char *rel, uint32_t uid, uint32_t gid)
{
    return (fs_unlink_as(rel, uid, gid) == E_OK) ? 0 : -1;
}

static int ramfs_be_rename(const char *oldrel, const char *newrel,
                           uint32_t uid, uint32_t gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    return ramfs_rename_locked(oldrel, newrel, &c);
}

const fs_backend_t ramfs_backend = {
    "ramfs",
    ramfs_probe,
    ramfs_be_open,
    ramfs_be_read,
    ramfs_be_write,
    ramfs_be_close,
    ramfs_be_ref,
    ramfs_be_stat,
    ramfs_be_listdir,
    ramfs_be_mkdir,
    ramfs_be_unlink,
    ramfs_be_rename
};
