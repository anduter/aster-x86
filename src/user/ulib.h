#ifndef ULIB_H
#define ULIB_H

/* 用户态最小 C 库：迁移期只依赖 int 0x81，不链接内核。
 * 系统调用编号/结构体/错误码的唯一来源是内核侧 include/abi.h；
 * 这里用相对路径引入，user 程序不依赖 -Isrc/kernel。 */
#include "../kernel/include/abi.h"

typedef unsigned int usize;

int syscall3(int n, int a, int b, int c);

/* SYS_ABI_VERSION 的标量封装：高 16 位 ABI major，低 16 位 feature bits */
int abi_version(void);

/* ---- IPC（ABI 1.5 预留：SYS_PORT_CREATE/SEND/RECV/REPLY/CAP_GRANT = 80..84）
 *
 * 消息帧 = 16B 头（magic/type/flags/ncaps/len）+ payload + cap 数组
 * （每项 8B：type/id）。port_send/port_reply 负责组头，payload ≤ 4096。
 * 端口 id 是内核返回的不透明句柄（带 generation）；ticket = (pid<<16)|port，
 * 由 port_ticket() 给出，请求方把它放进业务 payload 交给服务。
 * 失败返回 -1；超时返回 -IPC_EAGAIN；被信号打断返回 -IPC_EINTR。 */
#define IPC_MAGIC        0x49504331U
#define IPC_HDR_SIZE     16
#define IPC_PAYLOAD_MAX  4096
#define IPC_NCAPS_MAX    4

#define IPC_F_NEED_REPLY 0x1
#define IPC_F_NO_REPLY   0x2

#define IPC_CAP_PORT     1
#define IPC_CAP_MMIO     2
#define IPC_CAP_IRQ      3
#define IPC_CAP_DMA      4
#define IPC_CAP_FILE     5
#define IPC_CAP_SOCKET   6

#define IPC_RIGHT_READ   0x1
#define IPC_RIGHT_WRITE  0x2
#define IPC_RIGHTS_DEFAULT (IPC_RIGHT_READ | IPC_RIGHT_WRITE)

#define IPC_EINTR        4
#define IPC_EAGAIN       11

struct ipc_msg_hdr {
    unsigned int   magic;
    unsigned int   type;
    unsigned short flags;
    unsigned short ncaps;
    unsigned int   len;
} __attribute__((packed));

int port_create(int flags);
/* 直接发送已组好的整帧（含 16B 头）；len 是整帧长度。 */
int port_send_raw(int port, const void *msg, int len);
int port_send(int port, unsigned int type, unsigned int flags,
              const void *payload, int len);
int port_recv(int port, void *buf, int maxlen);
int port_reply_raw(int ticket, const void *msg, int len);
int port_reply(int ticket, unsigned int type, const void *payload, int len);
/* rights 走第 4 个寄存器（esi），0 = 默认 RW；见 docs/claims/ipc.md。 */
int cap_grant(int port, int type, int id, int rights);
/* 请求方 ticket：(本进程 pid << 16) | 端口句柄 */
int port_ticket(int port);

/* ---- 服务注册表（ABI 1.5：SYS_SVC_REGISTER/LOOKUP/HEARTBEAT = 86..88） ----
 *
 * manifest 文本格式见 docs/design/three-layer.md §5（name/version/type/
 * entry/requires/capabilities/restart/restart_limit/uid，一行一个 key=value）。
 * register 只允许 root（CAP_SYS_ADMIN 位图未落地）且只能登记自己的端口；
 * 重名/坏 manifest 返回 -1。
 *
 * lookup 返回内核分配的服务端口句柄；服务重启期间/隔离后返回 -1，客户端
 * 必须重新 lookup（旧端口已在重启时回收，旧句柄的 SEND 会直接失败）。
 */
int svc_register(const char *manifest, int port);
int svc_lookup(const char *name);
int svc_heartbeat(int port);
/* SYS_LOG_WRITE(93)：写一条带 tag 的 klog 行（msg 长度由 strlen 决定）。 */
int svc_log_write(const char *tag, const char *msg);
/* 一问一答：请求 payload = 4B ticket + req；回复 payload 拷进 reply。
 * 返回回复 payload 长度（最多 maxlen）；-1 失败；-IPC_EAGAIN 超时；
 * -IPC_EINTR 被打断。只用 1 个缓存客户端端口（当前 ABI 没有 PORT_CLOSE，
 * 见 docs/claims/ipc.md 设计问题 #7）。 */
int svc_call(int port, unsigned int type, const void *req, int reqlen,
             void *reply, int maxlen);
/* 按名字 lookup + svc_call；失败（-1，通常是服务刚重启）时重新 lookup 再试一次。 */
int svc_call_name(const char *name, unsigned int type, const void *req,
                  int reqlen, void *reply, int maxlen);

/* 改名/移动（同卷内）。newpath 是完整目标路径，不做 `mv` 的"移入目录"语义。
 * 成功返回 0，失败返回 -1（跨卷/目标非空目录/权限不足都算失败）。 */
static inline int fs_rename(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_RENAME, (int)(unsigned int)oldpath, (int)(unsigned int)newpath, 0);
}


/* ---- 硬链接 / 符号链接（ABI 追加：SYS_LINK/SYS_SYMLINK/SYS_READLINK/SYS_LSTAT）----
 * fs_link：同卷、普通文件；fs_symlink：目标文本原样保存；fs_readlink 返回长度；
 * fs_lstat 不跟随末段，输出布局与 fs_stat 一致（type/size/mtime/mode/uid/gid）。 */
