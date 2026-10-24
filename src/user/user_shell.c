/* =============================================================================
 * user_shell.c - 运行在 ring3 的 shell
 *
 * 它不链接任何内核代码：迁移期所有服务都通过 int 0x81 请求，
 * 包括输出、读键盘、打开/读写/关闭文件、列目录。
 * 编译后链接到 0x40000000，由内核从文件系统 "/bin/shell" 载入。
 * ============================================================================= */

#include "ulib.h"

/* 与内核 syscall.c 的 SYS_UDP_* 请求布局一致 */
struct udp_req {
    unsigned char  ip[4];
    unsigned short port;
    unsigned short len;
    unsigned char  data[64];
};
#define SYS_WRITE    1
#define SYS_EXIT     5
#define SYS_OPEN     6
#define SYS_FREAD    7
#define SYS_FWRITE   8
#define SYS_CLOSE    9
#define SYS_CAP_DROP 46
#define SYS_CAP_SEND 47
#define SYS_MMAP     48
#define SYS_MUNMAP   49
#define SYS_UDP_SEND 50
#define SYS_UDP_RECV 51
#define SYS_SOCKET   52
#define SYS_SENDTO   53
#define SYS_RECVFROM 54
#define SYS_CHROOT   55
#define SYS_GETCHAR  10
#define SYS_LIST     11
#define SYS_DRIVES   12
#define SYS_MKDIR    13
#define SYS_UNLINK   14
#define SYS_STAT     15
#define SYS_CHDIR    16
#define SYS_GETCWD   17
#define SYS_FORK     18
#define SYS_EXEC     19
#define SYS_WAIT     20
#define SYS_INPUT_SET 21
#define SYS_KILL     23
#define SYS_SLEEP    24
#define SYS_PIPE     25
#define SYS_DUP2     26
#define SYS_REBOOT   27
#define SYS_PANIC    28
#define SYS_OOPS     29
#define SYS_SIGACTION 30
#define SYS_SIGRETURN 31
#define SYS_SYNC      32
#define SYS_TIME      33
#define SYS_FSCK      34
#define SYS_PCI       35
#define SYS_POWEROFF  36
#define SYS_MOUSE     37
#define SYS_HANG      38
#define SYS_ETH       39
#define SYS_RESINFO   40
#define SYS_GETUID    41
#define SYS_GETGID    42
#define SYS_CHMOD     43
#define SYS_CHOWN     44
#define SYS_SETUID    45
#define SYS_GETPID    2

/* 打开标志（与内核 fs/fs.h 一致） */
#define FS_READ   1
#define FS_WRITE  2
#define FS_CREAT  4

static inline int sc(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int scmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void putstr(const char *s)
{
    sc(SYS_FWRITE, 1, (int)s, (int)slen(s));   /* 走 fd 1，支持重定向 */
}

static void putch(char c)
{
    sc(SYS_FWRITE, 1, (int)&c, 1);
}

/* 错误输出走 fd 2：不会被 `>` 重定向吞掉 */
static void puterr(const char *s)
{
    sc(SYS_FWRITE, 2, (int)s, (int)slen(s));
}

static void puterr2(const char *a, const char *b)
{
    puterr(a);
    puterr(b);
    puterr("\n");
}

/* 参数解析：支持双引号、单引号和反斜杠转义（就地改字符串）
 *   write system:/a.txt "hello world"   -> 2 个参数，第二个含空格 */
static int split(char *s, char **argv, int max)
{
    int argc = 0;

    while (*s != '\0' && argc < max) {
        char *w;
        char quote = 0;

        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;

        argv[argc++] = s;
        w = s;

        while (*s != '\0') {
            if (quote != 0) {
                if (*s == quote) { s++; quote = 0; continue; }
                if (*s == '\\' && s[1] != '\0') { s++; *w++ = *s++; continue; }
                *w++ = *s++;
            } else {
                if (*s == '"' || *s == '\'') { quote = *s++; continue; }
                if (*s == ' ' || *s == '\t') break;
                if (*s == '\\' && s[1] != '\0') { s++; *w++ = *s++; continue; }
                *w++ = *s++;
            }
        }
        /* 先跳过分隔符，再写 NUL：否则 w==s 时会把分隔符覆盖掉，s 再也走不动 */
        if (*s != '\0') s++;
        *w = '\0';
    }
    return argc;
}

/* 把“提示符 + 当前输入”画到底部输入框（SYS_INPUT_SET） */
static void show_input(const char *buf, int len)
{
    static char line[192];
    int n = 0;
    const char *p = "ushell> ";

    while (*p != '\0' && n < (int)sizeof(line) - 1) line[n++] = *p++;
    for (int i = 0; i < len && n < (int)sizeof(line) - 1; i++) {
        line[n++] = buf[i];
    }
    line[n] = '\0';

    sc(SYS_INPUT_SET, (int)line, n, 0);
}

/* 读一行：字符来自 SYS_GETCHAR（阻塞），显示走输入框，回车后写进输出区 */
static void readline(char *buf, int max)
{
    int len = 0;

    show_input(buf, 0);

    for (;;) {
        int c = sc(SYS_GETCHAR, 0, 0, 0);

        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            sc(SYS_INPUT_SET, (int)"", 0, 0);   /* 清空输入框 */
            putstr("ushell> ");                 /* 命令历史写进输出区 */
            putstr(buf);
            putstr("\n");
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                show_input(buf, len);
            }
            continue;
        }
        if (c < 0x20) continue;

        if (len + 1 < max) {
            buf[len++] = (char)c;
            show_input(buf, len);
        }
    }
}

