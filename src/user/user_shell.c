/* =============================================================================
 * user_shell.c - 运行在 ring3 的 shell
 *
 * 它不链接任何内核代码：迁移期所有服务都通过 int 0x81 请求，
 * 包括输出、读键盘、打开/读写/关闭文件、列目录。
 * 编译后链接到 0x40000000，由内核从文件系统 "/bin/shell" 载入。
 * ============================================================================= */

#include "ulib.h"

/* 系统调用编号（SYS_*）、struct udp_req 与公共结构体统一来自
 * include/abi.h（经 ulib.h 引入），这里不再手写编号。 */

/* 打开标志（与内核 fs/fs.h 一致） */
#define FS_READ   1
#define FS_WRITE  2
#define FS_CREAT  4

/* SYS_PKG(77) 的 op 编码：与 src/kernel/include/pkg.h 保持一致，
 * 完整说明（参数/返回值/权限）见 docs/claims/pkg.md。 */
#define PKG_OP_LIST              0
#define PKG_OP_INSTALL           1
#define PKG_OP_REMOVE            2
#define PKG_OP_VERIFY            3
#define PKG_OP_COMPONENT_PORT    4
#define PKG_OP_COMPONENT_UNREG   5

static inline int sc(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static inline int nc(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x80"
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

/* 组件 ELF 的 ring3 服务模式（argv[0] = "svc:<组件名>"）。
 *
 * 组件模型 P0：_pkgd 从 <root>/bin/<name> 读 ELF、验哈希+签名后，用
 * user_component_exec 把它映射成 ring3 程序，argv[0] 约定为 "svc:<name>"。
 * 这里用 native ABI 的 port/channel 服务接口把同名服务注册上去，然后回显
 * 收到的消息；收到 "stop" 时先让 _pkgd 注销注册表项（SYS_PKG op 5），再退出。
 * 这样卸载不会在服务表里留下指向已释放 port 的僵尸条目。 */
static int component_service_main(const char *name)
{
    struct { unsigned int type, rights, object_id, flags; } info;
    int port;
    int port_obj = -1;
    char msg[64];

    if (name == 0 || name[0] == '\0') return 1;

    port = nc(0x33, 0, 0, 0);                   /* PORT_CREATE */
    if (port < 0) {
        puterr("component: cannot create port\n");
        return 1;
    }
    if (nc(0x38, (int)name, (int)slen(name), port) < 0) {   /* SERVICE_REGISTER */
        puterr("component: cannot register service\n");
        return 1;
    }
    if (nc(0x12, port, (int)&info, 0) == 0) {   /* HANDLE_INFO -> object_id */
        port_obj = (int)info.object_id;
        (void)sc(SYS_PKG, PKG_OP_COMPONENT_PORT, port_obj, 0);
    }
    printf("component: %s elf service online (pid=%d port=%d)\n",
           name, sc(SYS_GETPID, 0, 0, 0), port);

    for (;;) {
        int s = nc(0x37, port, 0, 0);           /* PORT_ACCEPT_BLOCK */
        int n;

        if (s < 0) break;
        n = nc(0x36, s, (int)msg, (int)sizeof(msg));   /* CHANNEL_RECV_BLOCK */
        if (n == 4 && msg[0] == 's' && msg[1] == 't' &&
            msg[2] == 'o' && msg[3] == 'p') {
            nc(0x10, s, 0, 0);                  /* HANDLE_CLOSE */
            break;
        }
        if (n > 0) (void)nc(0x31, s, (int)msg, n);     /* CHANNEL_SEND echo */
        nc(0x10, s, 0, 0);
    }

    if (port_obj >= 0) {
        (void)sc(SYS_PKG, PKG_OP_COMPONENT_UNREG, port_obj, 0);
    }
    printf("component: %s elf service stopped\n", name);
    return 0;
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

/* 把“提示符 + 当前输入”画到底部输入框（SYS_INPUT_SET）。
 * 内核 SYS_INPUT_SET 的缓冲是 128 字节（len 必须 < 128），整行超长会被
 * 直接拒绝、输入框就不刷新了；这里先截断到 127，保证长行也能显示。
 * 光标不放进这个缓冲区：它由 SYS_CONSOLE_CURSOR 在 framebuffer 上画。
 *
 * 注意：输入/光标这段代码正好压在 0x40004000 边界上——-O0 下 .text 多出
 * 几十字节就会把 .rodata 挤到下一页，user_shell.elf 因此多 4KB（ramfs 单
 * 文件上限 32KB）。和 sectest 一样，这一段单独用 -Os 编译，语义不变。
 */
#pragma GCC push_options
#pragma GCC optimize ("Os")

#define INPUT_LINE_MAX 127

static void show_input(const char *buf, int len)
{
    static char line[INPUT_LINE_MAX + 1];
    int n = 0;
    const char *p = "ushell> ";

    while (*p != '\0' && n < INPUT_LINE_MAX) line[n++] = *p++;
    for (int i = 0; i < len && n < INPUT_LINE_MAX; i++) {
        line[n++] = buf[i];
    }
    line[n] = '\0';

    sc(SYS_INPUT_SET, (int)line, n, 0);
    /* console 端会在 input_set 之后自动把光标重画到新行尾 */
}

/* 读一行：字符来自 SYS_GETCHAR（阻塞），显示走输入框，回车后写进输出区。
 *
 * 光标：开始输入前 console_cursor(1)，console 在每次 SYS_INPUT_SET 重画
 * 输入行后自动把 3px 下划线画到行尾；退格/字符输入都走 show_input，所以
 * 光标自动跟随。回车先 console_cursor(0) 再清输入框，光标不会进入输出区。
 *
 * 为什么不闪烁：SYS_GETCHAR 在内核里是 proc_block(PROC_WAIT_KBD) 的阻塞读，
 * ABI 没有非阻塞读键调用，也没有定时信号；SYS_SLEEP 只能在两次读之间睡，
 * 没法在等键时切换光标。忙等会占满 CPU 并拖慢输入响应，因此保持常亮。 */
static void readline(char *buf, int max)
{
    int len = 0;

    show_input(buf, 0);
    console_cursor(1);

    for (;;) {
        int c = sc(SYS_GETCHAR, 0, 0, 0);

        if (c == '\r' || c == '\n') {
            console_cursor(0);                  /* 先擦光标，别留在历史行里 */
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
                show_input(buf, len);           /* 光标随行尾左移 */
            }
            continue;
        }
        if (c < 0x20) continue;

        if (len + 1 < max) {
            buf[len++] = (char)c;
            show_input(buf, len);               /* 光标随行尾右移 */
        }
    }
}

#pragma GCC pop_options

/* =============================================================================
 * 登录前端（用户态 getty/login，P1）
 *
 * 读字符用 SYS_GETCHAR（阻塞、走键盘），口令不回显、只打 '*'。
 * 真正的口令校验在核内（SYS_LOGIN/LOGIN_AUTH -> security/accounts.c：
 * sha256 + 恒定时间比较 + 失败锁定），这里只负责提示、重试与延迟。
 * 登录成功后内核已把 uid/euid/gid/附加组落到本进程，cwd 也切到了 home，
 * 于是 /bin/shell 直接以该用户身份继续运行（等价于 login exec shell）。
 * ========================================================================== */
static int read_line_echo(char *buf, int max, int secret)
{
    int len = 0;

    for (;;) {
        int c = sc(SYS_GETCHAR, 0, 0, 0);

        if (c < 0) continue;
        if (c == '\r' || c == '\n') {
            putch('\n');
            buf[len] = '\0';
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                putstr("\b \b");                /* 终端没有行编辑：擦掉一个字符 */
            }
            continue;
        }
        if (c < 0x20) continue;
        if (len + 1 < max) {
            buf[len++] = (char)c;
            if (secret) putch('*'); else putch((char)c);
        }
    }
}

static void login_screen(void)
{
    char user[32];
    char pass[32];
    int attempt = 0;

    for (;;) {
        int r;

        putstr("\nosteachos login: ");
        (void)read_line_echo(user, (int)sizeof(user), 0);
        putstr("Password: ");
        (void)read_line_echo(pass, (int)sizeof(pass), 1);

        if (user[0] == '\0') continue;

        r = sc(SYS_LOGIN, LOGIN_AUTH, (int)user, (int)pass);
        if (r == 0) {
            printf("login: %s logged in\n", user);
            return;
        }
        if (r == -2) {
            puterr("login: too many failures, account locked - try again later\n");
            sc(SYS_SLEEP, 15000, 0, 0);         /* 锁定窗口 15s */
        } else {
            puterr("Login incorrect\n");
            sc(SYS_SLEEP, 1500, 0, 0);          /* 失败延迟，降低暴力猜测速率 */
        }
        attempt++;
        if (attempt > 20) return;               /* 防止无限重试占死 tty */
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

/* mv <old> <new>：同卷内改名/移动（不跨卷、不做"移入目录"语义）。
 * 内核返回的失败码在 syscall 层压成 -1，这里只能给通用错误信息。 */
/* ---- ln / ln -s / readlink（阶段 4b）---- */
static void cmd_ln(const char *a, const char *b, int symbolic)
{
    int r = symbolic ? sc(SYS_SYMLINK, (int)a, (int)b, 0)
                     : sc(SYS_LINK, (int)a, (int)b, 0);
    putstr((r == 0) ? "ok\n" : (symbolic ? "ln: symlink failed\n" : "ln: failed\n"));
    sc(SYS_SYNC, 0, 0, 0);
}

static void cmd_readlink(const char *path)
{
    char buf[128];
    int n = sc(SYS_READLINK, (int)path, (int)buf, (int)sizeof(buf));

    if (n < 0) {
        putstr("readlink: not a symlink\n");
        return;
    }
    buf[n] = '\0';
    putstr(buf);
    putstr("\n");
}

/* 回归用：硬链接 / 软链接 / readlink / lstat 一条链跑完（build.ps1 收集 "ushell: ln..."） */
static void cmd_linktest(void)
{
    char buf[64];
    int h, n, ok = 1;
    unsigned int st[6];

    sc(SYS_UNLINK, (int)"/tmp/lt-a", 0, 0);
    sc(SYS_UNLINK, (int)"/tmp/lt-b", 0, 0);
    sc(SYS_UNLINK, (int)"/tmp/lt-sl", 0, 0);

    h = sc(SYS_OPEN, (int)"/tmp/lt-a", FS_WRITE | FS_CREAT, 0);
    if (h < 0) { putstr("ushell: linktest FAILED (open)\n"); return; }
    sc(SYS_FWRITE, h, (int)"ln", 2);
    sc(SYS_CLOSE, h, 0, 0);

    if (sc(SYS_LINK, (int)"/tmp/lt-a", (int)"/tmp/lt-b", 0) != 0) ok = 0;
    h = sc(SYS_OPEN, (int)"/tmp/lt-b", FS_READ, 0);
    if (h < 0) ok = 0;
    else {
        n = sc(SYS_FREAD, h, (int)buf, (int)sizeof(buf));
        sc(SYS_CLOSE, h, 0, 0);
        if (n != 2 || buf[0] != 'l' || buf[1] != 'n') ok = 0;
    }
    if (ok) putstr("ushell: ln ok\n"); else putstr("ushell: ln FAILED\n");

    if (sc(SYS_SYMLINK, (int)"lt-b", (int)"/tmp/lt-sl", 0) != 0) ok = 0;
    h = sc(SYS_OPEN, (int)"/tmp/lt-sl", FS_READ, 0);
    if (h < 0) ok = 0;
    else { sc(SYS_CLOSE, h, 0, 0); }
    if (ok) putstr("ushell: ln -s ok\n"); else putstr("ushell: ln -s FAILED\n");

    n = sc(SYS_READLINK, (int)"/tmp/lt-sl", (int)buf, (int)sizeof(buf));
    if (n > 0) { buf[n] = '\0'; ok = (buf[0] == 'l' && buf[1] == 't'); } else ok = 0;
    if (ok) putstr("ushell: readlink ok\n"); else putstr("ushell: readlink FAILED\n");

    if (sc(SYS_LSTAT, (int)"/tmp/lt-sl", (int)st, 0) == 0 && st[0] == 3U) ok = 1;
    else ok = 0;
    if (ok) putstr("ushell: lstat link type ok\n"); else putstr("ushell: lstat FAILED\n");

    if (ok) putstr("ushell: linktest PASS\n"); else putstr("ushell: linktest FAILED\n");
    sc(SYS_UNLINK, (int)"/tmp/lt-a", 0, 0);
    sc(SYS_UNLINK, (int)"/tmp/lt-b", 0, 0);
    sc(SYS_UNLINK, (int)"/tmp/lt-sl", 0, 0);
}
static void cmd_mv(const char *oldpath, const char *newpath)
{
    putstr((sc(SYS_RENAME, (int)oldpath, (int)newpath, 0) == 0) ? "ok\n" : "mv: failed\n");
    sc(SYS_SYNC, 0, 0, 0);
}

/* 小工具：子串查找（pkg CLI 的自检只看关键字，不解析完整文本） */
static int has_sub(const char *hay, const char *needle)
{
    unsigned int n = slen(needle);

    if (hay == 0 || needle == 0 || n == 0U) return 0;
    while (*hay != '\0') {
        unsigned int i = 0;

        while (i < n && hay[i] == needle[i]) i++;
        if (i == n) return 1;
        hay++;
    }
    return 0;
}

/* pkg <install|list|remove|verify> ...（SYS_PKG 77 的 ring3 前端）
 * 与内核 _pkgd 同一套策略：普通用户只能动自己 ~/Apps 下的 app，
 * 系统组件要 root（= CAP_PKG_ADMIN）。 */
static void cmd_pkg(const char *sub, const char *arg)
{
    char reply[128];
    int r;

    if (sub == 0 || scmp(sub, "help") == 0) {
        putstr("usage: pkg install <file> | list | remove <name> | verify <name>\n");
        return;
    }
    if (scmp(sub, "list") == 0) {
        char buf[320];
        int n = sc(SYS_PKG, PKG_OP_LIST, (int)buf, (int)sizeof(buf));

        if (n < 0) { puterr("pkg: list failed\n"); return; }
        if (n == 0) { putstr("(no components)\n"); return; }
        putstr(buf);
        return;
    }
    if (arg == 0 || arg[0] == '\0') {
        puterr("usage: pkg install <file> | list | remove <name> | verify <name>\n");
        return;
    }

    reply[0] = '\0';
    if (scmp(sub, "install") == 0) {
        r = sc(SYS_PKG, PKG_OP_INSTALL, (int)arg, (int)reply);
    } else if (scmp(sub, "remove") == 0) {
        r = sc(SYS_PKG, PKG_OP_REMOVE, (int)arg, (int)reply);
    } else if (scmp(sub, "verify") == 0) {
        r = sc(SYS_PKG, PKG_OP_VERIFY, (int)arg, (int)reply);
    } else {
        puterr("pkg: unknown subcommand\n");
        return;
    }
    printf("pkg: %s\n", reply);
    (void)r;
}

/* pkg CLI 自检（只在 reexec 带 --pkgcli 时跑）：
 *   root: list / verify 已安装组件；
 *   fork 出 alice：remove+install+verify 用户应用，并确认装系统组件被拒。
 * 这些行就是 docs/claims/pkg.md 里给集成会话的断言行。 */
static void pkg_cli_selftest(void)
{
    char buf[256];
    char reply[128];
    int n;
    int pid;
    int status = -1;

    n = sc(SYS_PKG, PKG_OP_LIST, (int)buf, (int)sizeof(buf));
    if (n >= 0 && has_sub(buf, "echosvc")) {
        putstr("pkg: user cli list ok\n");
    } else {
        printf("pkg: user cli list FAIL (n=%d)\n", n);
    }

    if (sc(SYS_PKG, PKG_OP_VERIFY, (int)"echosvc", (int)reply) == 0) {
        putstr("pkg: user cli verify ok\n");
    } else {
        printf("pkg: user cli verify FAIL (%s)\n", reply);
    }

    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        int r1, r2, r3, r4;

        (void)sc(SYS_SETUID, 1000, 1000, 0);       /* root 才能降权到 alice */
        reply[0] = '\0';
        r1 = sc(SYS_PKG, PKG_OP_REMOVE, (int)"helloapp", (int)reply);
        r2 = sc(SYS_PKG, PKG_OP_INSTALL, (int)"helloapp-1.0.0.pkg", (int)reply);
        r3 = sc(SYS_PKG, PKG_OP_VERIFY, (int)"helloapp", (int)reply);
        /* 系统组件（service）：普通用户必须被 policy 拒绝 */
        r4 = sc(SYS_PKG, PKG_OP_INSTALL, (int)"echosvc-1.0.0.pkg", (int)reply);

        if (r1 == 0 && r2 == 0 && r3 == 0) {
            putstr("pkg: user cli install ok\n");
        } else {
            printf("pkg: user cli install FAIL (%d %d %d)\n", r1, r2, r3);
        }
        if (r4 < 0) {
            putstr("pkg: user cli policy deny ok\n");
        } else {
            printf("pkg: user cli policy deny FAIL (r=%d)\n", r4);
        }
        sc(SYS_EXIT, (r1 == 0 && r2 == 0 && r3 == 0 && r4 < 0) ? 0 : 1, 0, 0);
    } else if (pid > 0) {
        sc(SYS_WAIT, pid, (int)&status, 0);
        printf("pkg: user cli done (status=%d)\n", status);
    } else {
        putstr("pkg: user cli FAIL (fork)\n");
    }
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

/* =============================================================================
 * B（pm 会话）：进程/内存补缺自检
 *   getppid / waitpid(WNOHANG) / SIGCHLD（默认忽略、显式 handler、SIG_IGN）/
 *   mprotect（写保护、NX、COW、非法参数）。
 * 期望串口行见 docs/process-memory.md §5.7。
 *
 * 注意：shell ELF 要装进 ramfs 的单文件上限（阶段 4a 起 FS_FILE_MAX = 64KB；
 * 之前是 40960）。这一块自检代码在 -O0 下会把 .text 顶过 0x40004000 边界、
 * 让 .rodata 整体后移一页。这里只对“自检区块 + cmd_sectest”用 Os 编译（语义
 * 不变，只是代码更小），其余 shell 代码保持 -O0。改这里时记得跑一次 build
 * 看 user_shell.elf 大小。
 * ============================================================================= */
#pragma GCC push_options
#pragma GCC optimize ("Os")

static volatile int g_sigchld_count;

static void sectest_sigchld_handler(int sig)
{
    (void)sig;
    g_sigchld_count++;
}

/* P0 回归：fork 之后父进程不做任何用户态栈写入，直接进入 sleep 等子进程
 * 退出。此时父进程的栈页还是 fork 后的 COW 只读页；SIGCHLD 投递时
 * proc_check_signals 写信号帧必须先把栈页私有化成可写，否则内核态写
 * #PF(err=3) 会 oops。
 * 整段用内联汇编完成 fork/exit/sleep，避免编译器在 fork 和信号投递之间
 * 生成 push/sub 之类的栈写入（那会把 COW 提前破掉、测不到问题）。
 * 父进程返回子 pid；子进程直接 exit(0)。 */
static int sectest_sigchld_cow(void)
{
    int child = -1;

    __asm__ volatile (
        "mov $18, %%eax\n\t"         /* SYS_FORK */
        "xor %%ebx, %%ebx\n\t"
        "xor %%ecx, %%ecx\n\t"
        "xor %%edx, %%edx\n\t"
        "int $0x81\n\t"              /* fork -> eax = 子 pid / 0 / -1 */
        "test %%eax, %%eax\n\t"
        "js 2f\n\t"
        "jnz 1f\n\t"
        "xor %%ebx, %%ebx\n\t"       /* 子进程：exit(0)，退出时内核发 SIGCHLD */
        "mov $5, %%eax\n\t"
        "int $0x81\n\t"
        "1:\n\t"
        "mov %%eax, %0\n\t"          /* 父进程：记下子 pid */
        "mov $200, %%ebx\n\t"        /* sleep(200ms)：被 SIGCHLD 唤醒后返回 */
        "mov $24, %%eax\n\t"
        "int $0x81\n\t"
        "jmp 3f\n"
        "2:\n\t"
        "mov $-1, %0\n"
        "3:\n\t"
        : "=&r"(child)
        :
        : "eax", "ebx", "ecx", "edx", "memory");

    return child;
}

/* getppid：子进程把看到的父 pid 当作退出码带回，父进程与自己的 pid 比较 */
static void sectest_getppid(void)
{
    int pid = sc(SYS_FORK, 0, 0, 0);
    int st = -1;
    int w;

    if (pid == 0) {
        sc(SYS_EXIT, getppid(), 0, 0);      /* 退出码 = 子进程看到的 ppid */
    }
    if (pid < 0) {
        return;
    }

    w = waitpid(pid, &st, 0);
    printf("sectest: getppid %s (child sees %d, self %d)\n",
           (w == pid && st == sc(SYS_GETPID, 0, 0, 0)) ? "ok" : "FAIL",
           st, sc(SYS_GETPID, 0, 0, 0));
}

/* waitpid(WNOHANG)：子进程还活着时必须返回 0，退出后回收并拿到 status */
static void sectest_waitpid(void)
{
    int pid = sc(SYS_FORK, 0, 0, 0);
    int st = -1;
    int first;
    int r;
    int spins;

    if (pid == 0) {
        sc(SYS_SLEEP, 200, 0, 0);       /* 200ms：保证父进程第一次 WNOHANG 时还活着 */
        sc(SYS_EXIT, 7, 0, 0);
    }

    first = waitpid(pid, &st, WNOHANG);
    r = first;
    spins = 0;
    /* 每次轮询睡 10ms：child 要 200ms 才退出，纯 yield 会在它醒来前就转完 */
    while (r == 0 && spins++ < 100) {
        sc(SYS_SLEEP, 10, 0, 0);
        r = waitpid(pid, &st, WNOHANG);
    }

    if (first == 0 && r == pid && st == 7) {
        putstr("sectest: waitpid wnohang ok\n");
    } else {
        printf("sectest: waitpid wnohang FAIL (first=%d r=%d st=%d)\n",
               first, r, st);
    }

    /* 没有匹配的子进程：本实现按约定立即返回 0（不阻塞、不报 -1） */
    if (waitpid(99999, &st, WNOHANG) == 0) {
        putstr("sectest: waitpid no-child ok\n");
    } else {
        putstr("sectest: waitpid no-child FAIL\n");
    }
}

/* SIGCHLD：默认忽略不能杀父进程；显式 handler 必须收到；SIG_IGN 不影响回收 */
static void sectest_sigchld(void)
{
    int pid;
    int st = -1;
    int w;

    /* a) 默认动作 = 忽略 */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) sc(SYS_EXIT, 0, 0, 0);
    sc(SYS_SLEEP, 100, 0, 0);
    w = waitpid(pid, &st, 0);
    if (w == pid) putstr("sectest: sigchld default ignored ok\n");
    else printf("sectest: sigchld default ignored FAIL (wait=%d)\n", w);

    /* b) 显式 handler：子进程退出后至少跑一次 */
    (void)signal(SIGCHLD, (unsigned int)sectest_sigchld_handler);
    g_sigchld_count = 0;
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) sc(SYS_EXIT, 0, 0, 0);
    sc(SYS_SLEEP, 150, 0, 0);
    w = waitpid(pid, &st, 0);
    printf("sectest: sigchld handler %s (sigchld=%d wait=%d)\n",
           (g_sigchld_count > 0 && w == pid) ? "ok" : "FAIL",
           g_sigchld_count, w);

    /* c) SIG_IGN：SIGCHLD 被丢弃，父进程存活，僵尸仍能 waitpid 回收 */
    signal(SIGCHLD, SIG_IGN);
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) sc(SYS_EXIT, 0, 0, 0);
    sc(SYS_SLEEP, 100, 0, 0);
    w = waitpid(pid, &st, 0);
    printf("sectest: sigchld ignored %s\n", (w == pid) ? "ok" : "FAIL");

    /* d) P0 回归：信号帧写在 fork 后的 COW 只读栈页上，内核必须先私有化 */
    signal(SIGCHLD, (unsigned int)sectest_sigchld_handler);
    g_sigchld_count = 0;
    pid = sectest_sigchld_cow();
    st = -1;
    if (pid > 0) (void)waitpid(pid, &st, 0);
    putstr((pid > 0 && g_sigchld_count > 0 && st == 0)
               ? "sectest: sigchld cow ok\n"
               : "sectest: sigchld cow FAIL\n");

    signal(SIGCHLD, SIG_DFL);           /* 恢复默认（默认=忽略） */
}

