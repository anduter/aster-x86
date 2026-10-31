/*
 * ng 审计会话（display/session 协议）：ring3 攻击/回归程序。
 *
 * 运行方式（在 QEMU 里的 shell 中）：
 *   spawn /mnt/disk/AUDIT.ELF all
 *   wait
 *
 * 它自己拉起（而不是依赖内核自检的）服务端：
 *   1. spawn /bin/displayd -> 读 /run/display.port -> 畸形 CREATE/LIST/MAP/
 *      COMMIT/FOCUS/DESTROY/REGISTER_HOTKEY + 表满 + 小缓冲/背压；
 *   2. 共享内存：MAP -> 越界写（子进程，预期 #PF 只杀它自己）-> 杀 owner
 *      （displayd）后自己的映射仍然可写、可重映射（refcount 依赖）；
 *   3. spawn /bin/sessiond -> 枚举发现它的主端口（检查协议对畸形/伪造
 *      消息的健壮性）-> REGISTER/HEARTBEAT/EXIT/LAUNCH/KILL 坏令牌、
 *      LOOKUP/LIST 令牌导出、伪造 LIST_REPLY（count 超大）-> 仍然存活。
 *
 * 证据行统一前缀 `ng: audit ...`；全部通过时最后打印
 *   ng: audit ui PASS (checks=N)
 * 并以 0 退出（shell 的 `wait` 会打印 status=0）。不修改内核、不读串口，
 * 只使用普通 ring3 syscall + L2 协议库。
 */
#include "ds_protocol.h"
#include "session_protocol.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号（与 src/ng/syscall.h 一致） ------------------------------ */
#define A_SYS_WRITE 0
#define A_SYS_EXIT 1
#define A_SYS_PORT_CREATE 10
#define A_SYS_PORT_SEND 11
#define A_SYS_PORT_RECV 12
#define A_SYS_PORT_CLOSE 14
#define A_SYS_OPEN 20
#define A_SYS_READ 21
#define A_SYS_CLOSE 23
#define A_SYS_UNLINK 27
#define A_SYS_SPAWN 40
#define A_SYS_WAIT 41
#define A_SYS_GETPID 42
#define A_SYS_KILL 43
#define A_SYS_SHM_ALLOC 45
#define A_SYS_SHM_MAP 46

#define A_O_RDONLY 0x000u
#define A_ETIMEDOUT (-110)

#define A_DISPLAYD "/bin/displayd"
#define A_SESSIOND "/bin/sessiond"
#define A_SELF "/mnt/disk/AUDIT.ELF"

/* 攻击进程自己的伪造 WM 令牌：seq=9, role=WM(1), nonce=0x7A5A。 */
#define A_FAKE_WM_TOKEN 0x7A5A0109u
/* 第二轮：第二个伪 WM 令牌（nonce 不同）—— "WM 令牌只认领一次"的负例。 */
#define A_FAKE_WM_TOKEN2 0x7B5B0109u
/* 第二轮：攻击进程自己伪造的 APP 令牌（seq=1, role=APP(2), nonce=0x3C3C），
 * 用来建一个带令牌的"会话窗口"，验证 FOCUS 的令牌策略与归属回收。 */
#define A_APP_TOKEN 0x3C3C0201u

/* 发现端口时枚举的 (generation, slot) 范围：端口 ticket = (gen << 16) | slot，
 * slot < 64；启动早期最多几十次端口创建/关闭，24 代足够覆盖。 */
#define A_ENUM_GENS 24u
#define A_ENUM_SLOTS 32u

/* ---- 状态 ---------------------------------------------------------------- */
static u32 g_pid;
static u32 g_port;     /* 主端口：所有正常请求的 from / 回复目标 */
static u32 g_port2;    /* 第二端口：小缓冲/背压 + 越权 from */
static u32 g_sleep_port;
static u32 g_checks;
static u32 g_fails;
static u32 g_displayd_pid;
static u32 g_sessiond_pid;
/*
 * 第二轮：ring3 越界写（a_child_shm_oob）目前会让内核 PANIC —— 这是
 * docs/claims/ng-audit-wip.md 里记的已知限制（ring3 #PF 未隔离，内核
 * trap 直接 panic），不属于本会话能改的范围（src/ng 归 syscall 审计）。
 * 所以它变成**可选**场景：`spawn /mnt/disk/audit.elf oob` 才跑，默认跳过，
 * 这样一次 QEMU 能把 display/session 的协议用例全部跑完。
 */
static int g_do_oob;

/* ------------------------------------------------------------------------- */
/* syscall / 输出                                                             */
/* ------------------------------------------------------------------------- */

static i64 a_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 a_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return a_sys4(nr, a1, a2, a3, 0);
}

