/*
 * ng WP2-1: VFS syscall 胶水接口（调用号 20..27 的内核侧实现）。
 *
 * syscall.c 只在这个头后面转一次参数；真正的路径/缓冲处理在 fs_syscall.c。
 * fs_sys_init() 需要调用方注入数据页分配器（内核绑 PMM，host 单测绑 fake
 * 页池），这样 src/services/fs 不依赖任何内核内部头。
 */
#ifndef NG_FS_SYSCALL_H
#define NG_FS_SYSCALL_H

#include "ramfs.h"
#include "types.h"
#include "vfs.h"

/* 单次 read/write 的内核 bounce buffer 上限；listdir 单次最多返回条数。 */
#define FS_SYS_IO_MAX 512u
#define FS_SYS_LISTDIR_MAX 8u

/*
 * 建根 ramfs（挂到 "/"）+ 建 /tmp + 清空 per-AS 上下文。
 * 返回 VFS_OK 或负 errno；失败后所有 fs syscall 返回 EINVAL。
 */
int fs_sys_init(const struct fs_env *env);

/* 内核自检拿上下文做直接调用（不经过用户指针）。 */
struct vfs_ctx *fs_sys_ctx(void);

int fs_sys_open(u64 upath, u32 flags, u32 mode);
int fs_sys_read(u32 fd, u64 ubuf, u64 len);
int fs_sys_write(u32 fd, u64 ubuf, u64 len);
int fs_sys_close(u32 fd);
int fs_sys_stat(u64 upath, u64 ustat);
int fs_sys_listdir(u64 upath, u64 uents, u32 max);
int fs_sys_mkdir(u64 upath, u32 mode);
int fs_sys_unlink(u64 upath);

#endif /* NG_FS_SYSCALL_H */