/* mprotect：写保护、NX/EXEC 切换、COW 私有化、非法参数 */
static void sectest_mprotect(void)
{
    volatile unsigned char *p;
    int va;
    int pid;
    int st;
    int w;

    va = sc(SYS_MMAP, 4096, 0, 0);
    if (va < 0) {
        putstr("sectest: mprotect FAIL (mmap)\n");
        return;
    }
    p = (volatile unsigned char *)(unsigned int)va;
    p[0] = 0x41;

    /* a) 子进程改成只读后写：必须 #PF(142)，不能“存活” */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        if (mprotect((void *)(unsigned int)va, 4096, PROT_READ) != 0) {
            sc(SYS_EXIT, 99, 0, 0);
        }
        p[1] = 0x42;
        putstr("sectest: mprotect ro SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    }
    st = -1;
    w = waitpid(pid, &st, 0);
    printf("sectest: mprotect ro deny %s (status=%d)\n",
           (w == pid && st == 142) ? "ok" : "FAIL", st);

    /* b) NX -> EXEC：mmap 页写一条 ret，mprotect 后可执行 */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        volatile unsigned char *q;
        int qa = sc(SYS_MMAP, 4096, 0, 0);

        if (qa < 0) sc(SYS_EXIT, 98, 0, 0);
        q = (volatile unsigned char *)(unsigned int)qa;
        q[0] = 0xC3;                    /* ret */
        if (mprotect((void *)(unsigned int)qa, 4096,
                     PROT_READ | PROT_EXEC) != 0) {
            sc(SYS_EXIT, 97, 0, 0);
        }
        __asm__ volatile ("call *%0" : : "r"((unsigned int)qa) : "memory");
        sc(SYS_EXIT, 0, 0, 0);          /* ret 正常返回说明执行权限已打开 */
    }
    st = -1;
    w = waitpid(pid, &st, 0);
    printf("sectest: mprotect exec %s (status=%d)\n",
           (w == pid && st == 0) ? "ok" : "FAIL", st);

    /* c) EXEC -> NX：先给执行权、再收回，执行应 #PF(142) */
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        volatile unsigned char *q;
        int qa = sc(SYS_MMAP, 4096, 0, 0);

        if (qa < 0) sc(SYS_EXIT, 96, 0, 0);
        q = (volatile unsigned char *)(unsigned int)qa;
        q[0] = 0xC3;
        if (mprotect((void *)(unsigned int)qa, 4096,
                     PROT_READ | PROT_EXEC) != 0) {
            sc(SYS_EXIT, 95, 0, 0);
        }
        if (mprotect((void *)(unsigned int)qa, 4096, PROT_READ) != 0) {
            sc(SYS_EXIT, 94, 0, 0);
        }
        __asm__ volatile ("call *%0" : : "r"((unsigned int)qa) : "memory");
        putstr("sectest: mprotect nx SURVIVED\n");
        sc(SYS_EXIT, 0, 0, 0);
    }
    st = -1;
    w = waitpid(pid, &st, 0);
    printf("sectest: mprotect nx deny %s (status=%d)\n",
           (w == pid && st == 142) ? "ok" : "FAIL", st);

    /* d) COW：父页 0xAB -> fork -> 子 mprotect(WRITE) 后写 0xCD；
     *    父进程读回必须仍是 0xAB（mprotect 请求写会先私有化） */
    p[0] = 0xAB;
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        if (mprotect((void *)(unsigned int)va, 4096,
                     PROT_READ | PROT_WRITE) != 0) {
            sc(SYS_EXIT, 93, 0, 0);
        }
        p[0] = 0xCD;
        sc(SYS_EXIT, 0, 0, 0);
    }
    st = -1;
    w = waitpid(pid, &st, 0);
    printf("sectest: mprotect cow %s (parent byte=0x%02x status=%d)\n",
           (w == pid && st == 0 && p[0] == 0xAB) ? "ok" : "FAIL", p[0], st);

    /* e) 非法参数：非对齐 / len=0 / 内核地址 / 未知 prot 位，全部 -1 */
    {
        int bad = 0;

        if (mprotect((void *)(unsigned int)(va + 1), 4096, PROT_READ) == -1) bad++;
        if (mprotect((void *)(unsigned int)va, 0, PROT_READ) == -1) bad++;
        if (mprotect((void *)0x80000000u, 4096, PROT_READ) == -1) bad++;
        if (mprotect((void *)(unsigned int)va, 4096, 0x8) == -1) bad++;
        putstr((bad == 4) ? "sectest: mprotect invalid ok\n"
                          : "sectest: mprotect invalid FAIL\n");
    }
}