static u32 a_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void a_put(const char *s)
{
    u32 n = a_strlen(s);

    if (n != 0) {
        (void)a_sys(A_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static u32 a_u32str(u32 v, char *out)
{
    char tmp[11];
    u32 n = 0;
    u32 i;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (v != 0 && n < 10) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    out[n] = '\0';
    return n;
}

static void a_putdec(u32 v)
{
    char buf[12];

    (void)a_u32str(v, buf);
    a_put(buf);
}

static u32 a_hexstr(u32 v, char *out)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[9];
    u32 n = 0;
    u32 i;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (v != 0 && n < 8) {
        tmp[n++] = digits[v & 0xFu];
        v >>= 4;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    out[n] = '\0';
    return n;
}

static void a_puthex(u32 v)
{
    char buf[10];

    a_put("0x");
    (void)a_hexstr(v, buf);
    a_put(buf);
}

static void a_check(int ok, const char *what, u32 detail)
{
    g_checks++;
    if (ok) {
        a_put("ng: audit ");
        a_put(what);
        a_put(" ok (");
        a_putdec(detail);
        a_put(")\r\n");
    } else {
        g_fails++;
        a_put("ng: audit ");
        a_put(what);
        a_put(" FAIL (");
        a_putdec(detail);
        a_put(")\r\n");
    }
}

/* 用 sleep 端口的 recv 超时当 sleep（每次 DS_POLL_TICKS ≈ 20ms）。 */
static void a_sleep_ms(u32 ms)
{
    u8 buf[8];
    u32 loops = (ms + 19u) / 20u;

    while (loops > 0) {
        (void)a_sys4(A_SYS_PORT_RECV, (i64)g_sleep_port, (i64)buf,
                     (i64)sizeof(buf), (i64)DS_POLL_TICKS);
        loops--;
    }
}

/* 纯十进制解析（argv 传参用）；失败返回 0。 */
static int a_parse_dec(const char *s, u32 *out)
{
    u32 v = 0;
    u32 digits = 0;
    u32 i;

    if (s == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        u32 c = (u32)(u8)s[i];
        u32 d;

        if (c < '0' || c > '9') {
            return 0;
        }
        d = c - '0';
        if (v > 429496729u) {
            return 0;
        }
        v = v * 10u + d;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* IPC 小工具                                                                 */
/* ------------------------------------------------------------------------- */

struct a_ipc_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};

static u32 a_port_create(u32 depth)
{
    i64 r = a_sys(A_SYS_PORT_CREATE, (i64)depth, 0, 0);

    return (r > 0) ? (u32)r : 0;
}

static int a_send_raw(u32 port, const void *buf, u32 len)
{
    i64 r = a_sys4(A_SYS_PORT_SEND, (i64)port, (i64)buf, (i64)len, 0);

    return (r == 0) ? 0 : -1;
}

/* 返回总字节数（头 + body）或负 errno。 */
static i64 a_recv_raw(u32 port, void *buf, u32 cap, u64 timeout)
{
    return a_sys4(A_SYS_PORT_RECV, (i64)port, (i64)buf, (i64)cap, (i64)timeout);
}

static int a_send_ds(u32 port, const struct ds_msg *m, u32 from)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct a_ipc_hdr *h = (struct a_ipc_hdr *)buf;
    int n = ds_msg_encode(m, buf + DS_IPC_HDR_SIZE, DS_MSG_MAX);

    if (n <= 0) {
        return -1;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = from;
    h->flags = 0;
    return a_send_raw(port, buf, DS_IPC_HDR_SIZE + (u32)n);
}

static int a_recv_ds(u32 port, struct ds_msg *out, u32 *out_from, u64 timeout)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct a_ipc_hdr h;
    i64 r = a_recv_raw(port, buf, (u32)sizeof(buf), timeout);

    if (r < (i64)DS_IPC_HDR_SIZE || r > (i64)sizeof(buf)) {
        return -1;
    }
    ds_memcopy(&h, buf, DS_IPC_HDR_SIZE);
    if ((u64)h.len + (u64)DS_IPC_HDR_SIZE != (u64)r) {
        return -1;
    }
    if (ds_msg_decode(buf + DS_IPC_HDR_SIZE, h.len, out) != DS_OK) {
        return -1;
    }
    if (out_from != NULL) {
        *out_from = h.from;
    }
    return (int)out->type;
}

/* 等某个 display 消息类型；返回 1 = 命中。 */
static int a_wait_ds(u32 port, u16 want, struct ds_msg *out, u32 tries)
{
    u32 i;

    for (i = 0; i < tries; i++) {
        int t = a_recv_ds(port, out, NULL, DS_POLL_TICKS);

        if (t == (int)want) {
            return 1;
        }
        /* 坏消息 / 别的类型：丢掉继续等。 */
    }
    return 0;
}

static int a_send_sess(u32 port, const struct session_msg *m, u32 from)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct a_ipc_hdr *h = (struct a_ipc_hdr *)buf;
    int n = session_msg_encode(m, buf + SESSION_IPC_HDR_SIZE, SESSION_MSG_MAX);

    if (n <= 0) {
        return -1;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = from;
    h->flags = 0;
    return a_send_raw(port, buf, SESSION_IPC_HDR_SIZE + (u32)n);
}

static int a_recv_sess(u32 port, struct session_msg *out, u32 *out_from,
                       u64 timeout)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct a_ipc_hdr h;
    i64 r = a_recv_raw(port, buf, (u32)sizeof(buf), timeout);

    if (r < (i64)SESSION_IPC_HDR_SIZE || r > (i64)sizeof(buf)) {
        return -1;
    }
    ds_memcopy(&h, buf, SESSION_IPC_HDR_SIZE);
    if ((u64)h.len + (u64)SESSION_IPC_HDR_SIZE != (u64)r) {
        return -1;
    }
    if (session_msg_decode(buf + SESSION_IPC_HDR_SIZE, h.len, out) != DS_OK) {
        return -1;
    }
    if (out_from != NULL) {
        *out_from = h.from;
    }
    return (int)out->type;
}

static int a_wait_sess(u32 port, u16 want, struct session_msg *out, u32 tries)
{
    u32 i;

    for (i = 0; i < tries; i++) {
        int t = a_recv_sess(port, out, NULL, DS_POLL_TICKS);

        if (t == (int)want) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 文件 / 进程                                                                */
/* ------------------------------------------------------------------------- */

static int a_read_display_ticket(void)
{
    char buf[24];
    u32 n;
    u32 v = 0;
    u32 i;
    int fd;
    i64 r;

    fd = (int)a_sys(A_SYS_OPEN, (i64)DS_PORT_PATH, (i64)A_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    r = a_sys(A_SYS_READ, (i64)fd, (i64)buf, (i64)(sizeof(buf) - 1u));
    (void)a_sys(A_SYS_CLOSE, (i64)fd, 0, 0);
    if (r <= 0) {
        return 0;
    }
    n = (u32)r;
    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c >= '0' && c <= '9') {
            v = v * 10u + (u32)(c - '0');
        } else if (c == '\r' || c == '\n' || c == ' ') {
            break;
        } else {
            return 0;
        }
    }
    return (int)v;
}

static u32 a_spawn(const char *path, const char *const *argv)
{
    u32 pid = 0;
    i64 rc = a_sys4(A_SYS_SPAWN, (i64)path, (i64)argv, (i64)&pid, 0);

    return (rc < 0 || pid == 0) ? 0 : pid;
}

static int a_wait_pid(u32 pid, int *out_status)
{
    int status = 0;
    i64 rc = a_sys4(A_SYS_WAIT, (i64)pid, (i64)&status, 0, 0);

    if (out_status != NULL) {
        *out_status = status;
    }
    return (rc < 0) ? -1 : 0;
}

/* 等 /run/display.port 出现一个与 old 不同的新 ticket。 */
static u32 a_wait_display_ticket(int old)
{
    u32 i;

    for (i = 0; i < 500u; i++) {
        int t = a_read_display_ticket();

        if (t != 0 && t != old) {
            return (u32)t;
        }
        a_sleep_ms(20u);
    }
    return 0;
}

/*
 * 每个 display 攻击阶段用一个新的 displayd：kernel 没有 SYS_SHM_FREE，
 * surface DESTROY 不会归还 SHM 对象（对象池只有 SHM_MAX=8），连续 CREATE
 * 会把池子耗干。阶段之间杀掉/重拉 displayd 同时也验证了"displayd 退出 ->
 * 它占的 SHM 全部按 refcount 回收"。
 */
static void a_displayd_stop(void);

static u32 a_displayd_start(void)
{
    const char *dargv[2];
    u32 old;
    u32 disp;

    a_displayd_stop();
    old = (u32)a_read_display_ticket();
    dargv[0] = A_DISPLAYD;
    dargv[1] = NULL;
    g_displayd_pid = a_spawn(A_DISPLAYD, dargv);
    if (g_displayd_pid == 0) {
        return 0;
    }
    disp = a_wait_display_ticket((int)old);
    return disp;
}

static void a_displayd_stop(void)
{
    if (g_displayd_pid == 0) {
        return;
    }
    (void)a_sys4(A_SYS_KILL, (i64)g_displayd_pid, 9, 0, 0);
    {
        int status = 0;

        (void)a_wait_pid(g_displayd_pid, &status);
    }
    g_displayd_pid = 0;
    a_sleep_ms(100u);
}

/* ------------------------------------------------------------------------- */
/* display 协议攻击                                                           */
/* ------------------------------------------------------------------------- */

static void a_fill_create(struct ds_msg *m, u32 w, u32 h, u32 flags, u32 x,
                          u32 y, u32 token, const char *title, u32 mode)
{
    ds_memzero(m, (u64)sizeof(*m));
    m->type = DS_MSG_CREATE;
    m->u.create.w = w;
    m->u.create.h = h;
    m->u.create.flags = flags;
    m->u.create.x = x;
    m->u.create.y = y;
    m->u.create.owner_pid = g_pid;
    m->u.create.token = token;
    if (mode == 1u) {
        /* 16 个大写 A，没有 NUL：解码器/标题截断必须自己补 NUL。 */
        u32 i;

        for (i = 0; i < DS_TITLE_WORDS; i++) {
            m->u.create.title[i] = 0x41414141u;
        }
    } else if (title != NULL) {
        ds_title_set(m->u.create.title, title);
    }
}

/* 发 CREATE 并等回复；成功收到回复返回 1（status 可能非 0）。 */
static int a_create(u32 port, u32 w, u32 h, u32 flags, u32 x, u32 y,
                    u32 token, const char *title, u32 title_mode,
                    u32 *out_id, u32 *out_shm, u32 *out_pages,
                    i32 *out_status)
{
    struct ds_msg m;
    struct ds_msg r;
    u32 tries;

    a_fill_create(&m, w, h, flags, x, y, token, title, title_mode);
    if (a_send_ds(port, &m, g_port) != 0) {
        return 0;
    }    for (tries = 0; tries < DS_REPLY_WAIT_TRIES; tries++) {
        int t = a_recv_ds(g_port, &r, NULL, DS_REPLY_WAIT_TICKS);

        if (t == (int)DS_MSG_CREATE_REPLY) {
            if (out_id != NULL) {
                *out_id = r.u.create_reply.surface_id;
            }
            if (out_shm != NULL) {
                *out_shm = r.u.create_reply.shm_handle;
            }
            if (out_pages != NULL) {
                *out_pages = r.u.create_reply.pages;
            }
            if (out_status != NULL) {
                *out_status = r.status;
            }
            return 1;
        }
        /* 其它类型/坏消息：继续等 CREATE_REPLY。 */
    }
    return 0;
}

/*
 * 第二轮：用**指定 from 端口**发 CREATE（回复也收到那个端口）。
 * 用来验证"别的端口发来的第二个 WM 令牌 CREATE 必须被拒"。
 */
static int a_create_from(u32 disp, u32 from, u32 w, u32 h, u32 token,
                         const char *title, u32 *out_id, i32 *out_status)
{
    struct ds_msg m;
    struct ds_msg r;
    u32 tries;

    a_fill_create(&m, w, h, 0, 0, 0, token, title, 0);
    if (a_send_ds(disp, &m, from) != 0) {
        return 0;
    }
    for (tries = 0; tries < DS_REPLY_WAIT_TRIES; tries++) {
        int t = a_recv_ds(from, &r, NULL, DS_REPLY_WAIT_TICKS);

        if (t == (int)DS_MSG_CREATE_REPLY) {
            if (out_id != NULL) {
                *out_id = r.u.create_reply.surface_id;
            }
            if (out_status != NULL) {
                *out_status = r.status;
            }
            return 1;
        }
    }
    return 0;
}

static int a_list(u32 port, struct ds_msg *out)
{
    struct ds_msg m;
    u32 tries;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_LIST;
    /*
     * 端口只有 16 深：洪泛/背压之后 LIST 也可能撞上 EAGAIN（这是正常的
     * 背压，不是 displayd 的缺陷）。重试几次再判定失败。
     */
    for (tries = 0; tries < 8u; tries++) {
        if (a_send_ds(port, &m, g_port) == 0) {
            break;
        }
        a_sleep_ms(10u);
    }
    if (tries == 8u) {
        return 0;
    }
    return a_wait_ds(g_port, DS_MSG_LIST_REPLY, out, DS_REPLY_WAIT_TRIES);
}

/*
 * 畸形输入：每一条都是"IPC 头合法、display 消息畸形"或反过来。displayd
 * 必须丢掉它们继续服务（以前只有 d_recv_msg 的基本校验，这里覆盖
 * magic/version/type/len 全组合 + 截断 + 超长）。
 */
static void a_attack_malformed(u32 disp)
{
    u8 msg[64];
    u32 bad = 0;

    /* 1) 空 body：h.len=0（IPC 合法，display 解码 < 20 字节）。 */
    ds_memzero(msg, (u64)sizeof(msg));
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE) == 0) {
        bad++;
    }
    /* 2) body 只有 4 字节。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_CREATE;
    ((struct a_ipc_hdr *)msg)->len = 4;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 4u) == 0) {
        bad++;
    }
    /* 3) 坏 magic。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_LIST;
    ((struct a_ipc_hdr *)msg)->len = 24;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = 0xDEADBEEFu; /* magic */
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 3;           /* version */
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = (u16)DS_MSG_LIST;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 24;          /* len */
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 24u) == 0) {
        bad++;
    }
    /* 4) 坏 version。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_LIST;
    ((struct a_ipc_hdr *)msg)->len = 24;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = DS_PROTO_MAGIC;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 9;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = (u16)DS_MSG_LIST;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 24;
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 24u) == 0) {
        bad++;
    }
    /* 5) 未知 type。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = 0xFFFFu;
    ((struct a_ipc_hdr *)msg)->len = 24;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = DS_PROTO_MAGIC;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 3;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = 0xFFFFu;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 24;
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 24u) == 0) {
        bad++;
    }
    /* 6) LIST body 长度不对（0 / 8，而不是 4）。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_LIST;
    ((struct a_ipc_hdr *)msg)->len = 20;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = DS_PROTO_MAGIC;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 3;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = (u16)DS_MSG_LIST;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 20;
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 20u) == 0) {
        bad++;
    }
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_LIST;
    ((struct a_ipc_hdr *)msg)->len = 28;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = DS_PROTO_MAGIC;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 3;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = (u16)DS_MSG_LIST;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 28;
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 28u) == 0) {
        bad++;
    }
    /* 7) 头部长度自相矛盾：IPC h.len 与 display mlen 不一致。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_CREATE;
    ((struct a_ipc_hdr *)msg)->len = 64;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[0] = DS_PROTO_MAGIC;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[1] = 3;
    ((u16 *)(msg + DS_IPC_HDR_SIZE))[2] = (u16)DS_MSG_CREATE;
    ((u32 *)(msg + DS_IPC_HDR_SIZE))[2] = 24; /* 说 24，实际 64 */
    if (a_send_raw(disp, msg, DS_IPC_HDR_SIZE + 64u) == 0) {
        bad++;
    }
    /* 8) 头部 h.len 超过 IPC payload 上限（内核应直接 EMSGSIZE）。 */
    ds_memzero(msg, (u64)sizeof(msg));
    ((struct a_ipc_hdr *)msg)->type = DS_MSG_LIST;
    ((struct a_ipc_hdr *)msg)->len = 0xFFFFFFFFu;
    ((struct a_ipc_hdr *)msg)->from = g_port;
    (void)a_send_raw(disp, msg, DS_IPC_HDR_SIZE);

    /* 攻击完必须还能 LIST。 */
    {
        struct ds_msg r;

        a_check(a_list(disp, &r), "display malformed", bad);
    }
}

