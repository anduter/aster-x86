/* =============================================================================
 * include/abi.h - 用户态系统调用 ABI 的单一来源（v1）
 *
 * 这个头文件同时被内核和用户库（src/user/ulib.h）包含，只放跨边界的契约：
 *   - legacy int 0x81 调用号（SYS_*）；int 0x80 的 Native ABI 编号在
 *     proc/trap/native.h（NATIVE_SYS_*），两套门编号空间互不相干；
 *   - ABI 版本、魔数、feature bits；
 *   - 用户可见共享结构体，布局由本文件末尾的 _Static_assert 固定；
 *   - legacy 门可能返回的用户可见错误码（复用 core/error.h 的 E_* 子集）。
 *
 * 版本规则（v1 冻结，详见 docs/api.md / docs/native-abi.md §18）：
 *   1. 已有编号、结构体字段偏移/大小、错误码值不得改变；
 *   2. 新增调用只能追加编号（>= SYS_NR_MAX + 1），并同步 bump
 *      OS_ABI_VERSION_MINOR；
 *   3. 结构体只能尾部追加字段，且必须同步更新下面的 _Static_assert；
 *   4. 删除的调用保留编号，legacy 门返回 -1（E_INVAL 语义）。
 * ========================================================================== */
#ifndef OS_INCLUDE_ABI_H
#define OS_INCLUDE_ABI_H

#include "types.h"
#include "../core/error.h"   /* E_*：legacy 门/文件系统可能原样返回的错误码 */

/* =============================================================================
 * 1. ABI 版本与标识
 * ========================================================================== */
#define OS_ABI_MAGIC          0x4F534142u   /* "OSAB"（小端字节序 'O','S','A','B'） */
#define OS_ABI_VERSION        1u            /* 头文件/编号空间的 major 版本 */
#define OS_ABI_VERSION_MAJOR  1
#define OS_ABI_VERSION_MINOR  2

/* SYS_ABI_VERSION 返回值的低 16 位（feature bits）：只增不减。
 *
 * 注意：bit2-4 只表示内核内部存在版本一致性检查（internal only），
 * 不是对外驱动/服务 API，不承诺兼容，也不代表要推进驱动服务化。 */
#define OS_ABI_F_NATIVE_GATE      (1u << 0)  /* int 0x80 Native ABI 门已安装 */
#define OS_ABI_F_VERSIONED_OPS    (1u << 1)  /* 内核内部版本校验存在（internal only） */
#define OS_ABI_F_DEVICE_API_V1    (1u << 2)  /* internal only：dev/device.h 内部 ops 版本 */
#define OS_ABI_F_VFS_BACKEND_V1   (1u << 3)  /* internal only：VFS 后端注册表校验 */
#define OS_ABI_F_SERVICE_MANIFEST_V1 (1u << 4) /* internal only：服务注册入口校验 */

#define OS_ABI_FEATURES                                                     \
    (OS_ABI_F_NATIVE_GATE | OS_ABI_F_VERSIONED_OPS |                        \
     OS_ABI_F_DEVICE_API_V1 | OS_ABI_F_VFS_BACKEND_V1 |                     \
     OS_ABI_F_SERVICE_MANIFEST_V1)

/* =============================================================================
 * 2. legacy int 0x81 系统调用号（1..58 为既有调用，只迁移不改语义）
 *
 * 参数约定：ebx/ecx/edx 依次为第 1/2/3 个参数；返回值为 eax。
 * >= 0 成功，< 0 失败（legacy 门大多数失败返回 -1）。
 * ========================================================================== */