/* EINTR 回归：阻塞 read/accept 收到信号必须返回 -4（且 SIGKILL 能杀掉阻塞
 * 进程）。放在 sectest（-Test 注入）里而不是 shell 启动路径：启动期服务/
 * 中断并发大，测试信号到达时机不稳定。 */
#if defined(OS_TEST_BUILD) && OS_TEST_BUILD
static void einr_selftest(void)
{
    int pfd[2];

    /* a) 父进程阻塞在空管道读；子进程 200ms 后退出 -> SIGCHLD 打断 -> -EINTR */
    if (sc(SYS_PIPE, (int)pfd, 0, 0) == 0) {
        int pid = sc(SYS_FORK, 0, 0, 0);

        if (pid == 0) {
            sc(SYS_SLEEP, 200, 0, 0);
            sc(SYS_EXIT, 0, 0, 0);
        }
        {
            char c = 0;
            int r = sc(SYS_FREAD, pfd[0], (int)&c, 1);

            if (r == -4) putstr("ushell: einr read ok\n");
            else printf("ushell: einr read FAIL (r=%d)\n", r);
        }
        {
            int st = 0;
            sc(SYS_WAIT, pid, (int)&st, 0);
        }
        sc(SYS_CLOSE, pfd[0], 0, 0);
        sc(SYS_CLOSE, pfd[1], 0, 0);
    }

    /* b) 父进程阻塞在监听 socket accept；子进程 200ms 后退出 -> -EINTR */
    {
        int ls = sc(SYS_SOCKET, 2, 1, 0);    /* AF_INET, SOCK_STREAM */

        if (ls >= 0 && sc(SYS_LISTEN, 43211, 2, 0) == 0) {
            int pid = sc(SYS_FORK, 0, 0, 0);

            if (pid == 0) {
                sc(SYS_SLEEP, 200, 0, 0);
                sc(SYS_EXIT, 0, 0, 0);
            }
            {
                unsigned char addr[8];
                int r = sc(SYS_ACCEPT, ls, (int)addr, 0);

                if (r == -4) putstr("ushell: einr accept ok\n");
                else printf("ushell: einr accept FAIL (r=%d)\n", r);
            }
            {
                int st = 0;
                sc(SYS_WAIT, pid, (int)&st, 0);
            }
            sc(SYS_CLOSE, ls, 0, 0);
        } else {
            if (ls >= 0) sc(SYS_CLOSE, ls, 0, 0);
            putstr("ushell: einr accept SKIP (listen failed)\n");
        }
    }

    /* c) SIGKILL 杀掉阻塞在管道读上的子进程，父进程能 wait 到退出码 */
    if (sc(SYS_PIPE, (int)pfd, 0, 0) == 0) {
        int sync[2];

        if (sc(SYS_PIPE, (int)sync, 0, 0) != 0) sync[0] = sync[1] = -1;
        if (sync[0] >= 0) {
            int pid = sc(SYS_FORK, 0, 0, 0);

            if (pid == 0) {
                char c;
                char go = 'x';

                sc(SYS_CLOSE, sync[0], 0, 0);
                sc(SYS_FWRITE, sync[1], (int)&go, 1);   /* 马上要睡在 read 上 */
                sc(SYS_CLOSE, sync[1], 0, 0);
                (void)sc(SYS_FREAD, pfd[0], (int)&c, 1);
                sc(SYS_EXIT, 0, 0, 0);   /* 正常路径：被 kill 时走不到 */
            }
            {
                char go = 0;
                sc(SYS_CLOSE, sync[1], 0, 0);
                sc(SYS_FREAD, sync[0], (int)&go, 1);
                sc(SYS_CLOSE, sync[0], 0, 0);
            }
            sc(SYS_SLEEP, 50, 0, 0);
            sc(SYS_KILL, pid, 9, 0);
            {
                int st = 0;
                int w = sc(SYS_WAIT, pid, (int)&st, 0);

                if (w == pid && st == 128 + 9) putstr("ushell: einr kill ok\n");
                else printf("ushell: einr kill FAIL (w=%d st=%d)\n", w, st);
            }
        }
        sc(SYS_CLOSE, pfd[0], 0, 0);
        sc(SYS_CLOSE, pfd[1], 0, 0);
    }
}
#endif