/* ls [-l] [path]：-l 显示类型、大小、mtime */
static bool ls_long;

static void print_num(unsigned int v)
{
    char num[12];
    int i = 0;

    if (v == 0U) num[i++] = '0';
    while (v > 0U) { num[i++] = (char)('0' + (v % 10U)); v /= 10U; }
    while (i > 0) putch(num[--i]);
}

/* 把 mode 权限位格式化成 "rwxr-xr-x" */
static void mode_string(unsigned int mode, char out[10])
{
    static const char rwx[3] = { 'r', 'w', 'x' };

    for (int i = 0; i < 9; i++) {
        out[i] = ((mode >> (8 - i)) & 1U) ? rwx[i % 3] : '-';
    }
    out[9] = '\0';
}

static void ls_entry_long(const char *dir, const char *name)
{
    char full[192];
    unsigned int st[6];
    char tbuf[24];
    char perms[10];
    int n = 0;

    /* 拼出完整路径：dir 为空（驱动器列表）或末尾有 '/' 时直接拼 */
    if (dir != 0 && dir[0] != '\0') {
        while (dir[n] != '\0' && n < (int)sizeof(full) - 2) { full[n] = dir[n]; n++; }
        if (n > 0 && full[n - 1] != '/') full[n++] = '/';
    } else {
        full[n++] = '/';
    }
    for (int i = 0; name[i] != '\0' && n < (int)sizeof(full) - 1; i++) full[n++] = name[i];
    full[n] = '\0';

    if (sc(SYS_STAT, (int)full, (int)st, 0) != 0) {
        putstr("  ?  ");
        putstr(name);
        putstr("\n");
        return;
    }

    mode_string(st[3], perms);
    putstr((st[0] == 2U) ? "d" : "-");
    putstr(perms);
    putch(' ');
    print_num(st[4]);           /* uid */
    putch(':');
    print_num(st[5]);           /* gid */
    putstr("  ");

    if (st[0] == 2U) {
        putstr("     -  ");
    } else {
        char pad[10];
        int digits = 0;
        unsigned int v = st[1];
        do { digits++; v /= 10U; } while (v > 0U && digits < 9);
        for (int i = 0; i < 6 - digits; i++) pad[i] = ' ';
        pad[(6 - digits > 0) ? (6 - digits) : 0] = '\0';
        putstr(pad);
        print_num(st[1]);
        putstr("  ");
    }

    if (st[2] != 0U && sc(SYS_TIME, (int)tbuf, (int)sizeof(tbuf), 0) == 0) {
        putstr(tbuf);
    } else {
        putstr("                   ");
    }
    putstr("  ");
    putstr(name);
    putstr("\n");
}

static void cmd_ls(const char *path)
{
    char name[64];
    int i;

    for (i = 0; ; i++) {
        if (sc(SYS_LIST, (int)path, i, (int)name) != 0) break;
        if (ls_long) {
            ls_entry_long(path, name);
        } else {
            putstr("  ");
            putstr(name);
            putstr("\n");
        }
    }
}

/* 上一个目录（cd - 用） */
static char prev_cwd[192];

static void cmd_drives(void)
{
    char name[32];
    int i;

    for (i = 0; ; i++) {
        if (sc(SYS_DRIVES, i, (int)name, (int)sizeof(name)) != 0) break;
        putstr("  ");
        putstr(name);
        putstr("\n");
    }
}

static void cmd_cd(const char *path)
{
    char cur[192];

    if (path == 0) return;
    if (sc(SYS_GETCWD, (int)cur, (int)sizeof(cur), 0) < 0) cur[0] = '\0';

    if (scmp(path, "-") == 0) {
        if (prev_cwd[0] == '\0') {
            puterr("cd: no previous directory\n");
            return;
        }
        path = prev_cwd;
    }

    if (sc(SYS_CHDIR, (int)path, 0, 0) == 0) {
        strcpy(prev_cwd, cur);
        putstr("ok\n");
    } else {
        puterr2("cd: no such directory: ", path);
    }
}

static void cmd_mkdir(const char *path)
{
    putstr((sc(SYS_MKDIR, (int)path, 0, 0) == 0) ? "ok\n" : "mkdir: failed\n");
    sc(SYS_SYNC, 0, 0, 0);
}

static void cmd_rm(const char *path)
{
    putstr((sc(SYS_UNLINK, (int)path, 0, 0) == 0) ? "ok\n" : "rm: failed\n");
    sc(SYS_SYNC, 0, 0, 0);
}

static void cmd_pwd(void)
{
    char cwd[64];
    int n = sc(SYS_GETCWD, (int)cwd, (int)sizeof(cwd), 0);

    if (n < 0) {
        putstr("pwd: failed\n");
        return;
    }
    putstr("cwd: ");
    putstr(cwd);
    putstr("\n");
}

