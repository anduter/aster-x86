/*
 * ng WP2-6 host 单测：用户态 shell 的 tokenizer / 路径解析 / 重定向 /
 * 行编辑 / 每个内建命令 / 错误分支。
 *
 * 做法：shell.c 用 -DNG_HOST_TEST 编译，sh_sys() 换成这里的
 * sh_host_syscall()。后端不是 mock 字符串，而是真实的一整套 L1 服务：
 *   - syscall 20..27 -> VFS + ramfs（fake 页池，和 test_vfs/test_auth 同一套）；
 *   - syscall 31 -> auth_login（auth_init 种入 root/alice 与 /etc/passwd）；
 *   - syscall 32/33 -> log_read/log_write；
 *   - syscall 34/35 -> pkg_list/pkg_verify（pkg_init 种入 hello/1.0.0）；
 *   - syscall 0 -> 捕获到 g_out（模拟串口/控制台）；
 *   - syscall 30 -> 脚本化的按键队列（含 -EAGAIN、Ctrl-L/C/U/D）；
 *   - syscall 1 -> 记录退出码，回到 shell_main 的返回路径。
 *
 * 这样 host 单测跑的是和 ring3 完全相同的命令解析/执行代码，只是把内核换成了
 * 进程内的 L1 服务；QEMU 里则由 run-ng 注入同样的命令序列做端到端验证。
 */
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "fake_ng_env.h"
#include "log.h"
#include "pkg.h"
#include "ramfs.h"
#include "test_util.h"
#include "vfs.h"

/* ---- shell.c 的入口 / host 钩子 ---------------------------------------- */
int shell_main(void);
int sh_test_tokenize(char *line, int *argc, char *argv[8], char **redir);
int sh_test_resolve(const char *cwd, const char *in, char *out,
                    unsigned int cap);
const char *sh_test_errstr(long long e);
unsigned int sh_test_status(void);
void sh_test_cwd(char *out, unsigned int cap);
long long sh_host_syscall(long long nr, long long a1, long long a2, long long a3,
                          long long a4);

/* ---- 环境 ---------------------------------------------------------------- */
static struct ramfs g_fs;
static struct fs_env g_env;
static struct vfs_ctx g_ctx;

#define OUT_MAX 16384u
#define KEY_MAX 1024u

static char g_out[OUT_MAX];
static unsigned int g_out_len;
static int g_exit_code;
static int g_exited;
static unsigned int g_eagain_left;
static int g_eof_sent;
static unsigned int g_key_n;
static unsigned int g_key_i;

struct key_in {
    unsigned char flags;
    unsigned char ascii;
    unsigned char scancode;
};

static struct key_in g_keys[KEY_MAX];

