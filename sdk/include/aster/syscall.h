/*
 * Aster SDK alpha —— syscall 调用号与内联封装（ABI 1.0 未冻结）
 *
 * 调用约定（与 src/ng/syscall.h 逐项一致）：
 *   rax = 调用号，rdi/rsi/rdx/r10 = 参数，返回值在 rax（负 = -errno）。
 *   用户指针由内核做 U/S 页校验 + SMAP copyin/copyout；SDK 只负责把参数
 *   放进寄存器。
 *
 * 调用号镜像（**改内核时必须同步这里**，tools/sdk-check.ps1 只保证语法，
 * 不做编号比对；差异由 code review + 本文件的注释表兜底）：
 *
 *   0  write(buf,len)                        -> 字节数 / -err
 *   1  exit(code)                            -> 不返回
 *  10  port_create(depth)                    -> ticket / -err
 *  11  port_send(ticket,msg,len,timeout)     -> 0 / -err
 *  12  port_recv(ticket,buf,max,timeout)     -> 总长 / -err
 *  13  port_reply(ticket,msg,len,timeout)    -> 0 / -err
 *  14  port_close(ticket)                    -> 0 / -err
 *  20  open(path,flags,mode)                 -> fd / -err
 *  21  read(fd,buf,len)                      -> 字节数 / -err
 *  22  write(fd,buf,len)                     -> 字节数 / -err
 *  23  close(fd)                             -> 0 / -err
 *  24  stat(path,&stat)                      -> 0 / -err
 *  25  listdir(path,&dirent,max)             -> 条数 / -err
 *  26  mkdir(path,mode)                      -> 0 / -err
 *  27  unlink(path)                          -> 0 / -err
 *  30  readkey()                             -> (flags<<24)|(ascii<<16)|scancode
 *  31  login(name,pass)                      -> uid / -err
 *  32  dmesg(buf,max)                        -> 字节数 / -err
 *  33  log_write(tag,msg)                    -> 0 / -err
 *  34  pkg_list(buf,max)                     -> 包数 / -err
 *  35  pkg_verify(name)                      -> 0 / -err
 *  40  spawn(path,argv,&pid)                 -> 0 / -err
 *  41  wait(pid,&status)                     -> 0 / -err（阻塞）
 *  42  getpid()                              -> pid
 *  43  kill(pid,sig)                         -> 0 / -err
 *  44  getppid()                             -> ppid
 *  45  shm_alloc(pages,&va,&handle)          -> 0 / -err（va 是 **8 字节**）
 *  46  shm_map(handle,&va)                   -> 0 / -err（va 是 **8 字节**）
 *  47  fb_map(&va,&fb_info)                  -> 0 / -err（root only）
 *
 * 时间单位：IPC 的 timeout 是 TSC tick（0 = 非阻塞，~0 = 永久）；QEMU TCG
 * 下约 1 tick ≈ 1 ns，文档里的 "20ms" 是以 20e6 tick 近似。
 */
#ifndef ASTER_SYSCALL_H
#define ASTER_SYSCALL_H

#include <aster/types.h>

/* ---- 调用号 -------------------------------------------------------------- */
#define ASTER_SYS_WRITE 0u
#define ASTER_SYS_EXIT 1u

#define ASTER_SYS_PORT_CREATE 10u
#define ASTER_SYS_PORT_SEND 11u
#define ASTER_SYS_PORT_RECV 12u
#define ASTER_SYS_PORT_REPLY 13u
#define ASTER_SYS_PORT_CLOSE 14u

#define ASTER_SYS_OPEN 20u
#define ASTER_SYS_READ 21u
#define ASTER_SYS_WRITE_FD 22u
#define ASTER_SYS_CLOSE 23u
#define ASTER_SYS_STAT 24u
#define ASTER_SYS_LISTDIR 25u
#define ASTER_SYS_MKDIR 26u
#define ASTER_SYS_UNLINK 27u

#define ASTER_SYS_READKEY 30u
#define ASTER_SYS_LOGIN 31u
#define ASTER_SYS_DMESG 32u
#define ASTER_SYS_LOG_WRITE 33u
#define ASTER_SYS_PKG_LIST 34u
#define ASTER_SYS_PKG_VERIFY 35u