static void cmd_stat(const char *path)
{
    unsigned int st[6];
    char perms[10];
    int n = sc(SYS_STAT, (int)path, (int)st, 0);

    if (n < 0) {
        putstr("stat: failed\n");
        return;
    }
    putstr((st[0] == 2U) ? "dir  " : "file ");
    putstr(path);
    putstr("  ");
    /* 简单十进制输出 */
    if (st[0] != 2U) {
        char num[12];
        unsigned int v = st[1];
        int i = 0;
        if (v == 0U) num[i++] = '0';
        while (v > 0U) { num[i++] = (char)('0' + (v % 10U)); v /= 10U; }
        while (i > 0) putch(num[--i]);
        putstr(" bytes");
    }
    if (st[2] != 0U) {
        char tbuf[24];
        if (sc(SYS_TIME, (int)tbuf, (int)sizeof(tbuf), 0) == 0) {
            putstr("  mtime=");
            putstr(tbuf);
        }
    }
    mode_string(st[3], perms);
    putstr("  mode=");
    putstr(perms);
    putstr("  owner=");
    print_num(st[4]);
    putstr(":");
    print_num(st[5]);
    putstr("\n");
}

static void cmd_cat(const char *path)
{
    char buf[64];
    int h = -1;

    if (path != 0) {
        h = sc(SYS_OPEN, (int)path, FS_READ, 0);
        if (h < 0) {
            putstr("cat: cannot open\n");
            return;
        }
    }

    /* 没有参数就从 stdin 读（支持 cat < file 和管道） */
    for (;;) {
        int n = (path != 0) ? sc(SYS_FREAD, h, (int)buf, (int)sizeof(buf))
                            : sc(SYS_FREAD, 0, (int)buf, (int)sizeof(buf));
        if (n <= 0) break;
        sc(SYS_FWRITE, 1, (int)buf, n);
    }

    if (path != 0) sc(SYS_CLOSE, h, 0, 0);
    putstr("\n");
}

static void cmd_write(const char *path, const char *text)
{
    int h = sc(SYS_OPEN, (int)path, FS_WRITE | FS_CREAT, 0);
    int n;

    if (h < 0) {
        putstr("write: cannot open\n");
        return;
    }
    n = sc(SYS_FWRITE, h, (int)text, (int)slen(text));
    sc(SYS_CLOSE, h, 0, 0);
    sc(SYS_SYNC, 0, 0, 0);          /* 立即落盘，掉电也不丢 */

    putstr((n < 0) ? "write: failed\n" : "ok\n");
}

