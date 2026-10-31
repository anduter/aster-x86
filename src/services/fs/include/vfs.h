/*
 * ng WP2-1: VFS 核心（L1 系统服务域，src/services/fs）。
 *
 * 分层：
 *   - VFS 只做“命名空间 + 节点抽象 + 打开文件描述符表”，不认识任何具体文件
 *     系统；具体 FS 通过 struct vfs_fs_ops 挂进来（本 WP 只有 ramfs）。
 *   - 依赖只有 types.h（L0 公共类型）与宿主注入的 fs_env（数据页分配）；
 *     **不** include src/kernel 或 src/ng 的内部头，也不用内核 heap。
 *   - host 单测与内核编译同一份源码（tests/unit-ng/test_vfs.c）。
 *
 * 路径语义（POSIX 风格的最小子集）：
 *   - 绝对路径从挂载树根开始；相对路径相对 ctx->cwd（默认 "/"）；
 *   - 重复 '/' 折叠，"." 忽略，".." 向上（根处 ".." 仍是根，不越界）；
 *   - 单个组件 <= VFS_NAME_MAX-1，整条路径 <= VFS_PATH_MAX-1；
 *   - 空路径 / 只有空白不是合法路径 -> EINVAL；超长 -> ENAMETOOLONG；
 *   - 符号链接按组件跟随（相对目标相对链接所在目录），最多
 *     VFS_SYMLINK_MAX 层，超出 -> ELOOP；
 *   - 中间组件不是目录 -> ENOTDIR；对文件做“当目录”的操作 -> ENOTDIR；
 *     对目录做文件操作（read/write/unlink/truncate）-> EISDIR。
 *
 * 打开文件描述符（fd）：
 *   - fd = (generation << 16) | slot，generation 1..0x7FFF，slot 0..15；
 *     close 后 generation +1，旧 fd 复用槽位也会因 generation 不匹配被拒
 *     （EBADF），与 ng handle 表同一套 slot+generation 语义。
 *   - fd 表属于 struct vfs_ctx（per-AS；本 WP 只有单 AS，内核全局一份）。
 *   - 打开目录只允许只读；listdir 走路径而不是目录 fd。
 *
 * 权限：本 WP 只做 owner 位检查（mode & 0400 / 0200）。ctx->uid == 0
 * （内核/自检）绕过；非 0 时按 owner 位判定，不足 -> EPERM。
 */
#ifndef VFS_H
#define VFS_H

#include "types.h"

/* ---- errno（负值；与 Linux/POSIX 同值，便于 ABI 1.0 对照） ---------------- */
#define VFS_OK 0
#define VFS_EPERM (-1)
#define VFS_ENOENT (-2)
#define VFS_EBADF (-9)
#define VFS_ENOMEM (-12)
#define VFS_EFAULT (-14)
#define VFS_EBUSY (-16)
#define VFS_EEXIST (-17)
#define VFS_ENOTDIR (-20)
#define VFS_EISDIR (-21)
#define VFS_EINVAL (-22)
#define VFS_ENFILE (-23)
#define VFS_EMFILE (-24)
#define VFS_ENOSPC (-28)
#define VFS_EROFS (-30)
#define VFS_ENAMETOOLONG (-36)
#define VFS_ENOTEMPTY (-39)
#define VFS_ELOOP (-40)

/* ---- 上限 ---------------------------------------------------------------- */
#define VFS_PATH_MAX 256u   /* 含 NUL 的整条路径缓冲 */
#define VFS_NAME_MAX 64u    /* 单个组件（含 NUL） */
#define VFS_MOUNT_MAX 8u    /* 同时挂载点数 */
#define VFS_FD_MAX 16u      /* 每 AS 打开文件上限 */
#define VFS_SYMLINK_MAX 8u  /* 符号链接跟随深度 */

/* ---- 节点类型 ------------------------------------------------------------ */
#define VFS_NODE_NONE 0u
#define VFS_NODE_FILE 1u
#define VFS_NODE_DIR 2u
#define VFS_NODE_SYMLINK 3u