#define ASTER_SYS_SPAWN 40u
#define ASTER_SYS_WAIT 41u
#define ASTER_SYS_GETPID 42u
#define ASTER_SYS_KILL 43u
#define ASTER_SYS_GETPPID 44u
#define ASTER_SYS_SHM_ALLOC 45u
#define ASTER_SYS_SHM_MAP 46u
#define ASTER_SYS_FB_MAP 47u

/* ---- errno（数值与 src/services/fs/include/vfs.h 一致） ------------------ */
#define ASTER_EPERM (-1)
#define ASTER_ENOENT (-2)
#define ASTER_EIO (-5)
#define ASTER_EBADF (-9)
#define ASTER_EAGAIN (-11)
#define ASTER_ENOMEM (-12)
#define ASTER_EACCES (-13)
#define ASTER_EFAULT (-14)
#define ASTER_EBUSY (-16)
#define ASTER_EEXIST (-17)
#define ASTER_ENODEV (-19)
#define ASTER_ENOTDIR (-20)
#define ASTER_EISDIR (-21)
#define ASTER_EINVAL (-22)
#define ASTER_ENFILE (-23)
#define ASTER_EMFILE (-24)
#define ASTER_ENOSPC (-28)
#define ASTER_EROFS (-30)
#define ASTER_ENAMETOOLONG (-36)
#define ASTER_ENOSYS (-38)
#define ASTER_ENOTEMPTY (-39)
#define ASTER_ELOOP (-40)
#define ASTER_EMSGSIZE (-90)
#define ASTER_ETIMEDOUT (-110)
#define ASTER_ECANCELED (-125)

/* ---- IPC 超时约定 -------------------------------------------------------- */
#define ASTER_TIMEOUT_NOW 0ULL
#define ASTER_TIMEOUT_FOREVER (~0ULL)

/* ---- 原始 syscall -------------------------------------------------------- */
#if !defined(__x86_64__)
#error "Aster SDK alpha implements only the x86_64 SysV syscall ABI"
#endif

