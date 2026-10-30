/* =============================================================================
 * apps/shell.c - 命令解释器（一个程序，不是内核的一部分）
 *
 * 定位：
 *   - 它是程序表里的 "shell@1"，由内核作为 init 启动；
 *   - 输入来自设备接口（kbd0 的 getc），不是内核内部函数；
 *   - 输出走 console（kprintf）；
 *   - 执行别的程序用 prog_run()，这就是“shell 能执行程序”的含义。
 *
 * 由于现在还没有用户态和进程，shell 运行在内核上下文里；
 * 等 P2 有了系统调用，这份逻辑会原样搬到用户态程序里，
 * 只把 kprintf/dev 调用换系统调用。
 * ============================================================================= */
#include "shell.h"
#include "../include/types.h"
#include "include/kernel.h"
#include "include/mm.h"
#include "include/userland.h"
#include "include/dev.h"
#include "include/apps.h"
#include "include/fs.h"
#include "include/proc.h"
#include "include/proc.h"
#include "include/klog.h"
#include "arch/x86/io.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/dev.h"

#define SHELL_LINE_MAX 64
#define SHELL_ARGV_MAX 6

#define SHELL_PROMPT "shell> "

static char     line_buf[SHELL_LINE_MAX];
static uint32_t line_len;
static char     input_display[SHELL_LINE_MAX + 16];

/* 把“提示符 + 当前输入”画到底部输入框里 */
static void render_input(void)
{
    uint32_t n = 0;
    const char *p = SHELL_PROMPT;

    while (*p != '\0' && n + 1U < sizeof(input_display)) {
        input_display[n++] = *p++;
    }
    for (uint32_t i = 0; i < line_len && n + 1U < sizeof(input_display); i++) {
        input_display[n++] = line_buf[i];
    }
    input_display[n] = '\0';

    console_input_set(input_display);
}

/* 把一行按空格切成参数（就地修改） */
static int split_args(char *s, char **argv, int max)
{
    int argc = 0;

    while (*s != '\0' && argc < max) {
        while (*s == ' ') s++;
        if (*s == '\0') break;
        argv[argc++] = s;
        while (*s != '\0' && *s != ' ') s++;
        if (*s != '\0') {
            *s++ = '\0';
        }
    }
    return argc;
}

/* ---------------------------- 显示 ---------------------------- */

static void cmd_help(void)
{
    kprintln("commands:");
    kprintln("  help                   show this help");
    kprintln("  prog list              list registered programs");
    kprintln("  prog run <name>        run a program");
    kprintln("  file ls                list the root directory");
    kprintln("  file write <p> <text>  write a file (creates it)");
    kprintln("  file read <p>          read a file");
    kprintln("  file mkdir <p>         create a directory");
    kprintln("  file rm <p>            remove a file");
    kprintln("  dev list               list devices");
    kprintln("  dev putc <n> <text>    write text to an output device");
    kprintln("  obj stat               object table usage");
    kprintln("  demo                   run the object-model self test");
    kprintln("  uptime                 show ticks since boot");
    kprintln("  irq                    show interrupt counts");
    kprintln("  dmesg [-n N]           show last N kernel log lines (default 24)");
    kprintln("  sys                    call int 0x81 and show the results");
    kprintln("  clear                  clear the screen");
}

/* ---------------------------- 程序 ---------------------------- */

static void cmd_prog_list(void)
{
    kprintf("prog: %u registered\n", prog_count());
    for (uint32_t i = 0; i < prog_count(); i++) {
        const program_t *p = prog_at(i);
        kprintf("  %s@%u  %s\n", p->name, p->version, (p->desc != 0) ? p->desc : "");
    }
}

static void cmd_prog_run(const char *name)
{
    if (strcmp(name, "shell") == 0) {
        kprintln("prog: shell is already running");
        return;
    }

    int r = prog_run(name);
    if (r == E_NOENT) {
        kprintf("prog: no such program '%s'\n", name);
    } else if (r == E_INVAL) {
        kprintf("prog: '%s' has no entry\n", name);
    } else {
        kprintf("prog: '%s' finished\n", name);
    }
}

/* ---------------------------- 文件 ---------------------------- */

static void cmd_file_ls(void)
{
    char name[FS_NAME_MAX];
    fs_stat_t st;
    uint32_t n = 0;

    kprintf("file: / (ram0)\n");
    while (fs_listdir("/", n, name, sizeof(name), &st) == E_OK) {
        kprintf("  %s%s%s  %u bytes\n",
                name, (st.type == FS_TYPE_DIR) ? "/" : "", "",
                st.size);
        n++;
    }
    kprintf("file: %u entries\n", n);
}

