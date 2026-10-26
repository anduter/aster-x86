#ifndef FS_FS_H
#define FS_FS_H

#include "../include/types.h"

/* 文件系统模块（当前后端是内存卷；接口按“卷”设计，日后可换块设备后端） */
#define FS_NAME_MAX     65    /* 文件名/目录名最长 64 字符（含结尾 0） */
#define FS_PATH_MAX     256
#define FS_FILE_MAX     65536 /* 单个文件最大字节数（64KB；shell/程序不再受 40KB 限制） */
#define FS_FILE_MAX_LEGACY 40960 /* XFS1 v4 镜像的单文件上限（旧镜像兼容） */
#define FS_DISK_VERSION 5u    /* XFS1 super 版本：v5 = 64KB 限额 + 容量字段 */
#define FS_MAX_NODES    64    /* 节点池（文件 + 目录，所有卷共用） */
#define FS_MAX_OPEN     32    /* 同时打开的文件句柄数 */
#define FS_MAX_VOLUMES  4     /* 最多挂载几个卷（C: D: ...） */
#define FS_DRIVE_MAX    32    /* 盘符/卷名最长 32 字节（含结尾 0） */
#define FS_CRED_GROUPS_MAX 16 /* 附加组上限（与 ABI_NGROUPS_MAX 一致） */

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

/* 权限位（和 Unix 一致的 9 位）+ setuid/setgid/sticky */
#define FS_MODE_IRUSR 0x100U
#define FS_MODE_IWUSR 0x080U
#define FS_MODE_IXUSR 0x040U
#define FS_MODE_IRGRP 0x020U
#define FS_MODE_IWGRP 0x010U
#define FS_MODE_IXGRP 0x008U
#define FS_MODE_IROTH 0x004U
#define FS_MODE_IWOTH 0x002U
#define FS_MODE_IXOTH 0x001U

/* 高 3 位（Linux 语义）：mode & 07777 全部保存，mode & 0777 是纯权限位 */
#define FS_MODE_SETUID  0x800U  /* 04000：执行时 euid = 文件属主 */
#define FS_MODE_SETGID  0x400U  /* 02000：执行时 egid = 文件属组 */
#define FS_MODE_STICKY  0x200U  /* 01000：目录内只有属主/root 能删（/tmp） */
#define FS_MODE_SPECIAL 0xE00U
#define FS_MODE_PERM    0x1FFU
#define FS_MODE_ALL     0xFFFU

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

/* =============================================================================
 * 凭据（ABI v1.3）：权限判定用生效身份 euid/egid + 附加组。
 *
 * 旧的 fs_*_as(path, ..., uid, gid) 保留（自检/内核内部按 root 调用），它等价于
 * 一个没有附加组的 cred。用户态 syscall 一律走 fs_*_cred()，传入进程的
 * euid/egid/groups——这样 setuid 位、sudo 提权和组权限都只有一处判定逻辑。
 *
 * root 判定基于 euid == 0（CAP_DAC_OVERRIDE），与 Linux 一致。
 * ========================================================================== */
typedef struct fs_cred {
    uint32_t euid;                            /* 生效用户 ID */
    uint32_t egid;                            /* 生效组 ID */
    uint32_t ngroups;                         /* 附加组个数 */
    uint32_t groups[FS_CRED_GROUPS_MAX];      /* 附加组（不含主组） */
    uint32_t umask;                           /* 创建时的权限掩码（0 = 不掩） */
} fs_cred_t;

/* 用 (uid, gid) 造一个没有附加组的 cred */
void fs_cred_from_ids(fs_cred_t *out, uint32_t uid, uint32_t gid);
/* cred 是否属于组 gid（主组或附加组） */
bool fs_cred_in_group(const fs_cred_t *c, uint32_t gid);
/* root（euid == 0）：保留 CAP_DAC_OVERRIDE */
bool fs_cred_is_root(const fs_cred_t *c);

