/*
 * ng WP3-5: S1 故障注入的客户机代理（ring3，L3，无 libc）。
 *
 * 这个程序不是产品组件，只在 `tools/s1-fault.ps1` 里被 ring3 shell
 * `spawn /mnt/disk/s1fault.elf <a|b|c|d>` 拉起来，作为"外部故障注入器"：
 *
 *   1. 自己 `spawn /bin/sessiond` 建一个**真实的会话**（displayd/wm/应用由
 *      sessiond 拉起，和正式启动路径一致）；mode d 额外带 `fault=spin`，
 *      sessiond 会把它透传给 WM，WM 收到 F12 后进入死循环（已知限制）。
 *   2. 按 `tests/s1/s1_logic.c` 的步骤表读 `/run/display.status` 断言状态，
 *      在合适的步骤用 kill syscall 杀掉 WM / 焦点应用 / sessiond。
 *   3. 每次状态满足就打印 `ng: s1 ...` 证据行；需要 host 注入按键时打印
 *      `ng: s1 input req (scenario=<a|b|c|d>)`，由 `tools/s1-fault.ps1`
 *      经 QMP 发 `1`（WM 启动器）或 `f12`（WM 故障钩子）。
 *   4. 结束时**不退出**（阻塞在 port_recv 超时轮询上）：父 shell 保持
 *      wait 阻塞，不会和 displayd 抢 readkey；QEMU 由 host 结束后收走。
 *
 * 为什么代理要放在 FAT16 磁盘上：内核镜像里的 7 个用户 ELF 是 build-ng
 * 编进去的，本 WP 不允许改 build-ng/kernel；`/mnt/disk` 是内核挂好的
 * FAT16，`spawn` 支持从任意 VFS 路径读 ELF，所以测试程序放在磁盘镜像里
 * 由 host 侧注入（tools/s1-fault.ps1 -> tests/s1/fat-add.ps1）。
 */
#include "s1_logic.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall（与 src/ng/syscall.h / ABI 草案一致）------------------------ */
#define S1_SYS_WRITE 0
#define S1_SYS_EXIT 1
#define S1_SYS_PORT_CREATE 10
#define S1_SYS_PORT_RECV 12
#define S1_SYS_OPEN 20
#define S1_SYS_READ 21
#define S1_SYS_CLOSE 23
#define S1_SYS_SPAWN 40
#define S1_SYS_GETPID 42
#define S1_SYS_KILL 43

#define S1_O_RDONLY 0x000u
#define S1_ETIMEDOUT (-110)

#define S1_STATUS_PATH "/run/display.status"
#define S1_SESSIOND_PATH "/bin/sessiond"

/*
 * 轮询节拍：20e6 TSC tick ≈ 20ms（与 displayd/sessiond 的 DS_POLL_TICKS 同
 * 假设；QEMU TCG 下 rdtsc 约 1 GHz，真机频率不同也只是等待快/慢）。
 * port_recv 的超时会阻塞并让出 CPU —— 代理不能忙等，否则协作式调度下
 * sessiond/displayd 永远拿不到 CPU。
 */
#define S1_POLL_TICKS 20000000ULL
#define S1_SLEEP_PORT_DEPTH 4u
#define S1_GRACE_ROUNDS 25u /* 杀 sessiond 后等 500ms 再断言"没有恢复" */

static u32 g_sleep_port;
static struct s1_ctx g_ctx;
static u32 g_step;
static u32 g_grace;

static i64 s1_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 s1_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return s1_sys4(nr, a1, a2, a3, 0);
}