static inline i64 aster_syscall4(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4)
{
    register i64 r10 __asm__("r10") = (i64)a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((i64)nr), "D"((i64)a1), "S"((i64)a2),
                       "d"((i64)a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline i64 aster_syscall3(u64 nr, u64 a1, u64 a2, u64 a3)
{
    return aster_syscall4(nr, a1, a2, a3, 0);
}

static inline i64 aster_syscall2(u64 nr, u64 a1, u64 a2)
{
    return aster_syscall4(nr, a1, a2, 0, 0);
}

static inline i64 aster_syscall1(u64 nr, u64 a1)
{
    return aster_syscall4(nr, a1, 0, 0, 0);
}

static inline i64 aster_syscall0(u64 nr)
{
    return aster_syscall4(nr, 0, 0, 0, 0);
}

/* ---- 极简 mem/str（无 libc；仅供 SDK 内部和示例使用） -------------------- */
static inline void aster_memzero(void *dst, u64 n)
{
    u8 *p = (u8 *)dst;
    u64 i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static inline void aster_memcopy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    if (d == NULL || s == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static inline u64 aster_strlen(const char *s)
{
    u64 n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline int aster_streq(const char *a, const char *b)
{
    u64 i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return (a[i] == b[i]) ? 1 : 0;
}

/* ---- 控制台（syscall 0） ------------------------------------------------- */
/* 内核单次 write 可靠上限按 256 字节分块（与 shell 一致）。 */
#define ASTER_CONSOLE_CHUNK 256u

static inline i64 aster_console_write(const char *buf, u64 len)
{
    u64 off = 0;
    i64 total = 0;

    if (buf == NULL) {
        return ASTER_EINVAL;
    }
    while (off < len) {
        u64 chunk = len - off;
        i64 rc;

        if (chunk > (u64)ASTER_CONSOLE_CHUNK) {
            chunk = (u64)ASTER_CONSOLE_CHUNK;
        }
        rc = aster_syscall3(ASTER_SYS_WRITE,
                            (u64)(usize)(buf + off), chunk, 0);
        if (rc < 0) {
            return (total > 0) ? total : rc;
        }
        if (rc == 0) {
            break;
        }
        total += rc;
        off += (u64)rc;
    }
    return total;
}

static inline i64 aster_puts(const char *s)
{
    return aster_console_write(s, aster_strlen(s));
}

static inline __attribute__((noreturn)) void aster_exit(int code)
{
    (void)aster_syscall1(ASTER_SYS_EXIT, (u64)(i64)code);
    for (;;) {
        /* 理论上不可达；保持 noreturn 语义。 */
    }
}

/* ---- 进程 ---------------------------------------------------------------- */
static inline u32 aster_getpid(void)
{
    return (u32)aster_syscall0(ASTER_SYS_GETPID);
}

static inline u32 aster_getppid(void)
{
    return (u32)aster_syscall0(ASTER_SYS_GETPPID);
}

static inline i64 aster_spawn(const char *path, const char *const argv[],
                              u32 *out_pid)
{
    u32 pid = 0;
    i64 rc;

    if (path == NULL) {
        return ASTER_EINVAL;
    }
    rc = aster_syscall3(ASTER_SYS_SPAWN, (u64)(usize)path,
                        (u64)(usize)argv, (u64)(usize)&pid);
    if (rc == 0 && out_pid != NULL) {
        *out_pid = pid;
    }
    return rc;
}

static inline i64 aster_wait(u32 pid, i32 *out_status)
{
    i32 status = 0;
    i64 rc = aster_syscall2(ASTER_SYS_WAIT, (u64)pid, (u64)(usize)&status);

    if (rc == 0 && out_status != NULL) {
        *out_status = status;
    }
    return rc;
}

static inline i64 aster_kill(u32 pid, u32 sig)
{
    return aster_syscall2(ASTER_SYS_KILL, (u64)pid, (u64)sig);
}

/* ---- 键盘（syscall 30；非阻塞，-EAGAIN = 没键） -------------------------- */
static inline i64 aster_readkey(void)
{
    return aster_syscall0(ASTER_SYS_READKEY);
}

#define ASTER_KEY_SCANCODE(k) ((u32)(k) & 0xFFu)
#define ASTER_KEY_ASCII(k) (((u32)(k) >> 16) & 0xFFu)
#define ASTER_KEY_FLAGS(k) (((u32)(k) >> 24) & 0xFFu)
#define ASTER_KEY_F_CTRL 0x04u

/* ---- 日志（syscall 33；tag/message 都是 NUL 结尾） ----------------------- */
static inline i64 aster_log_write(const char *tag, const char *msg)
{
    return aster_syscall2(ASTER_SYS_LOG_WRITE, (u64)(usize)tag,
                          (u64)(usize)msg);
}

static inline i64 aster_dmesg(char *buf, u32 max)
{
    return aster_syscall2(ASTER_SYS_DMESG, (u64)(usize)buf, (u64)max);
}

/* ---- 共享内存（syscall 45/46） ------------------------------------------- */
/*
 * 注意：内核 copyout 的是 **u64 va**，所以这里必须传 8 字节变量的地址。
 * （src/apps/demo/demo.c 里用 u32 接 va 是历史写法，SDK 不复制这个坑。）
 */
static inline i64 aster_shm_alloc(u32 pages, u64 *out_va, u32 *out_handle)
{
    u64 va = 0;
    u32 handle = 0;
    i64 rc = aster_syscall3(ASTER_SYS_SHM_ALLOC, (u64)pages,
                            (u64)(usize)&va, (u64)(usize)&handle);

    if (rc == 0) {
        if (out_va != NULL) {
            *out_va = va;
        }
        if (out_handle != NULL) {
            *out_handle = handle;
        }
    }
    return rc;
}

static inline i64 aster_shm_map(u32 handle, u64 *out_va)
{
    u64 va = 0;
    i64 rc;

    if (handle == 0) {
        return ASTER_EBADF;
    }
    rc = aster_syscall2(ASTER_SYS_SHM_MAP, (u64)handle, (u64)(usize)&va);
    if (rc == 0 && out_va != NULL) {
        *out_va = va;
    }
    return rc;
}

#endif /* ASTER_SYSCALL_H */