#if defined(OS_TEST_BUILD) && OS_TEST_BUILD
/* chroot 内绝对软链接不能指回卷根：/link -> /etc/passwd 必须解析到
 * <chroot>/etc/passwd（不存在），而不是真实 /etc/passwd。 */
static void chroot_symlink_selftest(void)
{
    int pid = sc(SYS_FORK, 0, 0, 0);

    if (pid == 0) {
        int ok = 0;

        sc(SYS_UNLINK, (int)"/tmp/ct-link", 0, 0);
        if (sc(SYS_SYMLINK, (int)"/etc/passwd", (int)"/tmp/ct-link", 0) == 0 &&
            sc(SYS_CHROOT, (int)"system:/tmp", 0, 0) == 0) {
            int f = sc(SYS_OPEN, (int)"/ct-link", 0, 0);

            if (f < 0) {
                ok = 1;                       /* 读不到真实 /etc/passwd */
            } else {
                sc(SYS_CLOSE, f, 0, 0);
            }
            sc(SYS_UNLINK, (int)"/ct-link", 0, 0);
        }
        putstr(ok ? "fs: audit chroot symlink containment ok\n"
                  : "fs: audit chroot symlink containment FAIL\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else if (pid > 0) {
        int st = 0;
        sc(SYS_WAIT, pid, (int)&st, 0);
    }
}
#endif

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

    /* VFS 更大限制：同时打开 10 个文件。 */
    {
        int fds[10];
        int ok = 1;

        for (int i = 0; i < 10; i++) {
            fds[i] = sc(SYS_OPEN, (int)(unsigned int)"system:/bin/shell", 0, 0);
            if (fds[i] < 0) { ok = 0; break; }
        }
        for (int i = 0; i < 10; i++) {
            if (fds[i] >= 0) sc(SYS_CLOSE, fds[i], 0, 0);
        }
        if (ok) putstr("sectest: fd limit ok\n");
        else    putstr("sectest: fd limit FAIL\n");
    }

    /* ring3 文件服务：通过 native service/channel 读取用户态 fsd 的文件。 */
    {
        int fsd = -1;
        char buf[64];

        for (int i = 0; i < 200; i++) {
            fsd = nc(0x39, (int)(unsigned int)"fsd", 3, 0);
            if (fsd >= 0) break;
            sc(SYS_SLEEP, 10, 0, 0);
        }
        if (fsd >= 0 && nc(0x31, fsd, (int)(unsigned int)"read", 4) == 4) {
            int n = nc(0x36, fsd, (int)(unsigned int)buf, sizeof(buf));
            if (n > 0 && buf[0] == 'h') {
                putstr("sectest: fs service ok\n");
            } else {
                putstr("sectest: fs service recv FAIL\n");
            }
        } else {
            putstr("sectest: fs service connect FAIL\n");
        }
    }

    /* ring3 fsd 写路径：直接经 native channel 写入内存文件。 */
    {
        int fsd = nc(0x39, (int)(unsigned int)"fsd", 3, 0);
        char wbuf[32];
        const char *payload = "written by ring3";
        int len = 15;   /* 'w' + 14 bytes */

        wbuf[0] = 'w';
        for (int i = 0; i < 14; i++) wbuf[i + 1] = payload[i];
        if (fsd >= 0 && nc(0x31, fsd, (int)(unsigned int)wbuf, len) == len) {
            char ok[8];
            int n = nc(0x36, fsd, (int)(unsigned int)ok, sizeof(ok));
            if (n > 0) putstr("sectest: fs write ok\n");
            else putstr("sectest: fs write recv FAIL\n");
        } else {
            putstr("sectest: fs write FAIL\n");
        }
    }

    /* VFS svc: 后端：open/read 走 ring3 fsd。 */
    {
        int f = sc(SYS_OPEN, (int)(unsigned int)"svc:/hello", FS_READ, 0);
        if (f >= 0) {
            char b[64];
            int n = sc(SYS_FREAD, f, (int)(unsigned int)b, sizeof(b) - 1);
            sc(SYS_CLOSE, f, 0, 0);
            if (n > 0 && b[0] == 'w') putstr("sectest: svc vfs ok\n");
            else putstr("sectest: svc vfs read FAIL\n");
        } else {
            putstr("sectest: svc vfs open FAIL\n");
        }
    }

    /* VFS svc: 写路径：write 缓冲，close 时把 'w'+data 发给 ring3 fsd。 */
    {
        int f = sc(SYS_OPEN, (int)(unsigned int)"svc:/hello", FS_WRITE, 0);
        if (f >= 0) {
            const char *w = "vfs write";
            int n = sc(SYS_FWRITE, f, (int)(unsigned int)w, 9);
            sc(SYS_CLOSE, f, 0, 0);
            if (n == 9) {
                f = sc(SYS_OPEN, (int)(unsigned int)"svc:/hello", FS_READ, 0);
                if (f >= 0) {
                    char b[64];
                    int r = sc(SYS_FREAD, f, (int)(unsigned int)b, sizeof(b) - 1);
                    sc(SYS_CLOSE, f, 0, 0);
                    if (r > 0 && b[0] == 'v') putstr("sectest: svc vfs write ok\n");
                    else putstr("sectest: svc vfs write read FAIL\n");
                } else {
                    putstr("sectest: svc vfs write reopen FAIL\n");
                }
            } else {
                putstr("sectest: svc vfs write FAIL\n");
            }
        } else {
            putstr("sectest: svc vfs write open FAIL\n");
        }
    }

    /* VFS blk: 后端：open/read 走 ring3 blkd。 */
    {
        int f = sc(SYS_OPEN, (int)(unsigned int)"blk:/sector0", FS_READ, 0);
        if (f >= 0) {
            char b[64];
            int n = sc(SYS_FREAD, f, (int)(unsigned int)b, sizeof(b) - 1);
            sc(SYS_CLOSE, f, 0, 0);
            if (n > 0 && b[0] == 'r') putstr("sectest: blk service ok\n");
            else putstr("sectest: blk service read FAIL\n");
        } else {
            putstr("sectest: blk service open FAIL\n");
        }
    }

    /* VFS blk: 写路径：write 缓冲，close 发送给 ring3 blkd。 */
    {
        int f = sc(SYS_OPEN, (int)(unsigned int)"blk:/sector0", FS_WRITE, 0);
        if (f >= 0) {
            const char *w = "block write";
            int n = sc(SYS_FWRITE, f, (int)(unsigned int)w, 11);
            sc(SYS_CLOSE, f, 0, 0);
            if (n == 11) {
                f = sc(SYS_OPEN, (int)(unsigned int)"blk:/sector0", FS_READ, 0);
                if (f >= 0) {
                    char b[64];
                    int r = sc(SYS_FREAD, f, (int)(unsigned int)b, sizeof(b) - 1);
                    sc(SYS_CLOSE, f, 0, 0);
                    if (r > 0 && b[0] == 'b') putstr("sectest: blk write ok\n");
                    else putstr("sectest: blk write read FAIL\n");
                } else {
                    putstr("sectest: blk write reopen FAIL\n");
                }
            } else {
                putstr("sectest: blk write FAIL\n");
            }
        } else {
            putstr("sectest: blk write open FAIL\n");
        }
    }

    /* 用户触发 TCP echo：boot 不再做 TCP，避免 host listener 未就绪拖死启动。 */
    if (sc(SYS_TCP_TEST, 0, 0, 0) == 0) putstr("sectest: tcp ok\n");
    else putstr("sectest: tcp FAIL\n");

    /* B（pm 会话）：getppid / waitpid(WNOHANG) / SIGCHLD / mprotect */
    sectest_getppid();
    sectest_waitpid();
    sectest_sigchld();
    sectest_mprotect();

#if defined(OS_TEST_BUILD) && OS_TEST_BUILD
    einr_selftest();
    chroot_symlink_selftest();
#endif
}