static u32 s1_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void s1_put(const char *s)
{
    u32 n = s1_strlen(s);

    if (n != 0u) {
        (void)s1_sys(S1_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static void s1_putdec(u32 v)
{
    char buf[12];

    (void)s1_u32_dec(buf, (u32)sizeof(buf), v);
    s1_put(buf);
}

/* 阻塞一个轮询节拍（让出 CPU）。 */
static void s1_tick(void)
{
    char buf[8];

    if (g_sleep_port != 0u) {
        (void)s1_sys4(S1_SYS_PORT_RECV, (i64)g_sleep_port, (i64)buf,
                      (i64)sizeof(buf), S1_POLL_TICKS);
        return;
    }
    /* 端口创建失败才走到这里：忙等会挂住系统，但那只可能是启动失败。 */
    {
        volatile u32 i;

        for (i = 0; i < 100000u; i++) {
            __asm__ volatile("pause");
        }
    }
}

static void s1_halt(void) __attribute__((noreturn));

static void s1_halt(void)
{
    for (;;) {
        s1_tick();
    }
}

static u32 s1_read_status(struct s1_status *st)
{
    char buf[S1_STATUS_MAX];
    i64 fd;
    i64 n;
    u32 total = 0;
    u32 found;

    (void)s1_status_parse("", 0, st);
    fd = s1_sys(S1_SYS_OPEN, (i64)S1_STATUS_PATH, (i64)S1_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    /*
     * fs_sys_read 的单次上限是 FS_SYS_IO_MAX（512）：状态行约 300~400 字节，
     * 但按 256 分块更稳（shell 也是这么读的），避免 EINVAL 把整次读打回。
     */
    while (total + 1u < S1_STATUS_MAX) {
        u32 want = S1_STATUS_MAX - 1u - total;
        u32 chunk = 256u;

        if (chunk > want) {
            chunk = want;
        }
        n = s1_sys(S1_SYS_READ, fd, (i64)(buf + total), (i64)chunk);
        if (n <= 0) {
            break;
        }
        total += (u32)n;
    }
    (void)s1_sys(S1_SYS_CLOSE, fd, 0, 0);
    if (total == 0u) {
        return 0;
    }
    buf[total] = '\0';
    found = s1_status_parse(buf, total, st);
    return found;
}

static int s1_kill(u32 pid)
{
    if (pid == 0u) {
        return -1;
    }
    return (s1_sys(S1_SYS_KILL, (i64)pid, 9, 0) == 0) ? 0 : -1;
}

static u32 s1_spawn(const char *path, const char *const *argv)
{
    u32 pid = 0;

    if (s1_sys(S1_SYS_SPAWN, (i64)path, (i64)argv, (i64)&pid) != 0) {
        return 0;
    }
    return pid;
}

static void s1_fail_here(int mode, u32 step) __attribute__((noreturn));

static void s1_fail_here(int mode, u32 step)
{
    char line[96];

    (void)s1_format_result(line, (u32)sizeof(line), mode, 0, step);
    s1_put(line);
    s1_put("\r\n");
    s1_halt();
}

/* ---- 证据行 -------------------------------------------------------------- */

static void s1_print_up(const struct s1_status *st)
{
    s1_put("ng: s1 session up (sessiond=");
    s1_putdec(st->spid);
    s1_put(" displayd=");
    s1_putdec(st->sdpid);
    s1_put(" wm=");
    s1_putdec(st->swmpid);
    s1_put(" app=");
    s1_putdec(st->sapppid);
    s1_put(" launches=");
    s1_putdec(st->slaunches);
    s1_put(" windows=");
    s1_putdec(st->swindows);
    s1_put(")\r\n");
}

/*
 * 每个步骤完成时执行的动作 + 证据行。ctx 已经在 step 0 之前抓过基线，
 * 这里只读 st 做展示。
 */
static void s1_on_step_ok(int mode, u32 step, const struct s1_status *st)
{
    switch (mode) {
    case S1_MODE_WM:
        if (step == 0u) {
            s1_print_up(st);
            if (s1_kill(g_ctx.wm_pid) != 0) {
                s1_put("ng: s1 kill wm FAIL (pid=");
                s1_putdec(g_ctx.wm_pid);
                s1_put(")\r\n");
                s1_fail_here(mode, step);
            }
            s1_put("ng: s1 wm killed ok (pid=");
            s1_putdec(g_ctx.wm_pid);
            s1_put(")\r\n");
        } else if (step == 1u) {
            s1_put("ng: s1 wm restart ok (old=");
            s1_putdec(g_ctx.wm_pid);
            s1_put(" new=");
            s1_putdec(st->swmpid);
            s1_put(" restarts=");
            s1_putdec(st->srestarts);
            s1_put(")\r\n");
        } else if (step == 2u) {
            s1_put("ng: s1 demo alive ok (app=");
            s1_putdec(g_ctx.app_pid);
            s1_put(" apps=");
            s1_putdec(st->sapps);
            s1_put(" windows=");
            s1_putdec(st->swindows);
            s1_put(" reaps=");
            s1_putdec(st->sreaps);
            s1_put(")\r\n");
            s1_put("ng: s1 input req (scenario=a)\r\n");
        } else {
            s1_put("ng: s1 input ok (launches=");
            s1_putdec(st->slaunches);
            s1_put(" app=");
            s1_putdec(st->sapppid);
            s1_put(")\r\n");
        }
        break;
    case S1_MODE_INPUT:
        if (step == 0u) {
            s1_print_up(st);
            if (s1_kill(g_ctx.app_pid) != 0) {
                s1_put("ng: s1 kill app FAIL (pid=");
                s1_putdec(g_ctx.app_pid);
                s1_put(")\r\n");
                s1_fail_here(mode, step);
            }
            s1_put("ng: s1 app killed ok (pid=");
            s1_putdec(g_ctx.app_pid);
            s1_put(")\r\n");
        } else if (step == 1u) {
            s1_put("ng: s1 app reaped ok (pid=");
            s1_putdec(g_ctx.app_pid);
            s1_put(" reaps=");
            s1_putdec(st->sreaps);
            s1_put(" wm=");
            s1_putdec(st->swmpid);
            s1_put(")\r\n");
            s1_put("ng: s1 input req (scenario=b)\r\n");
        } else {
            s1_put("ng: s1 input recovery ok (launches=");
            s1_putdec(st->slaunches);
            s1_put(" app=");
            s1_putdec(st->sapppid);
            s1_put(" windows=");
            s1_putdec(st->swindows);
            s1_put(")\r\n");
        }
        break;
    case S1_MODE_SESSIOND:
        if (step == 0u) {
            s1_print_up(st);
            g_ctx.kill_ok = 0;
            if (s1_kill(g_ctx.sessiond_pid) != 0) {
                s1_put("ng: s1 kill sessiond FAIL (pid=");
                s1_putdec(g_ctx.sessiond_pid);
                s1_put(")\r\n");
                s1_fail_here(mode, step);
            }
            g_ctx.kill_ok = 1;
            g_grace = S1_GRACE_ROUNDS;
            s1_put("ng: s1 sessiond killed ok (pid=");
            s1_putdec(g_ctx.sessiond_pid);
            s1_put(")\r\n");
        } else {
            s1_put("ng: s1 sessiond dead ok (pid=");
            s1_putdec(g_ctx.sessiond_pid);
            s1_put(" stale=1 restarts=");
            s1_putdec(st->srestarts);
            s1_put(" wm=");
            s1_putdec(st->swmpid);
            s1_put(" app=");
            s1_putdec(st->sapppid);
            s1_put(")\r\n");
            s1_put("ng: s1 input req (scenario=c)\r\n");
        }
        break;
    case S1_MODE_SPIN:
        s1_print_up(st);
        s1_put("ng: s1 spin armed (wm=");
        s1_putdec(st->swmpid);
        s1_put(" scancode=0x58)\r\n");
        s1_put("ng: s1 input req (scenario=d)\r\n");
        break;
    default:
        break;
    }
}

/* 每一步等多久（轮数 × 20ms）。 */
static u32 s1_timeout_rounds(int mode, u32 step)
{
    if (step == 0u) {
        return 600u; /* 12s：sessiond -> displayd -> wm -> 自动/按键首个应用 */
    }
    switch (mode) {
    case S1_MODE_WM:
        if (step == 1u) {
            return 800u; /* 16s：800ms 心跳超时 + 20/40/80ms 退避 + 新 WM */
        }
        return (step == 2u) ? 200u : 900u; /* 18s：等 host '1' -> LAUNCH */
    case S1_MODE_INPUT:
        return (step == 1u) ? 600u : 900u; /* 12s 心跳回收；18s 等 host */
    case S1_MODE_SESSIOND:
        return 100u;
    default:
        return 600u;
    }
}

int s1_main(long argc, char **argv)
{
    const char *arg;
    const char *sargv[3];
    struct s1_status st;
    int mode;
    u32 count;
    u32 i;
    u32 pid;
    u32 spid;
    i64 rc;
    u32 rounds = 0;
    char line[96];

    arg = (argc >= 2 && argv != NULL && argv[1] != NULL) ? argv[1] : "a";
    mode = s1_mode_parse(arg);
    pid = (u32)s1_sys(S1_SYS_GETPID, 0, 0, 0);
    if (mode <= 0) {
        s1_put("ng: s1 agent FAIL (mode)\r\n");
        s1_halt();
    }
    s1_put("ng: s1 agent up (pid=");
    s1_putdec(pid);
    s1_put(" mode=");
    s1_put(s1_mode_name(mode));
    s1_put(")\r\n");

    rc = s1_sys(S1_SYS_PORT_CREATE, (i64)S1_SLEEP_PORT_DEPTH, 0, 0);
    if (rc <= 0) {
        s1_put("ng: s1 agent FAIL (port)\r\n");
        s1_halt();
    }
    g_sleep_port = (u32)rc;

    i = 0;
    sargv[i++] = S1_SESSIOND_PATH;
    if (mode == S1_MODE_SPIN) {
        sargv[i++] = "fault=spin"; /* sessiond -> wm 的透传，仅为故障注入 */
    }
    sargv[i] = NULL;
    spid = s1_spawn(S1_SESSIOND_PATH, sargv);
    if (spid == 0u) {
        s1_put("ng: s1 agent FAIL (sessiond spawn)\r\n");
        s1_halt();
    }
    s1_put("ng: s1 sessiond spawn ok (pid=");
    s1_putdec(spid);
    s1_put(")\r\n");

    count = s1_step_count(mode);
    g_step = 0;
    g_grace = 0;
    for (;;) {
        int v;
        u32 rounds_limit;

        (void)s1_read_status(&st);
        if (g_grace != 0u) {
            g_grace--;
            s1_tick();
            continue;
        }
        v = s1_step_eval(mode, g_step, &st, &g_ctx);
        if (v == S1_OK) {
            if (g_step == 0u) {
                s1_ctx_capture(&g_ctx, &st);
            }
            s1_on_step_ok(mode, g_step, &st);
            g_step++;
            rounds = 0;
            if (g_step >= count) {
                if (mode == S1_MODE_SPIN) {
                    s1_halt(); /* 等 host 注入 F12；之后系统不再前进 */
                }
                (void)s1_format_result(line, (u32)sizeof(line), mode, 1,
                                       g_step);
                s1_put(line);
                s1_put("\r\n");
                s1_halt();
            }
            s1_tick();
            continue;
        }
        if (v == S1_FAIL) {
            s1_fail_here(mode, g_step);
        }
        rounds++;
        rounds_limit = s1_timeout_rounds(mode, g_step);
        if (rounds > rounds_limit) {
            s1_put("ng: s1 timeout (mode=");
            s1_put(s1_mode_name(mode));
            s1_put(" step=");
            s1_putdec(g_step);
            if (s1_step_name(mode, g_step) != NULL) {
                s1_put(" name=");
                s1_put(s1_step_name(mode, g_step));
            }
            s1_put(")\r\n");
            s1_fail_here(mode, g_step);
        }
        s1_tick();
    }
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call s1_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