static void out_reset(void)
{
    g_out_len = 0;
    g_out[0] = '\0';
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

static int out_count(const char *needle)
{
    const char *p = g_out;
    int n = 0;
    size_t len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

static void push_key(unsigned char flags, unsigned char ascii,
                     unsigned char scancode)
{
    if (g_key_n >= KEY_MAX) {
        return;
    }
    g_keys[g_key_n].flags = flags;
    g_keys[g_key_n].ascii = ascii;
    g_keys[g_key_n].scancode = scancode;
    g_key_n++;
}

/* 把一串字符变成按键：'\n' = Enter，'\b' = Backspace，'>' = Shift+'.'。 */
static void type_script(const char *s)
{
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s++;

        if (c == '\n') {
            push_key(0, 13, 0x1C);
        } else if (c == '\b') {
            push_key(0, 8, 0x0E);
        } else if (c == '>') {
            push_key(1, (unsigned char)'>', 0x34);
        } else if (c < 0x20u) {
            /* 控制字符：'\x03' = Ctrl-C、'\x04' = Ctrl-D、'\x0c' = Ctrl-L。 */
            push_key(0x04, (unsigned char)('a' + c - 1u), 0);
        } else {
            push_key(0, c, 0);
        }
    }
}

static void keys_reset(void)
{
    g_key_n = 0;
    g_key_i = 0;
    g_eagain_left = 0;
    g_eof_sent = 0;
}

static void proc_fake_reset(void);

static int env_reset(void)
{
    vfs_reset();
    fake_fs_reset();
    fake_fs_env(&g_env);
    ramfs_init(&g_fs, &g_env);
    if (vfs_mount("/", ramfs_ops(), &g_fs, ramfs_root(&g_fs)) != VFS_OK) {
        return 0;
    }
    vfs_ctx_init(&g_ctx);
    if (vfs_mkdir(&g_ctx, "/tmp", 0755) != VFS_OK) {
        return 0;
    }
    if (auth_init(&g_ctx) != AUTH_OK) {
        return 0;
    }
    if (pkg_init(&g_ctx) != PKG_OK) {
        return 0;
    }
    if (log_init() != 0) {
        return 0;
    }
    out_reset();
    keys_reset();
    proc_fake_reset();
    g_exit_code = -1;
    g_exited = 0;
    return 1;
}

/* 跑一条脚本（以 exit/Ctrl-D 结束），返回 shell 的退出码。 */
static int run(const char *script)
{
    out_reset();
    keys_reset();
    type_script(script);
    return shell_main();
}

static int read_file(const char *path, char *buf, unsigned int cap)
{
    int fd = vfs_open(&g_ctx, path, VFS_O_RDONLY, 0);
    int n;

    if (fd < 0) {
        return -1;
    }
    n = vfs_read(&g_ctx, (u32)fd, buf, cap);
    (void)vfs_close(&g_ctx, fd);
    return n;
}

static int file_exists(const char *path)
{
    struct vfs_stat st;

    return (vfs_stat(&g_ctx, path, &st) == VFS_OK) ? 1 : 0;
}

/* ---- WP3-0：spawn/wait/getpid 的 fake 后端 ------------------------------ */
static char g_spawn_path[64];
static char g_spawn_argv0[64];
static char g_spawn_argv1[64];
static int g_spawn_argc;
static int g_spawn_calls;
static u32 g_spawn_pid;
static u32 g_wait_pid;
static int g_wait_status;
static int g_wait_calls;

static void proc_fake_reset(void)
{
    g_spawn_path[0] = '\0';
    g_spawn_argv0[0] = '\0';
    g_spawn_argv1[0] = '\0';
    g_spawn_argc = 0;
    g_spawn_calls = 0;
    g_spawn_pid = 21;
    g_wait_pid = 0;
    g_wait_status = 0;
    g_wait_calls = 0;
}

static void copy_str(char *dst, const char *src, int cap)
{
    int i = 0;

    while (src != 0 && src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
/* ---- host syscall 后端 --------------------------------------------------- */
long long sh_host_syscall(long long nr, long long a1, long long a2, long long a3,
                          long long a4)
{
    (void)a4;
    switch (nr) {
    case 0: { /* write(buf,len) -> 捕获输出 */
        const char *b = (const char *)(void *)a1;
        unsigned int n = (unsigned int)a2;
        unsigned int i;

        for (i = 0; i < n; i++) {
            if (g_out_len + 1u >= OUT_MAX) {
                break;
            }
            g_out[g_out_len++] = b[i];
        }
        g_out[g_out_len] = '\0';
        return (long long)n;
    }
    case 1: /* exit(code) */
        g_exited = 1;
        g_exit_code = (int)a1;
        return 0;
    case 20:
        return vfs_open(&g_ctx, (const char *)(void *)a1, (u32)a2, (u32)a3);
    case 21:
        return vfs_read(&g_ctx, (int)a1, (void *)a2, (u64)a3);
    case 22:
        return vfs_write(&g_ctx, (int)a1, (const void *)a2, (u64)a3);
    case 23:
        return vfs_close(&g_ctx, (int)a1);
    case 24:
        return vfs_stat(&g_ctx, (const char *)(void *)a1, (void *)a2);
    case 25:
        return vfs_listdir(&g_ctx, (const char *)(void *)a1, (void *)a2,
                           (u32)a3);
    case 26:
        return vfs_mkdir(&g_ctx, (const char *)(void *)a1, (u32)a2);
    case 27:
        return vfs_unlink(&g_ctx, (const char *)(void *)a1);
    case 30: { /* readkey：先 EAGAIN，再脚本键，最后 EOF(Ctrl-D) */
        if (g_eagain_left > 0) {
            g_eagain_left--;
            return -11;
        }
        if (g_key_i < g_key_n) {
            struct key_in k = g_keys[g_key_i++];
            return ((long long)k.flags << 24) | ((long long)k.ascii << 16) |
                   (long long)k.scancode;
        }
        if (!g_eof_sent) {
            g_eof_sent = 1;
            return ((long long)0x04 << 24) | ((long long)'d' << 16) | 0x20;
        }
        return -11;
    }
    case 31:
        return auth_login((const char *)(void *)a1, (const char *)(void *)a2);
    case 32:
        return (long long)log_read((char *)(void *)a1, (u32)a2);
    case 33: {
        int rc = log_write((const char *)(void *)a1, (const char *)(void *)a2);
        return (rc < 0) ? (long long)rc : 0;
    }
    case 40: { /* spawn(path, argv, &pid) */
        const char *path = (const char *)(void *)a1;
        char *const *argv = (char *const *)(usize)a2;

        g_spawn_calls++;
        copy_str(g_spawn_path, path, 64);
        g_spawn_argc = 0;
        if (argv != NULL) {
            while (g_spawn_argc < 8 && argv[g_spawn_argc] != NULL) {
                if (g_spawn_argc == 0) {
                    copy_str(g_spawn_argv0, argv[0], 64);
                } else if (g_spawn_argc == 1) {
                    copy_str(g_spawn_argv1, argv[1], 64);
                }
                g_spawn_argc++;
            }
        }
        *(u32 *)(void *)a3 = g_spawn_pid;
        return 0;
    }
    case 41: { /* wait(pid, &status) */
        int *st = (int *)(void *)a2;

        g_wait_calls++;
        g_wait_pid = (u32)a1;
        if (st != NULL) {
            *st = g_wait_status;
        }
        return 0;
    }
    case 42:
        return 8;
    case 44:
        return 1;
    case 34:
        return pkg_list((char *)(void *)a1, (u32)a2);
    case 35:
        return pkg_verify((const char *)(void *)a1);
    default:
        return -1;
    }
}

/* ========================================================================== */
/* 1. tokenizer                                                              */
/* ========================================================================== */

static void t_tokenize(void)
{
    char buf[512];
    char *argv[8];
    char *redir;
    int argc;

    strcpy(buf, "ls /");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 2);
    CHECK(strcmp(argv[0], "ls") == 0);
    CHECK(strcmp(argv[1], "/") == 0);
    CHECK(redir == NULL);

    strcpy(buf, "  echo   hi   there\t");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 3);
    CHECK(strcmp(argv[0], "echo") == 0);
    CHECK(strcmp(argv[1], "hi") == 0);
    CHECK(strcmp(argv[2], "there") == 0);

    strcpy(buf, "echo hi > /tmp/s.txt");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 2);
    CHECK(redir != NULL && strcmp(redir, "/tmp/s.txt") == 0);

    strcpy(buf, "echo hi >/tmp/s.txt");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 2);
    CHECK(redir != NULL && strcmp(redir, "/tmp/s.txt") == 0);

    strcpy(buf, "> /tmp/only");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -3);

    strcpy(buf, "echo hi >");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -3);

    strcpy(buf, "echo a > b > c");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -4);

    strcpy(buf, "echo a >> b");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -4);

    strcpy(buf, "");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -2);
    strcpy(buf, "   \t");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -2);

    strcpy(buf, "a b c d e f g h");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 8);
    strcpy(buf, "a b c d e f g h i");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == -1);

    strcpy(buf, "\tls\t/\t");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 2);

    /* 不做引号/管道/`a>b` 分词：一个词就是原样，重定向必须独立成 token。 */
    strcpy(buf, "echo a>b");
    CHECK(sh_test_tokenize(buf, &argc, argv, &redir) == 0);
    CHECK(argc == 2);
    CHECK(strcmp(argv[1], "a>b") == 0);
    CHECK(redir == NULL);
}