static void putnum(unsigned int v)
{
    char num[12];
    int i = 0;

    if (v == 0U) num[i++] = '0';
    while (v > 0U) {
        num[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) putch(num[--i]);
}

static void cmd_fork(void)
{
    int pid = sc(SYS_FORK, 0, 0, 0);

    if (pid == 0) {
        /* 子进程：从 fork 返回 0 */
        putstr("child: hello from fork child\n");
        sc(SYS_EXIT, 3, 0, 0);
    } else if (pid > 0) {
        int status = -1;
        int w = sc(SYS_WAIT, pid, (int)&status, 0);

        printf("parent: waited pid %d, status %d\n", w, status);
    } else {
        putstr("fork: failed\n");
    }
}

/* NX 回归用：放在只读 rodata 段里的一条 ret。 */
static const unsigned char rodata_ret[8] __attribute__((section(".rodata"), used)) =
    { 0xC3, 0, 0, 0, 0, 0, 0, 0 };

/* 安全回归：子进程分别尝试写代码页、写栈 guard、执行栈、执行 rodata。 */
static void cmd_sectest(void)
{
    int pid;
    int status;
    int w;

    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        volatile unsigned char *code = (volatile unsigned char *)(unsigned int)cmd_sectest;

        *code = 0x90;
        putstr("sectest: code write SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else if (pid > 0) {
        status = -1;
        w = sc(SYS_WAIT, pid, (int)&status, 0);
        printf("sectest: code write pid=%d status=%d\n", w, status);
    } else {
        putstr("sectest: code fork failed\n");
    }

    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        volatile unsigned char *guard = (volatile unsigned char *)0x40100000U;

        *guard = 0x41;
        putstr("sectest: guard write SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else if (pid > 0) {
        status = -1;
        w = sc(SYS_WAIT, pid, (int)&status, 0);
        printf("sectest: guard write pid=%d status=%d\n", w, status);
    } else {
        putstr("sectest: guard fork failed\n");
    }

    /* NX 回归：在栈上放一条 ret，然后 call 到栈地址。
     * NX 生效时取指会 #PF；没有 NX 时会正常 ret 并打印 SURVIVED。 */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        volatile unsigned char code[8];

        code[0] = 0xC3;                 /* ret */
        __asm__ volatile ("call *%0" : : "r"((unsigned int)code) : "memory");
        putstr("sectest: stack exec SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else if (pid > 0) {
        status = -1;
        w = sc(SYS_WAIT, pid, (int)&status, 0);
        printf("sectest: stack exec pid=%d status=%d\n", w, status);
    } else {
        putstr("sectest: stack exec fork failed\n");
    }

    /* rodata 是 R-only/NX 段：call 到 rodata_ret 应当取指 #PF。 */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        __asm__ volatile ("call *%0" : : "r"((unsigned int)rodata_ret) : "memory");
        putstr("sectest: rodata exec SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else if (pid > 0) {
        status = -1;
        w = sc(SYS_WAIT, pid, (int)&status, 0);
        printf("sectest: rodata exec pid=%d status=%d\n", w, status);
    } else {
        putstr("sectest: rodata exec fork failed\n");
    }

    /* capability rights：以只读打开，再尝试写，必须被拒绝。 */
    {
        int h = sc(SYS_OPEN, (int)"system:/docs/hello.txt", FS_READ, 0);
        if (h >= 0) {
            int r = sc(SYS_FWRITE, h, (int)"x", 1);
            printf("sectest: fd write %s (%d)\n", (r < 0) ? "denied" : "ALLOWED", r);
            sc(SYS_CLOSE, h, 0, 0);
        } else {
            putstr("sectest: fd open failed\n");
        }
    }

    /* capability revoke：先拿读写，再显式 drop WRITE，写必须立即失败。 */
    {
        int h = sc(SYS_OPEN, (int)"system:/docs/hello.txt", FS_READ | FS_WRITE, 0);
        if (h >= 0) {
            sc(SYS_CAP_DROP, h, 2, 0);      /* FD_RIGHT_WRITE */
            int r = sc(SYS_FWRITE, h, (int)"x", 1);
            printf("sectest: cap drop %s (%d)\n", (r < 0) ? "denied" : "ALLOWED", r);
            sc(SYS_CLOSE, h, 0, 0);
        } else {
            putstr("sectest: cap open failed\n");
        }
    }

    /* capability transfer：父进程把只读文件 fd 发给子进程，pipe 只做同步。 */
    {
        int pp[2];
        if (sc(SYS_PIPE, (int)pp, 0, 0) == 0) {
            int pid2 = sc(SYS_FORK, 0, 0, 0);
            if (pid2 == 0) {
                char c = 0;
                char b[32];
                int n;
                sc(SYS_FREAD, pp[0], (int)&c, 1);
                n = sc(SYS_FREAD, 5, (int)b, 31);   /* target fd 预期为 5 */
                if (n > 0) {
                    b[n] = '\0';
                    printf("sectest: cap recv %s\n", b);
                } else {
                    putstr("sectest: cap recv FAIL\n");
                }
                sc(SYS_CLOSE, pp[0], 0, 0);
                sc(SYS_EXIT, 0, 0, 0);
            } else if (pid2 > 0) {
                int h = sc(SYS_OPEN, (int)"system:/docs/hello.txt", FS_READ, 0);
                int tfd = sc(SYS_CAP_SEND, pid2, h, FS_READ);
                char x = 'x';
                sc(SYS_FWRITE, pp[1], (int)&x, 1);
                sc(SYS_CLOSE, h, 0, 0);
                sc(SYS_CLOSE, pp[0], 0, 0);
                sc(SYS_CLOSE, pp[1], 0, 0);
                status = -1;
                w = sc(SYS_WAIT, pid2, (int)&status, 0);
                printf("sectest: cap xfer pid=%d fd=%d status=%d\n", w, tfd, status);
            } else {
                putstr("sectest: cap xfer fork failed\n");
            }
        } else {
            putstr("sectest: cap xfer pipe failed\n");
        }
    }

    /* 匿名 mmap：映射一页、写读、munmap。 */
    {
        int m = sc(SYS_MMAP, 4096, 3, 0);
        if (m > 0) {
            *(volatile unsigned int *)(unsigned int)m = 0x5A5A1234U;
            if (*(volatile unsigned int *)(unsigned int)m == 0x5A5A1234U) {
                putstr("sectest: mmap ok\n");
            } else {
                putstr("sectest: mmap read FAIL\n");
            }
            sc(SYS_MUNMAP, m, 4096, 0);
        } else {
            putstr("sectest: mmap FAIL\n");
        }
    }

    /* 用户态 UDP：发到宿主回显端口，再收回来。 */
    {
        struct udp_req req;

        req.ip[0] = 10; req.ip[1] = 0; req.ip[2] = 2; req.ip[3] = 2;
        req.port = 12345;
        req.len = 4;
        memcpy(req.data, "ping", 4);

        if (sc(SYS_UDP_SEND, (int)(unsigned int)&req, 0, 0) == 0) {
            req.len = sizeof(req.data);
            int n = sc(SYS_UDP_RECV, (int)(unsigned int)&req, 0, 0);
            if (n == 4 && req.data[0] == 'p' && req.data[1] == 'i' &&
                req.data[2] == 'n' && req.data[3] == 'g') {
                putstr("sectest: udp ok\n");
            } else {
                putstr("sectest: udp recv FAIL\n");
            }
        } else {
            putstr("sectest: udp send FAIL\n");
        }
    }

    /* socket fd 路径：socket -> sendto -> recvfrom -> close */
    {
        int s = sc(SYS_SOCKET, 2, 2, 0);    /* AF_INET, SOCK_DGRAM */
        if (s >= 0) {
            struct udp_req req;

            req.ip[0] = 10; req.ip[1] = 0; req.ip[2] = 2; req.ip[3] = 2;
            req.port = 12345;
            req.len = 4;
            memcpy(req.data, "sock", 4);
            if (sc(SYS_SENDTO, s, (int)(unsigned int)&req, 0) == 0) {
                req.len = sizeof(req.data);
                int n = sc(SYS_RECVFROM, s, (int)(unsigned int)&req, 0);
                if (n == 4 && req.data[0] == 's' && req.data[1] == 'o' &&
                    req.data[2] == 'c' && req.data[3] == 'k') {
                    putstr("sectest: socket udp ok\n");
                } else {
                    putstr("sectest: socket recv FAIL\n");
                }
            } else {
                putstr("sectest: socket send FAIL\n");
            }
            sc(SYS_CLOSE, s, 0, 0);
        } else {
            putstr("sectest: socket FAIL\n");
        }
    }

    /* VFS namespace：子进程 chroot 后 ../ 不能逃逸；父进程不受影响。 */
    {
        int pid = sc(SYS_FORK, 0, 0, 0);
        if (pid == 0) {
            if (sc(SYS_CHROOT, (int)(unsigned int)"system:/bin", 0, 0) == 0) {
                int f = sc(SYS_OPEN, (int)(unsigned int)"../host.txt", 0, 0);
                if (f < 0) putstr("sectest: chroot ok\n");
                else { sc(SYS_CLOSE, f, 0, 0); putstr("sectest: chroot ESCAPE\n"); }
            } else {
                putstr("sectest: chroot FAIL\n");
            }
            sc(SYS_EXIT, 0, 0, 0);
        } else if (pid > 0) {
            int status = 0;
            sc(SYS_WAIT, pid, (int)(unsigned int)&status, 0);
        }
    }
}

/* ---------------- 历史记录 ---------------- */

#define HIST_MAX 8
static char hist[HIST_MAX][128];
static int  hist_count;

static void hist_add(const char *line)
{
    int i;

    if (line == 0 || line[0] == '\0') return;
    if (hist_count > 0 && scmp(hist[hist_count - 1], line) == 0) return;

    if (hist_count < HIST_MAX) {
        strcpy(hist[hist_count], line);
        hist_count++;
        return;
    }
    for (i = 1; i < HIST_MAX; i++) strcpy(hist[i - 1], hist[i]);
    strcpy(hist[HIST_MAX - 1], line);
}

static void cmd_history(void)
{
    for (int i = 0; i < hist_count; i++) {
        putstr("  ");
        print_num((unsigned int)(i + 1));
        putstr("  ");
        putstr(hist[i]);
        putstr("\n");
    }
}

/* ---------------- 命令解析与执行 ---------------- */

static int find_char(const char *s, char c)
{
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] == c) return i;
    }
    return -1;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ') s++;
    e = s + slen(s);
    while (e > s && e[-1] == ' ') e--;
    *e = '\0';
    return s;
}

