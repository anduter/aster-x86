/* =============================================================================
 * ng WP2-6: Aster 64 位用户态 shell（L3 应用，独立 ELF64，ring3，无 libc）
 *
 * 定位：E2 的"最上层"产物。它不链接任何内核代码，只通过 syscall 访问系统：
 *   0  write(buf,len)                 控制台输出（内核把它写到串口 + 帧缓冲镜像）
 *   1  exit(code)                     退出（回到内核恢复点）
 *   20 open / 21 read / 22 write / 23 close / 24 stat /
 *   25 listdir / 26 mkdir / 27 unlink 文件操作（VFS/ramfs/FAT16 都由内核决定）
 *   30 readkey()                      非阻塞读键（-EAGAIN 表示没键）
 *   31 login(name,pass)              认证
 *   32 dmesg(buf,max) / 33 log_write(tag,msg)   日志
 *   34 pkg_list(buf,max) / 35 pkg_verify(name)  包
 * 所有用户指针（路径/缓冲）都由内核 uaccess 校验；本文件不做任何内核访问。
 *
 * 交互：
 *   - 提示符 "ng> "；可打印字符回显、Backspace 删字符、Enter 执行；
 *   - Ctrl-L 清屏、Ctrl-C 放弃当前行、Ctrl-U 删整行、Ctrl-D 退出；
 *   - 行缓冲 128 字节，最多 8 个参数（不做引号/管道/作业控制，见 WP2-6 边界）。
 *
 * 命令：help / ls [path] / cat <file> / echo <text> [> path] / mkdir <dir> /
 *       rm <path> / cd [dir] / pwd / stat <path> / dmesg [-n N] /
 *       log <tag> <msg> / pkg list|verify <name> / login <user> <pass> /
 *       clear / exit [code]。
 *
 * 错误：未知命令/参数缺失/文件不存在等打印一行友好错误（含 errno 文本），
 *       并把 shell 状态置 1；`exit` 不带参数时用这个状态退出（非零），
 *       `exit N` 用 N。任何错误都不 panic、不重启、不泄漏 fd。
 *
 * 证据行（run-ng 的自动化约定，dev shell 直接打在控制台上）：
 *   ng: shell ready               启动完成，可接受命令
 *   ng: shell ls/cat/write/dmesg/pkg/exit ok
 *                                 对应命令成功执行过一次（write = echo 重定向）
 * 内核在用户程序退出后补 `ng: WP2-6 selftest PASS/FAIL`。
 *
 * 实现约束：
 *   - 没有 libc：字符串/数字格式化都是本文件的 20 行小工具；
 *   - 不依赖 VFS 的 cwd：`cd`/`pwd` 是 shell 自己的字符串状态，每次文件操作
 *     都先解析成绝对路径再交给 syscall（内核 VFS 的 cwd 保持 "/"）；
 *   - 所有写缓冲 <= 256 字节（syscall 0 的上限），fd 读写按 256 分块；
 *   - 编译：clang --target=x86_64-elf -ffreestanding -nostdlib，链接到
 *     0x40000000（src/ng/user/user.ld）；host 单测用 -DNG_HOST_TEST 把
 *     sh_sys() 换成 sh_host_syscall()（tests/unit-ng/test_shell.c）。
 * ========================================================================== */

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

/* ---- syscall 号（ABI 1.0 草案，见 src/ng/syscall.h）---------------------- */
#define SH_SYS_WRITE 0
#define SH_SYS_EXIT 1
#define SH_SYS_OPEN 20
#define SH_SYS_READ 21
#define SH_SYS_WRITE_FD 22
#define SH_SYS_CLOSE 23
#define SH_SYS_STAT 24
#define SH_SYS_LISTDIR 25
#define SH_SYS_MKDIR 26
#define SH_SYS_UNLINK 27
#define SH_SYS_READKEY 30
#define SH_SYS_LOGIN 31
#define SH_SYS_DMESG 32
#define SH_SYS_LOG_WRITE 33
#define SH_SYS_PKG_LIST 34
#define SH_SYS_PKG_VERIFY 35
#define SH_SYS_SPAWN 40
#define SH_SYS_WAIT 41
#define SH_SYS_GETPID 42
#define SH_SYS_KILL 43
#define SH_SYS_GETPPID 44

/* ---- errno（与 src/services/fs/include/vfs.h 同值）---------------------- */
#define SH_EPERM (-1)
#define SH_ENOENT (-2)
#define SH_EIO (-5)
#define SH_EBADF (-9)
#define SH_EAGAIN (-11)
#define SH_ENOMEM (-12)
#define SH_EACCES (-13)
#define SH_EFAULT (-14)
#define SH_EEXIST (-17)
#define SH_ENOTDIR (-20)
#define SH_EISDIR (-21)
#define SH_EINVAL (-22)
#define SH_EMFILE (-24)
#define SH_ENOSPC (-28)
#define SH_EROFS (-30)
#define SH_ENAMETOOLONG (-36)
#define SH_ENOTEMPTY (-39)
#define SH_ELOOP (-40)

/* ---- open flags（VFS 低 8 位语义）--------------------------------------- */
#define SH_O_RDONLY 0x000u
#define SH_O_WRONLY 0x001u
#define SH_O_CREAT 0x040u
#define SH_O_TRUNC 0x200u

