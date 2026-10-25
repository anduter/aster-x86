#ifndef FS_RAMFS_H
#define FS_RAMFS_H

/* 兼容旧包含路径：文件系统的公开接口已经移到 fs.h，
 * 本文件只做转发，新代码请直接 #include "fs.h"。 */
#include "fs.h"

/* ---- ramfs 后端的凭据版本（ABI v1.3）----
 * 只有 ramfs 有 Unix 权限元数据，所以它直接吃 fs_cred_t（euid/egid/
 * 附加组/umask）。VFS 层（vfs.c）负责把 fs_*_cred() 路由到这里；
 * 其它后端按 (euid, egid) 转发。 */
int      ramfs_open_cred(const char *path, uint32_t flags, const fs_cred_t *c, uint8_t *be);
int      ramfs_mkdir_cred(const char *path, const fs_cred_t *c, uint8_t *be);
int      ramfs_unlink_cred(const char *path, const fs_cred_t *c, uint8_t *be);
int      ramfs_stat_cred(const char *path, fs_stat_t *out, const fs_cred_t *c, uint8_t *be);
int      ramfs_listdir_cred(const char *path, uint32_t index, char *name, uint32_t name_max,
                            fs_stat_t *out, const fs_cred_t *c, uint8_t *be);
int      ramfs_chmod_cred(const char *path, uint32_t mode, const fs_cred_t *c);
int      ramfs_chown_cred(const char *path, uint32_t uid, uint32_t gid, const fs_cred_t *c);
int      ramfs_access_cred(const char *path, uint32_t want, const fs_cred_t *c);
int      ramfs_rename_cred(const char *old_path, const char *new_path, const fs_cred_t *c);

#endif
