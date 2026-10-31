/*
 * Aster SDK alpha —— 文件系统（VFS）封装（ABI 1.0 未冻结）
 *
 * 对应内核调用号 20..27（src/ng/syscall.h / src/ng/fs_syscall.h）。结构体
 * 布局逐字节镜像 src/services/fs/include/vfs.h，用户态不 include 内核头。
 *
 * 语义要点：
 *   - 路径是绝对或相对 VFS cwd 的 POSIX 子集；单段 < 64 字节，整条 < 256。
 *   - fd = (generation << 16) | slot，close 后旧 fd 立即失效。
 *   - 单次 read/write 内核 bounce buffer 上限 512 字节（FS_SYS_IO_MAX），
 *     更大的 IO 由调用方分块；listdir 单次最多 8 条。
 *   - 失败返回负 errno（ASTER_E*）。
 */
#ifndef ASTER_FS_H
#define ASTER_FS_H

#include <aster/types.h>
#include <aster/syscall.h>

/* ---- 上限 ---------------------------------------------------------------- */
#define ASTER_PATH_MAX 256u
#define ASTER_NAME_MAX 64u
#define ASTER_LISTDIR_MAX 8u
#define ASTER_FS_IO_MAX 512u

/* ---- 节点类型 ------------------------------------------------------------ */
#define ASTER_NODE_NONE 0u
#define ASTER_NODE_FILE 1u
#define ASTER_NODE_DIR 2u
#define ASTER_NODE_SYMLINK 3u

/* ---- st_mode 类型位 ------------------------------------------------------ */
#define ASTER_S_IFREG 0x8000u
#define ASTER_S_IFDIR 0x4000u
#define ASTER_S_IFLNK 0xA000u
#define ASTER_S_IFMT 0xF000u

/* ---- open flags（与 vfs.h 一致） ----------------------------------------- */
#define ASTER_O_RDONLY 0x000u
#define ASTER_O_WRONLY 0x001u
#define ASTER_O_RDWR 0x002u
#define ASTER_O_ACCMODE 0x003u
#define ASTER_O_CREAT 0x040u
#define ASTER_O_EXCL 0x080u
#define ASTER_O_TRUNC 0x200u
#define ASTER_O_APPEND 0x400u
#define ASTER_O_DIRECTORY 0x800u

/* ---- 用户态 ABI 结构（布局必须与 vfs.h 相同） ---------------------------- */
struct aster_stat {
    u32 type; /* ASTER_NODE_* */
    u32 mode; /* ASTER_S_IF* | 权限位 */
    u64 size;
    u64 ino;
};
_Static_assert(sizeof(struct aster_stat) == 24, "aster_stat must be 24 bytes");

struct aster_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[ASTER_NAME_MAX];
};
_Static_assert(sizeof(struct aster_dirent) == 80,
               "aster_dirent must be 80 bytes");

/* ---- 封装 ---------------------------------------------------------------- */
static inline i64 aster_open(const char *path, u32 flags, u32 mode)
{
    if (path == NULL) {
        return ASTER_EINVAL;
    }
    return aster_syscall3(ASTER_SYS_OPEN, (u64)(usize)path, (u64)flags,
                          (u64)mode);
}

static inline i64 aster_read(int fd, void *buf, u64 len)
{
    if (buf == NULL && len != 0) {
        return ASTER_EINVAL;
    }
    return aster_syscall3(ASTER_SYS_READ, (u64)(i64)fd, (u64)(usize)buf, len);
}

static inline i64 aster_write(int fd, const void *buf, u64 len)
{
    if (buf == NULL && len != 0) {
        return ASTER_EINVAL;
    }
    return aster_syscall3(ASTER_SYS_WRITE_FD, (u64)(i64)fd, (u64)(usize)buf,
                          len);
}

static inline i64 aster_close(int fd)
{
    return aster_syscall1(ASTER_SYS_CLOSE, (u64)(i64)fd);
}

static inline i64 aster_stat(const char *path, struct aster_stat *out)
{
    if (path == NULL || out == NULL) {
        return ASTER_EINVAL;
    }
    return aster_syscall2(ASTER_SYS_STAT, (u64)(usize)path, (u64)(usize)out);
}

static inline i64 aster_listdir(const char *path, struct aster_dirent *ents,
                                u32 max)
{
    if (path == NULL || ents == NULL || max == 0) {
        return ASTER_EINVAL;
    }
    return aster_syscall3(ASTER_SYS_LISTDIR, (u64)(usize)path,
                          (u64)(usize)ents, (u64)max);
}

static inline i64 aster_mkdir(const char *path, u32 mode)
{
    if (path == NULL) {
        return ASTER_EINVAL;
    }
    return aster_syscall2(ASTER_SYS_MKDIR, (u64)(usize)path, (u64)mode);
}

static inline i64 aster_unlink(const char *path)
{
    if (path == NULL) {
        return ASTER_EINVAL;
    }
    return aster_syscall1(ASTER_SYS_UNLINK, (u64)(usize)path);
}

#endif /* ASTER_FS_H */