/*
 * 状态文件投毒：WM_STATUS/SESSION_STATUS 的字段都是客户端给的 u32，displayd
 * 会把它们拼进 /run/display.status。全 0xFFFFFFFF 时旧实现的最坏长度约
 * 573 字节 > 512 字节栈缓冲（栈溢出）。修复后必须截断且继续服务。
 */
static void a_attack_status_poison(u32 disp)
{
    struct ds_msg m;
    struct ds_msg r;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_WM_STATUS;
    m.u.wm_status.surface_id = 0xFFFFFFFFu;
    m.u.wm_status.tasks = 0xFFFFFFFFu;
    m.u.wm_status.focus = 0xFFFFFFFFu;
    m.u.wm_status.switches = 0xFFFFFFFFu;
    m.u.wm_status.lastswitch = 0xFFFFFFFFu;
    m.u.wm_status.launches = 0xFFFFFFFFu;
    m.u.wm_status.lastpid = 0xFFFFFFFFu;
    (void)a_send_ds(disp, &m, g_port);

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_SESSION_STATUS;
    m.u.session_status.pid = 0xFFFFFFFFu;
    m.u.session_status.dpid = 0xFFFFFFFFu;
    m.u.session_status.procs = 0xFFFFFFFFu;
    m.u.session_status.apps = 0xFFFFFFFFu;
    m.u.session_status.windows = 0xFFFFFFFFu;
    m.u.session_status.tokens = 0xFFFFFFFFu;
    m.u.session_status.launches = 0xFFFFFFFFu;
    m.u.session_status.kills = 0xFFFFFFFFu;
    m.u.session_status.exits = 0xFFFFFFFFu;
    m.u.session_status.reaps = 0xFFFFFFFFu;
    m.u.session_status.restarts = 0xFFFFFFFFu;
    m.u.session_status.wmpid = 0xFFFFFFFFu;
    m.u.session_status.wmstate = 0xFFFFFFFFu;
    m.u.session_status.apppid = 0xFFFFFFFFu;
    m.u.session_status.tokenfail = 0xFFFFFFFFu;
    m.u.session_status.registry = 0xFFFFFFFFu;
    m.u.session_status.reserved = 0xFFFFFFFFu;
    (void)a_send_ds(disp, &m, g_port);

    a_sleep_ms(80u);
    ds_memzero(&r, (u64)sizeof(r));
    a_check(a_list(disp, &r), "display status-poison", r.u.list_reply.count);
}