/* ---- readkey 编码：(flags<<24)|(ascii<<16)|scancode -------------------- */
#define SH_KEY_FLAGS(k) (((u32)(k) >> 24) & 0xFFu)
#define SH_KEY_ASCII(k) (((u32)(k) >> 16) & 0xFFu)
#define SH_F_CTRL 0x04u

/* ---- 节点/文件类型（vfs.h 的 VFS_NODE_* / VFS_S_IF*）------------------- */
#define SH_NODE_FILE 1u
#define SH_NODE_DIR 2u
#define SH_S_IFDIR 0x4000u

/* ---- 缓冲上限 ----------------------------------------------------------- */
#define SH_LINE_MAX 128u
#define SH_ARG_MAX 8
#define SH_PATH_MAX 256u
#define SH_IO_MAX 256u /* syscall 0 的单次写上限，fd 读写也按它分块 */
#define SH_DMESG_MAX 1024u
#define SH_PKG_MAX 512u
#define SH_TEXT_MAX 160u
#define SH_DIRENT_MAX 8u /* 内核 fs_sys_listdir 的 FS_SYS_LISTDIR_MAX */
#define SH_CAT_MAX 65536u

/* ---- 与 VFS 共享的用户结构（布局必须逐字节一致，见 vfs.h）-------------- */
struct sh_stat {
    u32 type;
    u32 mode;
    u64 size;
    u64 ino;
};

struct sh_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[64];
};

/* ========================================================================== */
/* syscall 薄封装                                                            */
/* ========================================================================== */

/*
 * host 单测（-DNG_HOST_TEST）把这一层换成 tests/unit-ng/test_shell.c 里的
 * sh_host_syscall()：单测因此能跑真实命令解析/执行路径，而不用 QEMU。
 */
static i64 sh_sys(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
#ifdef NG_HOST_TEST
    extern i64 sh_host_syscall(i64, i64, i64, i64, i64);
    return sh_host_syscall(nr, a1, a2, a3, a4);
#else
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
#endif
}

static i64 sh_write(const char *buf, u32 len)
{
    return sh_sys(SH_SYS_WRITE, (i64)buf, (i64)len, 0, 0);
}

/* ========================================================================== */
/* 输出小工具（全部走 syscall 0，按 256 字节分块）                          */
/* ========================================================================== */

static void sh_putn(const char *s, u32 n)
{
    u32 off = 0;

    while (off < n) {
        u32 chunk = n - off;
        i64 rc;

        if (chunk > SH_IO_MAX) {
            chunk = SH_IO_MAX;
        }
        rc = sh_write(s + off, chunk);
        if (rc != (i64)chunk) {
            return; /* 控制台写失败：放弃这一行，避免死循环 */
        }
        off += chunk;
    }
}

static void sh_puts(const char *s)
{
    u32 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    sh_putn(s, n);
}

static void sh_putc(char c)
{
    (void)sh_write(&c, 1);
}

/* 把文件/log 内容里的 '\n' 转成 "\r\n" 再输出（串口终端需要 CR）。 */
static void sh_put_content(const char *s, u32 n)
{
    char out[SH_IO_MAX];
    u32 o = 0;
    u32 i;

    for (i = 0; i < n; i++) {
        char c = s[i];

        if (c == '\n') {
            if (o + 2u > SH_IO_MAX) {
                sh_putn(out, o);
                o = 0;
            }
            out[o++] = '\r';
            out[o++] = '\n';
        } else {
            if (o + 1u > SH_IO_MAX) {
                sh_putn(out, o);
                o = 0;
            }
            out[o++] = c;
        }
    }
    if (o > 0) {
        sh_putn(out, o);
    }
}

static void sh_putdec(u64 v)
{
    char buf[24];
    u32 i = 0;

    if (v == 0) {
        sh_putc('0');
        return;
    }
    while (v != 0 && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        sh_putc(buf[--i]);
    }
}

static void sh_putoct(u32 v)
{
    char buf[12];
    u32 i = 0;

    do {
        buf[i++] = (char)('0' + (char)(v & 7u));
        v >>= 3;
    } while (v != 0 && i < sizeof(buf));
    while (i > 0) {
        sh_putc(buf[--i]);
    }
}

/* 单位数：1 = OK，0 = 非数字/为空/过大。 */
static int sh_parse_u32(const char *s, u32 *out)
{
    u32 v = 0;
    u32 digits = 0;

    if (s == 0 || *s == '\0') {
        return 0;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10u + (u32)(*s - '0');
        if (v > 1000000000u) {
            return 0;
        }
        digits++;
        s++;
    }
    *out = v;
    return (digits > 0) ? 1 : 0;
}

/* ========================================================================== */
/* 字符串 / 错误文本                                                         */
/* ========================================================================== */