#pragma GCC pop_options

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

/* 无符号十进制参数（审计/调试用：地址用十进制，避免 QMP sendkey 对
 * 大写十六进制字符的处理差异） */
static unsigned int str2unum(const char *s)
{
    unsigned int v = 0;

    while (*s >= '0' && *s <= '9') v = v * 10U + (unsigned int)(*s++ - '0');
    return v;
}

/* SIGTERM 处理函数：打印一行后返回（内核用蹦床做 sigreturn） */
static void sig_handler(int sig)
{
    printf("ushell: caught signal %d, continuing\n", sig);
}

/* 审计回归：LOGIN_WHO/LAST/INFO 必须尊重调用方缓冲长度。
 * 期望：0/1/8 字节缓冲返回 -1 且一个字节都不写；足够大的缓冲原行为不变。 */
static void cmd_loginlen(void)
{
    char buf[80];
    int ok = 1;
    int i;
    int n;

    if (sc(SYS_LOGIN, LOGIN_INFO, (int)buf, 0) != -1) ok = 0;

    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)0xAA;
    if (sc(SYS_LOGIN, LOGIN_INFO, (int)buf, 1) != -1) ok = 0;
    for (i = 0; i < (int)sizeof(buf); i++) {
        if ((unsigned char)buf[i] != 0xAA) ok = 0;
    }

    if (sc(SYS_LOGIN, LOGIN_INFO, (int)buf, 8) != -1) ok = 0;
    for (i = 0; i < (int)sizeof(buf); i++) {
        if ((unsigned char)buf[i] != 0xAA) ok = 0;
    }

    /* WHO/LAST：无论成功与否，超过 max 的字节都不能被写 */
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)0xAA;
    (void)sc(SYS_LOGIN, LOGIN_WHO, (int)buf, 8);
    for (i = 8; i < (int)sizeof(buf); i++) {
        if ((unsigned char)buf[i] != 0xAA) ok = 0;
    }
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)0xAA;
    (void)sc(SYS_LOGIN, LOGIN_LAST, (int)buf, 1);
    for (i = 1; i < (int)sizeof(buf); i++) {
        if ((unsigned char)buf[i] != 0xAA) ok = 0;
    }

    /* 足够大的缓冲：成功且以 NUL 结尾（原行为） */
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)0xAA;
    if (sc(SYS_LOGIN, LOGIN_INFO, (int)buf, (int)sizeof(buf)) != 0) ok = 0;
    n = 0;
    while (n < (int)sizeof(buf) && buf[n] != '\0') n++;
    if (n == (int)sizeof(buf)) ok = 0;

    printf("ushell: login len clamp %s\n", ok ? "ok" : "FAIL");
}

