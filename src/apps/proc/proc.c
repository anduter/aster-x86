/*
 * ng WP3-0: 用户态多进程 / 跨进程 IPC 演示程序（/bin/proc）。
 *
 * 一个 ELF、多种角色（argv 决定），被 shell / 内核 spawn 起来跑：
 *
 *   proc hello                  打印 pid/ppid 后 exit(0)（shell 的子进程证据）
 *   proc hello chain            3 进程链式 ping 的客户端：port_create -> spawn
 *                               relay -> recv(PONG) -> wait -> exit
 *   proc relay <up_ticket>      中间节点：port_create -> spawn server -> 转发
 *   proc server <up_ticket>     末端：port_create -> 发 RDY -> recv(PING) ->
 *                               reply(PONG) -> exit
 *   proc hold <tag>             把 tag 写进固定 VA 的"标记页"（0x40010000）后
 *                               永久阻塞在 recv，供内核做 AS 隔离 + kill 证据
 *
 * 链式 ping 的消息流（3 个进程、4 条消息、ticket 两跳）：
 *
 *   client(hello)          relay                     server
 *     port C                 port R                    port S
 *     spawn relay(C)  --->   spawn server(R)  --->    send(R, RDY, from=S)
 *     recv(C) = RDY <------- send(C, RDY, from=R)
 *     send(R, PING, from=C) -> recv(R)=PING
 *                              reply(C, ACK)
 *                              send(S, PING, from=R) --> recv(S)=PING
 *                                                        reply(R, PONG, from=S)
 *     recv(C) = PONG <------ send(C, PONG, from=R) <----- recv(R)=PONG
 *     wait(relay) -> 0        wait(server) -> 0         exit(0)
 *
 * ticket 来自 argv（父传）或消息头里的 from（内核填不了 from，由发送方填自己的
 * ticket，接收方据此 reply）—— 这就是 WP3-0 允许的跨进程能力传递方式。
 *
 * 无 libc：syscall 直接内联，字符串/十进制自己实现。
 */
/* 用户程序不 include 内核头（与 shell.c 一样自带最小类型）。 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned long long usize;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号（与 src/ng/syscall.h 一致）------------------------------- */
#define P_SYS_WRITE 0
#define P_SYS_EXIT 1
#define P_SYS_PORT_CREATE 10
#define P_SYS_PORT_SEND 11
#define P_SYS_PORT_RECV 12
#define P_SYS_PORT_REPLY 13
#define P_SYS_PORT_CLOSE 14
#define P_SYS_SPAWN 40
#define P_SYS_WAIT 41
#define P_SYS_GETPID 42
#define P_SYS_KILL 43
#define P_SYS_GETPPID 44

/* ---- IPC 消息头（与 struct ipc_msg_hdr 逐字节一致：16 字节）-------------- */
struct p_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};

#define P_TYPE_RDY 1u
#define P_TYPE_PING 2u
#define P_TYPE_ACK 3u
#define P_TYPE_PONG 4u

#define P_TIMEOUT_FOREVER (~0ULL)
#define P_MSG_MAX 64u

/* 标记页：内核在**每个**进程里都映射这个 VA，物理页各不相同。 */
#define P_MARKER_VA 0x0000000040010000ULL

/* 4 参数版本（r10 = 第 4 个参数）：recv 的 timeout 走这里。 */
static i64 p_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 p_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return p_sys4(nr, a1, a2, a3, 0);
}

