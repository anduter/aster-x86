/*
 * Aster native ABI 1.0 — 机器可读冻结头（E4 / WP4-1）
 *
 * 状态：FROZEN（2026-10-31）。语义说明见同目录 abi-1.0.md。
 *
 * 这是 ABI 1.0 的规范镜像，不是 SDK 的一部分；应用应使用
 * sdk/include/aster/syscall.h 的内联包装，不要直接包含本文件。
 *
 * 三重编号必须一致：
 *   - 本文件 ASTER_ABI_SYS_*（冻结契约）
 *   - sdk/include/aster/syscall.h ASTER_SYS_*（SDK 消费端）
 *   - src/ng/syscall.h NG_SYS_*（内核实现）
 * tools/abi-check.ps1 逐号比对；任何一处改动而不同步，门禁失败。
 *
 * 调用约定（x86_64）：
 *   rax = 调用号；rdi/rsi/rdx/r10 = 参数 1..4；返回值 = rax
 *   （有符号 64 位；负值 = -errno）。
 *   syscall 指令破坏 rcx/r11；ABI 只承诺 rax，其它寄存器不保证跨调用保留。
 *   用户指针必须是当前进程 AS 里的 U=1 页面；内核经 uaccess 校验。
 *
 * 冻结规则：
 *   - 已有调用号/参数/返回值/错误码值/结构体布局不变；
 *   - 新能力只能新增调用号或结构体尾部字段；
 *   - 删除/保留的调用号不复用；
 *   - 未分配调用号的行为不属于 ABI 1.0（当前实现返回 -EPERM，不要依赖）。
 */
#ifndef ASTER_ABI_1_0_H
#define ASTER_ABI_1_0_H

/* ------------------------------------------------------------------------- */
/* 版本                                                                       */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_VERSION_MAJOR 1
#define ASTER_ABI_VERSION_MINOR 0
#define ASTER_ABI_VERSION_PATCH 0
#define ASTER_ABI_VERSION 0x00010000
#define ASTER_ABI_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------------- */
/* 调用号（与 src/ng/syscall.h 逐号一致）                                     */
/* ------------------------------------------------------------------------- */

/* 基础：控制台 / 退出。0 是控制台写，不是 fd 写；fd 写是 22。 */
#define ASTER_ABI_SYS_WRITE 0
#define ASTER_ABI_SYS_EXIT 1

/* IPC / 端口（ticket 是全局端口身份）。 */
#define ASTER_ABI_SYS_PORT_CREATE 10
#define ASTER_ABI_SYS_PORT_SEND 11
#define ASTER_ABI_SYS_PORT_RECV 12
#define ASTER_ABI_SYS_PORT_REPLY 13
#define ASTER_ABI_SYS_PORT_CLOSE 14

/* 文件系统（fd 命名空间）。 */
#define ASTER_ABI_SYS_OPEN 20
#define ASTER_ABI_SYS_READ 21
#define ASTER_ABI_SYS_WRITE_FD 22
#define ASTER_ABI_SYS_CLOSE 23
#define ASTER_ABI_SYS_STAT 24
#define ASTER_ABI_SYS_LISTDIR 25
#define ASTER_ABI_SYS_MKDIR 26
#define ASTER_ABI_SYS_UNLINK 27

/* 输入 / L1 服务。 */
#define ASTER_ABI_SYS_READKEY 30
#define ASTER_ABI_SYS_LOGIN 31
#define ASTER_ABI_SYS_DMESG 32
#define ASTER_ABI_SYS_LOG_WRITE 33
#define ASTER_ABI_SYS_PKG_LIST 34
#define ASTER_ABI_SYS_PKG_VERIFY 35

/* 进程 / 共享内存 / LFB。 */
#define ASTER_ABI_SYS_SPAWN 40
#define ASTER_ABI_SYS_WAIT 41
#define ASTER_ABI_SYS_GETPID 42
#define ASTER_ABI_SYS_KILL 43
#define ASTER_ABI_SYS_GETPPID 44
#define ASTER_ABI_SYS_SHM_ALLOC 45
#define ASTER_ABI_SYS_SHM_MAP 46
#define ASTER_ABI_SYS_FB_MAP 47