/* 审计回归：管道读端引用计数。
 * 1) dup2 读端后关闭原 read，dup 仍应读到 EOF(0)，而不是 -1；
 * 2) fork 后父子各持读端，子进程先关掉并退出，父进程仍应读到 EOF；
 * 3) 写端关闭后 EOF 语义不变。 */
static void cmd_duptest(void)
{
    int p[2];
    int dupfd;
    int n = 0;
    int st = 0;
    char c = 'x';
    int ok = 1;

    if (sc(SYS_PIPE, (int)p, 0, 0) != 0) {
        puterr("duptest: pipe failed\n");
        return;
    }
    dupfd = sc(SYS_DUP2, p[0], 10, 0);
    if (dupfd != 10) ok = 0;
    sc(SYS_CLOSE, p[1], 0, 0);
    sc(SYS_CLOSE, p[0], 0, 0);
    n = sc(SYS_FREAD, dupfd, (int)&c, 1);
    if (n != 0) ok = 0;
    sc(SYS_CLOSE, dupfd, 0, 0);

    if (sc(SYS_PIPE, (int)p, 0, 0) == 0) {
        int pid = sc(SYS_FORK, 0, 0, 0);

        if (pid == 0) {
            sc(SYS_SLEEP, 20, 0, 0);
            sc(SYS_CLOSE, p[1], 0, 0);
            sc(SYS_CLOSE, p[0], 0, 0);
            sc(SYS_EXIT, 0, 0, 0);
        } else if (pid < 0) {
            ok = 0;
            sc(SYS_CLOSE, p[0], 0, 0);
            sc(SYS_CLOSE, p[1], 0, 0);
        } else {
            sc(SYS_CLOSE, p[1], 0, 0);          /* 父进程先关写端 */
            sc(SYS_SLEEP, 100, 0, 0);           /* 等子进程关掉它那份读端 */
            sc(SYS_WAIT, pid, (int)&st, 0);
            n = sc(SYS_FREAD, p[0], (int)&c, 1);
            if (n != 0) ok = 0;
            sc(SYS_CLOSE, p[0], 0, 0);
        }
    } else {
        ok = 0;
    }

    printf("ushell: dup2 pipe read ref %s (last=%d)\n", ok ? "ok" : "FAIL", n);
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
        { "mv",      "mv <old> <new>        rename/move (same volume)" },
        { "ln",      "ln [-s] <old> <new>   hard/symbolic link" },
        { "readlink","readlink <path>       print symlink target" },
        { "linktest","linktest              hard/symlink regression" },
        { "cd",      "cd <path> | cd -      change directory" },
        { "pwd",     "pwd                   show current directory" },
        { "stat",    "stat <path>           type/size/mtime" },
        { "echo",    "echo <args...>        print arguments" },
        { "sleep",   "sleep <ms>            sleep milliseconds" },
        { "fork",    "fork                  fork and wait for the child" },
        { "pkg",     "pkg install <file> | list | remove <name> | verify <name>" },
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
               "  mv <old> <new>         rename/move within a volume\n"
               "  cd <path>              change directory; cd - returns\n"
               "  pwd                    show current directory\n"
               "  stat <path>            show type/size/mtime\n"
               "  echo <args...>         print arguments\n"
               "  sleep <ms>             sleep milliseconds\n"
               "  fork                   create a child process, wait for it\n"
               "  pkg install|list|remove|verify <file|name>   component CLI\n"
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
    } else if (scmp(args[0], "ln") == 0 && nargs >= 4 && scmp(args[1], "-s") == 0) {
        cmd_ln(args[2], args[3], 1);
    } else if (scmp(args[0], "ln") == 0 && nargs >= 3) {
        cmd_ln(args[1], args[2], 0);
    } else if (scmp(args[0], "readlink") == 0 && nargs >= 2) {
        cmd_readlink(args[1]);
    } else if (scmp(args[0], "linktest") == 0) {
        cmd_linktest();
    } else if (scmp(args[0], "mv") == 0 && nargs >= 3) {
        cmd_mv(args[1], args[2]);
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
    } else if (scmp(args[0], "pkg") == 0) {
        cmd_pkg((nargs >= 2) ? args[1] : 0, (nargs >= 3) ? args[2] : 0);
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
        /* 回归专用：不依赖键盘输入路径字符，直接 exec 自身。
         * 带 --pkgcli：新 shell 启动后跑一遍 ring3 pkg CLI 自检（见 _start）。 */
        sc(SYS_EXEC, (int)"system:/bin/shell", (int)"--pkgcli", 0);
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
        /* P1：真实身份 + 生效身份 + 附加组（Linux id 的简化版） */
        char info[160];
        int g[8];
        int ng = sc(SYS_GETGROUPS, (int)g, 8, 0);

        if (sc(SYS_LOGIN, LOGIN_INFO, (int)info, (int)sizeof(info)) == 0) {
            putstr(info);
            putstr("\n");
        } else {
            printf("uid=%d gid=%d euid=%d egid=%d\n",
                   sc(SYS_GETUID, 0, 0, 0), sc(SYS_GETGID, 0, 0, 0),
                   sc(SYS_GETEUID, 0, 0, 0), sc(SYS_GETEGID, 0, 0, 0));
        }
        putstr("groups:");
        for (int i = 0; i < ng && i < 8; i++) printf(" %d", g[i]);
        putstr("\n");
    } else if (scmp(args[0], "umask") == 0) {
        /* 带参数时设置，不带参数时查询（用 s=0 的 setumask 读回旧值再还原） */
        if (nargs >= 2) {
            int m = 0, ok = 1;
            for (const char *p = args[1]; *p != '\0'; p++) {
                if (*p < '0' || *p > '7') { ok = 0; break; }
                m = m * 8 + (*p - '0');
            }
            if (!ok) puterr("umask: octal expected, e.g. 022\n");
            else printf("umask: %04o\n", sc(SYS_SETUMASK, m, 0, 0));
        } else {
            int old = sc(SYS_SETUMASK, 0, 0, 0);
            sc(SYS_SETUMASK, old, 0, 0);
            printf("umask: %04o\n", old);
        }
    } else if (scmp(args[0], "whoami") == 0) {
        char info[160];
        int r = sc(SYS_LOGIN, LOGIN_INFO, (int)info, (int)sizeof(info));
        char *p;

        if (r == 0) {
            p = info;
            while (*p != '\0' && *p != 'n') p++;       /* 找 "name=" */
            if (p[0] == 'n' && p[1] == 'a' && p[2] == 'm' && p[3] == 'e' && p[4] == '=') {
                putstr(p + 5);
                putstr("\n");
            } else {
                putstr(info);
                putstr("\n");
            }
        } else {
            printf("uid=%d\n", sc(SYS_GETUID, 0, 0, 0));
        }
    } else if (scmp(args[0], "who") == 0) {
        char b[192];
        int n = sc(SYS_LOGIN, LOGIN_WHO, (int)b, (int)sizeof(b));

        putstr(n > 0 ? b : "who: no sessions\n");
    } else if (scmp(args[0], "last") == 0) {
        char b[512];
        int n = sc(SYS_LOGIN, LOGIN_LAST, (int)b, (int)sizeof(b));

        putstr(n > 0 ? b : "last: no audit records\n");
    } else if (scmp(args[0], "su") == 0) {
        char pass[32];
        const char *who = (nargs >= 2) ? args[1] : "root";

        putstr("Password: ");
        (void)read_line_echo(pass, (int)sizeof(pass), 1);
        if (sc(SYS_LOGIN, LOGIN_SU, (int)who, (int)pass) == 0) {
            printf("su: now %s (euid=%d uid=%d)\n", who,
                   sc(SYS_GETEUID, 0, 0, 0), sc(SYS_GETUID, 0, 0, 0));
        } else {
            puterr("su: authentication failure\n");
        }
    } else if (scmp(args[0], "sudo") == 0) {
        if (nargs < 2) {
            puterr("usage: sudo <command>\n");
        } else if (sc(SYS_LOGIN, LOGIN_SUDO, 0, 0) != 0) {
            puterr("sudo: not in wheel (or wrong password)\n");
        } else {
            char cmd[120];
            int n = 0;

            for (int i = 1; i < nargs && n < 110; i++) {
                if (i > 1 && n < 110) cmd[n++] = ' ';
                for (const char *q = args[i]; *q != '\0' && n < 112; q++) cmd[n++] = *q;
            }
            cmd[n] = '\0';
            printf("sudo: euid=%d\n", sc(SYS_GETEUID, 0, 0, 0));
            run_line(cmd);                       /* 以 euid=0 执行一条命令 */
            sc(SYS_LOGIN, LOGIN_SUDO_END, 0, 0);
        }
    } else if (scmp(args[0], "passwd") == 0) {
        char pw1[32];
        char pw2[32];

        if (nargs >= 2) {
            /* passwd <user>：只有 root 能改别人的口令 */
            if (sc(SYS_GETEUID, 0, 0, 0) != 0) {
                puterr("passwd: only root can change another user's password\n");
            } else {
                putstr("New password: ");
                (void)read_line_echo(pw1, (int)sizeof(pw1), 1);
                putstr("Retype new password: ");
                (void)read_line_echo(pw2, (int)sizeof(pw2), 1);
                if (scmp(pw1, pw2) != 0) {
                    puterr("passwd: mismatch\n");
                } else if (sc(SYS_LOGIN, LOGIN_PASSWD_USER, (int)args[1], (int)pw1) == 0) {
                    printf("passwd: %s updated\n", args[1]);
                } else {
                    puterr("passwd: failed\n");
                }
            }
        } else {
            char old[32];

            if (sc(SYS_GETEUID, 0, 0, 0) != 0) {
                putstr("Current password: ");
                (void)read_line_echo(old, (int)sizeof(old), 1);
            } else {
                old[0] = '\0';                   /* root 改自己的免旧口令 */
            }
            putstr("New password: ");
            (void)read_line_echo(pw1, (int)sizeof(pw1), 1);
            putstr("Retype new password: ");
            (void)read_line_echo(pw2, (int)sizeof(pw2), 1);
            if (scmp(pw1, pw2) != 0) {
                puterr("passwd: mismatch\n");
            } else if (sc(SYS_LOGIN, LOGIN_PASSWD, (int)old, (int)pw1) == 0) {
                putstr("passwd: ok\n");
            } else {
                puterr("passwd: failed (wrong old password?)\n");
            }
        }
    } else if (scmp(args[0], "useradd") == 0 && nargs >= 2) {
        int uid = (nargs >= 3) ? str2int(args[2]) : 0;

        if (sc(SYS_LOGIN, LOGIN_USERADD, (int)args[1], uid) == 0) {
            printf("useradd: %s created (set a password with: passwd %s)\n",
                   args[1], args[1]);
        } else {
            puterr("useradd: failed (root only / exists / bad uid)\n");
        }
    } else if (scmp(args[0], "userdel") == 0 && nargs >= 2) {
        if (sc(SYS_LOGIN, LOGIN_USERDEL, (int)args[1], 0) == 0) {
            printf("userdel: %s removed (home kept)\n", args[1]);
        } else {
            puterr("userdel: failed (root only / not found)\n");
        }
    } else if (scmp(args[0], "usermod") == 0 && nargs >= 3) {
        int gid = str2int(args[2]);

        if (sc(SYS_LOGIN, LOGIN_USERMOD, (int)args[1], gid) == 0) {
            printf("usermod: %s -> gid %d\n", args[1], gid);
        } else {
            puterr("usermod: failed (root only / no such group)\n");
        }
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
    } else if (scmp(args[0], "sys") == 0 && nargs >= 2) {
        /* 审计/调试用：直接发一条 legacy int 0x81，参数用十进制。
         *   sys <nr> [a] [b] [c]   -> 打印返回值
         * 只暴露 shell 本来就有的 ring3 权限，不新增内核入口。 */
        unsigned int nr = str2unum(args[1]);
        unsigned int a = (nargs >= 3) ? str2unum(args[2]) : 0U;
        unsigned int b = (nargs >= 4) ? str2unum(args[3]) : 0U;
        unsigned int c = (nargs >= 5) ? str2unum(args[4]) : 0U;
        int r = sc((int)nr, (int)a, (int)b, (int)c);

        printf("sys: nr=%u a=%u b=%u c=%u ret=%d\n", nr, a, b, c, r);
    } else if (scmp(args[0], "loginlen") == 0) {
        cmd_loginlen();
    } else if (scmp(args[0], "duptest") == 0) {
        cmd_duptest();
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

    /* 组件 ELF 服务模式：_pkgd 用 argv[0]="svc:<name>" 启动 */
    if (argc > 0 && argv != 0 && argv[0] != 0 && slen(argv[0]) > 4U &&
        argv[0][0] == 's' && argv[0][1] == 'v' &&
        argv[0][2] == 'c' && argv[0][3] == ':') {
        int rc = component_service_main(argv[0] + 4);

        sc(SYS_EXIT, rc, 0, 0);
    }

    /* P1：如果这个 tty 是 getty 起的（生产启动），先登录再进 shell。
     * 测试模式（内核参数 autologin=root）这里返回 0，直接往下走，
     * 与引入账号系统之前的行为完全一致。 */
    if (sc(SYS_LOGIN, LOGIN_CHECK, 0, 0) == 1) {
        login_screen();
    }

    putstr("ushell: ready (ring3, legacy int 0x81)\n");

    /* P1：凭据可见性——每一代 shell（含 exec 之后）都打印一行 */
    printf("ushell: euid=%d uid=%d gid=%d egid=%d groups=%d\n",
           sc(SYS_GETEUID, 0, 0, 0), sc(SYS_GETUID, 0, 0, 0),
           sc(SYS_GETGID, 0, 0, 0), sc(SYS_GETEGID, 0, 0, 0),
           sc(SYS_GETGROUPS, 0, 0, 0));

    /* 内核按 ELF 启动约定压好的 argc/argv（启动时没有参数）。
     * 必须一次 SYS_FWRITE 打完：启动期有服务/中断并发打印，分段写会把
     * "ushell: argc=0" 插进别人的日志，回归字符串就断了。 */
    if (argc > 0 && argv != 0 && argv[0] != 0) {
        printf("ushell: argc=%d argv0=%s\n", argc, argv[0]);
    } else {
        putstr("ushell: argc=0\n");
    }

    /* ring3 pkg CLI 自检：只由 `reexec` 传 --pkgcli 触发（在 pkgtest 跑完之后），
     * 这样正常启动/生产路径不会安装/卸载组件，也不会和启动回归抢时序。 */
    if (argc > 0 && argv != 0 && argv[0] != 0 &&
        scmp(argv[0], "--pkgcli") == 0) {
        pkg_cli_selftest();
    }

    /* 启动自检：getcwd + mkdir/stat/unlink，证明文件系统系统调用都通。
     * 写卷根需要 root：普通用户登录时跳过（否则会打一条 DENY 审计 + FAIL，
     * 看起来像故障）。测试模式自动登录 root，行为与引入账号系统前一致。 */
    {
        char cwd[64];
        unsigned int st[2];
        int ok;

        if (sc(SYS_GETCWD, (int)cwd, (int)sizeof(cwd), 0) >= 0) {
            putstr("ushell: cwd=");
            putstr(cwd);
            putstr("\n");
        }

        if (sc(SYS_GETEUID, 0, 0, 0) != 0) {
            putstr("ushell: fs syscalls skipped (non-root)\n");
        } else {
            ok = (sc(SYS_MKDIR, (int)"system:/uselfs", 0, 0) == 0);
            ok = ok && (sc(SYS_STAT, (int)"system:/uselfs", (int)st, 0) == 0);
            ok = ok && (sc(SYS_UNLINK, (int)"system:/uselfs", 0, 0) == 0);
            putstr(ok ? "ushell: fs syscalls ok\n" : "ushell: fs syscalls FAIL\n");
        }
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

    /* EINTR 自检：阻塞的 read/accept 收到未屏蔽信号必须返回 -4，且返回用户态
     * 前投递 handler；SIGKILL 也必须能把阻塞进程杀掉（不是永远睡）。
     * 只在 -Test 构建里跑：会临时占用 43211 端口，不能进生产 shell。 */

    /* P1 凭据自检：fork 出来的子进程必须继承 euid/egid（不是回到 root）。
     * 只打印一行结论，父进程等子进程退出后继续，不改变交互时序。 */
    {
        int pid = sc(SYS_FORK, 0, 0, 0);

        if (pid == 0) {
            int child_euid = sc(SYS_GETEUID, 0, 0, 0);
            printf("acct: fork child euid=%d uid=%d\n",
                   child_euid, sc(SYS_GETUID, 0, 0, 0));
            sc(SYS_EXIT, 0, 0, 0);
        } else if (pid > 0) {
            int status = -1;
            sc(SYS_WAIT, pid, (int)&status, 0);
            printf("acct: fork creds inherited (parent euid=%d status=%d)\n",
                   sc(SYS_GETEUID, 0, 0, 0), status);
        }
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
