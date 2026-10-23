#ifndef FS_FS_H
#define FS_FS_H

#include "../types.h"

/* 文件系统模块（当前后端是内存卷；接口按“卷”设计，日后可换块设备后端） */
#define FS_NAME_MAX     65    /* 文件名/目录名最长 64 字符（含结尾 0） */
#define FS_PATH_MAX     256
#define FS_FILE_MAX     32768 /* 单个文件最大字节数（放得下 ELF 程序） */
#define FS_MAX_NODES    64    /* 节点池（文件 + 目录，所有卷共用） */
#define FS_MAX_OPEN     8     /* 同时打开的文件句柄数 */
#define FS_MAX_VOLUMES  4     /* 最多挂载几个卷（C: D: ...） */
#define FS_DRIVE_MAX    32    /* 盘符/卷名最长 32 字节（含结尾 0） */

/* fs_open 的打开标志 */
#define FS_OPEN_READ   0x01U
#define FS_OPEN_WRITE  0x02U
#define FS_OPEN_CREAT  0x04U

typedef enum {
    FS_TYPE_NONE = 0,
    FS_TYPE_FILE = 1,
    FS_TYPE_DIR  = 2
} fs_type_t;

typedef struct {
    fs_type_t type;
    uint32_t  size;
    uint32_t  mtime;      /* 最后修改时间（epoch 秒，来自 RTC） */
    uint32_t  mode;       /* 权限位：rwxrwxrwx（低 9 位） */
    uint32_t  uid;        /* 属主 */
    uint32_t  gid;        /* 属组 */
} fs_stat_t;

/* 权限位（和 Unix 一致的 9 位） */
#define FS_MODE_IRUSR 0x100U
#define FS_MODE_IWUSR 0x080U
#define FS_MODE_IXUSR 0x040U
#define FS_MODE_IRGRP 0x020U
#define FS_MODE_IWGRP 0x010U
#define FS_MODE_IXGRP 0x008U
#define FS_MODE_IROTH 0x004U
#define FS_MODE_IWOTH 0x002U
#define FS_MODE_IXOTH 0x001U

/* 默认权限：文件 0644、目录 0755 */
#define FS_MODE_FILE_DEFAULT 0x1A4U   /* 0644 */
#define FS_MODE_DIR_DEFAULT  0x1EDU   /* 0755 */

/* 判权用：读/写/执行 */
#define FS_ACCESS_READ  0x4U
#define FS_ACCESS_WRITE 0x2U
#define FS_ACCESS_EXEC  0x1U

/* 初始化卷：清空节点池并挂载默认卷 "C" */
void fs_init(void);

/* 挂载一个新的卷（盘符），名字最长 FS_DRIVE_MAX-1 字节，例如 "D" */
int fs_mount(const char *name);

/* 卷信息：数量、第 index 个卷的名字 */
uint32_t fs_volume_count(void);
int fs_volume_name(uint32_t index, char *buf, uint32_t name_max);

/* 打开文件，返回 OBJ_FILE 句柄；失败返回 0。
 * path 支持 "C:/dir/file" 或 "/dir/file"（无盘符时用默认卷 C:）。 */
uint32_t fs_open(const char *path, uint32_t flags);

/* 读写/关闭句柄（会校验句柄类型，传错类型返回 E_TYPE） */
int fs_read(uint32_t handle, void *buf, uint32_t len);
int fs_write(uint32_t handle, const void *buf, uint32_t len);
int fs_close(uint32_t handle);
/* 增加句柄引用（dup2 / fork 时用） */
int fs_ref(uint32_t handle);

/* 元数据与目录操作 */
int fs_stat(const char *path, fs_stat_t *out);
int fs_mkdir(const char *path);
int fs_unlink(const char *path);

/* 权限：改变 mode（仅属主/root）、改变属主属组（仅 root） */
int fs_chmod(const char *path, uint32_t mode, uint32_t uid);
int fs_chown(const char *path, uint32_t uid, uint32_t gid);

/* 以指定身份判权（供自检/内核调用；fs_open 用当前进程身份） */
int fs_access_as(const char *path, uint32_t want, uint32_t uid, uint32_t gid);

/* 目录遍历：index 从 0 递增；取得一项返回 E_OK，越界返回 E_NOENT */
int fs_listdir(const char *path, uint32_t index,
               char *name, uint32_t name_max, fs_stat_t *out);

/* ---- 持久化：把整棵文件树序列化到块设备 / 从块设备恢复 ---- */
int fs_format_disk(const char *blk_name);
int fs_sync(const char *blk_name);
int fs_mount_disk(const char *blk_name);

/* 持久化后端（ata0 / ram0）：启动流程选择，shell 的 sync 命令用它 */
void        fs_set_backing(const char *name);
const char *fs_backing(void);
int         fs_sync_backing(void);

/* 一致性检查：返回错误数，report 写一行摘要（nodes/files/dirs/errors） */
int fs_check(char *report, uint32_t max);

/* 已用节点数（诊断） */
unsigned int fs_node_count(void);

#endif