/* ------------------------------------------------------------------------- */
/* 错误码（负值；冻结后数值不变）                                             */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_EPERM (-1)
#define ASTER_ABI_ENOENT (-2)
#define ASTER_ABI_ESRCH (-3)
#define ASTER_ABI_EINTR (-4)
#define ASTER_ABI_EIO (-5)
#define ASTER_ABI_EBADF (-9)
#define ASTER_ABI_ECHILD (-10)
#define ASTER_ABI_EAGAIN (-11)
#define ASTER_ABI_ENOMEM (-12)
#define ASTER_ABI_EACCES (-13)
#define ASTER_ABI_EFAULT (-14)
#define ASTER_ABI_EBUSY (-16)
#define ASTER_ABI_EEXIST (-17)
#define ASTER_ABI_ENODEV (-19)
#define ASTER_ABI_ENOTDIR (-20)
#define ASTER_ABI_EISDIR (-21)
#define ASTER_ABI_EINVAL (-22)
#define ASTER_ABI_ENFILE (-23)
#define ASTER_ABI_EMFILE (-24)
#define ASTER_ABI_ENOSPC (-28)
#define ASTER_ABI_EROFS (-30)
#define ASTER_ABI_ENAMETOOLONG (-36)
#define ASTER_ABI_ENOSYS (-38) /* 保留：1.0 内核当前不返回；1.1 用于未分配号 */
#define ASTER_ABI_ENOTEMPTY (-39) /* 保留：1.0 没有 rmdir */
#define ASTER_ABI_ELOOP (-40)
#define ASTER_ABI_EMSGSIZE (-90)
#define ASTER_ABI_ETIMEDOUT (-110)
#define ASTER_ABI_ECANCELED (-125)

/* ------------------------------------------------------------------------- */
/* 句柄编码（四类命名空间共用 (generation << 16) | slot 的 32 位布局）        */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_HANDLE_SLOT_MASK 0xFFFFu
#define ASTER_ABI_HANDLE_GEN_SHIFT 16
#define ASTER_ABI_HANDLE_MAX 64u   /* 每进程句柄表槽数（PORT/THREAD/FILE） */
#define ASTER_ABI_FD_MAX 16u       /* VFS ctx 的 fd 槽数 */
#define ASTER_ABI_PORT_MAX 16u     /* 全局端口池 */
#define ASTER_ABI_SHM_MAX 8u       /* 全局共享内存对象表 */
#define ASTER_ABI_HANDLE_INVALID 0u

/*
 * 四类命名空间（数值可能相同，由 syscall 决定解释；0 永远非法）：
 *   fd      (gen<<16)|slot, slot 0..15  —— 1.0 是全局一份 VFS ctx
 *   handle  (gen<<16)|slot, slot 0..63  —— 每进程句柄表，spawn 不继承
 *   ticket  (gen<<16)|slot, slot 0..15  —— 全局端口池，唯一可跨进程的名字
 *   shm     (gen<<16)|slot, slot 0..7   —— 全局共享内存对象，可经 IPC 传递
 * 角色令牌（role_token）属于 L2 会话协议，不是 syscall ABI 的第五类命名空间。
 */

/* ------------------------------------------------------------------------- */
/* IPC 常量                                                                   */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_IPC_PAYLOAD_MAX 4096u
#define ASTER_ABI_IPC_MSG_MAX 4112u /* 16 B 头 + 4096 B payload */
#define ASTER_ABI_IPC_DEPTH_DEFAULT 16u
#define ASTER_ABI_IPC_DEPTH_MAX 64u

/* timeout 是不透明 TSC tick：0 = 非阻塞，~0 = 永久；其它值不承诺毫秒。 */
#define ASTER_ABI_TIMEOUT_NOW 0ULL
#define ASTER_ABI_TIMEOUT_FOREVER (~0ULL)

/* ------------------------------------------------------------------------- */
/* 文件系统常量 / flags / 结构体大小                                          */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_CONSOLE_MAX 256u     /* 单次控制台 write */
#define ASTER_ABI_FD_IO_MAX 512u       /* 单次 fd read/write */
#define ASTER_ABI_LISTDIR_MAX 8u       /* 单次 listdir 条数 */

#define ASTER_ABI_VFS_PATH_MAX 256u    /* 含 NUL；有效 <= 254 字符 */
#define ASTER_ABI_VFS_NAME_MAX 64u     /* 组件含 NUL；有效 <= 63 字符 */
#define ASTER_ABI_VFS_MOUNT_MAX 8u
#define ASTER_ABI_VFS_SYMLINK_MAX 8u   /* 符号链接深度上限 */

#define ASTER_ABI_STRUCT_VFS_STAT_SIZE 24u
#define ASTER_ABI_STRUCT_VFS_DIRENT_SIZE 80u
#define ASTER_ABI_STRUCT_IPC_MSG_HDR_SIZE 16u
#define ASTER_ABI_STRUCT_DS_FB_INFO_SIZE 48u