static void cmd_file_write(int argc, char **argv)
{
    /* 64KB 的单文件上限不能再放内核栈上（会踩栈）：用静态缓冲。
     * 内核 shell 只有一个实例，不存在重入问题。 */
    static char text[FS_FILE_MAX];
    uint32_t n = 0;
    uint32_t h;

    if (argc < 4) {
        kprintln("usage: file write <path> <text>");
        return;
    }

    for (int i = 3; i < argc; i++) {
        const char *w = argv[i];
        while (*w != '\0' && n + 1U < sizeof(text)) {
            text[n++] = *w++;
        }
        if (i + 1 < argc && n + 1U < sizeof(text)) {
            text[n++] = ' ';
        }
    }

    h = fs_open(argv[2], FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0) {
        kprintf("file: cannot open '%s' for writing\n", argv[2]);
        return;
    }
    int w = fs_write(h, text, n);
    fs_close(h);

    if (w == (int)n) {
        kprintf("file: wrote %u bytes to %s\n", n, argv[2]);
    } else {
        kprintf("file: write failed (%d)\n", w);
    }
}

static void cmd_file_read(const char *name)
{
    static char buf[FS_FILE_MAX];   /* 同上：64KB 不进内核栈 */
    uint32_t h = fs_open(name, FS_OPEN_READ);
    int n;

    if (h == 0) {
        kprintf("file: cannot open '%s'\n", name);
        return;
    }
    n = fs_read(h, buf, sizeof(buf) - 1U);
    fs_close(h);

    if (n < 0) {
        kprintf("file: read failed (%d)\n", n);
        return;
    }
    buf[n] = '\0';
    kprintf("file: %s (%d bytes): \"%s\"\n", name, n, buf);
}

static void cmd_file_rm(const char *name)
{
    int r = fs_unlink(name);
    if (r == E_OK) {
        kprintf("file: removed %s\n", name);
    } else {
        kprintf("file: remove failed (%d)\n", r);
    }
}

static void cmd_file_mkdir(const char *path)
{
    int r = fs_mkdir(path);
    if (r == E_OK) {
        kprintf("file: created directory %s\n", path);
    } else {
        kprintf("file: mkdir failed (%d)\n", r);
    }
}

/* ---------------------------- 设备 ---------------------------- */

static void cmd_dev_list(void)
{
    kprintf("dev: %u registered\n", dev_count());
    for (uint32_t i = 0; i < dev_count(); i++) {
        const device_t *d = dev_at(i);
        kprintf("  %s:%s\n", dev_class_name(d->cls), d->name);
    }
}

static void cmd_dev_putc(const char *name, const char *text)
{
    const device_t *d = dev_lookup(name);
    if (d == 0) {
        kprintf("dev: no such device '%s'\n", name);
        return;
    }
    if (d->putc == 0) {
        kprintf("dev: '%s' is not an output device\n", name);
        return;
    }
    for (const char *p = text; *p != '\0'; p++) {
        d->putc(*p);
    }
    kprintf("\ndev: sent %u byte(s) to %s\n", (uint32_t)strlen(text), name);
}

/* ---------------------------- 系统调用 ---------------------------- */

/* 通过这些调用真的走 int 0x81，而不是直接调内核函数 */
static void cmd_sys(void)
{
    static char msg[] = "sys: hello via int 0x81\n";
    int written = sys_call(SYS_WRITE, (int)(uintptr_t)msg, (int)strlen(msg), 0);
    int pid     = sys_call(SYS_GETPID, 0, 0, 0);
    int uptime  = sys_call(SYS_UPTIME, 0, 0, 0);

    kprintf("sys: write=%d pid=%d uptime=%d s (total calls=%u)\n",
            written, pid, uptime, syscall_count());
}

/* ---------------------------- 中断统计 ---------------------------- */

static void cmd_irq(void)
{
    kprintf("irq: ticks=%u spurious=%u\n",
            (uint32_t)timer_ticks(), irq_spurious_count());

    for (uint32_t i = 0; i < IRQ_MAX; i++) {
        uint32_t c = irq_count(i);
        if (c != 0U) {
            kprintf("  irq%u %s count=%u\n", i, irq_name(i), c);
        }
    }
}

/* ---------------------------- 内核日志 ---------------------------- */

/* dmesg [-n N]：打印内核日志环最近 N 行（默认 24，上限 100）。
 * 输出由 klog_print_tail 直接走 console/串口，不再经 kprintf 入环，
 * 避免 dmesg 把自己也记进下一次输出。 */
static void cmd_dmesg(int argc, char **argv)
{
    uint32_t lines = 24U;

    if (argc >= 2) {
        if (strcmp(argv[1], "-n") == 0 && argc >= 3) {
            const char *p = argv[2];
            uint32_t v = 0;

            while (*p >= '0' && *p <= '9') {
                v = v * 10U + (uint32_t)(*p - '0');
                p++;
            }
            if (*p != '\0' || v == 0U) {
                kprintln("usage: dmesg [-n N] (N=1..100)");
                return;
            }
            lines = (v > 100U) ? 100U : v;
        } else {
            kprintln("usage: dmesg [-n N]");
            return;
        }
    }
    (void)klog_print_tail(lines);
}

/* ---------------------------- 命令分发 ---------------------------- */