/* ---- 小工具（无 libc）---------------------------------------------------- */
static u32 p_strlen(const char *s)
{
    u32 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void p_put(const char *s)
{
    u32 n = p_strlen(s);

    if (n != 0) {
        (void)p_sys(P_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static void p_putdec(u64 v)
{
    char buf[21];
    int n = 0;

    if (v == 0) {
        (void)p_sys(P_SYS_WRITE, (i64)"0", 1, 0);
        return;
    }
    while (v != 0 && n < 20) {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    {
        char out[21];
        int i;

        for (i = 0; i < n; i++) {
            out[i] = buf[n - 1 - i];
        }
        (void)p_sys(P_SYS_WRITE, (i64)out, (i64)n, 0);
    }
}

static int p_streq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

/* 十进制 -> u32；非法返回 0（ticket 永远 > 0，0 当无效）。 */
static u32 p_parse_u32(const char *s)
{
    u32 v = 0;

    if (s == NULL || *s == '\0') {
        return 0;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10u + (u32)(*s - '0');
        s++;
    }
    return v;
}

static void p_u32str(u32 v, char *out)
{
    char tmp[11];
    int n = 0;
    int i;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v != 0 && n < 10) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
}

/* ---- IPC 包装 ------------------------------------------------------------ */
static u32 p_port_create(void)
{
    i64 r = p_sys(P_SYS_PORT_CREATE, 0, 0, 0);

    return (r > 0) ? (u32)r : 0;
}

static int p_send(u32 ticket, u32 type, u32 from)
{
    struct p_hdr h;
    i64 r;

    h.type = type;
    h.len = 0;
    h.from = from;
    h.flags = 0;
    r = p_sys(P_SYS_PORT_SEND, (i64)ticket, (i64)&h, (i64)sizeof(h));
    return (int)r;
}

static int p_reply(u32 ticket, u32 type, u32 from)
{
    struct p_hdr h;
    i64 r;

    h.type = type;
    h.len = 0;
    h.from = from;
    h.flags = 0;
    r = p_sys(P_SYS_PORT_REPLY, (i64)ticket, (i64)&h, (i64)sizeof(h));
    return (int)r;
}

/* 阻塞收一条；返回 type（负数 = 错误），*out_from 是发送方 ticket。 */
static int p_recv(u32 port, u32 *out_from)
{
    struct p_hdr h;
    i64 r;

    h.type = 0;
    h.len = 0;
    h.from = 0;
    h.flags = 0;
    r = p_sys4(P_SYS_PORT_RECV, (i64)port, (i64)&h, (i64)sizeof(h),
                (i64)P_TIMEOUT_FOREVER);
    if (r < 0) {
        return (int)r;
    }
    if (out_from != 0) {
        *out_from = h.from;
    }
    return (int)h.type;
}

static void p_exit(int code)
{
    (void)p_sys(P_SYS_EXIT, (i64)code, 0, 0);
    for (;;) {
    }
}

static int p_wait(u32 pid)
{
    int status = -1;

    if (p_sys(P_SYS_WAIT, (i64)pid, (i64)&status, 0) != 0) {
        return -1;
    }
    return status;
}

/* ---- 角色：hello（+ 可选 chain）----------------------------------------- */
static void p_role_hello(long argc, char **argv)
{
    u32 pid = (u32)p_sys(P_SYS_GETPID, 0, 0, 0);
    u32 ppid = (u32)p_sys(P_SYS_GETPPID, 0, 0, 0);

    if (argc >= 3 && p_streq(argv[2], "chain")) {
        u32 port_c = p_port_create();
        u32 pid_relay = 0;
        char tick[12];
        char *av[4];
        u32 from = 0;
        int type;
        int done = 0;
        int st;

        if (port_c == 0) {
            p_put("ng: proc ipc hello FAIL (no port)\r\n");
            p_exit(1);
        }
        p_u32str(port_c, tick);
        av[0] = (char *)"proc";
        av[1] = (char *)"relay";
        av[2] = tick;
        av[3] = 0;
        if (p_sys(P_SYS_SPAWN, (i64)"/bin/proc", (i64)av, (i64)&pid_relay) !=
            0) {
            p_put("ng: proc ipc hello FAIL (spawn relay)\r\n");
            p_exit(1);
        }
        while (!done) {
            type = p_recv(port_c, &from);
            if (type < 0) {
                break;
            }
            if (type == (int)P_TYPE_RDY) {
                /* from = relay 的端口 ticket：发 PING 过去，from 填自己的 C。 */
                if (p_send(from, P_TYPE_PING, port_c) != 0) {
                    break;
                }
            } else if (type == (int)P_TYPE_PONG) {
                done = 1;
            }
        }
        st = p_wait(pid_relay);
        if (!done || st != 0) {
            p_put("ng: proc ipc hello FAIL (chain)\r\n");
            p_exit(1);
        }
        p_put("ng: proc ipc hello ok (pid=");
        p_putdec(pid);
        p_put(" ppid=");
        p_putdec(ppid);
        p_put(")\r\n");
        p_exit(0);
    }

    p_put("ng: proc hello ok (pid=");
    p_putdec(pid);
    p_put(" ppid=");
    p_putdec(ppid);
    p_put(")\r\n");
    p_exit(0);
}

/* ---- 角色：relay <up_ticket> -------------------------------------------- */
static void p_role_relay(long argc, char **argv)
{
    u32 up = 0;
    u32 port_r;
    u32 pid_srv = 0;
    u32 srv = 0;
    u32 have_srv = 0;
    u32 have_ping = 0;
    u32 ping_from = 0;
    char tick[12];
    char *av[4];
    int type;
    int done = 0;
    int st;

    if (argc < 3) {
        p_exit(1);
    }
    up = p_parse_u32(argv[2]);
    port_r = p_port_create();
    if (up == 0 || port_r == 0) {
        p_exit(1);
    }
    /* 先把自己的端口告诉客户端（ticket 经消息 from 传递）。 */
    if (p_send(up, P_TYPE_RDY, port_r) != 0) {
        p_exit(1);
    }
    p_u32str(port_r, tick);
    av[0] = (char *)"proc";
    av[1] = (char *)"server";
    av[2] = tick;
    av[3] = 0;
    if (p_sys(P_SYS_SPAWN, (i64)"/bin/proc", (i64)av, (i64)&pid_srv) != 0) {
        p_exit(1);
    }
    while (!done) {
        u32 from = 0;

        type = p_recv(port_r, &from);
        if (type < 0) {
            break;
        }
        if (type == (int)P_TYPE_RDY) {
            srv = from;
            have_srv = 1;
            if (have_ping) {
                (void)p_send(srv, P_TYPE_PING, port_r);
            }
        } else if (type == (int)P_TYPE_PING) {
            ping_from = from;
            have_ping = 1;
            (void)p_reply(ping_from, P_TYPE_ACK, port_r);
            if (have_srv) {
                (void)p_send(srv, P_TYPE_PING, port_r);
            }
        } else if (type == (int)P_TYPE_PONG) {
            (void)p_send(up, P_TYPE_PONG, port_r);
            done = 1;
        }
    }
    st = p_wait(pid_srv);
    if (!done || st != 0) {
        p_put("ng: proc ipc relay FAIL\r\n");
        p_exit(1);
    }
    p_put("ng: proc ipc relay ok (pid=");
    p_putdec((u32)p_sys(P_SYS_GETPID, 0, 0, 0));
    p_put(" hops=2)\r\n");
    p_exit(0);
}

/* ---- 角色：server <up_ticket> ------------------------------------------- */
static void p_role_server(long argc, char **argv)
{
    u32 up = 0;
    u32 port_s;
    int type;
    u32 from = 0;

    if (argc < 3) {
        p_exit(1);
    }
    up = p_parse_u32(argv[2]);
    port_s = p_port_create();
    if (up == 0 || port_s == 0) {
        p_exit(1);
    }
    if (p_send(up, P_TYPE_RDY, port_s) != 0) {
        p_exit(1);
    }
    type = p_recv(port_s, &from);
    if (type != (int)P_TYPE_PING) {
        p_put("ng: proc ipc server FAIL (type=");
        if (type < 0) {
            p_put("-");
            p_putdec((u64)(-(i64)type));
        } else {
            p_putdec((u64)(u32)type);
        }
        p_put(" from=");
        p_putdec(from);
        p_put(" up=");
        p_putdec(up);
        p_put(" ps=");
        p_putdec(port_s);
        p_put(")\r\n");
        p_exit(1);
    }
    if (p_reply(from, P_TYPE_PONG, port_s) != 0) {
        p_exit(1);
    }
    p_put("ng: proc ipc server ok (pid=");
    p_putdec((u32)p_sys(P_SYS_GETPID, 0, 0, 0));
    p_put(")\r\n");
    p_exit(0);
}

/* ---- 角色：hold <tag>（AS 隔离 + kill 用）------------------------------- */
static void p_role_hold(long argc, char **argv)
{
    u32 tag = 0;
    u32 pid = (u32)p_sys(P_SYS_GETPID, 0, 0, 0);
    u32 ppid = (u32)p_sys(P_SYS_GETPPID, 0, 0, 0);
    u32 port;
    volatile u64 *marker = (volatile u64 *)(usize)P_MARKER_VA;

    if (argc >= 3) {
        tag = p_parse_u32(argv[2]);
    }
    /* 同一个 VA、不同 AS：内核读回的物理页内容就是这里的 tag。 */
    *marker = (u64)tag;
    p_put("ng: proc hold ok (pid=");
    p_putdec(pid);
    p_put(" ppid=");
    p_putdec(ppid);
    p_put(" tag=");
    p_putdec(tag);
    p_put(")\r\n");

    port = p_port_create();
    if (port == 0) {
        p_exit(1);
    }
    /* 永久阻塞在 recv：内核用 recv 里的等待链验证 kill 的清理路径。 */
    for (;;) {
        u32 from = 0;

        (void)p_recv(port, &from);
    }
}

/* ---- 入口 ---------------------------------------------------------------- */
int p_main(long argc, char **argv);

__attribute__((naked, noreturn)) void _start(void)
{
    /* 内核按 WP3-0 ABI 把 argc/argv 放在 rdi/rsi（进程初始寄存器）。 */
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call p_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}

int p_main(long argc, char **argv)
{
    if (argc >= 2 && p_streq(argv[1], "relay")) {
        p_role_relay(argc, argv);
    }
    if (argc >= 2 && p_streq(argv[1], "server")) {
        p_role_server(argc, argv);
    }
    if (argc >= 2 && p_streq(argv[1], "hold")) {
        p_role_hold(argc, argv);
    }
    p_role_hello(argc, argv);
    return 1;
}