/*
 * CREATE 几何/标题/表满 + LIST 小缓冲/端口背压。返回 1 = 全部通过。
 */
static void a_attack_create_geom(u32 disp)
{
    static const u32 bad_geom[][2] = {
        {0u, 300u},       {400u, 0u},          {0xFFFFFFFFu, 1u},
        {1u, 0xFFFFFFFFu}, {1281u, 800u},      {1280u, 801u},
        {0x80000000u, 0x80000000u}, {2000u, 2000u}};
    u32 i;
    u32 rejects = 0;

    for (i = 0; i < sizeof(bad_geom) / sizeof(bad_geom[0]); i++) {
        u32 id = 0;
        i32 st = 0;

        if (a_create(disp, bad_geom[i][0], bad_geom[i][1], 0, 0, 0, 0, NULL,
                     0, &id, NULL, NULL, &st) &&
            st != 0 && id == 0) {
            rejects++;
        }
    }
    a_check(rejects == (u32)(sizeof(bad_geom) / sizeof(bad_geom[0])),
            "display badgeom", rejects);

    /* 非法形态的 role_token（seq/nonce/role 位域不合法）必须被 CREATE 拒绝。 */
    {
        static const u32 bad_tok[] = {0xDEADBEEFu, 0x00010000u, 0x12340500u};
        u32 i;
        u32 tok_rejects = 0;

        for (i = 0; i < sizeof(bad_tok) / sizeof(bad_tok[0]); i++) {
            u32 id = 0;
            i32 st = 0;

            if (a_create(disp, 8, 8, 0, 0, 0, bad_tok[i], "bad", 0, &id,
                         NULL, NULL, &st) &&
                st != 0 && id == 0) {
                tok_rejects++;
            }
        }
        a_check(tok_rejects == 3u, "display bad-token", tok_rejects);
    }

    /* 超大/越界 x,y（POS flag）：必须贴边（裁剪到屏幕内）而不是越界写。 */
    {
        u32 id = 0;
        u32 shm = 0;
        u32 pages = 0;
        i32 st = 0;
        struct ds_msg r;
        int in_bounds = 0;

        ds_memzero(&r, (u64)sizeof(r));
        if (a_create(disp, 64, 48, DS_CREATE_F_POS, 0x7FFFFFF0u, 0x7FFFFFF0u,
                     0, "pos", 0, &id, &shm, &pages, &st) &&
            st == 0 && id != 0) {
            /* 映射+提交一次，确保坐标被真正用于合成（不能越界）。 */
            struct ds_msg m;

            ds_memzero(&m, (u64)sizeof(m));
            m.type = DS_MSG_MAP;
            m.u.map.surface_id = id;
            (void)a_send_ds(disp, &m, g_port);
            ds_memzero(&m, (u64)sizeof(m));
            m.type = DS_MSG_COMMIT;
            m.u.commit.surface_id = id;
            m.u.commit.x = 0;
            m.u.commit.y = 0;
            m.u.commit.w = 64;
            m.u.commit.h = 48;
            (void)a_send_ds(disp, &m, g_port);

            if (a_list(disp, &r)) {
                u32 k;

                for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
                    const struct ds_list_entry *le = &r.u.list_reply.entries[k];

                    if (le->id == id) {
                        in_bounds = (le->x + le->w <= 1280u &&
                                     le->y + le->h <= 800u)
                                        ? 1
                                        : 0;
                    }
                }
            }
            ds_memzero(&m, (u64)sizeof(m));
            m.type = DS_MSG_DESTROY;
            m.u.destroy.surface_id = id;
            (void)a_send_ds(disp, &m, g_port);
        }
        a_check(in_bounds, "display pos-clamp", id);
    }

    /* 标题：16 个无 NUL 的字节必须被截成 15 + NUL。 */
    {
        u32 id = 0;
        u32 shm = 0;
        u32 pages = 0;
        i32 st = 0;
        struct ds_msg r;
        u32 title_len = 0;
        int all_a = 0;

        if (a_create(disp, 400, 300, 0, 0, 0, 0, NULL, 1, &id, &shm, &pages,
                     &st) &&
            st == 0 && id != 0 && a_list(disp, &r)) {
            u32 k;

            for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
                if (r.u.list_reply.entries[k].id == id) {
                    char t[DS_TITLE_MAX];
                    u32 j;

                    ds_title_get(r.u.list_reply.entries[k].title, t,
                                 (u32)sizeof(t));
                    title_len = a_strlen(t);
                    all_a = 1;
                    for (j = 0; j < title_len; j++) {
                        if (t[j] != 'A') {
                            all_a = 0;
                        }
                    }
                }
            }
            {
                struct ds_msg m;

                ds_memzero(&m, (u64)sizeof(m));
                m.type = DS_MSG_DESTROY;
                m.u.destroy.surface_id = id;
                (void)a_send_ds(disp, &m, g_port);
            }
        }
        a_check(title_len == (DS_TITLE_MAX - 1u) && all_a, "display title",
                title_len);
    }

    /*
     * 第二轮：标题里的 CR/LF/控制符不能在串口上伪造证据行（displayd 打印前
     * 净化）。真正断言在宿主侧（serial 里不能出现以 "PANIC:" 开头的行），
     * 这里只负责把这段 payload 打进协议。
     */
    {
        u32 id = 0;
        u32 shm = 0;
        u32 pages = 0;
        i32 st = 0;

        if (a_create(disp, 8, 8, 0, 0, 0, 0, "\r\nPANIC: vec=0xe", 0, &id,
                     &shm, &pages, &st) &&
            st == 0 && id != 0) {
            struct ds_msg d;

            ds_memzero(&d, (u64)sizeof(d));
            d.type = DS_MSG_DESTROY;
            d.u.destroy.surface_id = id;
            (void)a_send_ds(disp, &d, g_port);
        }
        a_check(id != 0, "display title-inject", id);
    }

    /* LIST 小缓冲 + 端口背压：depth=1 的端口收 12 条 LIST_REPLY（11 条应
     * 被 displayd 以非阻塞方式丢掉），再用小缓冲截断读一条，最后确认服务
     * 仍然可用。 */
    {
        u32 p1 = a_port_create(1);
        struct ds_msg m;
        u8 small[32];
        i64 r;
        u32 sent = 0;
        u32 i;
        struct ds_msg rl;

        if (p1 != 0) {
            ds_memzero(&m, (u64)sizeof(m));
            m.type = DS_MSG_LIST;
            for (i = 0; i < 12u; i++) {
                if (a_send_ds(disp, &m, p1) == 0) {
                    sent++;
                }
            }
            r = a_recv_raw(p1, small, (u32)sizeof(small), DS_REPLY_WAIT_TICKS);
            /* 截断读：内核返回完整长度（>32），但只拷贝了 32 字节。 */
            a_check(sent == 12u && r > (i64)sizeof(small) &&
                        a_list(disp, &rl) != 0,
                    "display smallbuf", (u32)r);
            (void)a_sys(A_SYS_PORT_CLOSE, (i64)p1, 0, 0);
        } else {
            a_check(0, "display smallbuf", 0);
        }
    }
}