/* st_mode 的类型位（stat.mode = 类型位 | 权限位）。 */
#define VFS_S_IFREG 0x8000u
#define VFS_S_IFDIR 0x4000u
#define VFS_S_IFLNK 0xA000u
#define VFS_S_IFMT 0xF000u

/* ---- open flags（POSIX 风格低位；一次 open 只认这些） -------------------- */
#define VFS_O_RDONLY 0x000u
#define VFS_O_WRONLY 0x001u
#define VFS_O_RDWR 0x002u
#define VFS_O_ACCMODE 0x003u
#define VFS_O_CREAT 0x040u
#define VFS_O_EXCL 0x080u
#define VFS_O_TRUNC 0x200u
#define VFS_O_APPEND 0x400u
#define VFS_O_DIRECTORY 0x800u
#define VFS_O_FLAGS_MASK 0xEC3u /* RDONLY|WRONLY|RDWR|CREAT|EXCL|TRUNC|APPEND|DIR */

/* ---- 对外结构（也是用户态 ABI 的一部分，WP4-1 冻结） -------------------- */
struct vfs_stat {
    u32 type; /* VFS_NODE_* */
    u32 mode; /* VFS_S_IF* | 权限位 */
    u64 size;
    u64 ino; /* 单调递增，不复用 */
};

struct vfs_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[VFS_NAME_MAX];
};

/*
 * 一个 fs 节点在 VFS 面前的视图。**由具体文件系统内嵌并维护**（ramfs 的
 * ramfs_node 里就有一个），VFS 不分配、不释放，只读 type/mode/size/ino，
 * 再把 ops/fs_ctx/priv 当作不透明上下文转回给后端。
 */
struct vfs_node {
    u32 type;
    u32 mode; /* 只有权限位（& 0777），类型位在 vfs_mode_of() 里拼 */
    u64 size;
    u64 ino;
    const struct vfs_fs_ops *ops;
    void *fs_ctx;
    void *priv;
};

/*
 * 具体文件系统操作表。所有函数返回 VFS_OK / 负 errno。
 *   lookup    ：在 dir 里找名为 name[0..len) 的子节点；没有 -> ENOENT。
 *   create    ：在 dir 里建 type（FILE/DIR/SYMLINK），已存在 -> EEXIST。
 *   unlink    ：删 dir/name；目录 -> EISDIR（本 WP 不做 rmdir）。
 *   rename    ：同 fs 内改名；目标存在 -> EEXIST。
 *   readdir   ：index=0.. 顺序枚举 -> 1=有条目（填 name/st），0=结束。
 *   read/write：按 off 读写；返回字节数。read 到 EOF 返回 0。
 *   truncate  ：改 size；数据页按需分配/释放。
 *   readlink  ：符号链接目标拷进 buf（NUL 结尾），返回目标长度。
 *   symlink   ：dir 里建符号链接 name -> target。
 *   retain/release：open/close 的引用计数（unlink 后由最后一个 close 释放）。
 * 可选函数可以为 NULL；VFS 会按情况返回 EINVAL/EROFS。
 */
struct vfs_fs_ops {
    int (*lookup)(void *ctx, struct vfs_node *dir, const char *name, u32 len,
                  struct vfs_node **out);
    int (*create)(void *ctx, struct vfs_node *dir, const char *name, u32 len,
                  u32 type, u32 mode, struct vfs_node **out);
    int (*unlink)(void *ctx, struct vfs_node *dir, const char *name, u32 len);
    int (*rename)(void *ctx, struct vfs_node *olddir, const char *oldname,
                  u32 oldlen, struct vfs_node *newdir, const char *newname,
                  u32 newlen);
    int (*readdir)(void *ctx, struct vfs_node *dir, u32 index, char *name,
                   u32 name_max, struct vfs_stat *st);
    int (*read)(void *ctx, struct vfs_node *node, u64 off, void *buf, u64 len);
    int (*write)(void *ctx, struct vfs_node *node, u64 off, const void *buf,
                 u64 len);
    int (*truncate)(void *ctx, struct vfs_node *node, u64 size);
    int (*readlink)(void *ctx, struct vfs_node *node, char *buf, u32 max);
    int (*symlink)(void *ctx, struct vfs_node *dir, const char *name, u32 len,
                   const char *target);
    int (*retain)(void *ctx, struct vfs_node *node);
    void (*release)(void *ctx, struct vfs_node *node);
};