static int sh_streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static u32 sh_strlen(const char *s)
{
    u32 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static const char *sh_errstr(i64 e)
{
    switch (e) {
    case SH_EPERM:
        return "Operation not permitted";
    case SH_ENOENT:
        return "No such file or directory";
    case SH_EIO:
        return "Input/output error";
    case SH_EBADF:
        return "Bad file descriptor";
    case SH_EAGAIN:
        return "Resource temporarily unavailable";
    case SH_ENOMEM:
        return "Out of memory";
    case SH_EACCES:
        return "Permission denied";
    case SH_EFAULT:
        return "Bad address";
    case SH_EEXIST:
        return "File exists";
    case SH_ENOTDIR:
        return "Not a directory";
    case SH_EISDIR:
        return "Is a directory";
    case SH_EINVAL:
        return "Invalid argument";
    case SH_EMFILE:
        return "Too many open files";
    case SH_ENOSPC:
        return "No space left on device";
    case SH_EROFS:
        return "Read-only filesystem";
    case SH_ENAMETOOLONG:
        return "File name too long";
    case SH_ENOTEMPTY:
        return "Directory not empty";
    case SH_ELOOP:
        return "Too many levels of symbolic links";
    default:
        return "Unknown error";
    }
}

static u32 g_status; /* 上一条命令的状态（0 = 成功） */
static u32 g_child_pid; /* 最近一次 spawn 的子进程 pid（0 = 无） */

/*
 * 友好错误行：`cmd: arg: message`（arg 为空时省略）。只改状态，不退出。
 */
static void sh_error(const char *cmd, const char *arg, i64 e)
{
    sh_puts(cmd);
    if (arg != 0 && *arg != '\0') {
        sh_puts(": ");
        sh_puts(arg);
    }
    sh_puts(": ");
    sh_puts(sh_errstr(e));
    sh_puts("\r\n");
    g_status = 1;
}

/* 用法/参数错误行：`sh: <text>`。 */
static void sh_usage(const char *text)
{
    sh_puts("sh: ");
    sh_puts(text);
    sh_puts("\r\n");
    g_status = 1;
}

/* 命令成功后的自动化证据行：`ng: shell <name> ok`。 */
static void sh_evidence(const char *name)
{
    sh_puts("ng: shell ");
    sh_puts(name);
    sh_puts(" ok\r\n");
}

/* ========================================================================== */
/* 状态：cwd / 行缓冲 / 退出                                                 */
/* ========================================================================== */

static char g_cwd[SH_PATH_MAX] = "/";
static char g_line[SH_LINE_MAX];
static u32 g_line_len;
static char g_path[SH_PATH_MAX];
static char g_join[SH_PATH_MAX];
static char g_text[SH_TEXT_MAX];
static char g_dmesg[SH_DMESG_MAX];
static char g_pkg[SH_PKG_MAX];
static u8 g_fdbuf[SH_IO_MAX];
static struct sh_stat g_st;
static struct sh_dirent g_dents[SH_DIRENT_MAX];
static int g_exit;
static u32 g_exit_code;

/* ========================================================================== */
/* 路径解析：cwd + 输入 -> 规范化绝对路径（不查 FS）                         */
/* ========================================================================== */

/*
 * 词法规范化：折叠重复 '/'、丢掉 "."、处理 ".."（根之上仍是根）。
 * 返回 0 / SH_EINVAL（空输入）/ SH_ENAMETOOLONG。
 */
static int sh_resolve(const char *in, char *out, u32 cap)
{
    u32 j = 0;
    u32 i;
    u32 o = 0;

    if (in == 0 || *in == '\0' || cap < 2u) {
        return SH_EINVAL;
    }
    if (in[0] != '/') {
        u32 cl = sh_strlen(g_cwd);

        if (cl + 1u + 1u >= sizeof(g_join)) {
            return SH_ENAMETOOLONG;
        }
        for (i = 0; i < cl; i++) {
            g_join[j++] = g_cwd[i];
        }
        if (j == 0 || g_join[j - 1u] != '/') {
            g_join[j++] = '/';
        }
    }
    for (i = 0; in[i] != '\0'; i++) {
        if (j + 1u >= sizeof(g_join)) {
            return SH_ENAMETOOLONG;
        }
        g_join[j++] = in[i];
    }
    g_join[j] = '\0';

    out[0] = '\0';
    i = 0;
    while (i < j) {
        u32 start;
        u32 len;

        while (i < j && g_join[i] == '/') {
            i++;
        }
        start = i;
        while (i < j && g_join[i] != '/') {
            i++;
        }
        len = i - start;
        if (len == 0u || (len == 1u && g_join[start] == '.')) {
            continue;
        }
        if (len == 2u && g_join[start] == '.' && g_join[start + 1u] == '.') {
            while (o > 0 && out[o - 1u] != '/') {
                o--;
            }
            if (o > 0) {
                o--;
            }
            out[o] = '\0';
            continue;
        }
        if (o + 1u + len + 1u > cap) {
            return SH_ENAMETOOLONG;
        }
        out[o++] = '/';
        {
            u32 k;

            for (k = 0; k < len; k++) {
                out[o++] = g_join[start + k];
            }
        }
        out[o] = '\0';
    }
    if (o == 0u) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

/* ========================================================================== */
/* tokenizer / 重定向                                                        */
/* ========================================================================== */

struct sh_tokens {
    int argc;
    char *argv[SH_ARG_MAX];
    char *redir; /* '>' 后面的路径（未解析），没有则为 0 */
};

/*
 * 就地切分：空白分隔；`>` 单独一个 token 或紧贴路径（`>out`）都支持，
 * 只允许一个 `>` 且必须是重定向（不支持 `>>`/`<`/管道）。
 * 返回 0 / -1 参数过多 / -2 空行 / -3 缺重定向路径 / -4 非法重定向。
 */
static int sh_tokenize(char *s, struct sh_tokens *t)
{
    int n = 0;

    t->argc = 0;
    t->redir = 0;
    while (*s != '\0') {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        if (*s == '>') {
            if (t->redir != 0 || s[1] == '>') {
                return -4;
            }
            s++;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s == '\0') {
                return -3;
            }
            t->redir = s;
            while (*s != '\0' && *s != ' ' && *s != '\t') {
                s++;
            }
            if (*s != '\0') {
                *s = '\0';
                s++;
            }
            continue;
        }
        if (n >= SH_ARG_MAX) {
            return -1;
        }
        t->argv[n++] = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            s++;
        }
        if (*s != '\0') {
            *s = '\0';
            s++;
        }
    }
    t->argc = n;
    if (n == 0) {
        return (t->redir != 0) ? -3 : -2;
    }
    return 0;
}

