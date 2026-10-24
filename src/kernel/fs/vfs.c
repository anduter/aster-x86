/* =============================================================================
 * fs/vfs.c - 文件系统层的后端路由
 *
 * 这一层只做三件事：
 *   1. 按各后端的 probe() 选一个后端（"fat:" -> FAT16，其余 -> ramfs 兜底）；
 *   2. 把 (backend id, handle) 形式的调用转发到对应 ops；
 *   3. 给调用方返回后端 id，让 fd 表只需要记住 (be, handle)。
 *
 * syscall.c / proc 不再出现任何 "if (kind == FD_KIND_FAT16)" 分支；
 * 新增文件系统 = 实现一张 fs_backend_t + 在 backends[] 里加一项。
 * ============================================================================= */
#include "fs.h"
#include "fat16.h"
#include "include/kernel.h"
#include "include/kernel.h"

/* 顺序有意义：特殊后端在前，ramfs 的 probe 永远返回 1，作为兜底放最后 */
typedef struct {
    const fs_backend_t *backend;
    uint32_t            api_version;  /* 后端编写时对应的 FS_BACKEND_API_VERSION */
} fs_backend_reg_t;

static const fs_backend_reg_t backends[] = {
    { &fat16_backend, FS_BACKEND_API_VERSION },
    { &svc_backend,   FS_BACKEND_API_VERSION },
    { &blk_backend,   FS_BACKEND_API_VERSION },
    { &ramfs_backend, FS_BACKEND_API_VERSION }
};

/* 0 = 尚未校验, 1 = 通过, -1 = 拒绝（只打印一次） */
static int backends_checked;

int fs_backend_version_ok(uint32_t declared)
{
    if (declared == 0U) return E_OK;    /* 旧格式：缺省视为 v1 */
    return (declared == FS_BACKEND_API_VERSION) ? E_OK : E_INVAL;
}

int fs_backend_api_check(const fs_backend_t *b, uint32_t declared)
{
    const char *name = (b != 0 && b->name != 0) ? b->name : "?";

    if (fs_backend_version_ok(declared) == E_OK) return E_OK;
    kprintf("vfs: backend %s api_version=%u, expected %u: rejected\n",
            name, (unsigned int)declared,
            (unsigned int)FS_BACKEND_API_VERSION);
    return E_INVAL;
}

/* 注册表校验：新增/升级后端时版本不匹配会让所有 fs_*_ex 返回 -1，
 * 而不是用错布局的 ops 表继续跑。 */
static int backends_validate(void)
{
    if (backends_checked != 0) return (backends_checked > 0) ? 0 : -1;
    for (uint32_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (fs_backend_api_check(backends[i].backend,
                                 backends[i].api_version) != E_OK) {
            backends_checked = -1;
            return -1;
        }
    }
    backends_checked = 1;
    return 0;
}

static uint8_t be_id(const fs_backend_t *b)
{
    if (b == &fat16_backend) return FS_BE_FAT16;
    if (b == &svc_backend) return FS_BE_SVC;
    if (b == &blk_backend) return FS_BE_BLK;
    return FS_BE_RAMFS;
}

static const fs_backend_t *be_from_id(uint8_t id)
{
    if (id == FS_BE_FAT16) return &fat16_backend;
    if (id == FS_BE_SVC) return &svc_backend;
    if (id == FS_BE_BLK) return &blk_backend;
    return &ramfs_backend;
}

static const fs_backend_t *route(const char *path, const char **rel)
{
    if (path == 0) return 0;
    if (backends_validate() != 0) return 0;   /* api_version 不匹配 -> E_INVAL */

    for (uint32_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (backends[i].backend->probe(path, rel)) return backends[i].backend;
    }
    return 0;
}

int fs_is_fat(const char *path)
{
    const char *rel;

    return route(path, &rel) == &fat16_backend;
}

int fs_open_ex(const char *path, uint32_t flags, uint32_t uid, uint32_t gid, uint8_t *be)
{
    const fs_backend_t *b;
    const char *rel;
    int h;

    b = route(path, &rel);
    if (b == 0) return -1;

    h = b->open(rel, flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT), uid, gid);
    if (h < 0) return -1;
    if (be != 0) *be = be_id(b);
    return h;
}

int fs_read_ex(uint8_t be, int h, void *buf, uint32_t len)
{
    return be_from_id(be)->read(h, buf, len);
}

int fs_write_ex(uint8_t be, int h, const void *buf, uint32_t len)
{
    return be_from_id(be)->write(h, buf, len);
}

int fs_close_ex(uint8_t be, int h)
{
    return be_from_id(be)->close(h);
}

int fs_ref_ex(uint8_t be, int h)
{
    return be_from_id(be)->ref(h);
}

int fs_stat_ex(const char *path, fs_stat_t *out, uint8_t *be)
{
    const fs_backend_t *b;
    const char *rel;
    int r;

    if (out == 0) return -1;
    b = route(path, &rel);
    if (b == 0) return -1;

    r = b->stat(rel, out);
    if (r != 0) return -1;
    if (be != 0) *be = be_id(b);
    return 0;
}