/* 生效身份版本的文件操作（用户态 syscall 用这些） */
int      fs_open_cred(const char *path, uint32_t flags, const fs_cred_t *c, uint8_t *be);
int      fs_mkdir_cred(const char *path, const fs_cred_t *c, uint8_t *be);
int      fs_unlink_cred(const char *path, const fs_cred_t *c, uint8_t *be);
int      fs_rename_cred(const char *old_path, const char *new_path, const fs_cred_t *c);
int      fs_chmod_cred(const char *path, uint32_t mode, const fs_cred_t *c);
int      fs_chown_cred(const char *path, uint32_t uid, uint32_t gid, const fs_cred_t *c);
int      fs_access_cred(const char *path, uint32_t want, const fs_cred_t *c);
int      fs_stat_cred(const char *path, fs_stat_t *out, const fs_cred_t *c, uint8_t *be);
int      fs_listdir_cred(const char *path, uint32_t index, char *name, uint32_t name_max,
                         fs_stat_t *out, const fs_cred_t *c, uint8_t *be);

/* 读写/关闭句柄（会校验句柄类型，传错类型返回 E_TYPE） */
int fs_read(uint32_t handle, void *buf, uint32_t len);
int fs_write(uint32_t handle, const void *buf, uint32_t len);
int fs_close(uint32_t handle);
/* 增加句柄引用（dup2 / fork 时用） */
int fs_ref(uint32_t handle);

/* 元数据与目录操作 */
int fs_stat(const char *path, fs_stat_t *out);
int fs_mkdir(const char *path);
int fs_mkdir_as(const char *path, uint32_t uid, uint32_t gid);
int fs_unlink(const char *path);
int fs_unlink_as(const char *path, uint32_t uid, uint32_t gid);

/* 按 (uid, gid) 打开（自检/内核内部用；用户态走 fs_open_cred） */
uint32_t fs_open_as(const char *path, uint32_t flags, uint32_t uid, uint32_t gid);
/* 内核内部调用（加载 ELF、安装 shell 等）按 root 身份打开 */
uint32_t fs_open(const char *path, uint32_t flags);

/* ---- rename / move（rename(2) 的简化版） ----
 * 在同一卷内改名或移动；new_path 是完整的目标路径（不做 `mv` 的"移入目录"语义）。
 * 返回 E_OK 或 E_*：
 *   E_NOENT  源不存在 / 路径里的父目录不存在
 *   E_INVAL  跨卷、路径非法、把目录移动到自己的子树里、只读后端
 *   E_ACCES  源或目标父目录缺少 w+x（uid==0 时绕过）
 *   E_EXIST  目标类型不匹配，或目标是非空目录
 *   E_BUSY   目标文件仍被打开（替换它会破坏已有句柄）
 * fs_rename() 按 root 身份执行（内核内部用）；用户态走 fs_rename_as()。 */
int fs_rename(const char *old_path, const char *new_path);
int fs_rename_as(const char *old_path, const char *new_path, uint32_t uid, uint32_t gid);

/* 权限：改变 mode（仅属主/root）、改变属主属组（仅 root） */
int fs_chmod(const char *path, uint32_t mode, uint32_t uid);
int fs_chown(const char *path, uint32_t uid, uint32_t gid);
int fs_chown_as(const char *path, uint32_t uid, uint32_t gid, uint32_t caller_uid);

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

/* =============================================================================
 * SIP-lite：系统路径运行时只读（实现在 fs/sip.c）
 *
 * system:/System、system:/bin、system:/drivers 下的写操作（创建/写打开、
 * mkdir、unlink、rename、chmod、chown）一律 E_ACCES，root 同样被拒。
 * 例外是"签名更新器作用域"：_pkgd 在安装/卸载期间 sip_updater_enter()，
 * 只有进入时那个线程的写放行（scope 绑定 pid，不是全局开关）。
 * 判定前会规范化路径（"."、".."、重复 '/'），所以穿越写法也拦得住。
 * 关闭：内核参数 sip=0，或运行中 sip_set_enabled(0)。
 * ========================================================================== */
void sip_init(void);
int  sip_enabled(void);
void sip_set_enabled(int on);
int  sip_path_protected(const char *path);
/* 写操作守卫：E_OK 放行 / E_ACCES 拒绝（VFS 写路径统一调用） */
int  sip_guard_write(const char *path);
void sip_updater_enter(void);
void sip_updater_exit(void);
int  sip_updater_active(void);
void sip_selftest(void);

/* 已用节点数（诊断） */
unsigned int fs_node_count(void);

