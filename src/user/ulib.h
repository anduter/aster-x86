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

/* 改名/移动（同卷内）。newpath 是完整目标路径，不做 `mv` 的"移入目录"语义。
 * 成功返回 0，失败返回 -1（跨卷/目标非空目录/权限不足都算失败）。 */
static inline int fs_rename(const char *oldpath, const char *newpath)
{
    return syscall3(SYS_RENAME, (int)(unsigned int)oldpath, (int)(unsigned int)newpath, 0);
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

#endif