int fs_listdir_ex(const char *path, uint32_t index, char *name, uint32_t name_max,
                  fs_stat_t *out, uint8_t *be)
{
    const fs_backend_t *b;
    const char *rel;
    int r;

    if (name == 0) return -1;
    b = route(path, &rel);
    if (b == 0) return -1;

    r = b->listdir(rel, index, name, name_max, out);
    if (r != 0) return -1;
    if (be != 0) *be = be_id(b);
    return 0;
}

int fs_mkdir_ex(const char *path, uint32_t uid, uint32_t gid, uint8_t *be)
{
    const fs_backend_t *b;
    const char *rel;
    int r;

    b = route(path, &rel);
    if (b == 0) return -1;

    r = b->mkdir(rel, uid, gid);
    if (r != 0) return -1;
    if (be != 0) *be = be_id(b);
    return 0;
}

int fs_unlink_ex(const char *path, uint32_t uid, uint32_t gid, uint8_t *be)
{
    const fs_backend_t *b;
    const char *rel;
    int r;

    b = route(path, &rel);
    if (b == 0) return -1;

    r = b->unlink(rel, uid, gid);
    if (r != 0) return -1;
    if (be != 0) *be = be_id(b);
    return 0;
}

/* =============================================================================
 * rename / move
 *
 * 语义（详见 fs.h）：
 *   - 只在同一个后端、同一个卷内改名或移动；跨卷直接 E_INVAL；
 *   - 权限：源父目录和目标父目录都要 w+x，uid==0 绕过；
 *   - 覆盖策略由后端决定：目标文件可替换（ramfs），非空目录拒绝。
 *
 * 权限检查放在 VFS 是为了让所有后端走同一条规则；FAT16 没有 mode/uid/gid
 * （stat 全 0），按权限位判会一律拒绝，所以跳过（FAT 本身没有 Unix 权限模型）。
 * ramfs 后端还会用自己的节点再判一次，属于防御性重复检查。
 * ============================================================================= */

/* 取路径的父目录（去掉末尾 '/' 和最后一个名字）：
 *   "system:/a/b.txt" -> "system:/a"
 *   "system:/a.txt"   -> "system:"（卷根）
 *   "/a.txt"          -> "/"（默认卷根）
 *   "a.txt"           -> 无法推导（返回 0，交给后端判权）
 * 返回 1 表示 out 是有效父路径。 */
static int rename_parent(const char *path, char *out, uint32_t max)
{
    uint32_t n = (uint32_t)strlen(path);

    while (n > 0U && path[n - 1U] == '/') n--;
    while (n > 0U && path[n - 1U] != '/') n--;
    if (n == 0U) return 0;
    if (n + 1U > max) return 0;

    memcpy(out, path, n);
    out[n] = '\0';

    while (n > 1U && out[n - 1U] == '/') out[--n] = '\0';
    return (out[0] != '\0') ? 1 : 0;
}

/* 目录是否允许 uid/gid 写入并穿越。返回 E_OK / E_NOENT / E_ACCES。 */
static int rename_check_dir(const char *dirpath, uint32_t uid, uint32_t gid)
{
    fs_stat_t st;
    uint32_t bits;
    const uint32_t want = FS_ACCESS_WRITE | FS_ACCESS_EXEC;

    if (fs_stat_ex(dirpath, &st, 0) != 0) return E_NOENT;
    if (st.type != FS_TYPE_DIR) return E_NOENT;
    if (uid == 0U) return E_OK;                     /* root 绕过 */

    if (st.uid == uid) bits = (st.mode >> 6) & 7U;
    else if (st.gid == gid) bits = (st.mode >> 3) & 7U;
    else bits = st.mode & 7U;

    return ((bits & want) == want) ? E_OK : E_ACCES;
}

int fs_rename_as(const char *old_path, const char *new_path, uint32_t uid, uint32_t gid)
{
    const fs_backend_t *ob;
    const fs_backend_t *nb;
    const char *orel;
    const char *nrel;
    char oparent[FS_PATH_MAX];
    char nparent[FS_PATH_MAX];
    int r;

    if (old_path == 0 || new_path == 0) return E_INVAL;
    if (old_path[0] == '\0' || new_path[0] == '\0') return E_INVAL;

    ob = route(old_path, &orel);
    nb = route(new_path, &nrel);
    if (ob == 0 || nb == 0) return E_INVAL;
    if (ob != nb) return E_INVAL;                   /* 跨卷 rename 不支持 */
    if (ob->rename == 0) return E_INVAL;            /* svcfs / blkfs 只读 */

    /* 源/目标父目录都要 w+x（root 绕过） */
    if (ob != &fat16_backend) {
        if (rename_parent(old_path, oparent, sizeof(oparent))) {
            r = rename_check_dir(oparent, uid, gid);
            if (r != E_OK) return r;
        }
        if (rename_parent(new_path, nparent, sizeof(nparent))) {
            r = rename_check_dir(nparent, uid, gid);
            if (r != E_OK) return r;
        }
    }

    return ob->rename(orel, nrel, uid, gid);
}

/* 内核内部调用按 root 身份执行 */
int fs_rename(const char *old_path, const char *new_path)
{
    return fs_rename_as(old_path, new_path, 0U, 0U);
}
