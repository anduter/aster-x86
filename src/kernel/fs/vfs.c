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
#include "../core/error.h"
#include "../lib/string.h"

/* 顺序有意义：特殊后端在前，ramfs 的 probe 永远返回 1，作为兜底放最后 */
static const fs_backend_t *const backends[] = {
    &fat16_backend,
    &svc_backend,
    &blk_backend,
    &ramfs_backend
};

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

    for (uint32_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (backends[i]->probe(path, rel)) return backends[i];
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