static inline int fs_link(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_LINK, (int)(unsigned int)oldpath, (int)(unsigned int)newpath, 0);
}
static inline int fs_symlink(const char *target, const char *linkpath)
{
    return syscall3(SYS_SYMLINK, (int)(unsigned int)target, (int)(unsigned int)linkpath, 0);
}
static inline int fs_readlink(const char *path, char *buf, int max)
{
    return syscall3(SYS_READLINK, (int)(unsigned int)path, (int)(unsigned int)buf, max);
}
static inline int fs_lstat(const char *path, unsigned int *st)
{
    return syscall3(SYS_LSTAT, (int)(unsigned int)path, (int)(unsigned int)st, 0);
}
/* 基于 fd 的读写（0=stdin，1=stdout，其余是文件/管道） */
int fd_read(int fd, void *buf, int len);
int fd_write(int fd, const void *buf, int len);

/* ---- 进程/内存（ABI v1.1，pm 会话） ---- */
/* 等待子进程：options=0 阻塞；WNOHANG 时无可回收子进程立即返回 0。
 * 成功回收返回子 pid；失败 -1。 */
int waitpid(int pid, int *status, int options);
/* 当前进程的父 pid；无父返回 0 */
int getppid(void);
/* 修改本进程一段用户内存的保护位（PROT_*）；成功 0，失败 -1 */
int mprotect(void *addr, unsigned int len, int prot);
/* 设置信号处理函数（SIG_DFL/SIG_IGN/用户地址）；返回旧 handler，失败 -1 */
int signal(int sig, unsigned int handler);

/* ---- 控制台（ABI v1.2） ---- */
/* 输入框下划线光标：visible!=0 显示，0 隐藏。只在 framebuffer 输入框上画，
 * 不写串口；位置自动跟随最后一次 SYS_INPUT_SET 画出的行尾。 */
int console_cursor(int visible);

/* ---- 身份/账号（ABI v1.3，P1）----
 * 权限判定用"生效身份"（euid/egid + 附加组）；root = euid 0。
 * 这些是 int 0x81 的薄包装，内核侧实现在 security/security_syscall.c。 */
static inline int geteuid(void)
{
    return syscall3(SYS_GETEUID, 0, 0, 0);
}

static inline int getegid(void)
{
    return syscall3(SYS_GETEGID, 0, 0, 0);
}

static inline int seteuid(int euid)
{
    return syscall3(SYS_SETEUID, euid, 0, 0);
}

static inline int setegid(int egid)
{
    return syscall3(SYS_SETEGID, egid, 0, 0);
}

static inline int setgid(int gid)
{
    return syscall3(SYS_SETGID, gid, 0, 0);
}

/* 附加组：返回组数（buf 可为 0，只查询个数） */
static inline int getgroups(int *buf, int max)
{
    return syscall3(SYS_GETGROUPS, (int)buf, max, 0);
}

static inline int setgroups(const int *buf, int n)
{
    return syscall3(SYS_SETGROUPS, (int)buf, n, 0);
}

/* 返回旧 umask */
static inline int umask(int mask)
{
    return syscall3(SYS_SETUMASK, mask, 0, 0);
}

/* ---- SYS_LOGIN 的封装（PAM-lite：口令在核内比较） ---- */
static inline int login_needed(void)
{
    return syscall3(SYS_LOGIN, LOGIN_CHECK, 0, 0);
}

static inline int login_auth(const char *user, const char *pass)
{
    return syscall3(SYS_LOGIN, LOGIN_AUTH, (int)user, (int)pass);
}

static inline int login_su(const char *user, const char *pass)
{
    return syscall3(SYS_LOGIN, LOGIN_SU, (int)user, (int)pass);
}

static inline int login_sudo(const char *pass)
{
    return syscall3(SYS_LOGIN, LOGIN_SUDO, (int)pass, 0);
}

static inline int login_sudo_end(void)
{
    return syscall3(SYS_LOGIN, LOGIN_SUDO_END, 0, 0);
}

static inline int login_logout(void)
{
    return syscall3(SYS_LOGIN, LOGIN_LOGOUT, 0, 0);
}

/* 改自己的口令：ecx=旧口令, edx=新口令（顺序与内核参数一致） */
static inline int login_passwd_self(const char *oldpw, const char *newpw)
{
    return syscall3(SYS_LOGIN, LOGIN_PASSWD, (int)oldpw, (int)newpw);
}

/* root 给任意账户设口令 */
static inline int login_passwd_user(const char *user, const char *newpw)
{
    return syscall3(SYS_LOGIN, LOGIN_PASSWD_USER, (int)user, (int)newpw);
}

/* who / last：返回行数，文本写进 buf */
static inline int login_who(char *buf, int max)
{
    return syscall3(SYS_LOGIN, LOGIN_WHO, (int)buf, max);
}

static inline int login_last(char *buf, int max)
{
    return syscall3(SYS_LOGIN, LOGIN_LAST, (int)buf, max);
}

/* "uid=.. euid=.. gid=.. egid=.. groups=.. name=.." */
static inline int login_info(char *buf, int max)
{
    return syscall3(SYS_LOGIN, LOGIN_INFO, (int)buf, max);
}

int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);

void *malloc(usize n);
void free(void *p);

usize strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
void *memcpy(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

#endif