static int str2int(const char *s)
{
    int v = 0;

    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* SIGTERM 处理函数：打印一行后返回（内核用蹦床做 sigreturn） */
static void sig_handler(int sig)
{
    printf("ushell: caught signal %d, continuing\n", sig);
}

/* 单命令帮助：help <cmd> */
static void cmd_help_one(const char *name)
{
    static const struct {
        const char *name;
        const char *usage;
    } tbl[] = {
        { "help",    "help [cmd]            show help" },
        { "drives",  "drives                list drives" },
        { "ls",      "ls [-l] [path]        list drives/dir; -l long format" },
        { "cat",     "cat [path]            print file (no path = stdin)" },
        { "write",   "write <path> <text>   create/overwrite (quotes allowed)" },
        { "mkdir",   "mkdir <path>          create a directory" },
        { "rm",      "rm <path>             remove a file/dir" },
        { "cd",      "cd <path> | cd -      change directory" },
        { "pwd",     "pwd                   show current directory" },
        { "stat",    "stat <path>           type/size/mtime" },
        { "echo",    "echo <args...>        print arguments" },
        { "sleep",   "sleep <ms>            sleep milliseconds" },
        { "fork",    "fork                  fork and wait for the child" },
        { "sectest", "sectest               code/guard write protection test" },
        { "exec",    "exec <path> [args]    replace this shell" },
        { "reexec",  "reexec                exec /bin/shell (self-test)" },
        { "history", "history               show recent commands" },
        { "reboot",  "reboot                reboot the machine" },
        { "crash",   "crash                 trigger a kernel panic (debug)" },
        { "trap",    "trap                  install a SIGTERM handler" },
        { "kill",    "kill <pid> <sig>      send a signal" },
        { "sync",    "sync                  flush filesystem to disk" },
        { "fsck",    "fsck                  check filesystem consistency" },
        { "date",    "date                  show RTC time" },
        { "exit",    "exit                  exit the shell" }
    };

    for (unsigned int i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (scmp(tbl[i].name, name) == 0) {
            putstr(tbl[i].usage);
            putstr("\n");
            return;
        }
    }
    puterr2("help: no such command: ", name);
}

static void run_line(char *cmd)
{
    char *args[4];
    int nargs = split(cmd, args, 8);

    if (nargs == 0) return;

    if (scmp(args[0], "help") == 0) {
        if (nargs >= 2) {
            cmd_help_one(args[1]);
            return;
        }
        putstr("ushell: commands\n"
               "  help [cmd]             this help / help for one command\n"
               "  drives                 list drives (system, data, fat, ...)\n"
               "  ls [-l] [path]         no path = list drives; -l = long format\n"
               "  cat [path]             show a file, e.g. system:/docs/a.txt\n"
               "  write <path> <text>    create/overwrite a file (quotes allowed)\n"
               "  mkdir <path>           create a directory\n"
               "  rm <path>              remove a file/dir\n"
               "  cd <path>              change directory; cd - returns\n"
               "  pwd                    show current directory\n"
               "  stat <path>            show type/size/mtime\n"
               "  echo <args...>         print arguments\n"
               "  sleep <ms>             sleep milliseconds\n"
               "  fork                   create a child process, wait for it\n"
               "  sectest                code/guard write protection test\n"
               "  exec <path> [args]     replace this shell with a program\n"
               "  reexec                 exec /bin/shell (self-test)\n"
               "  <cmd> > <file>         redirect stdout\n"
               "  <cmd> < <file>         redirect stdin\n"
               "  <cmd1> | <cmd2>        pipe\n"
               "  history                show recent commands\n"
               "  !!  / !n               repeat last / n-th command\n"
               "  reboot                 reboot the machine\n"
               "  crash                  trigger a kernel panic (debug)\n"
               "  trap                   install a SIGTERM handler\n"
               "  kill <pid> <sig>       send a signal\n"
               "  sync                   flush filesystem to disk\n"
               "  fsck                   check filesystem consistency\n"
               "  date                   show RTC time\n"
               "  pci                    list PCI devices\n"
               "  mouse                  show PS/2 mouse state\n"
               "  eth                    e1000 MAC/link + TX test frame\n"
               "  res                    kernel resource usage\n"
               "  id                     show uid/gid\n"
               "  chmod <octal> <path>   change permissions\n"
               "  chown <uid> <gid> <p>  change owner (root only)\n"
               "  drop <uid>             drop to uid (root only, one-way)\n"
               "  hang                   spin with IRQs off (NMI watchdog test)\n"
               "  shutdown               ACPI poweroff\n"
               "  exit\n");
    } else if (scmp(args[0], "drives") == 0) {
        cmd_drives();
    } else if (scmp(args[0], "ls") == 0) {
        int argi = 1;

        ls_long = false;
        if (nargs > 1 && scmp(args[1], "-l") == 0) { ls_long = true; argi = 2; }
        cmd_ls((nargs > argi) ? args[argi] : 0);
    } else if (scmp(args[0], "history") == 0) {
        cmd_history();
    } else if (scmp(args[0], "cat") == 0) {
        cmd_cat((nargs >= 2) ? args[1] : 0);
    } else if (scmp(args[0], "write") == 0 && nargs >= 3) {
        cmd_write(args[1], args[2]);
    } else if (scmp(args[0], "mkdir") == 0 && nargs >= 2) {
        cmd_mkdir(args[1]);
    } else if (scmp(args[0], "rm") == 0 && nargs >= 2) {
        cmd_rm(args[1]);
    } else if (scmp(args[0], "cd") == 0 && nargs >= 2) {
        cmd_cd(args[1]);
    } else if (scmp(args[0], "pwd") == 0) {
        cmd_pwd();
    } else if (scmp(args[0], "stat") == 0 && nargs >= 2) {
        cmd_stat(args[1]);
    } else if (scmp(args[0], "echo") == 0) {
        for (int i = 1; i < nargs; i++) {
            if (i > 1) putch(' ');
            putstr(args[i]);
        }
        putch('\n');
    } else if (scmp(args[0], "sleep") == 0 && nargs >= 2) {
        sc(SYS_SLEEP, str2int(args[1]), 0, 0);
    } else if (scmp(args[0], "fork") == 0) {
        cmd_fork();
    } else if (scmp(args[0], "sectest") == 0) {
        cmd_sectest();
    } else if (scmp(args[0], "exec") == 0 && nargs >= 2) {
        char argbuf[64];
        int n = 0;

        for (int i = 2; i < nargs && n < 60; i++) {
            if (n > 0) argbuf[n++] = ' ';
            for (const char *p = args[i]; *p != '\0' && n < 62; p++) {
                argbuf[n++] = *p;
            }
        }
        argbuf[n] = '\0';
        sc(SYS_EXEC, (int)args[1], (int)argbuf, 0);
        putstr("exec: failed\n");
    } else if (scmp(args[0], "reexec") == 0) {
        /* 回归专用：不依赖键盘输入路径字符，直接 exec 自身。 */
        sc(SYS_EXEC, (int)"system:/bin/shell", 0, 0);
        putstr("reexec: failed\n");
    } else if (scmp(args[0], "reboot") == 0) {
        sc(SYS_REBOOT, 0, 0, 0);
        putstr("reboot: failed\n");
    } else if (scmp(args[0], "crash") == 0) {
        sc(SYS_PANIC, 0, 0, 0);
        putstr("crash: failed\n");
    } else if (scmp(args[0], "oops") == 0) {
        sc(SYS_OOPS, 0, 0, 0);
        putstr("oops: survived\n");
    } else if (scmp(args[0], "fsck") == 0) {
        char rep[96];
        int errs = sc(SYS_FSCK, (int)rep, (int)sizeof(rep), 0);
        putstr("fsck: ");
        putstr(rep);
        putstr(errs == 0 ? "  OK\n" : "  FAIL\n");
    } else if (scmp(args[0], "pci") == 0) {
        char b[64];
        int i;

        for (i = 0; ; i++) {
            if (sc(SYS_PCI, i, (int)b, 0) != 0) break;
            putstr("  ");
            putstr(b);
            putstr("\n");
        }
    } else if (scmp(args[0], "id") == 0) {
        printf("uid=%d gid=%d\n", sc(SYS_GETUID, 0, 0, 0), sc(SYS_GETGID, 0, 0, 0));
    } else if (scmp(args[0], "drop") == 0 && nargs >= 2) {
        int uid = str2int(args[1]);
        if (sc(SYS_SETUID, uid, uid == 0 ? 0 : uid, 0) == 0) {
            printf("drop: now uid=%d gid=%d (reboot to regain root)\n", uid, uid);
        } else {
            puterr("drop: only root can change uid\n");
        }
    } else if (scmp(args[0], "chmod") == 0 && nargs >= 3) {
        int mode = 0, ok = 1;
        for (const char *p = args[1]; *p != '\0'; p++) {
            if (*p < '0' || *p > '7') { ok = 0; break; }
            mode = mode * 8 + (*p - '0');
        }
        if (!ok) {
            puterr("chmod: mode must be octal, e.g. 644\n");
        } else if (sc(SYS_CHMOD, (int)args[2], mode, 0) == 0) {
            putstr("ok\n");
            sc(SYS_SYNC, 0, 0, 0);
        } else {
            puterr("chmod: permission denied or no such file\n");
        }
    } else if (scmp(args[0], "chown") == 0 && nargs >= 4) {
        int uid = str2int(args[1]);
        int gid = str2int(args[2]);
        if (sc(SYS_CHOWN, (int)args[3], uid, gid) == 0) {
            putstr("ok\n");
            sc(SYS_SYNC, 0, 0, 0);
        } else {
            puterr("chown: only root can change owner\n");
        }
    } else if (scmp(args[0], "res") == 0) {
        unsigned int r[10];
        if (sc(SYS_RESINFO, (int)r, 0, 0) == 0) {
            printf("res: mem  free=%u/%u pages (%u/%u KB)\n",
                   r[0], r[1], r[0] * 4U, r[1] * 4U);
            printf("res: heap used=%u/%u KB\n", r[2] / 1024U, r[3] / 1024U);
            printf("res: procs=%u syscalls=%u cow=%u\n", r[4], r[5], r[6]);
            printf("res: fs nodes=%u devices=%u pci=%u\n", r[7], r[8], r[9]);
        } else {
            puterr("res: failed\n");
        }
    } else if (scmp(args[0], "eth") == 0) {
        unsigned char info[8];
        if (sc(SYS_ETH, 0, (int)info, 0) == 0 && info[6] != 0) {
            printf("eth: mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
                   info[0], info[1], info[2], info[3], info[4], info[5],
                   info[7] ? "up" : "down");
            if (sc(SYS_ETH, 1, 0, 0) == 0) {
                putstr("eth: test frame sent\n");
            } else {
                putstr("eth: send failed\n");
            }
        } else {
            puterr("eth: no e1000 device\n");
        }
    } else if (scmp(args[0], "hang") == 0) {
        putstr("hang: spinning with interrupts off (debug)\n");
        sc(SYS_HANG, 0, 0, 0);
        putstr("hang: survived\n");
    } else if (scmp(args[0], "mouse") == 0) {
        int m[3];
        if (sc(SYS_MOUSE, (int)m, 0, 0) == 0) {
            printf("mouse: x=%d y=%d buttons=%d\n", m[0], m[1], m[2]);
        } else {
            puterr("mouse: not available\n");
        }
    } else if (scmp(args[0], "shutdown") == 0) {
        putstr("shutdown: ACPI S5\n");
        sc(SYS_POWEROFF, 0, 0, 0);
        putstr("shutdown: failed\n");
    } else if (scmp(args[0], "date") == 0) {
        char tbuf[24];
        if (sc(SYS_TIME, (int)tbuf, (int)sizeof(tbuf), 0) == 0) {
            putstr(tbuf);
            putstr("\n");
        } else {
            putstr("date: failed\n");
        }
    } else if (scmp(args[0], "sync") == 0) {
        putstr((sc(SYS_SYNC, 0, 0, 0) == 0) ? "sync: ok\n" : "sync: no disk\n");
    } else if (scmp(args[0], "trap") == 0) {
        sc(SYS_SIGACTION, 15, (int)sig_handler, 0);
        putstr("trap: SIGTERM handler installed\n");
    } else if (scmp(args[0], "kill") == 0 && nargs >= 3) {
        int r = sc(SYS_KILL, str2int(args[1]), str2int(args[2]), 0);
        printf("kill: ret=%d\n", r);
    } else if (scmp(args[0], "exit") == 0) {
        sc(SYS_EXIT, 0, 0, 0);
    } else {
        putstr("ushell: unknown command\n");
    }
}

/* 重定向：out=1 覆盖 stdout，out=0 覆盖 stdin */
static void run_redirect(char *cmd, char *path, int out)
{
    int fd = out ? sc(SYS_OPEN, (int)path, FS_WRITE | FS_CREAT, 0)
                 : sc(SYS_OPEN, (int)path, FS_READ, 0);
    int pid;

    if (fd < 0) {
        putstr("redirect: cannot open\n");
        return;
    }

    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_DUP2, fd, out ? 1 : 0, 0);
        sc(SYS_CLOSE, fd, 0, 0);
        run_line(cmd);
        sc(SYS_EXIT, 0, 0, 0);
    }
    sc(SYS_CLOSE, fd, 0, 0);
    if (pid > 0) {
        int status;
        sc(SYS_WAIT, pid, (int)&status, 0);
    }
}