/* 表满：单独的 displayd（8 个 surface = 8 个 SHM 对象，正好耗干对象池）。 */
static void a_attack_table(u32 disp)
{
    u32 ids[DS_SURFACES_MAX];
    u32 i;
    u32 made = 0;
    u32 extra_id = 0;
    i32 extra_st = 0;
    struct ds_msg r;

    for (i = 0; i < DS_SURFACES_MAX; i++) {
        ids[i] = 0;
        if (a_create(disp, 8, 8, 0, 0, 0, 0, "", 0, &ids[i], NULL, NULL,
                     NULL) &&
            ids[i] != 0) {
            made++;
        }
    }
    (void)a_create(disp, 8, 8, 0, 0, 0, 0, "", 0, &extra_id, NULL, NULL,
                   &extra_st);
    {
        u32 full_ok = (made == DS_SURFACES_MAX && extra_st != 0);
        u32 destroyed = 0;

        for (i = 0; i < DS_SURFACES_MAX; i++) {
            struct ds_msg d;

            if (ids[i] == 0) {
                continue;
            }
            ds_memzero(&d, (u64)sizeof(d));
            d.type = DS_MSG_DESTROY;
            d.u.destroy.surface_id = ids[i];
            (void)a_send_ds(disp, &d, g_port);
            destroyed++;
        }
        if (extra_id != 0) {
            struct ds_msg d;

            ds_memzero(&d, (u64)sizeof(d));
            d.type = DS_MSG_DESTROY;
            d.u.destroy.surface_id = extra_id;
            (void)a_send_ds(disp, &d, g_port);
        }
        a_sleep_ms(60u);
        ds_memzero(&r, (u64)sizeof(r));
        a_check(full_ok && a_list(disp, &r) && r.u.list_reply.count == 0u,
                "display table", made * 100u + destroyed);
    }
}

/* MAP/COMMIT/FOCUS/DESTROY 的坏 id / 越界矩形 / 假令牌。 */
static void a_attack_badops(u32 disp)
{
    struct ds_msg m;
    struct ds_msg r;
    u32 id = 0;
    u32 shm = 0;
    u32 pages = 0;
    i32 st = 0;

    if (!a_create(disp, 8, 8, 0, 0, 0, 0, "badops", 0, &id, &shm, &pages,
                  &st) ||
        st != 0 || id == 0) {
        a_check(0, "display badops", 0);
        return;
    }
    /* MAP 坏 id。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_MAP;
    m.u.map.surface_id = 999u;
    (void)a_send_ds(disp, &m, g_port);
    /* COMMIT 坏 id；合法 id 但矩形完全越界/空。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = 999u;
    (void)a_send_ds(disp, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = id;
    m.u.commit.x = 0xFFFFFFFFu;
    m.u.commit.y = 0xFFFFFFFFu;
    m.u.commit.w = 0xFFFFFFFFu;
    m.u.commit.h = 0xFFFFFFFFu;
    (void)a_send_ds(disp, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = id;
    m.u.commit.w = 0;
    m.u.commit.h = 0;
    (void)a_send_ds(disp, &m, g_port);
    /* FOCUS 坏 id / 假令牌。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.u.focus.surface_id = 999u;
    m.u.focus.token = 0x0001DEADu;
    (void)a_send_ds(disp, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.u.focus.surface_id = id;
    m.u.focus.token = 0x0001DEADu;
    (void)a_send_ds(disp, &m, g_port);
    /* DESTROY 坏 id。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = 999u;
    (void)a_send_ds(disp, &m, g_port);
    a_sleep_ms(60u);

    /* 正常路径仍然可用：MAP + COMMIT 整块，然后 LIST。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_MAP;
    m.u.map.surface_id = id;
    (void)a_send_ds(disp, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = id;
    m.u.commit.w = 8;
    m.u.commit.h = 8;
    (void)a_send_ds(disp, &m, g_port);
    ds_memzero(&r, (u64)sizeof(r));
    a_check(a_list(disp, &r) && r.u.list_reply.count >= 1u, "display badops",
            r.u.list_reply.count);

    /* 清场。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = id;
    (void)a_send_ds(disp, &m, g_port);
}

/* DESTROY 归属 + REGISTER_HOTKEY 令牌。 */
static void a_attack_authz(u32 disp)
{
    u32 a = 0;
    u32 b = 0;
    u32 w = 0;
    u32 p2 = g_port2;
    struct ds_msg m;
    struct ds_msg r;
    u32 found_b = 0;
    u32 found_a = 0;
    u32 found_w = 0;

    if (!a_create(disp, 8, 8, 0, 0, 0, 0, "A", 0, &a, NULL, NULL, NULL) ||
        !a_create(disp, 8, 8, 0, 0, 0, 0, "B", 0, &b, NULL, NULL, NULL) ||
        p2 == 0) {
        a_check(0, "destroy auth", 0);
        return;
    }

    /* 外来端口用 token=0 销毁 B：displayd 必须拒绝（归属不符）。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = b;
    (void)a_send_ds(disp, &m, p2);
    a_sleep_ms(60u);
    if (a_list(disp, &r)) {
        u32 k;

        for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
            if (r.u.list_reply.entries[k].id == b) {
                found_b = 1;
            }
        }
    }
    a_check(found_b, "destroy auth", b);

    /* 建一个"WM 面板"（带 WM 角色令牌的 surface），displayd 会记住该令牌。 */
    if (!a_create(disp, 8, 8, 0, 0, 0, A_FAKE_WM_TOKEN, "W", 0, &w, NULL,
                  NULL, NULL)) {
        a_check(0, "destroy wm-token", 0);
        return;
    }
    /*
     * 第二轮：WM 角色令牌**只认领一次**。已经有 WM 令牌时，另一个客户端
     * 拿另一个"形状合法"的 WM 令牌 CREATE 必须被拒（以前会覆盖 g_wm_token，
     * 于是任何应用都能抢走 WM 的跨窗口权限 + 全局热键通道）。
     */
    {
        u32 w2 = 0;
        i32 st2 = 0;
        u32 found_w2 = 0;

        (void)a_create_from(disp, p2, 8, 8, A_FAKE_WM_TOKEN2, "W2", &w2,
                            &st2);
        if (a_list(disp, &r)) {
            u32 k;

            for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
                if (r.u.list_reply.entries[k].id == w2 && w2 != 0) {
                    found_w2 = 1;
                }
            }
        }
        a_check(st2 != 0 && w2 == 0 && found_w2 == 0, "display wm-claim",
                (u32)st2);
    }
    /* WM 令牌可以销毁别人的窗口。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = a;
    m.u.destroy.token = A_FAKE_WM_TOKEN;
    (void)a_send_ds(disp, &m, p2);
    a_sleep_ms(60u);
    if (a_list(disp, &r)) {
        u32 k;

        for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
            if (r.u.list_reply.entries[k].id == a) {
                found_a = 1;
            }
            if (r.u.list_reply.entries[k].id == w) {
                found_w = 1;
            }
        }
    }
    a_check(!found_a && found_w, "destroy wm-token", a);

    /* 热键：token=0 的外来注册必须被拒；WM 令牌可以；然后洪泛 40 次。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_REGISTER_HOTKEY;
    m.u.register_hotkey.surface_id = w;
    m.u.register_hotkey.key = DS_HOTKEY_ANY;
    m.u.register_hotkey.token = 0;
    (void)a_send_ds(disp, &m, p2);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_REGISTER_HOTKEY;
    m.u.register_hotkey.surface_id = w;
    m.u.register_hotkey.key = DS_HOTKEY_ANY;
    m.u.register_hotkey.token = A_FAKE_WM_TOKEN;
    (void)a_send_ds(disp, &m, g_port);
    a_sleep_ms(60u);
    {
        u32 i;
        u32 accepted = 1;
        u32 sent = 0;

        for (i = 0; i < 40u; i++) {
            struct ds_msg h;
            u32 tries;

            ds_memzero(&h, (u64)sizeof(h));
            h.type = DS_MSG_REGISTER_HOTKEY;
            h.u.register_hotkey.surface_id = w;
            h.u.register_hotkey.key = DS_HOTKEY_ANY;
            h.u.register_hotkey.token = A_FAKE_WM_TOKEN;
            /*
             * 端口只有 16 深：洪泛时内核会给 EAGAIN（这是正常的背压，不是
             * displayd 的缺陷）。每条最多重试 8 次，只有真的送不进去才算失败。
             */
            for (tries = 0; tries < 8u; tries++) {
                if (a_send_ds(disp, &h, g_port) == 0) {
                    sent++;
                    break;
                }
                a_sleep_ms(10u);
            }
            if (tries == 8u) {
                accepted = 0;
            }
        }
        a_check(a_list(disp, &r) && accepted && sent == 40u, "hotkey flood",
                40u);
    }

    /* 销毁 WM 面板（用它的令牌）后再用 token=0 注册：回到 v2 兼容路径。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = w;
    m.u.destroy.token = A_FAKE_WM_TOKEN;
    (void)a_send_ds(disp, &m, p2);
    a_sleep_ms(60u);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_REGISTER_HOTKEY;
    m.u.register_hotkey.surface_id = 0;
    m.u.register_hotkey.key = DS_HOTKEY_ANY;
    m.u.register_hotkey.token = 0;
    (void)a_send_ds(disp, &m, p2);
    a_check(a_list(disp, &r), "hotkey legacy", 0u);

    /*
     * 第二轮：WM 面板销毁后，WM 令牌必须被回收；带令牌的会话窗口不能被
     * "token=0 + 知道 id" 或"旧 WM 令牌"抢焦点。
     *   - 以前 g_wm_token 永不回收（销毁后再读已清零的槽位，条件恒假）；
     *   - 以前 FOCUS 的 token=0 对任何窗口都放行。
     *
     * 注意 SHM 对象池只有 8 个（src/ng/shm.h，DESTROY 不释放）：这个 displayd
     * 到此时已经用掉 7 个（pos/title/title-inject/badops/a/b/w），所以这里
     * **只能再建一个** surface（V）。焦点锚点复用仍然活着的无令牌窗口 B。
     */
    {
        u32 v = 0;
        u32 shm = 0;
        u32 pages = 0;
        i32 st = 0;
        u32 found_v = 0;
        u32 focused_v = 0;
        u32 focused_b = 0;
        u32 own_ok = 0;

        /* 锚点：把焦点放到无令牌的 B 上（v1/v2 兼容路径，必须成功）。 */
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_FOCUS;
        m.u.focus.surface_id = b;
        (void)a_send_ds(disp, &m, g_port);
        a_sleep_ms(50u);
        /* 最后一个可用 surface：带 APP 令牌的 V。 */
        if (!a_create(disp, 8, 8, 0, 0, 0, A_APP_TOKEN, "V", 0, &v, &shm,
                      &pages, &st) ||
            st != 0 || v == 0) {
            a_check(0, "display focus-token", 0);
            goto authz_done;
        }
        /* 1) 外来端口 token=0：拒绝（V 有令牌）。 */
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_FOCUS;
        m.u.focus.surface_id = v;
        (void)a_send_ds(disp, &m, p2);
        a_sleep_ms(50u);
        /* 2) 旧的 WM 令牌（面板已销毁）：也必须拒绝。 */
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_FOCUS;
        m.u.focus.surface_id = v;
        m.u.focus.token = A_FAKE_WM_TOKEN;
        (void)a_send_ds(disp, &m, p2);
        a_sleep_ms(50u);
        if (a_list(disp, &r)) {
            u32 k;

            for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
                if (r.u.list_reply.entries[k].id == v) {
                    found_v = 1;
                    focused_v = r.u.list_reply.entries[k].focused;
                }
                if (r.u.list_reply.entries[k].id == b) {
                    focused_b = r.u.list_reply.entries[k].focused;
                }
            }
        }
        a_check(found_v != 0 && focused_v == 0 && focused_b == 1,
                "display focus-token", focused_v * 10u + focused_b);
        /* 3) 正例：窗口自己的令牌可以改焦点。 */
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_FOCUS;
        m.u.focus.surface_id = v;
        m.u.focus.token = A_APP_TOKEN;
        (void)a_send_ds(disp, &m, g_port);
        a_sleep_ms(50u);
        if (a_list(disp, &r)) {
            u32 k;

            for (k = 0; k < r.u.list_reply.count && k < DS_LIST_MAX; k++) {
                if (r.u.list_reply.entries[k].id == v) {
                    own_ok = r.u.list_reply.entries[k].focused;
                }
            }
        }
        a_check(own_ok == 1, "display focus-own", own_ok);
        /* 清场：带令牌的窗口用自己的令牌销毁。 */
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_DESTROY;
        m.u.destroy.surface_id = v;
        m.u.destroy.token = A_APP_TOKEN;
        (void)a_send_ds(disp, &m, g_port);
    }
