/*
 * ng WP3-1: demo app（L3，/bin/demo）—— display server 的第一个客户端。
 *
 * 默认角色（内核自检 spawn）：
 *   1. 读 /run/display.port 拿到 displayd 的 ticket；
 *   2. 建自己的回复端口，CREATE(400x300) 拿 {surface_id, shm_handle, pages}；
 *   3. SYS_SHM_MAP(shm_handle) 把数据面映射进来，画渐变 + 矩形 + 文字；
 *   4. MAP / COMMIT / FOCUS；
 *   5. 阻塞等 INPUT（displayd 把 PS/2 按键转给它），收到后打印
 *      `ng: input route ok (key=..)`；没有输入（-NoQmp）则打印 timeout；
 *   6. DESTROY 后退出 0。
 *
 * `demo hold` 角色：只建端口并等 INPUT（等不到就超时），用于"displayd 被杀
 * 之后客户端自己还能干净退出"的崩溃隔离证据。
 *
 * 无 libc：syscall 内联；绘制用 L2 的 ds_draw（同一份纯函数，host 单测覆盖）。
 */
#include "ds_draw.h"
#include "ds_protocol.h"
#include "session_protocol.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号 ---------------------------------------------------------- */
#define E_SYS_WRITE 0
#define E_SYS_EXIT 1
#define E_SYS_PORT_CREATE 10
#define E_SYS_PORT_SEND 11
#define E_SYS_PORT_RECV 12
#define E_SYS_PORT_REPLY 13
#define E_SYS_PORT_CLOSE 14
#define E_SYS_OPEN 20
#define E_SYS_READ 21
#define E_SYS_CLOSE 23
#define E_SYS_READKEY 30
#define E_SYS_GETPID 42
#define E_SYS_SHM_ALLOC 45
#define E_SYS_SHM_MAP 46
#define E_SYS_FB_MAP 47

#define E_O_RDONLY 0x000u
#define E_ETIMEDOUT (-110)

static i64 e_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 e_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return e_sys4(nr, a1, a2, a3, 0);
}

/* ------------------------------------------------------------------------- */
/* 字符串 / 数字                                                              */
/* ------------------------------------------------------------------------- */