#define SYS_WRITE   1   /* ebx=buf, ecx=len            -> 写入字节数 */
#define SYS_GETPID  2   /*                              -> 当前 pid */
#define SYS_UPTIME  3   /*                              -> 运行秒数 */
#define SYS_YIELD   4   /*                              -> 0 */
#define SYS_EXIT    5   /* ebx=退出码                   -> 不返回 */
#define SYS_OPEN    6   /* ebx=路径, ecx=标志           -> 文件句柄 */
#define SYS_FREAD   7   /* ebx=句柄, ecx=buf, edx=len   -> 读到字节数 */
#define SYS_FWRITE  8   /* ebx=句柄, ecx=buf, edx=len   -> 写入字节数 */
#define SYS_CLOSE   9   /* ebx=句柄                     -> 0 */
#define SYS_GETCHAR 10  /*                              -> 字符或 -1 */
#define SYS_LIST    11  /* ebx=路径, ecx=序号, edx=buf     -> 0 / -1 */
#define SYS_DRIVES  12  /* ebx=序号, ecx=buf, edx=max      -> 0 / -1 */
#define SYS_MKDIR   13  /* ebx=路径                       -> 0 / -1 */
#define SYS_UNLINK  14  /* ebx=路径                       -> 0 / -1 */
#define SYS_STAT    15  /* ebx=路径, ecx=buf(sys_stat_t)  -> 0 / -1 */
#define SYS_CHDIR   16  /* ebx=路径                       -> 0 / -1 */
#define SYS_GETCWD  17  /* ebx=buf, ecx=max               -> 长度 / -1 */
#define SYS_FORK    18  /*                                -> 子 pid / 0（子进程） */
#define SYS_EXEC    19  /* ebx=路径                       -> 不返回（成功） */
#define SYS_WAIT    20  /* ebx=pid, ecx=&status           -> 子 pid / -1 */
#define SYS_INPUT_SET 21 /* ebx=buf, ecx=len              -> 0 / -1（画到输入框） */
#define SYS_BRK     22  /* ebx=增量                       -> 新的堆顶 / -1 */
#define SYS_KILL    23  /* ebx=pid, ecx=sig                -> 0 / -1 */
#define SYS_SLEEP   24  /* ebx=毫秒                        -> 0 */
#define SYS_PIPE    25  /* ebx=用户 int[2]                 -> 0 / -1 */
#define SYS_DUP2    26  /* ebx=旧 fd, ecx=新 fd            -> 新 fd / -1 */
#define SYS_REBOOT  27  /*                                -> 不返回（机器复位） */
#define SYS_PANIC   28  /*                                -> 不返回（调试用：触发内核 panic） */
#define SYS_OOPS    29  /*                                -> 不返回（调试用：触发内核 oops） */
#define SYS_SIGACTION  30 /* ebx=sig, ecx=handler           -> 旧 handler / -1 */
#define SYS_SIGRETURN  31 /*                                -> 恢复信号前的现场 */
#define SYS_SYNC       32 /*                                -> 0 / -1（落盘到持久化设备） */
#define SYS_TIME       33 /* ebx=缓冲, ecx=长度             -> 0（格式化时间字符串） */
#define SYS_FSCK       34 /* ebx=缓冲, ecx=长度             -> 错误数（缓冲里是摘要） */
#define SYS_PCI        35 /* ebx=index, ecx=缓冲            -> 0（PCI 设备信息）/ -1 */
#define SYS_POWEROFF   36 /*                                -> 不返回（ACPI S5 关机） */
#define SYS_MOUSE      37 /* ebx=用户 int[3]                -> 0（{x,y,buttons}） */
#define SYS_HANG       38 /* 调试：关中断死循环（验证 NMI 看门狗） */
#define SYS_ETH        39 /* ebx=0 取信息 / 1 发测试帧, ecx=缓冲 */
#define SYS_RESINFO    40 /* ebx=用户 u32[10]               -> 0（资源统计） */
#define SYS_GETUID     41 /*                                -> uid */
#define SYS_GETGID     42 /*                                -> gid */
#define SYS_CHMOD      43 /* ebx=路径, ecx=mode             -> 0 / -1 */
#define SYS_CHOWN      44 /* ebx=路径, ecx=uid, edx=gid     -> 0 / -1（root） */
#define SYS_SETUID     45 /* ebx=uid, ecx=gid               -> 0 / -1（root 才能降权） */
#define SYS_CAP_DROP   46 /* ebx=fd, ecx=要撤销的 rights    -> 0 / -1 */
#define SYS_CAP_SEND   47 /* ebx=pid, ecx=fd, edx=rights   -> 目标 fd / -1 */
#define SYS_MMAP       48 /* ebx=len, ecx=prot             -> 用户 VA / -1 */
#define SYS_MUNMAP     49 /* ebx=addr, ecx=len             -> 0 / -1 */
#define SYS_UDP_SEND   50 /* ebx=&udp_req                 -> 0 / -1 */
#define SYS_UDP_RECV   51 /* ebx=&udp_req                 -> payload len / -1 */
#define SYS_SOCKET     52 /* ebx=domain, ecx=type, edx=proto -> fd / -1 */
#define SYS_SENDTO     53 /* ebx=fd, ecx=&udp_req          -> 0 / -1 */
#define SYS_RECVFROM   54 /* ebx=fd, ecx=&udp_req          -> len / -1 */
#define SYS_CHROOT     55 /* ebx=path                      -> 0 / -1 */
#define SYS_TCP_TEST   56 /* 运行一次 TCP echo 自检           -> 0 / -1 */
#define SYS_BLK_READ   57 /* ebx=lba, ecx=user buf           -> 0 / -1 */
#define SYS_RENAME     58 /* ebx=old path, ecx=new path      -> 0 / -1（同卷 rename） */