authz_done:
    /* 清场：B 由创建者端口（g_port，token=0 的旧式窗口）销毁。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.u.destroy.surface_id = b;
    (void)a_send_ds(disp, &m, g_port);
}

/* ------------------------------------------------------------------------- */
/* 共享内存攻击                                                               */
/* ------------------------------------------------------------------------- */

static void a_child_shm_oob(u32 handle, u32 pages)
{
    u32 va = 0;
    i64 rc;
    volatile u32 *px;
    u32 bad_off;

    rc = a_sys4(A_SYS_SHM_MAP, (i64)handle, (i64)&va, 0, 0);
    if (rc != 0 || va == 0) {
        a_put("ng: audit shm-oob map FAIL\r\n");
        (void)a_sys(A_SYS_EXIT, 3, 0, 0);
    }
    a_put("ng: audit shm-oob child up (va=");
    a_puthex(va);
    a_put(")\r\n");
    /* 越过映射末尾一页（bump 分配器留的守护洞）：必须 #PF，且只杀本进程。 */
    bad_off = (pages + 1u) * DS_SHM_PAGE_SIZE;
    px = (volatile u32 *)(usize)(va + bad_off);
    *px = 0xDEADBEEFu;
    /* 没崩才走到这里（修复前/内核 bug 的证据）。 */
    a_put("ng: audit shm-oob survived\r\n");
    (void)a_sys(A_SYS_EXIT, 0, 0, 0);
}