/* =============================================================================
 * VFS 后端接口
 *
 * `fs_*` 是内存卷（ramfs）的实现；`fs_*_ex` 是文件系统层对外的统一入口：
 * 它按路径前缀（"fat:"）路由到具体后端，并把"后端 id"一起返回。
 *
 * 调用方（syscall / proc）只认 (fs_be, handle) 这个二元组，
 * 不再需要判断"这个 fd 属于哪个文件系统"。
 * 新增一个后端 = 在 vfs.c 里加一条路由 + 一组适配函数，调用方不动。
 * ============================================================================= */
#define FS_BE_RAMFS 0U
#define FS_BE_FAT16 1U
#define FS_BE_SVC   2U
#define FS_BE_BLK   3U

/* 打开：成功返回后端句柄（>=0），失败返回 -1；*be 写回后端 id */
int fs_open_ex(const char *path, uint32_t flags, uint32_t uid, uint32_t gid, uint8_t *be);
int fs_read_ex(uint8_t be, int h, void *buf, uint32_t len);
int fs_write_ex(uint8_t be, int h, const void *buf, uint32_t len);
int fs_close_ex(uint8_t be, int h);
int fs_ref_ex(uint8_t be, int h);

int fs_stat_ex(const char *path, fs_stat_t *out, uint8_t *be);
int fs_listdir_ex(const char *path, uint32_t index, char *name, uint32_t name_max,
                  fs_stat_t *out, uint8_t *be);
int fs_mkdir_ex(const char *path, uint32_t uid, uint32_t gid, uint8_t *be);
int fs_unlink_ex(const char *path, uint32_t uid, uint32_t gid, uint8_t *be);

/* 路径属于哪个后端（给驱动器枚举/调试用） */
int fs_is_fat(const char *path);

/* =============================================================================
 * 后端描述表
 *
 * 每个文件系统实现一张 ops 表并注册到 vfs.c；VFS 层只按 probe() 选后端，
 * 不再出现 "if (fat) ... else ..."。新增后端 = 实现一张表 + 在 vfs.c 的
 * 表数组里加一项，syscall / proc 一行不用改。
 * ============================================================================= */
#define FS_BACKEND_API_VERSION 1u

/* 版本号写在 vfs.c 的注册表（fs_backend_reg_t）里，而不是结构体字段：
 * 现有后端使用位置初始化器，给结构体尾部加字段会触发
 * -Wmissing-field-initializers（-Wextra -Werror 下直接失败）。
 * 注册时由 vfs.c 校验；0 表示旧格式，按 v1 解释。 */

typedef struct fs_backend {
    const char *name;
    /* 该后端是否接管此路径；接管时 *rel 返回后端自己的相对路径 */
    int (*probe)(const char *path, const char **rel);
    int (*open)(const char *rel, uint32_t flags, uint32_t uid, uint32_t gid);
    int (*read)(int h, void *buf, uint32_t len);
    int (*write)(int h, const void *buf, uint32_t len);
    int (*close)(int h);
    int (*ref)(int h);
    int (*stat)(const char *rel, fs_stat_t *out);
    int (*listdir)(const char *rel, uint32_t index, char *name,
                   uint32_t name_max, fs_stat_t *out);
    int (*mkdir)(const char *rel, uint32_t uid, uint32_t gid);
    int (*unlink)(const char *rel, uint32_t uid, uint32_t gid);
    /* 改名/移动（同一后端内）。返回 E_OK / E_*。
     * VFS 负责路由和跨卷拒绝；权限模型（源/目标父目录 w+x）由后端按自己的
     * 元数据判断：ramfs 有 Unix 权限位，FAT16 没有（视为不限）。 */
    int (*rename)(const char *oldrel, const char *newrel, uint32_t uid, uint32_t gid);
} fs_backend_t;

/* 纯判定：declared 是后端声明/注册的 API 版本；0 表示旧格式（按 v1 解释）。
 * 返回 E_OK 或 E_INVAL，不打印。 */
int fs_backend_version_ok(uint32_t declared);

/* 注册校验：版本不匹配时打印一行并返回 E_INVAL（b 可为 0，只用于日志名）。 */
int fs_backend_api_check(const fs_backend_t *b, uint32_t declared);

/* 各后端实例 */
extern const fs_backend_t ramfs_backend;
extern const fs_backend_t fat16_backend;
extern const fs_backend_t svc_backend;
extern const fs_backend_t blk_backend;

#endif