static u32 e_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void e_put(const char *s)
{
    u32 n = e_strlen(s);

    if (n != 0) {
        (void)e_sys(E_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static u32 e_u32str(u32 v, char *out)
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

static void e_putdec(u32 v)
{
    char buf[12];

    (void)e_u32str(v, buf);
    e_put(buf);
}

static void e_puthex(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[9];
    u32 n = 0;
    u32 i;

    e_put("0x");
    if (v == 0) {
        e_put("0");
        return;
    }
    while (v != 0 && n < 8) {
        tmp[n++] = digits[v & 0xFu];
        v >>= 4;
    }
    for (i = 0; i < n; i++) {
        char c = tmp[n - 1u - i];

        (void)e_sys(E_SYS_WRITE, (i64)&c, 1, 0);
    }
}

static int e_streq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return (a[i] == b[i]) ? 1 : 0;
}

/* 读 /run/display.port（十进制 ticket + 可选换行）。0 = 失败。 */
static u32 e_read_server_ticket(void)
{
    char buf[24];
    u32 n = 0;
    u32 v = 0;
    int fd;
    i64 r;

    fd = (int)e_sys(E_SYS_OPEN, (i64)DS_PORT_PATH, (i64)E_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    r = e_sys(E_SYS_READ, (i64)fd, (i64)buf, (i64)(sizeof(buf) - 1u));
    (void)e_sys(E_SYS_CLOSE, (i64)fd, 0, 0);
    if (r <= 0) {
        return 0;
    }
    n = (u32)r;
    {
        u32 i;

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
    }
    return (v != 0) ? v : 0;
}

/* ------------------------------------------------------------------------- */
/* IPC 小包装                                                                 */
/* ------------------------------------------------------------------------- */

static u32 e_port_create(void)
{
    i64 r = e_sys(E_SYS_PORT_CREATE, (i64)DS_CLIENT_PORT_DEPTH, 0, 0);

    return (r > 0) ? (u32)r : 0;
}

static int e_send_msg(u32 to_ticket, const struct ds_msg *m, u32 from)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct ds_ipc_hdr *h = (struct ds_ipc_hdr *)buf;
    int n = ds_msg_encode(m, buf + DS_IPC_HDR_SIZE, DS_MSG_MAX);

    if (n <= 0) {
        return DS_EINVAL;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = from;
    h->flags = 0;
    return (int)e_sys4(E_SYS_PORT_SEND, (i64)to_ticket, (i64)buf,
                       (i64)(DS_IPC_HDR_SIZE + (u32)n), 0);
}

static int e_recv_msg(u32 port, struct ds_msg *m, u32 *out_from, u64 timeout)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct ds_ipc_hdr h;
    i64 r = e_sys4(E_SYS_PORT_RECV, (i64)port, (i64)buf, (i64)sizeof(buf),
                   (i64)timeout);

    if (r < 0) {
        return (int)r;
    }
    if ((u64)r < (u64)DS_IPC_HDR_SIZE || (u64)r > (u64)sizeof(buf)) {
        return DS_EINVAL;
    }
    ds_memcopy(&h, buf, DS_IPC_HDR_SIZE);
    if ((u64)h.len + (u64)DS_IPC_HDR_SIZE != (u64)r ||
        h.len > (u32)DS_MSG_MAX) {
        return DS_EINVAL;
    }
    if (ds_msg_decode(buf + DS_IPC_HDR_SIZE, h.len, m) != DS_OK) {
        return DS_EINVAL;
    }
    if (out_from != NULL) {
        *out_from = h.from;
    }
    return (int)m->type;
}

/* ------------------------------------------------------------------------- */
/* 会话通道（WP3-3：sessiond 启动应用时经 argv 发 ticket/token）              */
/* ------------------------------------------------------------------------- */

static u32 e_session; /* sessiond 端口 ticket（0 = 没有会话通道） */
static u32 e_token;   /* 本应用的 role_token */

static int e_send_session(u32 to, const struct session_msg *m, u32 from)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct session_ipc_hdr *h = (struct session_ipc_hdr *)buf;
    int n = session_msg_encode(m, buf + SESSION_IPC_HDR_SIZE, SESSION_MSG_MAX);

    if (n <= 0) {
        return DS_EINVAL;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = from;
    h->flags = 0;
    return (int)e_sys4(E_SYS_PORT_SEND, (i64)to, (i64)buf,
                       (i64)(SESSION_IPC_HDR_SIZE + (u32)n), 0);
}

static void e_session_register(u32 port, u32 surface_id, const char *title)
{
    struct session_msg m;

    if (e_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_REGISTER;
    m.seq = 1;
    m.u.reg.token = e_token;
    m.u.reg.port = port;
    m.u.reg.surface_id = surface_id;
    ds_title_set(m.u.reg.title, title);
    (void)e_send_session(e_session, &m, port);
    e_put("ng: demo session ok (session=");
    e_putdec(e_session);
    e_put(" token=");
    e_puthex(e_token);
    e_put(")\r\n");
}

static void e_session_heartbeat(u32 port, u32 rounds)
{
    struct session_msg m;

    if (e_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    m.seq = rounds + 1u;
    m.u.hb.token = e_token;
    m.u.hb.rounds = rounds;
    (void)e_send_session(e_session, &m, port);
}

static void e_session_exit(u32 port, u32 code)
{
    struct session_msg m;

    if (e_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_EXIT;
    m.u.exit.token = e_token;
    m.u.exit.code = code;
    (void)e_send_session(e_session, &m, port);
}

/*
 * 负向探针（WP3-3 自检）：
 *   1. 用**应用令牌**发 LAUNCH（只有 WM 角色可以）-> sessiond 拒绝；
 *   2. 用**应用令牌** FOCUS 别人的窗口 -> displayd 校验失败、拒绝。
 * 两条都真的走 IPC，不是纯函数自测。
 */
static void e_probe_session(u32 port)
{
    struct session_msg m;

    if (e_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_LAUNCH;
    m.u.launch.token = e_token;
    /* 小端：直接按字节写进 u32 数组就是 session_path 的打包格式。 */
    ds_memcopy(m.u.launch.path, "/bin/demo", 10);
    (void)e_send_session(e_session, &m, port);
    e_put("ng: demo role probe ok (path=/bin/demo)\r\n");
}

/* 用应用令牌 FOCUS 另一个窗口（预期 displayd 拒绝）。 */
static void e_probe_focus(u32 server, u32 port, u32 self_id)
{
    struct ds_msg m;
    u32 tries;
    u32 i;
    u32 target = 0;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_LIST;
    if (e_send_msg(server, &m, port) != 0) {
        return;
    }
    for (tries = 0; tries < DS_REPLY_WAIT_TRIES; tries++) {
        int type = e_recv_msg(port, &m, NULL, DS_REPLY_WAIT_TICKS);

        if (type == (int)DS_MSG_LIST_REPLY) {
            break;
        }
        if (type < 0 && type != E_ETIMEDOUT) {
            return;
        }
    }
    if (m.type != (u16)DS_MSG_LIST_REPLY) {
        return;
    }
    for (i = 0; i < m.u.list_reply.count && i < DS_LIST_MAX; i++) {
        const struct ds_list_entry *le = &m.u.list_reply.entries[i];

        if (le->id != self_id && le->token != e_token) {
            target = le->id;
            break;
        }
    }
    if (target == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.u.focus.surface_id = target;
    m.u.focus.token = e_token;
    (void)e_send_msg(server, &m, port);
    e_put("ng: demo focus probe ok (id=");
    e_putdec(target);
    e_put(" token=");
    e_puthex(e_token);
    e_put(")\r\n");
}

/* ------------------------------------------------------------------------- */
/* 角色：默认（建 surface / 画 / commit / 等输入）                            */
/* ------------------------------------------------------------------------- */

/* 绘制颜色必须带 A=0xFF（ds_draw 走 source-over，A=0 等于没画）。 */
#define E_OPAQUE(rgb) (0xFF000000u | (rgb))

static void e_draw_surface(u32 *px, u32 w, u32 h)
{
    struct ds_buffer b;

    ds_buffer_init(&b, px, w, h);
    ds_draw_gradient(&b);
    /* 实心矩形：QMP 截图像素断言就打在 (25,25)（落在这个矩形里）。 */
    ds_draw_fill(&b, 20, 20, 140, 70, E_OPAQUE(DS_DEMO_ACCENT));
    ds_draw_outline(&b, 18, 18, 144, 74, 2, E_OPAQUE(0x00FFFFFFu));
    ds_draw_text(&b, 16, 110, "Aster ng WP3-2 demo window",
                 E_OPAQUE(0x00FFFFFFu), E_OPAQUE(0x00101820u));
    ds_draw_text(&b, 16, 126, "title: Demo   (L3 client)",
                 E_OPAQUE(0x00C8D8FFu), E_OPAQUE(0x00101820u));
    ds_draw_text(&b, 16, 142, "composited via shared memory",
                 E_OPAQUE(0x00C8D8FFu), E_OPAQUE(0x00101820u));
}

static int e_wait_reply(u32 port, struct ds_msg *out)
{
    u32 tries;

    for (tries = 0; tries < DS_REPLY_WAIT_TRIES; tries++) {
        int type = e_recv_msg(port, out, NULL, DS_REPLY_WAIT_TICKS);

        if (type == (int)DS_MSG_CREATE_REPLY) {
            return 0;
        }
        if (type == E_ETIMEDOUT) {
            continue;
        }
        if (type < 0) {
            return (int)type;
        }
    }
    return DS_ETIMEDOUT;
}

static int e_create_surface(u32 server, u32 my, u32 w, u32 h, const char *title,
                            u32 flags, u32 pos_x, u32 pos_y, u32 token,
                            u32 *out_id, u32 *out_handle, u32 *out_pages)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_CREATE;
    m.u.create.w = w;
    m.u.create.h = h;
    m.u.create.flags = flags;
    m.u.create.x = pos_x;
    m.u.create.y = pos_y;
    m.u.create.owner_pid = (u32)e_sys(E_SYS_GETPID, 0, 0, 0);
    m.u.create.token = token;
    ds_title_set(m.u.create.title, title);
    if (e_send_msg(server, &m, my) != 0) {
        return -1;
    }
    if (e_wait_reply(my, &m) != 0 || m.status != 0) {
        return -1;
    }
    *out_id = m.u.create_reply.surface_id;
    *out_handle = m.u.create_reply.shm_handle;
    *out_pages = m.u.create_reply.pages;
    if (*out_id == 0 || *out_handle == 0 || *out_pages == 0) {
        return -1;
    }
    return 0;
}

static void e_role_client(void)
{
    u32 my = 0;
    u32 server = 0;
    u32 surface_id = 0;
    u32 shm_handle = 0;
    u32 pages = 0;
    u32 va = 0;
    u32 w = DS_DEMO_W;
    u32 h = DS_DEMO_H;
    struct ds_msg m;
    u32 pid = (u32)e_sys(E_SYS_GETPID, 0, 0, 0);
    u32 got_input = 0;
    u32 key = 0;
    u32 tries;
    i64 rc;

    server = e_read_server_ticket();
    if (server == 0) {
        e_put("ng: demo FAIL (no display port, pid=");
        e_putdec(pid);
        e_put(")\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    my = e_port_create();
    if (my == 0) {
        e_put("ng: demo FAIL (port)\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }

    /*
     * 协议自检：先建一个 64x48 的 scratch surface，映射后 DESTROY。
     * 这样 DESTROY 路径在真机上被走到，同时不破坏主 surface 的状态证据。
     */
    {
        u32 sid = 0;
        u32 sh = 0;
        u32 pg = 0;
        u32 sva = 0;

        if (e_create_surface(server, my, 64, 48, "scratch", 0, 0, 0, 0, &sid,
                             &sh, &pg) != 0) {
            e_put("ng: demo FAIL (scratch create)\r\n");
            (void)e_sys(E_SYS_EXIT, 1, 0, 0);
        }
        if (e_sys(E_SYS_SHM_MAP, (i64)sh, (i64)&sva, 0) != 0 || sva == 0) {
            e_put("ng: demo FAIL (scratch map)\r\n");
            (void)e_sys(E_SYS_EXIT, 1, 0, 0);
        }
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_MAP;
        m.u.map.surface_id = sid;
        (void)e_send_msg(server, &m, my);
        ds_memzero(&m, (u64)sizeof(m));
        m.type = DS_MSG_DESTROY;
        m.u.destroy.surface_id = sid;
        (void)e_send_msg(server, &m, my);
    }

    /* CREATE(w,h,title=Demo) --from=my --> displayd */
    if (e_create_surface(server, my, w, h, DS_DEMO_TITLE, 0, 0, 0, e_token,
                         &surface_id, &shm_handle, &pages) != 0) {
        e_put("ng: demo FAIL (create)\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    e_put("ng: demo surface ok (id=");
    e_putdec(surface_id);
    e_put(" shm=");
    e_puthex(shm_handle);
    e_put(" pages=");
    e_putdec(pages);
    e_put(")\r\n");
    e_put("ng: demo title ok (id=");
    e_putdec(surface_id);
    e_put(" title=");
    e_put(DS_DEMO_TITLE);
    e_put(")\r\n");

    /* 数据面：把 displayd 分配的共享页映射进本进程（跨进程 SHM）。 */
    rc = e_sys(E_SYS_SHM_MAP, (i64)shm_handle, (i64)&va, 0);
    if (rc != 0 || va == 0) {
        e_put("ng: demo FAIL (shm map rc=");
        e_putdec((u32)(-rc));
        e_put(")\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    e_put("ng: shm map ok (va=");
    e_puthex(va);
    e_put(" pages=");
    e_putdec(pages);
    e_put(")\r\n");

    e_draw_surface((u32 *)(usize)va, w, h);

    /* MAP（告诉 displayd 客户端已经映射好） */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_MAP;
    m.u.map.surface_id = surface_id;
    (void)e_send_msg(server, &m, my);

    /* COMMIT（surface 局部脏矩形；这里整块提交） */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = surface_id;
    m.u.commit.x = 0;
    m.u.commit.y = 0;
    m.u.commit.w = w;
    m.u.commit.h = h;
    (void)e_send_msg(server, &m, my);

    /*
     * FOCUS（置焦点；displayd 之后把按键推给本客户端）。
     * v3 审计加固：会话窗口必须出示自己的 role_token —— displayd 不再接受
     * "token=0 + 知道 surface id" 就抢焦点（别的窗口也能这么被抢）。直连模式
     * 下 e_token 就是 0，行为与 WP3-2 一致。
     */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.u.focus.surface_id = surface_id;
    m.u.focus.token = e_token;
    (void)e_send_msg(server, &m, my);

    e_put("ng: demo input ready (id=");
    e_putdec(surface_id);
    e_put(")\r\n");

    for (tries = 0; tries < DS_INPUT_WAIT_TRIES; tries++) {
        int type = e_recv_msg(my, &m, NULL, DS_INPUT_WAIT_TICKS);

        if (type == (int)DS_MSG_INPUT) {
            got_input = 1;
            key = m.u.input.key;
            break;
        }
        if (type < 0 && type != E_ETIMEDOUT) {
            break;
        }
    }
    if (got_input) {
        e_put("ng: input route ok (key=");
        if (key >= 0x20u && key < 0x7Fu) {
            char c = (char)key;

            (void)e_sys(E_SYS_WRITE, (i64)&c, 1, 0);
        } else {
            e_put("0");
        }
        e_put(" id=");
        e_putdec(surface_id);
        e_put(")\r\n");
    } else {
        e_put("ng: input route ok (timeout id=");
        e_putdec(surface_id);
        e_put(")\r\n");
    }

    (void)e_sys(E_SYS_PORT_CLOSE, (i64)my, 0, 0);

    e_put("ng: demo done (id=");
    e_putdec(surface_id);
    e_put(" input=");
    e_putdec(got_input);
    e_put(")\r\n");
    (void)e_sys(E_SYS_EXIT, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* 角色：hold（displayd 死了也要自己干净退出）                               */
/* ------------------------------------------------------------------------- */

static void e_role_hold(void)
{
    u32 my = e_port_create();
    u32 pid = (u32)e_sys(E_SYS_GETPID, 0, 0, 0);
    u32 tries;

    if (my == 0) {
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    e_put("ng: demo hold ready (pid=");
    e_putdec(pid);
    e_put(")\r\n");
    for (tries = 0; tries < DS_INPUT_WAIT_TRIES; tries++) {
        struct ds_msg m;
        int type = e_recv_msg(my, &m, NULL, DS_INPUT_WAIT_TICKS);

        if (type == (int)DS_MSG_INPUT) {
            break;
        }
        if (type < 0 && type != E_ETIMEDOUT) {
            break;
        }
    }
    (void)e_sys(E_SYS_PORT_CLOSE, (i64)my, 0, 0);
    e_put("ng: demo hold done (alive=1)\r\n");
    (void)e_sys(E_SYS_EXIT, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* 角色：window（常驻窗口；WM 的任务切换/QMP 截图用）                        */
/* ------------------------------------------------------------------------- */

#define E_WINDOW_ROUNDS 500u /* 500 * 20ms ≈ 10s；内核会提前 kill */

static void e_role_window(void)
{
    u32 my = 0;
    u32 server = 0;
    u32 surface_id = 0;
    u32 shm_handle = 0;
    u32 pages = 0;
    u32 va = 0;
    u32 pid = (u32)e_sys(E_SYS_GETPID, 0, 0, 0);
    u32 got_input = 0;
    u32 tries;
    struct ds_msg m;
    i64 rc;

    server = e_read_server_ticket();
    if (server == 0) {
        e_put("ng: demo FAIL (no display port, pid=");
        e_putdec(pid);
        e_put(")\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    my = e_port_create();
    if (my == 0) {
        e_put("ng: demo FAIL (port)\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    if (e_create_surface(server, my, DS_DEMO_W, DS_DEMO_H, DS_DEMO_TITLE, 0, 0,
                         0, e_token, &surface_id, &shm_handle, &pages) != 0) {
        e_put("ng: demo FAIL (window create)\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    rc = e_sys(E_SYS_SHM_MAP, (i64)shm_handle, (i64)&va, 0);
    if (rc != 0 || va == 0) {
        e_put("ng: demo FAIL (window shm map)\r\n");
        (void)e_sys(E_SYS_EXIT, 1, 0, 0);
    }
    e_draw_surface((u32 *)(usize)va, DS_DEMO_W, DS_DEMO_H);

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_MAP;
    m.u.map.surface_id = surface_id;
    (void)e_send_msg(server, &m, my);

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = surface_id;
    m.u.commit.x = 0;
    m.u.commit.y = 0;
    m.u.commit.w = DS_DEMO_W;
    m.u.commit.h = DS_DEMO_H;
    (void)e_send_msg(server, &m, my);

    /*
     * 注意：**不发 FOCUS**。窗口焦点归会话策略（sessiond 在 REGISTER 之后
     * 发 FOCUS，令牌就是本窗口的；WP3-2 直连时由 WM 的 Tab 切换）。
     */
    e_put("ng: demo window up (id=");
    e_putdec(surface_id);
    e_put(" title=");
    e_put(DS_DEMO_TITLE);
    e_put(")\r\n");

    if (e_session != 0) {
        e_session_register(my, surface_id, DS_DEMO_TITLE);
        e_probe_session(my);
        e_probe_focus(server, my, surface_id);
    }

    for (tries = 0; tries < E_WINDOW_ROUNDS; tries++) {
        int type = e_recv_msg(my, &m, NULL, DS_INPUT_WAIT_TICKS);

        if (type == (int)DS_MSG_INPUT) {
            got_input++;
        }
        if (type < 0 && type != E_ETIMEDOUT) {
            break;
        }
        if (e_session != 0 && (tries % 10u) == 9u) {
            e_session_heartbeat(my, tries);
        }
    }
    e_session_exit(my, 0);
    (void)e_sys(E_SYS_PORT_CLOSE, (i64)my, 0, 0);
    e_put("ng: demo window done (id=");
    e_putdec(surface_id);
    e_put(" input=");
    e_putdec(got_input);
    e_put(")\r\n");
    (void)e_sys(E_SYS_EXIT, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int e_main(long argc, char **argv);

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call e_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}

int e_main(long argc, char **argv)
{
    long i;

    /* sessiond 启动应用时经 argv 发 `session=<ticket>` / `token=<hex>`。 */
    e_session = 0;
    e_token = 0;
    for (i = 1; argv != NULL && i < argc; i++) {
        u32 v = 0;

        if (session_arg_parse_u32(argv[i], "session=", &v) != 0) {
            e_session = v;
        } else if (session_arg_parse_u32(argv[i], "token=", &v) != 0) {
            e_token = v;
        }
    }
    if (argc >= 2 && argv != NULL && e_streq(argv[1], "hold")) {
        e_role_hold();
        return 0;
    }
    if (argc >= 2 && argv != NULL && e_streq(argv[1], "window")) {
        e_role_window();
        return 0;
    }
    e_role_client();
    return 0;
}