static void a_attack_shm(u32 disp)
{
    u32 id = 0;
    u32 shm = 0;
    u32 pages = 0;
    i32 st = 0;
    u32 va = 0;
    i64 rc;
    volatile u32 *px;

    if (!a_create(disp, 64, 48, 0, 0, 0, 0, "shm", 0, &id, &shm, &pages,
                  &st) ||
        st != 0 || shm == 0 || pages == 0) {
        a_check(0, "shm map", 0);
        return;
    }
    rc = a_sys4(A_SYS_SHM_MAP, (i64)shm, (i64)&va, 0, 0);
    if (rc != 0 || va == 0) {
        a_check(0, "shm map", (u32)(-rc));
        return;
    }
    px = (volatile u32 *)(usize)va;
    px[0] = 0xA5A5C3C3u;
    a_check(px[0] == 0xA5A5C3C3u && pages * 4096u >= 64u * 48u * 4u,
            "shm map", pages);

    /* 子进程越界写：应该被 #PF 杀掉，displayd 与父进程不受影响。 */
    if (g_do_oob != 0)
    {
        char arg1[16];
        char arg2[16];
        const char *argv[5];
        u32 child;
        int status = 0;

        (void)a_u32str(shm, arg1);
        (void)a_u32str(pages, arg2);
        argv[0] = A_SELF;
        argv[1] = "shm-oob";
        argv[2] = arg1;
        argv[3] = arg2;
        argv[4] = NULL;
        child = a_spawn(A_SELF, argv);
        if (child == 0) {
            a_check(0, "shm oob contained", 0);
        } else {
            int rcw = a_wait_pid(child, &status);

            a_sleep_ms(80u);
            a_check(rcw == 0 && status != 0, "shm oob contained",
                    (u32)status);
        }
    }

    /* 坏 handle。 */
    {
        static const u32 bad[] = {0u, 0xFFFFFFFFu, 0x00010020u};
        u32 i;
        u32 rejected = 0;

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            u32 tmp = 0;
            i64 r = a_sys4(A_SYS_SHM_MAP, (i64)bad[i], (i64)&tmp, 0, 0);

            if (r != 0) {
                rejected++;
            }
        }
        a_check(rejected == 3u, "shm bad-handle", rejected);
    }

    /* 杀 owner（displayd）：本进程的映射必须仍然可写；重新 MAP 同一个
     * handle（refcnt 依赖）也必须成功。 */
    if (g_displayd_pid != 0) {
        (void)a_sys4(A_SYS_KILL, (i64)g_displayd_pid, 9, 0, 0);
        a_sleep_ms(300u);
        px[1] = 0x5A5A1234u;
        {
            u32 va2 = 0;
            i64 r2 = a_sys4(A_SYS_SHM_MAP, (i64)shm, (i64)&va2, 0, 0);

            a_check(px[1] == 0x5A5A1234u && r2 == 0 && va2 != 0,
                    "shm owner-exit", (u32)(-r2));
        }
        {
            int status = 0;

            (void)a_wait_pid(g_displayd_pid, &status);
        }
        g_displayd_pid = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* session 协议攻击                                                           */
/* ------------------------------------------------------------------------- */

/*
 * 端口 ticket = (gen << 16) | slot。内核的 IPC 检查 h.from 是否是"某个存活
 * 端口"，但不限制发送者只能用自己的端口 —— 所以我们可以枚举/发现 sessiond
 * 的主端口（这一步本身就是审计要记录的攻击面）。
 *
 * 先向所有候选 ticket 发一条 SESSION_MSG_LIST（from=g_port），再收回复：
 * s_reply_sess 把 IPC 头 from 填成 sessiond 的主端口 ticket。
 */
static u32 a_discover_session_port(void)
{
    u32 attempt;
    u32 gen;
    u32 slot;

    for (attempt = 0; attempt < 3u; attempt++) {
        for (gen = 1; gen <= A_ENUM_GENS; gen++) {
            for (slot = 0; slot < A_ENUM_SLOTS; slot++) {
                u32 ticket = (gen << 16) | slot;
                struct session_msg m;

                if (ticket == 0 || ticket == g_port || ticket == g_port2 ||
                    ticket == g_sleep_port) {
                    continue;
                }
                ds_memzero(&m, (u64)sizeof(m));
                m.type = SESSION_MSG_LIST;
                (void)a_send_sess(ticket, &m, g_port);
            }
        }
        {
            u32 i;

            for (i = 0; i < 100u; i++) {
                struct session_msg r;
                u32 from = 0;
                int t = a_recv_sess(g_port, &r, &from, DS_REPLY_WAIT_TICKS);

                if (t == (int)SESSION_MSG_LIST_REPLY && from != 0) {
                    return from;
                }
            }
        }
    }
    return 0;
}

/* 把伪造的 LIST_REPLY（count=0xFFFFFFFF）广播给所有候选 DS 端口。 */
static void a_poison_list_reply(void)
{
    u32 gen;
    u32 slot;

    for (gen = 1; gen <= A_ENUM_GENS; gen++) {
        for (slot = 0; slot < A_ENUM_SLOTS; slot++) {
            u32 ticket = (gen << 16) | slot;
            struct ds_msg m;

            if (ticket == 0 || ticket == g_port || ticket == g_port2 ||
                ticket == g_sleep_port) {
                continue;
            }
            ds_memzero(&m, (u64)sizeof(m));
            m.type = DS_MSG_LIST_REPLY;
            m.u.list_reply.count = 0xFFFFFFFFu;
            (void)a_send_ds(ticket, &m, g_port);
        }
    }
}

/*
 * 第二轮：伪造**旧版本（v2）**的 LIST_REPLY 并广播。
 *
 * v2 的 entry 是 44 字节（没有 token），v3 是 48 字节。sessiond 以前只按
 * v3 的 struct 读 entries[]，收到一条 v2 消息后从 entry[1] 起全部错位 ——
 * 于是把垃圾当成 token/owner_pid，token_fail++ 并打出
 * "ng: session registry mismatch"（内核 wp33 读的 registry/tokenfail 证据被
 * 搅乱）。修复后 sessiond 只接受 version>=3 的 LIST_REPLY，这条消息被忽略。
 *
 * 这里手工拼字节（ds_msg_encode 只会编 v3），body 360 字节全 0xAB：
 * 按 v3 结构读 entry[0].token 会拿到 title 区的 0xABABABAB != 0。
 */
static void a_poison_list_reply_v2(void)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_HDR_SIZE + DS_LIST_REPLY_BODY_V2];
    struct a_ipc_hdr *ih = (struct a_ipc_hdr *)buf;
    u8 *p = buf + DS_IPC_HDR_SIZE;
    u32 body = DS_LIST_REPLY_BODY_V2; /* 360 */
    u32 total = DS_MSG_HDR_SIZE + body;
    u32 magic = DS_PROTO_MAGIC;
    u16 version = 2;
    u16 type = (u16)DS_MSG_LIST_REPLY;
    u32 seq = 1;
    i32 status = 0;
    u32 count = 1;
    u32 reserved = 0;
    u32 gen;
    u32 slot;

    ds_memzero(buf, (u64)sizeof(buf));
    /* display 消息头（20 字节，小端字段）。 */
    ds_memcopy(p + 0, &magic, 4);
    ds_memcopy(p + 4, &version, 2);
    ds_memcopy(p + 6, &type, 2);
    ds_memcopy(p + 8, &total, 4);
    ds_memcopy(p + 12, &status, 4);
    ds_memcopy(p + 16, &seq, 4);
    /* body：count + reserved + 360 字节全 0xAB。 */
    ds_memcopy(p + DS_MSG_HDR_SIZE + 0, &count, 4);
    ds_memcopy(p + DS_MSG_HDR_SIZE + 4, &reserved, 4);
    {
        u32 i;

        for (i = 8u; i < body; i++) {
            p[DS_MSG_HDR_SIZE + i] = 0xABu;
        }
    }
    /* IPC 头。 */
    ih->type = DS_MSG_LIST_REPLY;
    ih->len = total;
    ih->from = g_port;
    ih->flags = 0;

    for (gen = 1; gen <= A_ENUM_GENS; gen++) {
        for (slot = 0; slot < A_ENUM_SLOTS; slot++) {
            u32 ticket = (gen << 16) | slot;

            if (ticket == 0 || ticket == g_port || ticket == g_port2 ||
                ticket == g_sleep_port) {
                continue;
            }
            (void)a_send_raw(ticket, buf,
                             (u32)(DS_IPC_HDR_SIZE + total));
        }
    }
}

static int a_session_alive(u32 sess, struct session_msg *out, u32 *out_from)
{
    struct session_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_LIST;
    if (a_send_sess(sess, &m, g_port) != 0) {
        return 0;
    }
    return a_recv_sess(g_port, out, out_from, DS_POLL_TICKS) ==
                   (int)SESSION_MSG_LIST_REPLY ||
           a_wait_sess(g_port, SESSION_MSG_LIST_REPLY, out,
                       DS_REPLY_WAIT_TRIES);
}