static void run_line(const char *text)
{
    char buf[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];
    int argc;

    if (text == 0 || strlen(text) >= SHELL_LINE_MAX) return;

    strcpy(buf, text);
    argc = split_args(buf, argv, SHELL_ARGV_MAX);
    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "demo") == 0) {
        demo_run();
    } else if (strcmp(argv[0], "clear") == 0) {
        console_clear();
    } else if (strcmp(argv[0], "uptime") == 0) {
        kprintf("uptime: %u s (ticks %u)\n",
                timer_ticks() / timer_hz(), (uint32_t)timer_ticks());
    } else if (strcmp(argv[0], "irq") == 0) {
        cmd_irq();
    } else if (strcmp(argv[0], "dmesg") == 0) {
        cmd_dmesg(argc, argv);
    } else if (strcmp(argv[0], "sys") == 0) {
        cmd_sys();
    } else if (strcmp(argv[0], "obj") == 0 && argc >= 2 && strcmp(argv[1], "stat") == 0) {
        kprintf("obj: %u live handles (max %u)\n",
                obj_live_count(), (uint32_t)OBJ_MAX);
    } else if (strcmp(argv[0], "prog") == 0 && argc >= 2) {
        if (strcmp(argv[1], "list") == 0) {
            cmd_prog_list();
        } else if (strcmp(argv[1], "run") == 0 && argc >= 3) {
            cmd_prog_run(argv[2]);
        } else {
            kprintln("usage: prog list | prog run <name>");
        }
    } else if (strcmp(argv[0], "file") == 0 && argc >= 2) {
        if (strcmp(argv[1], "ls") == 0) {
            cmd_file_ls();
        } else if (strcmp(argv[1], "write") == 0 && argc >= 4) {
            cmd_file_write(argc, argv);
        } else if (strcmp(argv[1], "read") == 0 && argc >= 3) {
            cmd_file_read(argv[2]);
        } else if (strcmp(argv[1], "rm") == 0 && argc >= 3) {
            cmd_file_rm(argv[2]);
        } else if (strcmp(argv[1], "mkdir") == 0 && argc >= 3) {
            cmd_file_mkdir(argv[2]);
        } else {
            kprintln("usage: file ls | write <p> <text> | read <p> | mkdir <p> | rm <p>");
        }
    } else if (strcmp(argv[0], "dev") == 0 && argc >= 2) {
        if (strcmp(argv[1], "list") == 0) {
            cmd_dev_list();
        } else if (strcmp(argv[1], "putc") == 0 && argc >= 4) {
            cmd_dev_putc(argv[2], argv[3]);
        } else {
            kprintln("usage: dev list | dev putc <name> <text>");
        }
    } else {
        kprintf("shell: unknown command '%s'\n", argv[0]);
    }
}

/* 行编辑：回车执行、退格删除、可打印字符追加。
 * 输入只画在底部输入框里；回车后才把整行写进输出区，形成命令历史。 */
static void input_char(char c)
{
    if (c == '\r' || c == '\n') {
        kprintf("%s\n", input_display);     /* 命令回显到输出区 */
        if (line_len > 0U) {
            line_buf[line_len] = '\0';
            run_line(line_buf);
        }
        line_len = 0;
        render_input();
        return;
    }

    if (c == '\b') {
        if (line_len > 0U) {
            line_len--;
            render_input();
        }
        return;
    }

    if ((unsigned char)c < 0x20U) {
        return;
    }

    if (line_len + 1U < SHELL_LINE_MAX) {
        line_buf[line_len++] = c;
        render_input();
    }
}

/* -----------------------------------------------------------------------------
 * shell_entry - 程序入口
 *
 * 阻塞式循环：从 kbd0 取字符；没有输入就 hlt 等中断（时钟会唤醒）。
 * 正因为它是“程序”，内核把控制权交出来之后就不再管命令解析。
 * ----------------------------------------------------------------------------- */
void shell_entry(void)
{
    const device_t *kbd = dev_lookup("kbd0");
    unsigned int last_sec = 0;
    char c;

    line_len = 0;
    kprintln("shell: command interpreter started (type 'help')");
    kprintln("shell: output scrolls above, input is edited below");
    render_input();

    for (;;) {
        bool busy = false;

        /* 1. 把键盘缓冲区里的字符全部取出来处理 */
        if (kbd != 0 && kbd->getc != 0) {
            while (kbd->getc(&c) == E_OK) {
                input_char(c);
                busy = true;
            }
        }

        /* 2. 每秒刷新状态栏；正在输入时让路。
              屏幕只在状态栏更新（内容区保持静止），串口另留一条日志供测试断言。 */
        unsigned int sec = timer_ticks() / timer_hz();
        if (sec != last_sec && line_len == 0U) {
            char status[64];
            char line[64];

            last_sec = sec;
            ksnprintf(status, sizeof(status), "shell - uptime %u s", sec);
            console_set_status(status);

            /* 整行一次性走 console 锁，避免和 AP 上的心跳/调度日志交错。 */
            ksnprintf(line, sizeof(line), "uptime: %u s (ticks %u)\n",
                      sec, (unsigned int)timer_ticks());
            console_serial_line(line);
        }

        /* 3. 没有输入就让出 CPU，等下一个中断 */
        if (!busy) {
            __asm__ volatile("hlt");
        }
    }
}