/* v1 新增（只追加）：标量 ABI 版本查询。
 * 返回 (OS_ABI_VERSION << 16) | OS_ABI_FEATURES；高 16 位是 major，
 * 低 16 位是 feature bits。需要完整结构（major/minor/patch/features）时
 * 走 int 0x80 的 NATIVE_SYS_ABI_VERSION（0x00）。 */
#define SYS_ABI_VERSION 59

/* v1.1 新增（只追加）：进程/内存补缺（pm 会话）。
 * SYS_WAIT(20) 的阻塞语义保持不变；带 options 的等待走 SYS_WAITPID。 */
#define SYS_WAITPID    60 /* ebx=pid, ecx=&status, edx=options -> 子 pid / 0 / -1 */
#define SYS_GETPPID    61 /*                                    -> 父 pid（0=无） */
#define SYS_MPROTECT   62 /* ebx=addr, ecx=len, edx=prot         -> 0 / -1 */

/* v1.2 新增（只追加）：输入框光标显示/隐藏。
 * 只在 framebuffer 的输入框里画一条下划线，不写串口、不动输出区；
 * 位置跟随最后一次 SYS_INPUT_SET 画出的行尾。 */
#define SYS_CONSOLE_CURSOR 63 /* ebx=0 隐藏 / 1 显示              -> 0 / -1 */

/* SYS_WAITPID 的 options 位 */
#define WNOHANG 1

/* SYS_MPROTECT 的 prot 位（数值沿用 POSIX） */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* 用户可见信号编号（当前只暴露 SIGCHLD；数值与 proc/proc.h 一致） */
#define SIGCHLD 17
#define SIG_DFL 0u
#define SIG_IGN 1u

/* 已分配的最大编号；新增调用必须从 SYS_NR_MAX + 1 开始 */
#define SYS_NR_MAX 63

/* =============================================================================
 * 3. 用户可见共享结构体（布局冻结，见文件末尾断言）
 * ========================================================================== */

/* SYS_STAT 写出：6 个 u32，与内核 fs_stat_t 的字段顺序一致 */
typedef struct {
    uint32_t type;      /* 0=none, 1=file, 2=dir */
    uint32_t size;
    uint32_t mtime;     /* epoch 秒 */
    uint32_t mode;      /* 低 9 位 rwxrwxrwx */
    uint32_t uid;
    uint32_t gid;
} sys_stat_t;

/* SYS_UDP_SEND / SYS_UDP_RECV / SYS_SENDTO / SYS_RECVFROM 的请求/应答缓冲。
 * 用户先填 ip/port/len/data，SYS_UDP_RECV / SYS_RECVFROM 返回时内核回填
 * ip/port/len（data 里是 payload）。len 不能超过 sizeof(data)=64。 */
struct udp_req {
    uint8_t  ip[4];
    uint16_t port;
    uint16_t len;
    uint8_t  data[64];
};

/* SYS_MOUSE 写出：3 个 int32，{x, y, buttons} */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} sys_mouse_t;