static void a_attack_session(void)
{
    const char *argv[2];
    u32 sess;
    struct session_msg m;
    struct session_msg r;
    u32 from = 0;
    u32 wm_surface = 0;
    u32 bad_tokens = 0;
    u32 tokens_exported = 0;
    u32 i;

    argv[0] = A_SESSIOND;
    argv[1] = NULL;
    g_sessiond_pid = a_spawn(A_SESSIOND, argv);
    if (g_sessiond_pid == 0) {
        a_check(0, "session spawn", 0);
        return;
    }
    /* 等 sessiond 拉起的新 displayd 写好端口文件。 */
    {
        u32 old = (u32)a_read_display_ticket();
        u32 t = a_wait_display_ticket((int)old);

        if (t == 0) {
            a_check(0, "session spawn", g_sessiond_pid);
            return;
        }
    }
    a_check(1, "session spawn", g_sessiond_pid);

    sess = a_discover_session_port();
    if (sess == 0) {
        a_check(0, "session discover", 0);
        return;
    }
    a_check(1, "session discover", sess);

    /* 1) 坏令牌：REGISTER/HEARTBEAT/EXIT/LAUNCH/KILL 全部拒绝但不崩。 */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_REGISTER;
    m.u.reg.token = 0xDEADBEEFu;
    m.u.reg.port = g_port;
    (void)a_send_sess(sess, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    m.u.hb.token = 0xDEADBEEFu;
    (void)a_send_sess(sess, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_EXIT;
    m.u.exit.token = 0xDEADBEEFu;
    (void)a_send_sess(sess, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_LAUNCH;
    m.u.launch.token = session_token_make(SESSION_ROLE_APP, 1, 1);
    ds_memcopy(m.u.launch.path, "/bin/demo", 10);
    (void)a_send_sess(sess, &m, g_port);
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_KILL;
    m.u.kill.token = session_token_make(SESSION_ROLE_APP, 2, 2);
    m.u.kill.pid = g_pid;
    (void)a_send_sess(sess, &m, g_port);
    a_sleep_ms(100u);
    if (a_session_alive(sess, &r, &from)) {
        bad_tokens = 1;
    }
    a_check(bad_tokens, "session bad-token", 1u);

    /* 2) 令牌不导出：会话 LIST/LOOKUP 的 token 字段必须为 0。
     *    先等 WM 注册（sessiond 在 displayd ping 通之后才 spawn WM）。 */
    for (i = 0; i < 100u && wm_surface == 0; i++) {
        if (a_session_alive(sess, &r, &from)) {
            u32 k;

            for (k = 0; k < r.u.list_reply.count &&
                        k < SESSION_MAX_ENTRIES;
                 k++) {
                if (r.u.list_reply.entries[k].token != 0) {
                    tokens_exported++;
                }
                if (r.u.list_reply.entries[k].role == SESSION_ROLE_WM &&
                    r.u.list_reply.entries[k].surface_id != 0) {
                    wm_surface = r.u.list_reply.entries[k].surface_id;
                }
            }
        }
        if (wm_surface == 0) {
            a_sleep_ms(40u);
        }
    }
    if (wm_surface != 0) {
        ds_memzero(&m, (u64)sizeof(m));
        m.type = SESSION_MSG_LOOKUP;
        m.u.lookup.surface_id = wm_surface;
        if (a_send_sess(sess, &m, g_port) == 0 &&
            a_wait_sess(g_port, SESSION_MSG_ENTRY, &r, DS_REPLY_WAIT_TRIES)) {
            if (r.u.entry.found != 0 && r.u.entry.info.token != 0) {
                tokens_exported++;
            }
        }
    }
    a_check(tokens_exported == 0u && wm_surface != 0, "session token-export",
            tokens_exported);

    /* 3) 伪造 LIST_REPLY（count 超大）广播到 sessiond 的 display 客户端端口：
     * 修复前会让 session_sync_list 越界读 -> sessiond 崩。 */
    a_poison_list_reply();
    a_sleep_ms(1000u);
    /* 清掉广播到自己端口里的垃圾（解码失败的/DS 消息）。 */
    for (i = 0; i < 8u; i++) {
        u8 junk[DS_IPC_HDR_SIZE + DS_MSG_MAX];

        (void)a_recv_raw(g_port, junk, (u32)sizeof(junk), DS_POLL_TICKS);
    }
    a_check(a_session_alive(sess, &r, &from), "session count-clamp", 1u);

    /*
     * 3b) 第二轮：伪造**旧版本（v2）**的 LIST_REPLY 广播。
     * v2 的 entry 44 字节、v3 是 48 字节；sessiond 以前按 v3 布局解析，
     * 从 entry[1] 起全部错位 -> token_fail++ / "session registry mismatch"
     * （内核 wp33 读的 registry 证据被搅乱）。真正的断言在宿主侧：
     * 串口里不允许出现 "session registry mismatch"。
     */
    a_poison_list_reply_v2();
    a_sleep_ms(600u);
    a_check(a_session_alive(sess, &r, &from), "session v2-poison", 2u);

    /* 4) KILL 风暴：64 条坏令牌 KILL + REGISTER 洪泛，然后仍然存活。 */
    for (i = 0; i < 64u; i++) {
        ds_memzero(&m, (u64)sizeof(m));
        m.type = SESSION_MSG_KILL;
        m.u.kill.token = 0xDEAD0000u | i;
        m.u.kill.pid = g_pid;
        (void)a_send_sess(sess, &m, g_port);
        ds_memzero(&m, (u64)sizeof(m));
        m.type = SESSION_MSG_REGISTER;
        m.u.reg.token = 0xBEEF0000u | i;
        m.u.reg.port = g_port;
        (void)a_send_sess(sess, &m, g_port);
    }
    a_sleep_ms(120u);
    a_check(a_session_alive(sess, &r, &from), "session kill-storm", 64u);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int a_main(long argc, char **argv)
{
    u32 disp;

    g_pid = (u32)a_sys(A_SYS_GETPID, 0, 0, 0);
    g_checks = 0;
    g_fails = 0;

    g_port = a_port_create(DS_CLIENT_PORT_DEPTH);
    g_port2 = a_port_create(1);
    g_sleep_port = a_port_create(1);
    if (g_port == 0 || g_port2 == 0 || g_sleep_port == 0) {
        a_put("ng: audit ui FAIL (ports)\r\n");
        (void)a_sys(A_SYS_EXIT, 1, 0, 0);
        return 1;
    }

    /* 子进程模式：共享内存越界写（预期 #PF 自杀）。 */
    if (argc >= 4 && argv[1] != NULL && ds_streq(argv[1], "shm-oob") != 0) {
        u32 handle = 0;
        u32 pages = 0;

        (void)a_parse_dec(argv[2], &handle);
        (void)a_parse_dec(argv[3], &pages);
        a_child_shm_oob(handle, pages);
        return 0;
    }
    /* 可选场景：`oob` 才跑会让当前内核 PANIC 的 ring3 越界写（见 g_do_oob）。 */
    if (argc >= 2 && argv[1] != NULL && ds_streq(argv[1], "oob") != 0) {
        g_do_oob = 1;
    }

    a_put("ng: audit ui start (pid=");
    a_putdec(g_pid);
    a_put(" ports=");
    a_puthex(g_port);
    a_put("/");
    a_puthex(g_port2);
    a_put(")\r\n");

    /* ---- displayd：由本程序拉起，避免依赖内核自检的清理时序 ---- */
    disp = a_displayd_start();
    if (disp == 0) {
        a_put("ng: audit display spawn FAIL\r\n");
        (void)a_sys(A_SYS_EXIT, 1, 0, 0);
        return 1;
    }
    a_check(1, "display spawn", g_displayd_pid);

    a_attack_status_poison(disp);
    a_attack_malformed(disp);
    a_attack_create_geom(disp);
    a_attack_badops(disp);
    a_attack_authz(disp);

    /* 表满需要 8 个 SHM 对象：单独一个 displayd。 */
    a_displayd_stop();
    disp = a_displayd_start();
    if (disp == 0) {
        a_put("ng: audit display spawn2 FAIL\r\n");
        (void)a_sys(A_SYS_EXIT, 1, 0, 0);
        return 1;
    }
    a_attack_table(disp);
    a_displayd_stop();

    /* ---- 共享内存（最后一个 displayd 会在 owner-exit 测试里被杀） ---- */
    disp = a_displayd_start();
    if (disp == 0) {
        a_put("ng: audit display spawn3 FAIL\r\n");
        (void)a_sys(A_SYS_EXIT, 1, 0, 0);
        return 1;
    }
    a_attack_shm(disp);

    /* ---- sessiond（重新拉起 displayd + WM） ---- */
    a_attack_session();

    if (g_fails == 0) {
        a_put("ng: audit ui PASS (checks=");
        a_putdec(g_checks);
        a_put(")\r\n");
        (void)a_sys(A_SYS_EXIT, 0, 0, 0);
        return 0;
    }
    a_put("ng: audit ui FAIL (fails=");
    a_putdec(g_fails);
    a_put(" checks=");
    a_putdec(g_checks);
    a_put(")\r\n");
    (void)a_sys(A_SYS_EXIT, 1, 0, 0);
    return 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call a_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