/* ========================================================================== */
/* 命令实现                                                                  */
/* ========================================================================== */

static int sh_cmd_help(const struct sh_tokens *t)
{
    (void)t;
    sh_puts("Aster ng shell (WP2-6) - 64-bit ring3 shell\r\n");
    sh_puts("commands:\r\n");
    sh_puts("  help                 this help\r\n");
    sh_puts("  ls [path]            list a directory (first 8 entries)\r\n");
    sh_puts("  cat <file>           print a file\r\n");
    sh_puts("  echo <text> [> path] print text, or write it to a file\r\n");
    sh_puts("  mkdir <dir>          create a directory\r\n");
    sh_puts("  rm <path>            remove a file\r\n");
    sh_puts("  cd [dir]             change directory (default /)\r\n");
    sh_puts("  pwd                  print working directory\r\n");
    sh_puts("  stat <path>          show type/mode/size/ino\r\n");
    sh_puts("  dmesg [-n N]         show kernel log (last N lines)\r\n");
    sh_puts("  log <tag> <msg>      append a line to the kernel log\r\n");
    sh_puts("  pkg list             list packages\r\n");
    sh_puts("  pkg verify <name>    verify a package (manifest + sha256)\r\n");
    sh_puts("  login <user> <pass>  authenticate\r\n");
    sh_puts("  spawn <path> [args]  start a child process (WP3-0)\r\n");
    sh_puts("  wait [pid]           wait for a child, print its status\r\n");
    sh_puts("  pid                  print this shell's pid / ppid\r\n");
    sh_puts("  clear                clear the screen\r\n");
    sh_puts("  exit [code]          leave the shell\r\n");
    sh_puts("keys: Enter run, Backspace edit, Ctrl-L clear, Ctrl-C cancel, "
            "Ctrl-D exit\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_ls(const struct sh_tokens *t)
{
    const char *arg = (t->argc > 1) ? t->argv[1] : ".";
    i64 n;
    i64 i;

    if (t->argc > 2) {
        sh_usage("ls: usage: ls [path]");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("ls: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(arg, g_path, SH_PATH_MAX) != 0) {
        sh_error("ls", arg, SH_ENAMETOOLONG);
        return 1;
    }
    n = sh_sys(SH_SYS_LISTDIR, (i64)g_path, (i64)g_dents, SH_DIRENT_MAX, 0);
    if (n < 0) {
        sh_error("ls", arg, n);
        return 1;
    }
    for (i = 0; i < n; i++) {
        u32 len = g_dents[i].name_len;

        if (len >= sizeof(g_dents[i].name)) {
            len = sizeof(g_dents[i].name) - 1u;
        }
        sh_putn(g_dents[i].name, len);
        if (g_dents[i].type == SH_NODE_DIR) {
            sh_puts("/\r\n");
        } else {
            sh_puts("  ");
            sh_putdec(g_dents[i].size);
            sh_puts("\r\n");
        }
    }
    if (n == 0) {
        sh_puts("(empty)\r\n");
    } else if (n == (i64)SH_DIRENT_MAX) {
        sh_puts("ls: (showing the first 8 entries)\r\n");
    }
    sh_evidence("ls");
    g_status = 0;
    return 0;
}

static int sh_cmd_cat(const struct sh_tokens *t)
{
    u64 total = 0;
    i64 fd;
    i64 n;

    if (t->argc != 2) {
        sh_usage("cat: usage: cat <file>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("cat: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(t->argv[1], g_path, SH_PATH_MAX) != 0) {
        sh_error("cat", t->argv[1], SH_ENAMETOOLONG);
        return 1;
    }
    fd = sh_sys(SH_SYS_OPEN, (i64)g_path, SH_O_RDONLY, 0, 0);
    if (fd < 0) {
        sh_error("cat", t->argv[1], fd);
        return 1;
    }
    for (;;) {
        n = sh_sys(SH_SYS_READ, fd, (i64)g_fdbuf, SH_IO_MAX, 0);
        if (n < 0) {
            sh_error("cat", t->argv[1], n);
            (void)sh_sys(SH_SYS_CLOSE, fd, 0, 0, 0);
            return 1;
        }
        if (n == 0) {
            break;
        }
        sh_put_content((const char *)g_fdbuf, (u32)n);
        total += (u64)n;
        if (total >= SH_CAT_MAX) {
            sh_puts("cat: (output truncated at 64 KiB)\r\n");
            break;
        }
    }
    (void)sh_sys(SH_SYS_CLOSE, fd, 0, 0, 0);
    sh_evidence("cat");
    g_status = 0;
    return 0;
}

static int sh_cmd_echo(const struct sh_tokens *t)
{
    u32 len = 0;
    int i;

    for (i = 1; i < t->argc; i++) {
        const char *w = t->argv[i];
        u32 wl = sh_strlen(w);

        if (i > 1) {
            if (len + 2u > SH_TEXT_MAX) {
                sh_usage("echo: text too long (max 157 bytes)");
                return 1;
            }
            g_text[len++] = ' ';
        }
        if (len + wl + 2u > SH_TEXT_MAX) {
            sh_usage("echo: text too long (max 157 bytes)");
            return 1;
        }
        {
            u32 k;

            for (k = 0; k < wl; k++) {
                g_text[len++] = w[k];
            }
        }
    }
    g_text[len] = '\0';

    if (t->redir != 0) {
        i64 fd;
        i64 n;

        if (sh_resolve(t->redir, g_path, SH_PATH_MAX) != 0) {
            sh_error("echo", t->redir, SH_ENAMETOOLONG);
            return 1;
        }
        fd = sh_sys(SH_SYS_OPEN, (i64)g_path,
                    SH_O_CREAT | SH_O_WRONLY | SH_O_TRUNC, 0644, 0);
        if (fd < 0) {
            sh_error("echo", t->redir, fd);
            return 1;
        }
        g_text[len++] = '\n';
        n = sh_sys(SH_SYS_WRITE_FD, fd, (i64)g_text, len, 0);
        if (n != (i64)len) {
            sh_error("echo", t->redir, (n < 0) ? n : SH_EIO);
            (void)sh_sys(SH_SYS_CLOSE, fd, 0, 0, 0);
            return 1;
        }
        if (sh_sys(SH_SYS_CLOSE, fd, 0, 0, 0) != 0) {
            sh_error("echo", t->redir, SH_EIO);
            return 1;
        }
        sh_evidence("write");
        g_status = 0;
        return 0;
    }

    sh_putn(g_text, len);
    sh_puts("\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_mkdir(const struct sh_tokens *t)
{
    i64 rc;

    if (t->argc != 2) {
        sh_usage("mkdir: usage: mkdir <dir>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("mkdir: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(t->argv[1], g_path, SH_PATH_MAX) != 0) {
        sh_error("mkdir", t->argv[1], SH_ENAMETOOLONG);
        return 1;
    }
    rc = sh_sys(SH_SYS_MKDIR, (i64)g_path, 0755, 0, 0);
    if (rc != 0) {
        sh_error("mkdir", t->argv[1], rc);
        return 1;
    }
    g_status = 0;
    return 0;
}

static int sh_cmd_rm(const struct sh_tokens *t)
{
    i64 rc;

    if (t->argc != 2) {
        sh_usage("rm: usage: rm <path>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("rm: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(t->argv[1], g_path, SH_PATH_MAX) != 0) {
        sh_error("rm", t->argv[1], SH_ENAMETOOLONG);
        return 1;
    }
    rc = sh_sys(SH_SYS_UNLINK, (i64)g_path, 0, 0, 0);
    if (rc != 0) {
        sh_error("rm", t->argv[1], rc);
        return 1;
    }
    g_status = 0;
    return 0;
}

static int sh_cmd_cd(const struct sh_tokens *t)
{
    const char *arg = (t->argc > 1) ? t->argv[1] : "/";
    i64 rc;
    u32 i;

    if (t->argc > 2) {
        sh_usage("cd: usage: cd [dir]");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("cd: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(arg, g_path, SH_PATH_MAX) != 0) {
        sh_error("cd", arg, SH_ENAMETOOLONG);
        return 1;
    }
    rc = sh_sys(SH_SYS_STAT, (i64)g_path, (i64)&g_st, 0, 0);
    if (rc != 0) {
        sh_error("cd", arg, rc);
        return 1;
    }
    if (g_st.type != SH_NODE_DIR) {
        sh_error("cd", arg, SH_ENOTDIR);
        return 1;
    }
    for (i = 0; g_path[i] != '\0' && i + 1u < SH_PATH_MAX; i++) {
        g_cwd[i] = g_path[i];
    }
    g_cwd[i] = '\0';
    g_status = 0;
    return 0;
}

static int sh_cmd_pwd(const struct sh_tokens *t)
{
    (void)t;
    sh_puts(g_cwd);
    sh_puts("\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_stat(const struct sh_tokens *t)
{
    i64 rc;

    if (t->argc != 2) {
        sh_usage("stat: usage: stat <path>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("stat: redirection is only supported by echo");
        return 1;
    }
    if (sh_resolve(t->argv[1], g_path, SH_PATH_MAX) != 0) {
        sh_error("stat", t->argv[1], SH_ENAMETOOLONG);
        return 1;
    }
    rc = sh_sys(SH_SYS_STAT, (i64)g_path, (i64)&g_st, 0, 0);
    if (rc != 0) {
        sh_error("stat", t->argv[1], rc);
        return 1;
    }
    sh_puts("  file: ");
    sh_puts(g_path);
    sh_puts("\r\n  type: ");
    if (g_st.type == SH_NODE_DIR) {
        sh_puts("dir\r\n");
    } else if (g_st.type == SH_NODE_FILE) {
        sh_puts("file\r\n");
    } else {
        sh_puts("other\r\n");
    }
    sh_puts("  mode: 0");
    sh_putoct(g_st.mode & 07777u);
    sh_puts("\r\n  size: ");
    sh_putdec(g_st.size);
    sh_puts("\r\n  ino:  ");
    sh_putdec(g_st.ino);
    sh_puts("\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_dmesg(const struct sh_tokens *t)
{
    u32 want = 0;
    u32 start = 0;
    i64 n;

    if (t->argc == 3 && sh_streq(t->argv[1], "-n")) {
        if (!sh_parse_u32(t->argv[2], &want) || want == 0u || want > 64u) {
            sh_usage("dmesg: -n needs a line count in 1..64");
            return 1;
        }
    } else if (t->argc != 1) {
        sh_usage("dmesg: usage: dmesg [-n N]");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("dmesg: redirection is only supported by echo");
        return 1;
    }
    n = sh_sys(SH_SYS_DMESG, (i64)g_dmesg, SH_DMESG_MAX - 1u, 0, 0);
    if (n < 0) {
        sh_error("dmesg", "", n);
        return 1;
    }
    g_dmesg[n] = '\0';
    if (want > 0u && n > 0) {
        u32 i = (u32)n;
        u32 lines = 0;
        u32 pos;

        while (i > 0u && g_dmesg[i - 1u] == '\n') {
            i--;
        }
        pos = i;
        while (pos > 0u) {
            if (g_dmesg[pos - 1u] == '\n') {
                lines++;
                if (lines >= want) {
                    start = pos;
                    break;
                }
            }
            pos--;
        }
    }
    if (start < (u32)n) {
        sh_put_content(g_dmesg + start, (u32)n - start);
        if (g_dmesg[n - 1] != '\n') {
            sh_puts("\r\n");
        }
    } else if (n == 0) {
        sh_puts("dmesg: (log is empty)\r\n");
    }
    sh_evidence("dmesg");
    g_status = 0;
    return 0;
}

static int sh_log_tag_ok(const char *tag)
{
    u32 n = sh_strlen(tag);
    u32 i;

    if (n < 1u || n > 15u) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        u8 c = (u8)tag[i];

        if (c < 0x21u || c > 0x7Eu || c == ':') {
            return 0;
        }
    }
    return 1;
}

static int sh_cmd_log(const struct sh_tokens *t)
{
    u32 len = 0;
    int i;
    i64 rc;

    if (t->argc < 3) {
        sh_usage("log: usage: log <tag> <msg>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("log: redirection is only supported by echo");
        return 1;
    }
    if (!sh_log_tag_ok(t->argv[1])) {
        sh_usage("log: tag must be 1..15 printable chars without ':'");
        return 1;
    }
    for (i = 2; i < t->argc; i++) {
        const char *w = t->argv[i];
        u32 wl = sh_strlen(w);
        u32 k;

        if (i > 2) {
            if (len + 2u > 127u) {
                sh_usage("log: message too long (max 126 bytes)");
                return 1;
            }
            g_text[len++] = ' ';
        }
        if (len + wl + 1u > 127u) {
            sh_usage("log: message too long (max 126 bytes)");
            return 1;
        }
        for (k = 0; k < wl; k++) {
            u8 c = (u8)w[k];

            if (c < 0x20u || c > 0x7Eu) {
                sh_usage("log: message must be printable ASCII");
                return 1;
            }
            g_text[len++] = w[k];
        }
    }
    g_text[len] = '\0';
    if (len == 0u) {
        sh_usage("log: message is empty");
        return 1;
    }
    rc = sh_sys(SH_SYS_LOG_WRITE, (i64)t->argv[1], (i64)g_text, 0, 0);
    if (rc != 0) {
        sh_error("log", t->argv[1], rc);
        return 1;
    }
    sh_puts("log: appended (tag=");
    sh_puts(t->argv[1]);
    sh_puts(", bytes=");
    sh_putdec((u64)len);
    sh_puts(")\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_pkg(const struct sh_tokens *t)
{
    if (t->argc >= 2 && sh_streq(t->argv[1], "list")) {
        i64 n;

        if (t->argc != 2) {
            sh_usage("pkg: usage: pkg list");
            return 1;
        }
        n = sh_sys(SH_SYS_PKG_LIST, (i64)g_pkg, SH_PKG_MAX - 1u, 0, 0);
        if (n < 0) {
            sh_error("pkg", "list", n);
            return 1;
        }
        g_pkg[SH_PKG_MAX - 1u] = '\0';
        if (n == 0) {
            sh_puts("(no packages)\r\n");
        } else {
            sh_put_content(g_pkg, sh_strlen(g_pkg));
        }
        sh_evidence("pkg");
        g_status = 0;
        return 0;
    }
    if (t->argc >= 2 && sh_streq(t->argv[1], "verify")) {
        i64 rc;

        if (t->argc != 3) {
            sh_usage("pkg: usage: pkg verify <name>");
            return 1;
        }
        rc = sh_sys(SH_SYS_PKG_VERIFY, (i64)t->argv[2], 0, 0, 0);
        if (rc != 0) {
            sh_error("pkg", t->argv[2], rc);
            return 1;
        }
        sh_puts("pkg: ");
        sh_puts(t->argv[2]);
        sh_puts(": verified\r\n");
        sh_evidence("pkg");
        g_status = 0;
        return 0;
    }
    sh_usage("pkg: usage: pkg list | pkg verify <name>");
    return 1;
}

static int sh_cmd_login(const struct sh_tokens *t)
{
    i64 uid;

    if (t->argc != 3) {
        sh_usage("login: usage: login <user> <pass>");
        return 1;
    }
    if (t->redir != 0) {
        sh_usage("login: redirection is only supported by echo");
        return 1;
    }
    uid = sh_sys(SH_SYS_LOGIN, (i64)t->argv[1], (i64)t->argv[2], 0, 0);
    if (uid < 0) {
        sh_error("login", t->argv[1], uid);
        return 1;
    }
    sh_puts("login: ");
    sh_puts(t->argv[1]);
    sh_puts(": uid=");
    sh_putdec((u64)uid);
    sh_puts("\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_clear(const struct sh_tokens *t)
{
    (void)t;
    /* 0x0C = 换页：内核控制台镜像把它解释成清屏；串口终端忽略它。 */
    sh_putc('\f');
    g_status = 0;
    return 0;
}

/* ========================================================================== */
/* WP3-0：进程命令（spawn / wait / pid）                                      */
/* ========================================================================== */

static int sh_cmd_spawn(const struct sh_tokens *t)
{
    char path[SH_PATH_MAX];
    char *argv[SH_ARG_MAX + 1];
    u32 pid = 0;
    i64 rc;
    u32 i;

    if (t->argc < 2) {
        sh_usage("spawn: usage: spawn <path> [args]");
        return 1;
    }
    if (sh_resolve(t->argv[1], path, (u32)sizeof(path)) != 0) {
        sh_usage("spawn: path too long");
        return 1;
    }
    /* 子进程的 argv[0] 就是它自己的路径。 */
    argv[0] = path;
    for (i = 2; (int)i < t->argc; i++) {
        argv[i - 1] = t->argv[i];
    }
    argv[t->argc - 1] = 0;
    rc = sh_sys(SH_SYS_SPAWN, (i64)path, (i64)argv, (i64)&pid, 0);
    if (rc < 0) {
        sh_error("spawn", t->argv[1], rc);
        return 1;
    }
    g_child_pid = pid;
    sh_puts("ng: shell spawn ok (pid=");
    sh_putdec((u64)pid);
    sh_puts(" path=");
    sh_puts(path);
    sh_puts(")\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_wait(const struct sh_tokens *t)
{
    u32 pid = g_child_pid;
    int status = 0;
    i64 rc;

    if (t->argc > 2) {
        sh_usage("wait: usage: wait [pid]");
        return 1;
    }
    if (t->argc == 2 && (!sh_parse_u32(t->argv[1], &pid) || pid == 0)) {
        sh_usage("wait: pid must be a positive number");
        return 1;
    }
    if (pid == 0) {
        sh_usage("wait: no child (use 'spawn' first or 'wait <pid>')");
        return 1;
    }
    rc = sh_sys(SH_SYS_WAIT, (i64)pid, (i64)&status, 0, 0);
    if (rc < 0) {
        sh_error("wait", "", rc);
        return 1;
    }
    if (pid == g_child_pid) {
        g_child_pid = 0;
    }
    sh_puts("ng: shell wait ok (pid=");
    sh_putdec((u64)pid);
    sh_puts(" status=");
    sh_putdec((u64)(u32)status);
    sh_puts(")\r\n");
    g_status = (status == 0) ? 0u : 1u;
    return 0;
}

static int sh_cmd_pid(const struct sh_tokens *t)
{
    (void)t;
    sh_puts("pid=");
    sh_putdec((u64)sh_sys(SH_SYS_GETPID, 0, 0, 0, 0));
    sh_puts(" ppid=");
    sh_putdec((u64)sh_sys(SH_SYS_GETPPID, 0, 0, 0, 0));
    sh_puts("\r\n");
    g_status = 0;
    return 0;
}

static int sh_cmd_exit(const struct sh_tokens *t)
{
    u32 code = g_status;

    if (t->argc > 2) {
        sh_usage("exit: usage: exit [code]");
        return 1;
    }
    if (t->argc == 2) {
        if (!sh_parse_u32(t->argv[1], &code) || code > 255u) {
            sh_usage("exit: code must be 0..255");
            return 1;
        }
    }
    sh_evidence("exit");
    g_exit = 1;
    g_exit_code = code;
    (void)sh_sys(SH_SYS_EXIT, (i64)code, 0, 0, 0);
    return 0;
}

/* ========================================================================== */
/* 分发                                                                      */
/* ========================================================================== */

struct sh_cmd {
    const char *name;
    int (*fn)(const struct sh_tokens *t);
};

static const struct sh_cmd g_cmds[] = {
    {"help", sh_cmd_help},   {"ls", sh_cmd_ls},       {"cat", sh_cmd_cat},
    {"echo", sh_cmd_echo},   {"mkdir", sh_cmd_mkdir}, {"rm", sh_cmd_rm},
    {"cd", sh_cmd_cd},       {"pwd", sh_cmd_pwd},     {"stat", sh_cmd_stat},
    {"dmesg", sh_cmd_dmesg}, {"log", sh_cmd_log},     {"pkg", sh_cmd_pkg},
    {"login", sh_cmd_login}, {"spawn", sh_cmd_spawn}, {"wait", sh_cmd_wait},
    {"pid", sh_cmd_pid},     {"clear", sh_cmd_clear}, {"exit", sh_cmd_exit},
};

static void sh_run_line(char *line)
{
    struct sh_tokens t;
    int rc;
    u32 i;

    rc = sh_tokenize(line, &t);
    if (rc == -2) {
        /* 空行不改变上一条命令的状态（$? 语义）。 */
        return;
    }
    if (rc == -1) {
        sh_usage("too many arguments (max 8)");
        return;
    }
    if (rc == -3) {
        sh_usage("redirection '>' needs a path");
        return;
    }
    if (rc == -4) {
        sh_usage("only one '>' redirection is supported");
        return;
    }
    for (i = 0; i < sizeof(g_cmds) / sizeof(g_cmds[0]); i++) {
        if (sh_streq(t.argv[0], g_cmds[i].name)) {
            if (t.redir != 0 && g_cmds[i].fn != sh_cmd_echo) {
                sh_usage("only echo supports '>' redirection");
                return;
            }
            (void)g_cmds[i].fn(&t);
            return;
        }
    }
    sh_puts("sh: ");
    sh_puts(t.argv[0]);
    sh_puts(": command not found (try 'help')\r\n");
    g_status = 1;
}

static void sh_prompt(void)
{
    sh_puts("ng> ");
}

/* 无键可读时的忙等提示：PAUSE 让出流水线（ring3 里没有 sleep/IRQ 机制）。 */
static void sh_idle_hint(void)
{
#ifndef NG_HOST_TEST
    u32 i;

    for (i = 0; i < 64u; i++) {
        __asm__ volatile("pause");
    }
#endif
}

/* ========================================================================== */
/* 入口                                                                      */
/* ========================================================================== */

int shell_main(void)
{
    u32 i;

    for (i = 0; i < SH_LINE_MAX; i++) {
        g_line[i] = '\0';
    }
    g_line_len = 0;
    g_cwd[0] = '/';
    g_cwd[1] = '\0';
    g_status = 0;
    g_exit = 0;
    g_exit_code = 0;

    sh_puts("Aster ng shell (WP2-6, ring3) - type 'help'\r\n");
    sh_puts("ng: shell ready\r\n");
    sh_prompt();

    for (;;) {
        i64 k = sh_sys(SH_SYS_READKEY, 0, 0, 0, 0);
        u32 flags;
        u32 ascii;

        if (k == SH_EAGAIN) {
            sh_idle_hint();
            continue;
        }
        if (k < 0) {
            continue;
        }
        flags = SH_KEY_FLAGS(k);
        ascii = SH_KEY_ASCII(k);
        if ((flags & SH_F_CTRL) != 0u) {
            if (ascii == 'd') {
                sh_evidence("exit");
                g_exit = 1;
                g_exit_code = 0;
                (void)sh_sys(SH_SYS_EXIT, 0, 0, 0, 0);
                break;
            }
            if (ascii == 'l') {
                sh_putc('\f');
                sh_prompt();
                sh_putn(g_line, g_line_len);
                continue;
            }
            if (ascii == 'c') {
                sh_puts("^C\r\n");
                g_line_len = 0;
                g_status = 1;
                sh_prompt();
                continue;
            }
            if (ascii == 'u') {
                while (g_line_len > 0u) {
                    g_line_len--;
                    (void)sh_write("\b \b", 3);
                }
                continue;
            }
            continue;
        }
        if (ascii == 13u || ascii == 10u) {
            sh_puts("\r\n");
            g_line[g_line_len] = '\0';
            sh_run_line(g_line);
            g_line_len = 0;
            if (g_exit) {
                break;
            }
            sh_prompt();
            continue;
        }
        if (ascii == 8u || ascii == 127u) {
            if (g_line_len > 0u) {
                g_line_len--;
                (void)sh_write("\b \b", 3);
            }
            continue;
        }
        if (ascii >= 0x20u && ascii <= 0x7Eu) {
            if (g_line_len + 1u < SH_LINE_MAX) {
                g_line[g_line_len++] = (char)ascii;
                sh_putc((char)ascii);
            }
            continue;
        }
        /* 其它键（方向键/功能键等）：忽略。 */
    }
    return (int)g_exit_code;
}

#ifdef NG_HOST_TEST
/* ------------------------------------------------------------------------ */
/* host 单测钩子（内核 / 用户 ELF 构建都不编译这一段）                       */
/* ------------------------------------------------------------------------ */

int sh_test_tokenize(char *line, int *argc, char *argv[SH_ARG_MAX],
                     char **redir)
{
    struct sh_tokens t;
    int rc;
    int i;

    for (i = 0; i < SH_ARG_MAX; i++) {
        argv[i] = 0;
    }
    *argc = 0;
    *redir = 0;
    rc = sh_tokenize(line, &t);
    if (rc == 0) {
        *argc = t.argc;
        for (i = 0; i < t.argc; i++) {
            argv[i] = t.argv[i];
        }
        *redir = t.redir;
    }
    return rc;
}

int sh_test_resolve(const char *cwd, const char *in, char *out, u32 cap)
{
    u32 i = 0;

    while (cwd[i] != '\0' && i + 1u < SH_PATH_MAX) {
        g_cwd[i] = cwd[i];
        i++;
    }
    g_cwd[i] = '\0';
    return sh_resolve(in, out, cap);
}

const char *sh_test_errstr(i64 e)
{
    return sh_errstr(e);
}

u32 sh_test_status(void)
{
    return g_status;
}

void sh_test_cwd(char *out, u32 cap)
{
    u32 i = 0;

    while (g_cwd[i] != '\0' && i + 1u < cap) {
        out[i] = g_cwd[i];
        i++;
    }
    out[i] = '\0';
}
#endif /* NG_HOST_TEST */

/* 内核从 ELF e_entry 进用户态；这里把栈对齐到 SysV ABI 再调 C。 */
#ifndef NG_HOST_TEST
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call shell_main\n\t"
                     "1: jmp 1b\n\t");
}
#endif