#define ASTER_ABI_O_RDONLY 0x000u
#define ASTER_ABI_O_WRONLY 0x001u
#define ASTER_ABI_O_RDWR 0x002u
#define ASTER_ABI_O_ACCMODE 0x003u
#define ASTER_ABI_O_CREAT 0x040u
#define ASTER_ABI_O_EXCL 0x080u
#define ASTER_ABI_O_TRUNC 0x200u
#define ASTER_ABI_O_APPEND 0x400u
#define ASTER_ABI_O_DIRECTORY 0x800u
#define ASTER_ABI_O_FLAGS_MASK 0xEC3u

#define ASTER_ABI_S_IFREG 0x8000u
#define ASTER_ABI_S_IFDIR 0x4000u
#define ASTER_ABI_S_IFLNK 0xA000u
#define ASTER_ABI_S_IFMT 0xF000u

/* ------------------------------------------------------------------------- */
/* 输入（readkey：bits 23..16 ascii / 15..8 scancode / 7..0 flags）           */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_KEY_F_SHIFT 0x01u
#define ASTER_ABI_KEY_F_CAPS 0x02u
#define ASTER_ABI_KEY_F_CTRL 0x04u
#define ASTER_ABI_KEY_F_EXT 0x08u

#define ASTER_ABI_KEY_SCANCODE_MASK 0xFFu
#define ASTER_ABI_KEY_ASCII_SHIFT 16
#define ASTER_ABI_KEY_FLAGS_SHIFT 24

/* ------------------------------------------------------------------------- */
/* auth / log / pkg 上限                                                      */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_LOGIN_NAME_MAX 32u   /* 含 NUL；有效 <= 30 字符 */
#define ASTER_ABI_LOGIN_PASS_MAX 64u   /* 含 NUL；有效 <= 62 字符 */
#define ASTER_ABI_AUTH_MAX_USERS 8u
#define ASTER_ABI_AUTH_ITERATIONS 4096u
#define ASTER_ABI_AUTH_MAX_FAILS 5u

#define ASTER_ABI_LOG_TAG_MAX 16u      /* 服务层 1..15；syscall 路径有效 <= 14 */
#define ASTER_ABI_LOG_MSG_MAX 128u     /* 服务层 1..127；syscall 路径有效 <= 126 */
#define ASTER_ABI_LOG_CAPACITY 65536u  /* 64 KiB 环形缓冲 */
#define ASTER_ABI_DMESG_MAX 4096u

#define ASTER_ABI_PKG_NAME_MAX 32u     /* 含 NUL；syscall 路径有效 <= 30 */
#define ASTER_ABI_PKG_LIST_MAX 512u    /* syscall 34 的 max 上限（服务层 1024） */
#define ASTER_ABI_PKG_MAX_PACKAGES 16u

/* ------------------------------------------------------------------------- */
/* 进程 / 地址空间 / 共享内存 / LFB 上限                                      */
/* ------------------------------------------------------------------------- */

#define ASTER_ABI_PROC_MAX 32u
#define ASTER_ABI_SPAWN_PATH_MAX 64u      /* 含 NUL；有效 <= 62 字符 */
#define ASTER_ABI_SPAWN_ARGS_MAX 8u
#define ASTER_ABI_SPAWN_ARG_MAX 128u      /* 含 NUL；有效 <= 126 字符 */
#define ASTER_ABI_SPAWN_ARGV_TOTAL_MAX 1024u /* 所有 argv 字符串 + NUL 总和 */
#define ASTER_ABI_SPAWN_ELF_MAX 262144u    /* 256 KiB */
#define ASTER_ABI_PROC_STACK_PAGES 4u
#define ASTER_ABI_PROC_MARKER_VA 0x0000000040010000ULL
#define ASTER_ABI_PROC_STATUS_KILL_FLAG 0x100

#define ASTER_ABI_USER_VA_MAX 0x00007FFFFFFFFFFFULL
#define ASTER_ABI_USER_STACK_TOP 0x0000000040104000ULL
#define ASTER_ABI_USER_STACK_GUARD 0x00000000400FF000ULL

#define ASTER_ABI_SHM_PAGES_MAX 1024u
#define ASTER_ABI_SHM_MAPS_MAX 8u
#define ASTER_ABI_SHM_PAGE_SIZE 4096ULL
#define ASTER_ABI_SHM_VA_BASE 0x0000000050000000ULL
#define ASTER_ABI_SHM_VA_LIMIT 0x000000005F000000ULL

#define ASTER_ABI_FB_VA_BASE 0x0000000060000000ULL

#endif /* ASTER_ABI_1_0_H */