/* ========================================================================== */
/* 2. 路径解析                                                               */
/* ========================================================================== */

static void t_resolve(void)
{
    char out[VFS_PATH_MAX];
    char big[VFS_PATH_MAX + 64];
    unsigned int i;

    CHECK(sh_test_resolve("/", "/", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/") == 0);

    CHECK(sh_test_resolve("/", "a", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/a") == 0);

    CHECK(sh_test_resolve("/", "a//b/", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/a/b") == 0);

    CHECK(sh_test_resolve("/", ".", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/") == 0);

    CHECK(sh_test_resolve("/", "..", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/") == 0);

    CHECK(sh_test_resolve("/", "/a/../b", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/b") == 0);

    CHECK(sh_test_resolve("/", "/a/b/..", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/a") == 0);

    CHECK(sh_test_resolve("/x", "a", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/x/a") == 0);

    CHECK(sh_test_resolve("/x", "../a", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/a") == 0);

    CHECK(sh_test_resolve("/x/y", "..", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/x") == 0);

    CHECK(sh_test_resolve("/", "///", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/") == 0);

    CHECK(sh_test_resolve("/", "a/./b/../c", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "/a/c") == 0);

    /* 空路径 EINVAL(-22)；超长 ENAMETOOLONG(-36)；缓冲不足也是 -36。 */
    CHECK(sh_test_resolve("/", "", out, sizeof(out)) == -22);
    CHECK(sh_test_resolve("/", "/abc", out, 3) == -36);
    for (i = 0; i + 1u < sizeof(big); i++) {
        big[i] = 'p';
    }
    big[0] = '/';
    big[sizeof(big) - 1u] = '\0';
    CHECK(sh_test_resolve("/", big, out, sizeof(out)) == -36);
}

/* ========================================================================== */
/* 3. errno 文本                                                             */
/* ========================================================================== */

static void t_errstr(void)
{
    CHECK(strcmp(sh_test_errstr(-2), "No such file or directory") == 0);
    CHECK(strcmp(sh_test_errstr(-13), "Permission denied") == 0);
    CHECK(strcmp(sh_test_errstr(-21), "Is a directory") == 0);
    CHECK(strcmp(sh_test_errstr(-20), "Not a directory") == 0);
    CHECK(strcmp(sh_test_errstr(-36), "File name too long") == 0);
    CHECK(strcmp(sh_test_errstr(-17), "File exists") == 0);
    CHECK(strcmp(sh_test_errstr(-22), "Invalid argument") == 0);
    CHECK(strcmp(sh_test_errstr(-999), "Unknown error") == 0);
    CHECK(sh_test_errstr(0) != NULL);
}

/* ========================================================================== */
/* 4. help / 提示符 / 证据行                                                 */
/* ========================================================================== */

static void t_help(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("help\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell ready"));
    CHECK(out_has("ng> "));
    CHECK(out_has("commands:"));
    CHECK(out_has("ls [path]"));
    CHECK(out_has("cat <file>"));
    CHECK(out_has("echo <text> [> path]"));
    CHECK(out_has("pkg verify <name>"));
    CHECK(out_has("exit [code]"));
    CHECK(out_has("Ctrl-D"));
    CHECK(out_has("ng: shell exit ok"));
    CHECK(sh_test_status() == 0);
}

/* ========================================================================== */
/* 5. ls                                                                     */
/* ========================================================================== */

static void t_ls(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("ls /\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell ls ok"));
    CHECK(out_has("etc/"));
    CHECK(out_has("tmp/"));
    CHECK(out_has("System/"));

    rc = run("ls /tmp\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell ls ok"));
    CHECK(out_has("(empty)"));

    rc = run("ls /nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("ls: /nope: No such file or directory"));
    CHECK(!out_has("ng: shell ls ok"));

    rc = run("ls /etc/passwd\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("Not a directory"));

    rc = run("ls a b\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("ls: usage: ls [path]"));

    rc = run("ls / > /tmp/x\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("only echo supports '>' redirection"));
    CHECK(file_exists("/tmp/x") == 0);
}

/* ========================================================================== */
/* 6. cat                                                                    */
/* ========================================================================== */

static void t_cat(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("cat /etc/passwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell cat ok"));
    CHECK(out_has("root:0:"));
    CHECK(out_has("alice:1000:"));
    CHECK(vfs_fd_live(&g_ctx) == 0);

    rc = run("cat /nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cat: /nope: No such file or directory"));

    rc = run("cat\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cat: usage: cat <file>"));

    rc = run("cat /tmp\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("Is a directory"));

    rc = run("cat /etc/passwd /etc/passwd\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cat: usage: cat <file>"));

    rc = run("cat /etc/passwd > /tmp/x\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("only echo supports '>' redirection"));
    CHECK(file_exists("/tmp/x") == 0);
    CHECK(vfs_fd_live(&g_ctx) == 0);
}

/* ========================================================================== */
/* 7. echo / 重定向                                                          */
/* ========================================================================== */

static void t_echo(void)
{
    char buf[32];
    struct vfs_stat st;
    int n;
    int rc;

    CHECK(env_reset() == 1);
    rc = run("echo hi > /tmp/s.txt\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell write ok"));
    CHECK(file_exists("/tmp/s.txt") == 1);
    CHECK(vfs_fd_live(&g_ctx) == 0);
    n = read_file("/tmp/s.txt", buf, sizeof(buf));
    CHECK(n == 3);
    CHECK(n == 3 && memcmp(buf, "hi\n", 3) == 0);
    CHECK(vfs_stat(&g_ctx, "/tmp/s.txt", &st) == VFS_OK);
    CHECK((st.mode & 0777u) == 0644u);

    rc = run("cat /tmp/s.txt\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("hi\r\n"));
    CHECK(out_has("ng: shell cat ok"));

    rc = run("echo hello world\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("hello world\r\n"));
    CHECK(!out_has("ng: shell write ok"));

    rc = run("echo\nexit\n");
    CHECK(rc == 0);

    rc = run("echo a b c d e f g h i\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("too many arguments (max 8)"));

    rc = run("echo hi > /nope/s.txt\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("echo: /nope/s.txt: No such file or directory"));

    rc = run("echo a > /tmp/e1\necho b > /tmp/e2\ncat /tmp/e1\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("a\r\n"));
    CHECK(file_exists("/tmp/e2") == 1);

    /* 相对路径 + cd 之后的重定向。 */
    CHECK(env_reset() == 1);
    rc = run("cd /tmp\necho rel > rel.txt\ncat rel.txt\nexit\n");
    CHECK(rc == 0);
    CHECK(file_exists("/tmp/rel.txt") == 1);
    CHECK(out_has("rel\r\n"));
}

/* ========================================================================== */
/* 8. mkdir / rm                                                             */
/* ========================================================================== */

static void t_mkdir_rm(void)
{
    struct vfs_stat st;
    int rc;

    CHECK(env_reset() == 1);
    rc = run("mkdir /tmp/d1\nls /tmp\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("d1/"));
    CHECK(vfs_stat(&g_ctx, "/tmp/d1", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_DIR);
    CHECK((st.mode & 0777u) == 0755u);

    rc = run("mkdir /tmp/d1\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("mkdir: /tmp/d1: File exists"));

    rc = run("mkdir /nope/d2\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("No such file or directory"));

    rc = run("rm /tmp/d1\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("Is a directory"));
    CHECK(file_exists("/tmp/d1") == 1);

    rc = run("echo x > /tmp/f\nrm /tmp/f\nstat /tmp/f\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("No such file or directory"));
    CHECK(file_exists("/tmp/f") == 0);

    rc = run("rm /nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("rm: /nope: No such file or directory"));

    rc = run("rm\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("rm: usage: rm <path>"));

    rc = run("mkdir\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("mkdir: usage: mkdir <dir>"));
}

/* ========================================================================== */
/* 9. cd / pwd                                                               */
/* ========================================================================== */

static void t_cd_pwd(void)
{
    char cwd[VFS_PATH_MAX];
    int rc;

    CHECK(env_reset() == 1);
    rc = run("pwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("/\r\n"));
    sh_test_cwd(cwd, sizeof(cwd));
    CHECK(strcmp(cwd, "/") == 0);

    rc = run("cd /tmp\npwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("/tmp\r\n"));
    sh_test_cwd(cwd, sizeof(cwd));
    CHECK(strcmp(cwd, "/tmp") == 0);

    rc = run("cd /etc\ncd ../tmp\npwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("/tmp\r\n"));

    rc = run("cd /tmp\ncd\npwd\nexit\n");
    CHECK(rc == 0);
    sh_test_cwd(cwd, sizeof(cwd));
    CHECK(strcmp(cwd, "/") == 0);

    rc = run("cd /etc/passwd\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cd: /etc/passwd: Not a directory"));
    sh_test_cwd(cwd, sizeof(cwd));
    CHECK(strcmp(cwd, "/") == 0);

    rc = run("cd /nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cd: /nope: No such file or directory"));

    rc = run("cd /tmp /var\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("cd: usage: cd [dir]"));
}

/* ========================================================================== */
/* 10. stat                                                                  */
/* ========================================================================== */

static void t_stat(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("stat /etc/passwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("file: /etc/passwd"));
    CHECK(out_has("type: file"));
    CHECK(out_has("size: "));
    CHECK(out_has("ino:  "));

    rc = run("echo hi > /tmp/s.txt\nstat /tmp/s.txt\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("mode: 0644"));

    rc = run("stat /tmp\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("type: dir"));

    rc = run("stat /nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("stat: /nope: No such file or directory"));

    rc = run("stat\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("stat: usage: stat <path>"));
}

/* ========================================================================== */
/* 11. dmesg / log                                                           */
/* ========================================================================== */

static void t_dmesg_log(void)
{
    char logbuf[4096];
    u32 n;
    int rc;

    CHECK(env_reset() == 1);
    rc = run("log wp26 hello world\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("log: appended (tag=wp26"));
    n = log_read(logbuf, sizeof(logbuf));
    CHECK(n >= 18);
    logbuf[(n < sizeof(logbuf)) ? n : sizeof(logbuf) - 1] = '\0';
    CHECK(strstr(logbuf, "wp26: hello world\n") != NULL);

    rc = run("dmesg\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell dmesg ok"));
    CHECK(out_has("wp26: hello world"));

    rc = run("log a one\nlog a two\ndmesg -n 1\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("a: two"));
    CHECK(!out_has("a: one"));

    rc = run("log a one\nlog a two\ndmesg -n 2\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("a: one"));
    CHECK(out_has("a: two"));

    rc = run("dmesg -n 0\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("dmesg: -n needs a line count in 1..64"));

    rc = run("dmesg -n abc\nexit\n");
    CHECK(rc == 1);

    rc = run("dmesg -n 65\nexit\n");
    CHECK(rc == 1);

    rc = run("dmesg -n 3 extra\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("dmesg: usage: dmesg [-n N]"));

    rc = run("log bad:tag msg\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("log: tag must be 1..15 printable chars"));

    rc = run("log abcdefghijklmnop msg\nexit\n");
    CHECK(rc == 1);

    rc = run("log tag\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("log: usage: log <tag> <msg>"));
}

/* ========================================================================== */
/* 12. pkg                                                                   */
/* ========================================================================== */

static void t_pkg(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("pkg list\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("hello 1.0.0"));
    CHECK(out_has("ng: shell pkg ok"));

    rc = run("pkg verify hello\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("pkg: hello: verified"));
    CHECK(out_has("ng: shell pkg ok"));

    rc = run("pkg verify nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("pkg: nope: No such file or directory"));

    rc = run("pkg\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("pkg: usage: pkg list | pkg verify <name>"));

    rc = run("pkg install x\nexit\n");
    CHECK(rc == 1);

    rc = run("pkg verify\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("pkg: usage: pkg verify <name>"));

    rc = run("pkg list extra\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("pkg: usage: pkg list"));
}

/* ========================================================================== */
/* 13. login                                                                 */
/* ========================================================================== */

static void t_login(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("login root root\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("login: root: uid=0"));

    rc = run("login alice alice\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("login: alice: uid=1000"));

    rc = run("login root wrong\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("login: root: Permission denied"));

    rc = run("login nobody x\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("login: nobody: No such file or directory"));

    rc = run("login root\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("login: usage: login <user> <pass>"));
}

/* ========================================================================== */
/* 14. clear / Ctrl 键 / 行编辑                                              */
/* ========================================================================== */

static void t_keys(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("clear\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("\f"));

    /* Ctrl-D：直接退出（退出码 0）。 */
    out_reset();
    keys_reset();
    type_script("pwd\n");
    push_key(0x04, 'd', 0x20);
    rc = shell_main();
    CHECK(rc == 0);
    CHECK(g_exited == 1);
    CHECK(out_has("ng: shell exit ok"));
    CHECK(out_has("/\r\n"));

    /* Ctrl-L：清屏 + 重画当行。 */
    out_reset();
    keys_reset();
    type_script("pwd");
    push_key(0x04, 'l', 0x26);
    type_script("\nexit\n");
    rc = shell_main();
    CHECK(rc == 0);
    CHECK(out_has("\f"));
    {
        const char *esc = strchr(g_out, '\f');

        CHECK(esc != NULL);
        CHECK(esc != NULL && strstr(esc, "ng> pwd") != NULL);
    }

    /* Ctrl-C：放弃当前行，不执行它；后面的 pwd 正常执行并把状态清零。 */
    rc = run("ls /\003pwd\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("^C"));
    CHECK(!out_has("ng: shell ls ok"));
    CHECK(out_has("/\r\n"));

    /* 只按 Ctrl-C 再 exit：状态是非零。 */
    rc = run("ls /\003\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("^C"));

    /* Ctrl-U：删整行（两次 backspace 回显）。 */
    out_reset();
    keys_reset();
    type_script("ab");
    push_key(0x04, 'u', 0x16);
    type_script("pwd\nexit\n");
    rc = shell_main();
    CHECK(rc == 0);
    CHECK(out_count("\b \b") >= 2);

    /* Backspace：删掉多余的 'X' 后执行 "ls /"。 */
    rc = run("ls /X\b\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("ng: shell ls ok"));
    CHECK(out_has("etc/"));
}

/* ========================================================================== */
/* 15. 错误 / 退出码 / 未知命令                                              */
/* ========================================================================== */

static void t_errors(void)
{
    int rc;

    CHECK(env_reset() == 1);
    rc = run("nope\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("sh: nope: command not found (try 'help')"));
    CHECK(sh_test_status() == 1);

    rc = run("\nexit\n");
    CHECK(rc == 0);

    rc = run("exit 7\n");
    CHECK(rc == 7);
    CHECK(g_exit_code == 7);
    CHECK(g_exited == 1);
    CHECK(out_has("ng: shell exit ok"));

    rc = run("exit 256\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("exit: code must be 0..255"));

    rc = run("exit abc\nexit\n");
    CHECK(rc == 1);

    rc = run("exit 1 2\nexit\n");
    CHECK(rc == 1);
    CHECK(out_has("exit: usage: exit [code]"));

    /* 命令失败后 exit 沿用非零状态；成功命令把它清零。 */
    rc = run("ls /nope\nls /\nexit\n");
    CHECK(rc == 0);
    rc = run("ls /\nls /nope\nexit\n");
    CHECK(rc == 1);
}

/* ========================================================================== */
/* 16. readkey -EAGAIN                                                       */
/* ========================================================================== */

static void t_eagain(void)
{
    int rc;

    CHECK(env_reset() == 1);
    out_reset();
    keys_reset();
    g_eagain_left = 7;
    type_script("pwd\nexit\n");
    rc = shell_main();
    CHECK(rc == 0);
    CHECK(out_has("/\r\n"));
    CHECK(out_has("ng: shell exit ok"));
}

/* ========================================================================== */
/* 17. ls 截断 / fd 回收                                                     */
/* ========================================================================== */

static void t_ls_truncate(void)
{
    char path[64];
    int rc;
    u32 i;

    CHECK(env_reset() == 1);
    for (i = 0; i < 9u; i++) {
        int fd;

        (void)snprintf(path, sizeof(path), "/tmp/f%u", i);
        fd = vfs_open(&g_ctx, path, VFS_O_CREAT | VFS_O_RDWR, 0644);
        CHECK(fd > 0);
        if (fd > 0) {
            CHECK(vfs_close(&g_ctx, fd) == VFS_OK);
        }
    }
    rc = run("ls /tmp\nexit\n");
    CHECK(rc == 0);
    CHECK(out_has("showing the first 8 entries"));
    CHECK(out_has("f0"));
    CHECK(out_has("f7"));
    CHECK(!out_has("f8"));
    CHECK(vfs_fd_live(&g_ctx) == 0);
}

/* ========================================================================== */
/* 18. WP3-0：spawn / wait / pid                                             */
/* ========================================================================== */

static void t_proc_cmds(void)
{
    int rc;

    /* spawn：路径解析 + argv[0]=路径，子进程 pid 由内核写回。 */
    CHECK(env_reset() == 1);
    rc = run("spawn /bin/proc hello\nwait\npid\nexit\n");
    CHECK(rc == 0);
    CHECK(g_spawn_calls == 1);
    CHECK(strcmp(g_spawn_path, "/bin/proc") == 0);
    CHECK(g_spawn_argc == 2);
    CHECK(strcmp(g_spawn_argv0, "/bin/proc") == 0);
    CHECK(strcmp(g_spawn_argv1, "hello") == 0);
    CHECK(out_has("ng: shell spawn ok (pid=21 path=/bin/proc)"));
    CHECK(g_wait_calls == 1);
    CHECK(g_wait_pid == 21u);
    CHECK(out_has("ng: shell wait ok (pid=21 status=0)"));
    CHECK(out_has("pid=8 ppid=1"));

    /* spawn 缺参数：用法错误，不调用内核。 */
    CHECK(env_reset() == 1);
    rc = run("spawn\nexit\n");
    CHECK(rc == 1);
    CHECK(g_spawn_calls == 0);
    CHECK(out_has("sh: spawn: usage: spawn <path> [args]"));

    /* 没有子进程的 wait：用法错误，不调用内核。 */
    CHECK(env_reset() == 1);
    rc = run("wait\nexit\n");
    CHECK(rc == 1);
    CHECK(g_wait_calls == 0);
    CHECK(out_has("sh: wait: no child"));

    /* wait <pid> 指定进程；非零状态把 $? 置 1。 */
    CHECK(env_reset() == 1);
    g_wait_status = 265; /* 0x100|9：被 kill 的进程 */
    rc = run("wait 7\nexit\n");
    CHECK(rc == 1);
    CHECK(g_wait_calls == 1);
    CHECK(g_wait_pid == 7u);
    CHECK(out_has("ng: shell wait ok (pid=7 status=265)"));

    /* wait 0 / 非数字：用法错误。 */
    CHECK(env_reset() == 1);
    rc = run("wait 0\nwait abc\nexit\n");
    CHECK(rc == 1);
    CHECK(g_wait_calls == 0);
    CHECK(out_has("wait: pid must be a positive number"));
}

int test_shell(void)
{
    t_tokenize();
    t_resolve();
    t_errstr();
    t_help();
    t_ls();
    t_cat();
    t_echo();
    t_mkdir_rm();
    t_cd_pwd();
    t_stat();
    t_dmesg_log();
    t_pkg();
    t_login();
    t_keys();
    t_errors();
    t_eagain();
    t_ls_truncate();
    t_proc_cmds();
    return 0;
}