/* 管道：左边 stdout -> 管道，右边 stdin <- 管道 */
static void run_pipeline(char *left, char *right)
{
    int fds[2];
    int p1, p2, status;

    if (sc(SYS_PIPE, (int)fds, 0, 0) != 0) {
        putstr("pipe: failed\n");
        return;
    }

    p1 = sc(SYS_FORK, 0, 0, 0);
    if (p1 == 0) {
        sc(SYS_DUP2, fds[1], 1, 0);
        sc(SYS_CLOSE, fds[0], 0, 0);
        sc(SYS_CLOSE, fds[1], 0, 0);
        run_line(left);
        sc(SYS_EXIT, 0, 0, 0);
    }

    p2 = sc(SYS_FORK, 0, 0, 0);
    if (p2 == 0) {
        sc(SYS_DUP2, fds[0], 0, 0);
        sc(SYS_CLOSE, fds[0], 0, 0);
        sc(SYS_CLOSE, fds[1], 0, 0);
        run_line(right);
        sc(SYS_EXIT, 0, 0, 0);
    }

    sc(SYS_CLOSE, fds[0], 0, 0);
    sc(SYS_CLOSE, fds[1], 0, 0);
    if (p1 > 0) sc(SYS_WAIT, p1, (int)&status, 0);
    if (p2 > 0) sc(SYS_WAIT, p2, (int)&status, 0);
}

