/*
 * ng WP3-1: display server（L2a，ring3 进程 /bin/displayd）。
 *
 * 职责（只做 display server，不做 shell/WM/session manager）：
 *   - `SYS_FB_MAP` 拿到 LFB（PCD/PWT/NX，root-only），扫描输出就这么一小步；
 *   - `PORT_CREATE` 建显示端口，ticket 写进 `/run/display.port`（ramfs 文本，
 *     客户端发现入口）；状态写 `/run/display.status`（内核自检读）；
 *   - 协议：CREATE / MAP / COMMIT / DESTROY / FOCUS / INPUT（见 ds_protocol.h）；
 *   - 数据面：surface 像素在**共享内存**里（displayd 用 SYS_SHM_ALLOC 分配，
 *     客户端用 SYS_SHM_MAP 映射同一个 handle）；控制面只有小消息；
 *   - 合成：ARGB8888 源按 z-order 合成到 LFB（source-over，A=255 直接拷贝），
 *     基础光标（8x12 块光标）；
 *   - 输入：轮询 PS/2（syscall 30 readkey），把按下事件推给焦点客户端。
 *
 * 崩溃隔离：displayd 是普通进程，内核只保留"共享内存对象表 + LFB 映射"两条
 * 机制；kill 它只会回收它自己的端口/AS/共享页，不影响内核和其他进程
 * （内核自检里有一个 hold 客户端在 displayd 被杀后自己超时退出的证据）。
 *
 * 无 libc：syscall 内联，字符串/十进制/十六进制自己实现。
 */
#include "ds_composite.h"
#include "ds_protocol.h"
#include "ds_surface.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号（与 src/ng/syscall.h 一致） ------------------------------ */
#define D_SYS_WRITE 0
#define D_SYS_EXIT 1
#define D_SYS_PORT_CREATE 10
#define D_SYS_PORT_SEND 11
#define D_SYS_PORT_RECV 12
#define D_SYS_PORT_REPLY 13
#define D_SYS_PORT_CLOSE 14
#define D_SYS_OPEN 20
#define D_SYS_READ 21
#define D_SYS_WRITE_FD 22
#define D_SYS_CLOSE 23
#define D_SYS_MKDIR 26
#define D_SYS_READKEY 30
#define D_SYS_SHM_ALLOC 45
#define D_SYS_SHM_MAP 46
#define D_SYS_FB_MAP 47

/* ---- VFS 常量（与 src/services/fs/include/vfs.h 一致） ------------------- */
#define D_O_WRONLY 0x001u
#define D_O_CREAT 0x040u
#define D_O_TRUNC 0x200u
#define D_EEXIST (-17)

#define D_ETIMEDOUT (-110)

static i64 d_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 d_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return d_sys4(nr, a1, a2, a3, 0);
}

/* ------------------------------------------------------------------------- */
/* 字符串 / 数字                                                              */
/* ------------------------------------------------------------------------- */

