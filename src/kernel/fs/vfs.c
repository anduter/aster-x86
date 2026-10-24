/* =============================================================================
 * fs/vfs.c - 文件系统层的后端路由
 *
 * 这一层只做两件事：
 *   1. 判断一个路径属于哪个后端（目前：'fat:' 前缀 -> FAT16，其余 -> ramfs）；
 *   2. 把 (backend, handle) 形式的调用转发给对应实现。
 *
 * 有了它，syscall.c 里不再出现 `if (kind == FD_KIND_FAT16)` 这种分支：
 * 系统调用层只负责 fd 表，文件系统差异全部留在这一层和各自后端里。
 * ============================================================================= */
#include "fs.h"
#include "fat16.h"
#include "../core/error.h"
#include "../lib/string.h"

/* "fat:/NAME" 或 "fat:NAME" -> NAME；不是 FAT 路径返回 0 */
static const char *fat_rel(const char *path)
{
    if (path == 0) return 0;
    if ((path[0] != 'f' && path[0] != 'F') ||
        (path[1] != 'a' && path[1] != 'A') ||
        (path[2] != 't' && path[2] != 'T') ||
        path[3] != ':') {
        return 0;
    }
    return path + 4;
}

int fs_is_fat(const char *path)
{
    return fat_rel(path) != 0;
}

int fs_open_ex(const char *path, uint32_t flags, uint8_t *be)
{
    uint32_t f = flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT);

    if (path == 0) return -1;

    const char *rel = fat_rel(path);
    if (rel != 0) {
        int h;

        if (!fat16_mounted()) return -1;
        h = fat16_open(rel, f);
        if (h < 0) return -1;
        if (be != 0) *be = FS_BE_FAT16;
        return h;
    }

    {
        uint32_t h = fs_open(path, f);

        if (h == 0U) return -1;
        if (be != 0) *be = FS_BE_RAMFS;
        return (int)h;
    }
}

int fs_read_ex(uint8_t be, int h, void *buf, uint32_t len)
{
    if (be == FS_BE_FAT16) return fat16_read(h, buf, len);
    return fs_read((uint32_t)h, buf, len);
}

int fs_write_ex(uint8_t be, int h, const void *buf, uint32_t len)
{
    if (be == FS_BE_FAT16) return fat16_write(h, buf, len);
    return fs_write((uint32_t)h, buf, len);
}

int fs_close_ex(uint8_t be, int h)
{
    if (be == FS_BE_FAT16) return fat16_close(h);
    return fs_close((uint32_t)h);
}

int fs_ref_ex(uint8_t be, int h)
{
    /* FAT16 后端没有句柄引用计数：dup2/transfer 会得到两个共用同一句柄的 fd，
     * 关一个就把另一个也关了。与其静默出错，不如显式拒绝。 */
    if (be == FS_BE_FAT16) return -1;
    return fs_ref((uint32_t)h);
}

int fs_stat_ex(const char *path, fs_stat_t *out, uint8_t *be)
{
    const char *rel;

    if (path == 0 || out == 0) return -1;

    rel = fat_rel(path);
    if (rel != 0) {
        if (!fat16_mounted() || fat16_stat(rel, out) != E_OK) return -1;
        /* FAT16 没有 Unix 权限位 */
        out->mode = 0;
        out->uid = 0;
        out->gid = 0;
        if (be != 0) *be = FS_BE_FAT16;
        return 0;
    }

    if (fs_stat(path, out) != E_OK) return -1;
    if (be != 0) *be = FS_BE_RAMFS;
    return 0;
}

int fs_listdir_ex(const char *path, uint32_t index, char *name, uint32_t name_max,
                  fs_stat_t *out, uint8_t *be)
{
    const char *rel;

    if (name == 0) return -1;

    rel = fat_rel(path);
    if (rel != 0) {
        if (!fat16_mounted()) return -1;
        if (fat16_listdir(rel, index, name, name_max, out) != E_OK) return -1;
        if (be != 0) *be = FS_BE_FAT16;
        return 0;
    }

    if (fs_listdir(path, index, name, name_max, out) != E_OK) return -1;
    if (be != 0) *be = FS_BE_RAMFS;
    return 0;
}

int fs_mkdir_ex(const char *path, uint8_t *be)
{
    const char *rel;

    if (path == 0) return -1;

    rel = fat_rel(path);
    if (rel != 0) {
        if (!fat16_mounted() || fat16_mkdir(rel) != E_OK) return -1;
        if (be != 0) *be = FS_BE_FAT16;
        return 0;
    }

    if (fs_mkdir(path) != E_OK) return -1;
    if (be != 0) *be = FS_BE_RAMFS;
    return 0;
}

int fs_unlink_ex(const char *path, uint8_t *be)
{
    const char *rel;

    if (path == 0) return -1;

    rel = fat_rel(path);
    if (rel != 0) {
        if (!fat16_mounted() || fat16_unlink(rel) != E_OK) return -1;
        if (be != 0) *be = FS_BE_FAT16;
        return 0;
    }

    if (fs_unlink(path) != E_OK) return -1;
    if (be != 0) *be = FS_BE_RAMFS;
    return 0;
}