/* ---- 打开文件表（per-AS） ------------------------------------------------- */
struct vfs_fd {
    u64 offset;
    struct vfs_node *node;
    u32 gen; /* 0 = 槽位从未使用 */
    u32 flags;
    u32 used;
};

struct vfs_ctx {
    struct vfs_fd fds[VFS_FD_MAX];
    char cwd[VFS_PATH_MAX];
    u32 uid; /* 0 = root（绕过权限位）；非 0 = 按 owner 位检查 */
    u32 live;
    u32 peak;
    u64 opens;
    u64 closes;
    u32 bad_fd;   /* 参数非法 / 槽位空 / generation 不匹配 */
    u32 stale_fd; /* generation 不匹配（close 之后的旧 fd） */
    u32 full;     /* fd 表满 */
    u32 perm_denied;
};

/* ------------------------------------------------------------------------- */
/* 生命周期 / 挂载表                                                         */
/* ------------------------------------------------------------------------- */

/* 清空挂载表（host 单测的每个子用例都从干净表开始）。不释放 FS 私有状态。 */
void vfs_reset(void);

/*
 * 挂载：path 必须先存在且是目录（根 "/" 例外），root 是该 FS 的根节点，
 * root->ops/root->fs_ctx 由后端填好（VFS 会再写一遍）。重复路径 EEXIST。
 */
int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *ctx,
              struct vfs_node *root);

/* 卸载：根挂载 EBUSY，有子挂载 EBUSY，路径不存在 ENOENT。 */
int vfs_umount(const char *path);

u32 vfs_mount_count(void);
const char *vfs_mount_path_at(u32 index); /* index 越界返回 NULL */

/* 初始化 per-AS 上下文：fd 表清零、cwd="/"、uid=0。 */
void vfs_ctx_init(struct vfs_ctx *c);

/* ------------------------------------------------------------------------- */
/* 路径                                                                      */
/* ------------------------------------------------------------------------- */

/* 词法规范化（不查 FS）：cwd 为 NULL 或非绝对时按 "/"；空路径 EINVAL。 */
int vfs_normalize(const char *cwd, const char *path, char *out, u32 out_size);

/* 解析到节点（跟随符号链接）；out 可为 NULL（只判断存在性）。 */
int vfs_resolve(struct vfs_ctx *c, const char *path, struct vfs_node **out);

/* 读符号链接目标（不跟随）；buf 不够 -> ENAMETOOLONG。 */
int vfs_readlink(struct vfs_ctx *c, const char *path, char *buf, u32 max);

/* ------------------------------------------------------------------------- */
/* 文件操作                                                                  */
/* ------------------------------------------------------------------------- */

int vfs_open(struct vfs_ctx *c, const char *path, u32 flags, u32 mode);
int vfs_read(struct vfs_ctx *c, int fd, void *buf, u64 len);
int vfs_write(struct vfs_ctx *c, int fd, const void *buf, u64 len);
int vfs_close(struct vfs_ctx *c, int fd);

int vfs_stat(struct vfs_ctx *c, const char *path, struct vfs_stat *out);
int vfs_listdir(struct vfs_ctx *c, const char *path, struct vfs_dirent *ents,
                u32 max);
int vfs_mkdir(struct vfs_ctx *c, const char *path, u32 mode);
int vfs_unlink(struct vfs_ctx *c, const char *path);
int vfs_rename(struct vfs_ctx *c, const char *oldpath, const char *newpath);
int vfs_truncate(struct vfs_ctx *c, const char *path, u64 size);
int vfs_symlink(struct vfs_ctx *c, const char *path, const char *target);
int vfs_chdir(struct vfs_ctx *c, const char *path);

/* 诊断：打开文件数 / fd 解码（host 单测断言用）。 */
u32 vfs_fd_live(const struct vfs_ctx *c);
u32 vfs_fd_slot_of(int fd);
u32 vfs_fd_gen_of(int fd);

#endif /* VFS_H */