/* SYS_RESINFO 写出：10 个 u32，顺序固定 */
typedef struct {
    uint32_t free_pages;    /* pmm 空闲页 */
    uint32_t total_pages;   /* pmm 总页 */
    uint32_t heap_used;     /* 内核堆已用字节 */
    uint32_t heap_total;    /* 内核堆总字节 */
    uint32_t proc_count;    /* 进程数 */
    uint32_t syscall_count; /* legacy 门调用次数 */
    uint32_t cow_copies;    /* COW 复制次数 */
    uint32_t fs_nodes;      /* 文件系统节点数 */
    uint32_t devices;       /* 设备数 */
    uint32_t pci_devices;   /* PCI 设备数 */
} sys_resinfo_t;

/* =============================================================================
 * 4. 用户可见错误码（core/error.h 的 E_* 子集）
 *
 * legacy 门没有统一 errno：多数 syscall 失败返回 -1，但文件系统路径
 * （如 fs_sync/fs_write 的错误）可能把下面的值原样带到用户态。
 * Native ABI 有另一套 NATIVE_E_*（proc/trap/native.h），数值不同，不能混用。
 * ========================================================================== */
_Static_assert(E_OK == 0, "ABI v1: E_OK must stay 0");
_Static_assert(E_INVAL == -1, "ABI v1: E_INVAL must stay -1");
_Static_assert(E_NOENT == -2, "ABI v1: E_NOENT must stay -2");
_Static_assert(E_NOSPC == -3, "ABI v1: E_NOSPC must stay -3");
_Static_assert(E_TYPE == -4, "ABI v1: E_TYPE must stay -4");
_Static_assert(E_BADH == -5, "ABI v1: E_BADH must stay -5");
_Static_assert(E_EXIST == -6, "ABI v1: E_EXIST must stay -6");
_Static_assert(E_FULL == -7, "ABI v1: E_FULL must stay -7");
_Static_assert(E_ACCES == -8, "ABI v1: E_ACCES must stay -8");
_Static_assert(E_BUSY == -9, "ABI v1: E_BUSY must stay -9");

/* =============================================================================
 * 5. 编译期布局断言（-Werror 下同样必须通过）
 * ========================================================================== */
_Static_assert(OS_ABI_MAGIC == 0x4F534142u, "ABI v1: magic is frozen");

_Static_assert(sizeof(sys_stat_t) == 24, "ABI v1: sys_stat_t is 6 x u32");
_Static_assert(__builtin_offsetof(sys_stat_t, type) == 0,
               "ABI v1: sys_stat_t.type offset");
_Static_assert(__builtin_offsetof(sys_stat_t, size) == 4,
               "ABI v1: sys_stat_t.size offset");
_Static_assert(__builtin_offsetof(sys_stat_t, mtime) == 8,
               "ABI v1: sys_stat_t.mtime offset");
_Static_assert(__builtin_offsetof(sys_stat_t, mode) == 12,
               "ABI v1: sys_stat_t.mode offset");
_Static_assert(__builtin_offsetof(sys_stat_t, uid) == 16,
               "ABI v1: sys_stat_t.uid offset");
_Static_assert(__builtin_offsetof(sys_stat_t, gid) == 20,
               "ABI v1: sys_stat_t.gid offset");

_Static_assert(sizeof(struct udp_req) == 72, "ABI v1: udp_req is 72 bytes");
_Static_assert(__builtin_offsetof(struct udp_req, ip) == 0,
               "ABI v1: udp_req.ip offset");
_Static_assert(__builtin_offsetof(struct udp_req, port) == 4,
               "ABI v1: udp_req.port offset");
_Static_assert(__builtin_offsetof(struct udp_req, len) == 6,
               "ABI v1: udp_req.len offset");
_Static_assert(__builtin_offsetof(struct udp_req, data) == 8,
               "ABI v1: udp_req.data offset");

_Static_assert(sizeof(sys_mouse_t) == 12, "ABI v1: sys_mouse_t is 3 x s32");

_Static_assert(sizeof(sys_resinfo_t) == 40, "ABI v1: sys_resinfo_t is 10 x u32");
_Static_assert(__builtin_offsetof(sys_resinfo_t, pci_devices) == 36,
               "ABI v1: sys_resinfo_t.pci_devices offset");

#endif /* OS_INCLUDE_ABI_H */