static void parse_and_run(char *line)
{
    int i = find_char(line, '|');

    if (i >= 0) {
        line[i] = '\0';
        run_pipeline(trim(line), trim(line + i + 1));
        return;
    }
    i = find_char(line, '>');
    if (i >= 0) {
        line[i] = '\0';
        run_redirect(trim(line), trim(line + i + 1), 1);
        return;
    }
    i = find_char(line, '<');
    if (i >= 0) {
        line[i] = '\0';
        run_redirect(trim(line), trim(line + i + 1), 0);
        return;
    }
    run_line(line);
}

/* 入口必须落在映像第一个字节（内核固定从 0x40000000 进入）。
 * argc/argv 由内核按 ELF 启动约定放在用户栈上。 */
__attribute__((section(".text.start")))
void _start(int argc, char **argv)
{
    char line[128];

    putstr("ushell: ready (ring3, legacy int 0x81)\n");

    /* 内核按 ELF 启动约定压好的 argc/argv（启动时没有参数） */
    putstr("ushell: argc=");
    putnum((unsigned int)argc);
    if (argc > 0 && argv != 0) {
        putstr(" argv0=");
        putstr(argv[0]);
    }
    putstr("\n");

    /* 启动自检：getcwd + mkdir/stat/unlink，证明文件系统系统调用都通 */
    {
        char cwd[64];
        unsigned int st[2];
        int ok;

        if (sc(SYS_GETCWD, (int)cwd, (int)sizeof(cwd), 0) >= 0) {
            putstr("ushell: cwd=");
            putstr(cwd);
            putstr("\n");
        }

        ok = (sc(SYS_MKDIR, (int)"system:/uselfs", 0, 0) == 0);
        ok = ok && (sc(SYS_STAT, (int)"system:/uselfs", (int)st, 0) == 0);
        ok = ok && (sc(SYS_UNLINK, (int)"system:/uselfs", 0, 0) == 0);
        putstr(ok ? "ushell: fs syscalls ok\n" : "ushell: fs syscalls FAIL\n");
    }

    /* libc 自检：malloc 走 SYS_BRK，printf 走 SYS_WRITE */
    {
        char *p = (char *)malloc(32);
        if (p != 0) {
            strcpy(p, "libc");
            printf("ushell: libc ok (argc=%d, ptr=%p, str=%s)\n", argc, p, p);
        } else {
            putstr("ushell: libc FAIL\n");
        }
    }

    /* 管道 + sleep 自检：一写一读，再睡 20ms */
    {
        int fds[2];
        int ok = 0;

        if (sc(SYS_PIPE, (int)fds, 0, 0) == 0) {
            char c = 0;
            sc(SYS_FWRITE, fds[1], (int)"p", 1);
            if (sc(SYS_FREAD, fds[0], (int)&c, 1) == 1 && c == 'p') ok = 1;
            sc(SYS_CLOSE, fds[0], 0, 0);
            sc(SYS_CLOSE, fds[1], 0, 0);
        }
        sc(SYS_SLEEP, 20, 0, 0);
        putstr(ok ? "ushell: pipe ok sleep ok\n" : "ushell: pipe FAIL\n");
    }

    /* 信号自检：给自己发 SIGTERM，下一个系统调用返回时投递到 handler */
    {
        int me = sc(SYS_GETPID, 0, 0, 0);
        sc(SYS_SIGACTION, 15, (int)sig_handler, 0);
        sc(SYS_KILL, me, 15, 0);
        putstr("ushell: signal selftest done\n");
    }

    for (;;) {
        readline(line, (int)sizeof(line));

        if (line[0] == '\0') continue;

        /* 历史展开：!! 重放上一条，!n 重放第 n 条 */
        if (line[0] == '!') {
            int idx = -1;

            if (line[1] == '!' && line[2] == '\0') {
                idx = hist_count - 1;
            } else if (line[1] >= '1' && line[1] <= '9') {
                int v = 0, ok = 1;
                for (int i = 1; line[i] != '\0'; i++) {
                    if (line[i] < '0' || line[i] > '9') { ok = 0; break; }
                    v = v * 10 + (line[i] - '0');
                }
                if (ok && v >= 1 && v <= hist_count) idx = v - 1;
            }

            if (idx < 0 || idx >= hist_count) {
                puterr("history: no such command\n");
                continue;
            }

            {
                char tmp[128];
                strcpy(tmp, hist[idx]);
                putstr("-> ");
                putstr(tmp);
                putstr("\n");
                parse_and_run(tmp);
            }
            continue;
        }

        hist_add(line);
        parse_and_run(line);
    }
}