static u32 d_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void d_put(const char *s)
{
    u32 n = d_strlen(s);

    if (n != 0) {
        (void)d_sys(D_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static u32 d_u32str(u32 v, char *out)
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

static void d_putdec(u32 v)
{
    char buf[12];

    (void)d_u32str(v, buf);
    d_put(buf);
}

static u32 d_hexstr(u32 v, char *out)
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

static void d_puthex(u32 v)
{
    char buf[10];

    d_put("0x");
    (void)d_hexstr(v, buf);
    d_put(buf);
}

/* ------------------------------------------------------------------------- */
/* 全局状态                                                                   */
/* ------------------------------------------------------------------------- */

static u32 g_port;      /* displayd 的端口 ticket（客户端从 /run/display.port 读） */
static u32 g_commits;   /* COMMIT 次数 */
static u32 g_inputs;    /* 转发给客户端的按键数 */
static u32 g_px;        /* 最近一次合成后 LFB 探针像素（证据） */
static u32 g_hotkey_client;  /* REGISTER_HOTKEY 的客户端 ticket（0 = 没注册） */
static u32 g_hotkey_surface; /* 注册者的 surface id（INPUT 副本里的 surface_id） */
static u32 g_hotkey_key;     /* DS_HOTKEY_ANY = 所有键 */
static u32 g_hotkey_prints;  /* 热键转发证据行计数 */
static u32 g_last_list_n = 0xFFFFFFFFu;     /* LIST 证据行去重 */
static u32 g_last_list_focus = 0xFFFFFFFFu;
/* WP3-2：WM 通过 WM_STATUS 报上来的计数（汇总进 /run/display.status）。 */
static u32 g_wm_seen;
static u32 g_wm_tasks;
static u32 g_wm_focus;
static u32 g_wm_switch;
static u32 g_wm_lastswitch;
static u32 g_wm_launch;
static u32 g_wm_lastpid;
static struct ds_table g_table;
static struct ds_screen g_screen;

/* ------------------------------------------------------------------------- */
/* 状态文件（内核自检读；每次状态变化重写一遍）                               */
/* ------------------------------------------------------------------------- */

static void d_status_append(char *buf, u32 *len, const char *s)
{
    while (s != NULL && *s != '\0') {
        buf[(*len)++] = *s++;
    }
}

static void d_status_append_u32(char *buf, u32 *len, u32 v)
{
    char num[12];
    u32 n = d_u32str(v, num);
    u32 i;

    for (i = 0; i < n; i++) {
        buf[(*len)++] = num[i];
    }
}

static void d_status_write(void)
{
    char buf[320];
    char hex[10];
    u32 len = 0;
    u32 mapped = 0;
    u32 i;
    int fd;

    d_status_append(buf, &len, "up=1 port=1 surface=");
    d_status_append_u32(buf, &len, ds_table_count(&g_table));
    for (i = 0; i < ds_table_count(&g_table); i++) {
        struct ds_surface *s = ds_table_at(&g_table, i);

        if (s != NULL && s->mapped) {
            mapped++;
        }
    }
    d_status_append(buf, &len, " mapped=");
    d_status_append_u32(buf, &len, mapped);
    d_status_append(buf, &len, " commits=");
    d_status_append_u32(buf, &len, g_commits);
    d_status_append(buf, &len, " focus=");
    d_status_append_u32(buf, &len, ds_table_focus_id(&g_table));
    d_status_append(buf, &len, " input=");
    d_status_append_u32(buf, &len, g_inputs);
    d_status_append(buf, &len, " hotkey=");
    d_status_append_u32(buf, &len, (g_hotkey_client != 0) ? 1u : 0u);
    /* WP3-2：WM 的自治计数（内核 wp32 自检读同一份状态文件）。 */
    d_status_append(buf, &len, " wm=");
    d_status_append_u32(buf, &len, (g_wm_seen != 0) ? 1u : 0u);
    if (g_wm_seen != 0) {
        d_status_append(buf, &len, " wmtasks=");
        d_status_append_u32(buf, &len, g_wm_tasks);
        d_status_append(buf, &len, " wmfocus=");
        d_status_append_u32(buf, &len, g_wm_focus);
        d_status_append(buf, &len, " wmswitch=");
        d_status_append_u32(buf, &len, g_wm_switch);
        d_status_append(buf, &len, " wmlast=");
        d_status_append_u32(buf, &len, g_wm_lastswitch);
        d_status_append(buf, &len, " wmlaunch=");
        d_status_append_u32(buf, &len, g_wm_launch);
        d_status_append(buf, &len, " wmlastpid=");
        d_status_append_u32(buf, &len, g_wm_lastpid);
    }
    d_status_append(buf, &len, " px=0x");
    (void)d_hexstr(g_px, hex);
    for (i = 0; hex[i] != '\0'; i++) {
        buf[len++] = hex[i];
    }
    buf[len++] = '\n';
    buf[len] = '\0';

    fd = (int)d_sys(D_SYS_OPEN, (i64)DS_STATUS_PATH,
                    (i64)(D_O_WRONLY | D_O_CREAT | D_O_TRUNC), 0644);
    if (fd < 0) {
        d_put("ng: displayd status FAIL (open)\r\n");
        return;
    }
    if (d_sys(D_SYS_WRITE_FD, (i64)fd, (i64)buf, (i64)len) != (i64)len) {
        d_put("ng: displayd status FAIL (write)\r\n");
    }
    (void)d_sys(D_SYS_CLOSE, (i64)fd, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* IPC 消息                                                                   */
/* ------------------------------------------------------------------------- */

static int d_send_msg(u32 to_ticket, int is_reply, const struct ds_msg *m,
                      u32 from)
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
    if (is_reply) {
        return (int)d_sys4(D_SYS_PORT_REPLY, (i64)to_ticket, (i64)buf,
                           (i64)(DS_IPC_HDR_SIZE + (u32)n), 0);
    }
    return (int)d_sys4(D_SYS_PORT_SEND, (i64)to_ticket, (i64)buf,
                       (i64)(DS_IPC_HDR_SIZE + (u32)n), 0);
}

/* 返回消息类型（>0）、超时（D_ETIMEDOUT）或负 errno。 */
static int d_recv_msg(u32 port, struct ds_msg *m, u32 *out_from, u64 timeout)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct ds_ipc_hdr h;
    i64 r = d_sys4(D_SYS_PORT_RECV, (i64)port, (i64)buf, (i64)sizeof(buf),
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
/* 合成 / 光标                                                                */
/* ------------------------------------------------------------------------- */

static u32 d_screen_probe(const struct ds_surface *sf)
{
    u32 x = sf->x + DS_DEMO_PROBE_X;
    u32 y = sf->y + DS_DEMO_PROBE_Y;

    if (x >= g_screen.w || y >= g_screen.h) {
        return 0;
    }
    return ds_screen_read(&g_screen, (i64)x, (i64)y);
}

static void d_cursor_place(void)
{
    ds_cursor_draw(&g_screen, (g_screen.w > DS_CURSOR_W + 4u)
                                  ? (g_screen.w - DS_CURSOR_W - 4u)
                                  : 0u,
                   (g_screen.h > DS_CURSOR_H + 4u)
                       ? (g_screen.h - DS_CURSOR_H - 4u)
                       : 0u);
}

static void d_composite_surface(struct ds_surface *sf)
{
    struct ds_rect region;

    region.x = sf->x;
    region.y = sf->y;
    region.w = sf->w;
    region.h = sf->h;
    (void)ds_composite_region(&g_screen, &g_table, &region, NULL);
    d_cursor_place();

    g_px = d_screen_probe(sf);
    d_put("ng: composite ok (px=");
    d_puthex(g_px);
    d_put(" at=");
    d_putdec(sf->x);
    d_put(",");
    d_putdec(sf->y);
    d_put(")\r\n");
}

/* ------------------------------------------------------------------------- */
/* 输入转发                                                                   */
/* ------------------------------------------------------------------------- */

static void d_send_input(u32 ticket, u32 surface_id, u32 ascii, u32 scancode,
                         u32 flags)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_INPUT;
    m.u.input.surface_id = surface_id;
    m.u.input.kind = DS_INPUT_KEY_DOWN;
    m.u.input.key = ascii;
    m.u.input.scancode = scancode;
    m.u.input.flags = flags;
    (void)d_send_msg(ticket, 0, &m, g_port);
}

static void d_poll_input(void)
{
    u32 n;

    for (n = 0; n < 16u; n++) {
        i64 v = d_sys(D_SYS_READKEY, 0, 0, 0);
        u32 flags;
        u32 ascii;
        u32 scancode;
        struct ds_surface *focus;
        u32 hotkey_sent = 0;

        if (v < 0) {
            return; /* EAGAIN：没有键了 */
        }
        flags = (u32)((v >> 24) & 0xFFu);
        ascii = (u32)((v >> 16) & 0xFFu);
        scancode = (u32)(v & 0xFFu);

        /*
         * WP3-2：WM 注册了全局热键 —— 先把按键给 WM 一份（带 GLOBAL 位），
         * 再按焦点转发。WM 用它做 Tab 任务切换 / 1..9 启动器。
         */
        if (g_hotkey_client != 0 &&
            (g_hotkey_key == DS_HOTKEY_ANY || ascii == g_hotkey_key)) {
            d_send_input(g_hotkey_client, g_hotkey_surface, ascii, scancode,
                         flags | DS_INPUT_F_GLOBAL);
            hotkey_sent = 1;
            if (g_hotkey_prints < 8u) {
                g_hotkey_prints++;
                d_put("ng: displayd hotkey ok (key=");
                d_puthex(ascii);
                d_put(" scancode=");
                d_puthex(scancode);
                d_put(")\r\n");
            }
        }

        focus = ds_table_find(&g_table, ds_table_focus_id(&g_table));
        if (focus == NULL || focus->client_ticket == 0) {
            continue;
        }
        /* 焦点客户端就是 WM 时，热键副本已经送过了（不重复投递）。 */
        if (!(hotkey_sent != 0 && focus->client_ticket == g_hotkey_client)) {
            d_send_input(focus->client_ticket, focus->id, ascii, scancode,
                         flags);
        }
        g_inputs++;
        d_put("ng: displayd input ok (key=");
        if (ascii >= 0x20u && ascii < 0x7Fu) {
            char c = (char)ascii;
            (void)d_sys(D_SYS_WRITE, (i64)&c, 1, 0);
        } else {
            d_put("0");
        }
        d_put(" scancode=");
        d_puthex(scancode);
        d_put(")\r\n");
        d_status_write();
    }
}

/* ------------------------------------------------------------------------- */
/* 协议处理                                                                   */
/* ------------------------------------------------------------------------- */

static void d_reply_status(u32 to, u32 type, i32 status, u32 seq)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = (u16)type;
    m.status = status;
    m.seq = seq;
    (void)d_send_msg(to, 1, &m, g_port);
}

static void d_handle_create(const struct ds_msg *m, u32 from)
{
    struct ds_msg r;
    struct ds_surface *sf;
    u32 w = m->u.create.w;
    u32 h = m->u.create.h;
    u64 pages64;
    u32 pages;
    u64 va = 0;
    u32 handle = 0;
    u32 id = 0;
    u32 owner_pid = 0;
    i32 px = -1;
    i32 py = -1;
    char title[DS_TITLE_MAX];
    i64 rc;

    title[0] = '\0';
    /* v2 才带 title / owner_pid / 指定位置；v1 客户端走默认（空标题+级联）。 */
    if (m->version >= 2u) {
        ds_title_get(m->u.create.title, title, (u32)sizeof(title));
        owner_pid = m->u.create.owner_pid;
        if ((m->u.create.flags & DS_CREATE_F_POS) != 0u) {
            px = (i32)m->u.create.x;
            py = (i32)m->u.create.y;
        }
    }
    if (w == 0 || h == 0 || w > DS_SURFACE_MAX_W || h > DS_SURFACE_MAX_H) {
        d_reply_status(from, DS_MSG_CREATE_REPLY, DS_EINVAL, m->seq);
        d_put("ng: surface create FAIL (geom)\r\n");
        return;
    }
    pages64 = ((u64)w * (u64)h * 4ULL + (u64)DS_SHM_PAGE_SIZE - 1ULL) /
              (u64)DS_SHM_PAGE_SIZE;
    if (pages64 == 0 || pages64 > (u64)DS_SHM_PAGES_MAX) {
        d_reply_status(from, DS_MSG_CREATE_REPLY, DS_EINVAL, m->seq);
        d_put("ng: surface create FAIL (pages)\r\n");
        return;
    }
    pages = (u32)pages64;
    rc = d_sys(D_SYS_SHM_ALLOC, (i64)pages, (i64)&va, (i64)&handle);
    if (rc != 0) {
        d_reply_status(from, DS_MSG_CREATE_REPLY, (i32)rc, m->seq);
        d_put("ng: surface create FAIL (shm rc=");
        d_putdec((u32)(-rc));
        d_put(")\r\n");
        return;
    }
    rc = ds_table_create_ex(&g_table, w, h, owner_pid, from, handle, pages, va,
                            title, px, py, &id);
    if (rc != DS_OK) {
        d_reply_status(from, DS_MSG_CREATE_REPLY, (i32)rc, m->seq);
        d_put("ng: surface create FAIL (table)\r\n");
        return;
    }
    sf = ds_table_find(&g_table, id);
    ds_memzero(&r, (u64)sizeof(r));
    r.type = DS_MSG_CREATE_REPLY;
    r.status = 0;
    r.seq = m->seq;
    r.u.create_reply.surface_id = id;
    r.u.create_reply.shm_handle = handle;
    r.u.create_reply.pages = pages;
    r.u.create_reply.w = w;
    r.u.create_reply.h = h;
    if (sf != NULL) {
        r.u.create_reply.x = sf->x;
        r.u.create_reply.y = sf->y;
    }
    (void)d_send_msg(from, 1, &r, g_port);

    d_put("ng: surface create ok (id=");
    d_putdec(id);
    d_put(" w=");
    d_putdec(w);
    d_put(" h=");
    d_putdec(h);
    d_put(" shm=");
    d_puthex(handle);
    d_put(" pages=");
    d_putdec(pages);
    d_put(")\r\n");
    if (title[0] != '\0') {
        d_put("ng: surface title ok (id=");
        d_putdec(id);
        d_put(" title=");
        d_put(title);
        d_put(")\r\n");
    }
    g_last_list_n = 0xFFFFFFFFu;
    d_status_write();
}

/* WP3-2：LIST -> LIST_REPLY（surface 表快照；WM 的任务列表/任务切换用）。 */
static void d_handle_list(const struct ds_msg *m, u32 from)
{
    struct ds_msg r;
    u32 n;
    u32 focus = ds_table_focus_id(&g_table);

    ds_memzero(&r, (u64)sizeof(r));
    r.type = DS_MSG_LIST_REPLY;
    r.status = 0;
    r.seq = m->seq;
    n = ds_table_fill_list(&g_table, r.u.list_reply.entries, DS_LIST_MAX);
    r.u.list_reply.count = n;
    (void)d_send_msg(from, 1, &r, g_port);

    if (n != g_last_list_n || focus != g_last_list_focus) {
        g_last_list_n = n;
        g_last_list_focus = focus;
        d_put("ng: displayd list ok (n=");
        d_putdec(n);
        d_put(" focus=");
        d_putdec(focus);
        d_put(")\r\n");
    }
}

/* WP3-2：WM 注册全局热键（key = DS_HOTKEY_ANY 表示所有键先给 WM 一份）。 */
static void d_handle_register_hotkey(const struct ds_msg *m, u32 from)
{
    g_hotkey_client = from;
    g_hotkey_surface = m->u.register_hotkey.surface_id;
    g_hotkey_key = m->u.register_hotkey.key;
    d_reply_status(from, DS_MSG_REGISTER_HOTKEY, DS_OK, m->seq);
    d_put("ng: hotkey registered ok (surface=");
    d_putdec(g_hotkey_surface);
    d_put(" key=");
    if (g_hotkey_key == DS_HOTKEY_ANY) {
        d_put("any");
    } else {
        d_puthex(g_hotkey_key);
    }
    d_put(")\r\n");
    d_status_write();
}

/* WP3-2：WM 把切换/启动计数报上来，displayd 汇总进 /run/display.status。 */
static void d_handle_wm_status(const struct ds_msg *m)
{
    u32 new_switch = m->u.wm_status.switches;
    u32 new_launch = m->u.wm_status.launches;
    int changed = (g_wm_seen == 0) || (new_switch != g_wm_switch) ||
                  (new_launch != g_wm_launch);

    g_wm_seen = 1;
    g_wm_tasks = m->u.wm_status.tasks;
    g_wm_focus = m->u.wm_status.focus;
    g_wm_switch = new_switch;
    g_wm_lastswitch = m->u.wm_status.lastswitch;
    g_wm_launch = new_launch;
    g_wm_lastpid = m->u.wm_status.lastpid;
    if (changed) {
        d_put("ng: displayd wm status ok (switch=");
        d_putdec(g_wm_switch);
        d_put(" launch=");
        d_putdec(g_wm_launch);
        d_put(" tasks=");
        d_putdec(g_wm_tasks);
        d_put(")\r\n");
    }
    d_status_write();
}

static void d_handle_commit(const struct ds_msg *m)
{
    struct ds_rect r;
    struct ds_rect clip;
    struct ds_surface *sf;

    r.x = m->u.commit.x;
    r.y = m->u.commit.y;
    r.w = m->u.commit.w;
    r.h = m->u.commit.h;
    if (ds_table_commit(&g_table, m->u.commit.surface_id, &r, &clip) != DS_OK) {
        d_put("ng: composite FAIL (id=");
        d_putdec(m->u.commit.surface_id);
        d_put(")\r\n");
        return;
    }
    sf = ds_table_find(&g_table, m->u.commit.surface_id);
    if (sf == NULL) {
        return;
    }
    d_composite_surface(sf);
    g_commits++;
    d_status_write();
}

static void d_handle_msg(int type, const struct ds_msg *m, u32 from)
{
    switch (type) {
    case DS_MSG_CREATE:
        d_handle_create(m, from);
        break;
    case DS_MSG_MAP:
        if (ds_table_mark_mapped(&g_table, m->u.map.surface_id) != DS_OK) {
            d_reply_status(from, DS_MSG_MAP, DS_ENOENT, m->seq);
            d_put("ng: surface map FAIL (id=");
            d_putdec(m->u.map.surface_id);
            d_put(")\r\n");
            break;
        }
        d_put("ng: surface map ok (id=");
        d_putdec(m->u.map.surface_id);
        d_put(")\r\n");
        d_status_write();
        break;
    case DS_MSG_COMMIT:
        d_handle_commit(m);
        break;
    case DS_MSG_FOCUS:
        (void)ds_table_focus(&g_table, m->u.focus.surface_id);
        d_status_write();
        break;
    case DS_MSG_DESTROY:
        (void)ds_table_destroy(&g_table, m->u.destroy.surface_id);
        g_last_list_n = 0xFFFFFFFFu;
        d_status_write();
        break;
    case DS_MSG_LIST:
        d_handle_list(m, from);
        break;
    case DS_MSG_REGISTER_HOTKEY:
        d_handle_register_hotkey(m, from);
        break;
    case DS_MSG_WM_STATUS:
        d_handle_wm_status(m);
        break;
    case DS_MSG_BYE:
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

static int d_init_fb(void)
{
    struct ds_fb_info info;
    u64 va = 0;
    i64 rc;

    ds_memzero(&info, (u64)sizeof(info));
    rc = d_sys(D_SYS_FB_MAP, (i64)&va, (i64)&info, 0);
    if (rc != 0) {
        d_put("ng: displayd FAIL (fb_map rc=");
        d_putdec((u32)(-rc));
        d_put(")\r\n");
        return 1;
    }
    if (info.bpp != 32u || info.pitch < info.width * 4u) {
        d_put("ng: displayd FAIL (bpp/pitch)\r\n");
        return 1;
    }
    g_screen.px = (u32 *)(usize)info.base;
    g_screen.w = info.width;
    g_screen.h = info.height;
    g_screen.pitch_px = info.pitch / 4u;

    d_put("ng: displayd up (fb=");
    d_puthex((u32)info.phys);
    d_put(" ");
    d_putdec(info.width);
    d_put("x");
    d_putdec(info.height);
    d_put(" pitch=");
    d_putdec(info.pitch);
    d_put(" bpp=");
    d_putdec(info.bpp);
    d_put(")\r\n");

    ds_table_init(&g_table, info.width, info.height);
    d_cursor_place();
    return 0;
}

static int d_open_port(void)
{
    char buf[24];
    u32 len = 0;
    int fd;
    i64 rc;

    rc = d_sys(D_SYS_MKDIR, (i64)"/run", 0755, 0);
    if (rc != 0 && rc != D_EEXIST) {
        d_put("ng: displayd FAIL (mkdir /run)\r\n");
        return 1;
    }
    rc = d_sys(D_SYS_PORT_CREATE, (i64)DS_CLIENT_PORT_DEPTH, 0, 0);
    if (rc <= 0) {
        d_put("ng: displayd FAIL (port_create)\r\n");
        return 1;
    }
    g_port = (u32)rc;
    len = d_u32str(g_port, buf);
    buf[len++] = '\n';
    buf[len] = '\0';

    fd = (int)d_sys(D_SYS_OPEN, (i64)DS_PORT_PATH,
                    (i64)(D_O_WRONLY | D_O_CREAT | D_O_TRUNC), 0644);
    if (fd < 0) {
        d_put("ng: displayd FAIL (open port file)\r\n");
        return 1;
    }
    (void)d_sys(D_SYS_WRITE_FD, (i64)fd, (i64)buf, (i64)len);
    (void)d_sys(D_SYS_CLOSE, (i64)fd, 0, 0);
    d_status_write();

    d_put("ng: display port ok (ticket=");
    d_putdec(g_port);
    d_put(" path=");
    d_put(DS_PORT_PATH);
    d_put(")\r\n");
    return 0;
}

int d_main(void)
{
    if (d_init_fb() != 0) {
        return 1;
    }
    if (d_open_port() != 0) {
        return 1;
    }
    for (;;) {
        struct ds_msg m;
        u32 from = 0;
        int type = d_recv_msg(g_port, &m, &from, DS_POLL_TICKS);

        if (type > 0) {
            d_handle_msg(type, &m, from);
        } else if (type == D_ETIMEDOUT) {
            d_poll_input();
        } else if (type == DS_EINVAL) {
            /* 坏消息：丢掉继续服务（控制面不 panic）。 */
            continue;
        } else {
            /* 端口关闭（理论上只有内核回收时发生）：退出。 */
            if (type < 0) {
                break;
            }
        }
    }
    (void)d_sys(D_SYS_PORT_CLOSE, (i64)g_port, 0, 0);
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call d_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
