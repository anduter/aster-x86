/*
 * ng WP3-2: shell/WM（L2b，ring3 进程 /bin/wm）。
 * ng WP3-3: WM 只是 UI 前端 —— 应用生命周期归 sessiond。
 *
 * 两种运行方式：
 *   - 会话模式（init -> sessiond -> wm）：argv 带 `session=<ticket>` 与
 *     `token=<hex>`。WM 用 REGISTER/HEARTBEAT 把面板与存活报给 sessiond，
 *     启动器发 LAUNCH（sessiond spawn 应用），不再自己 spawn 应用。
 *   - 直连模式（WP3-2 自检：内核直接 spawn WM，没有会话通道）：启动器保留
 *     本地 spawn 作为兼容路径；真实启动永远走会话模式。
 *
 * 其它职责与 WP3-2 相同：面板 surface + 共享内存绘制 + REGISTER_HOTKEY +
 * LIST/FOCUS 任务切换 + WM_STATUS 计数。
 *
 * 证据行（内核 wp32/wp33 自检 + run-ng）：
 *   ng: wm up (pid=.. panel=1280x32)
 *   ng: wm panel ok (px=0x202838 at=0,0)
 *   ng: wm session ok (ticket=.. token=0x..)
 *   ng: wm registry ok (apps=.. windows=.. procs=..)
 *   ng: wm launcher ok (apps=3 pkgs=1)
 *   ng: wm tasks ok (n=2 apps=1)
 *   ng: wm task switch ok (focus=2 from=1 via=key|auto)
 *   ng: wm launch req ok (app=/bin/demo via=key|auto)      [会话模式]
 *   ng: wm launch ok (app=/bin/demo pid=.. via=key|auto)  [直连兼容]
 *   ng: wm exit (rounds=.. switch=.. launch=..)
 *
 * 没有输入（-NoQmp）时，WM 在 3s 后走 auto 分支做一次切换/启动；没有任何
 * 窗口时也会 auto 启动第一个应用（WP3-3 会话自检用）。
 *
 * 无 libc：syscall 内联；字符串/十进制/十六进制自己实现。
 */
#include "ds_draw.h"
#include "ds_protocol.h"
#include "ds_wm.h"
#include "session_protocol.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号（与 src/ng/syscall.h 一致） ------------------------------ */
#define W_SYS_WRITE 0
#define W_SYS_EXIT 1
#define W_SYS_PORT_CREATE 10
#define W_SYS_PORT_SEND 11
#define W_SYS_PORT_RECV 12
#define W_SYS_PORT_REPLY 13
#define W_SYS_PORT_CLOSE 14
#define W_SYS_OPEN 20
#define W_SYS_READ 21
#define W_SYS_WRITE_FD 22
#define W_SYS_CLOSE 23
#define W_SYS_LISTDIR 25
#define W_SYS_MKDIR 26
#define W_SYS_SPAWN 40
#define W_SYS_GETPID 42
#define W_SYS_SHM_MAP 46

/* ---- VFS 常量（与 src/services/fs/include/vfs.h 一致） ------------------- */
#define W_O_RDONLY 0x000u
#define W_O_WRONLY 0x001u
#define W_O_CREAT 0x040u
#define W_O_TRUNC 0x200u
#define W_EEXIST (-17)
#define W_ETIMEDOUT (-110)
#define W_NODE_DIR 2u

/* ---- 时序 ---------------------------------------------------------------- */
#define W_POLL_TICKS DS_POLL_TICKS      /* 20ms */
#define W_ROUNDS 900u                   /* 900 * 20ms ≈ 18s（内核会提前 kill） */
#define W_LIST_EVERY 10u                /* 每 10 轮（~200ms）LIST 一次 */
#define W_AUTO_ROUNDS 150u              /* 无输入 3s 后的自检 fallback */
#define W_STATUS_MAX 256u

/* ---- 与 VFS 共享的用户结构（布局必须逐字节一致，见 vfs.h） --------------- */
struct wm_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[64];
};

/* ------------------------------------------------------------------------- */
/* syscall / 字符串                                                           */
/* ------------------------------------------------------------------------- */

static i64 w_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 w_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return w_sys4(nr, a1, a2, a3, 0);
}

static u32 w_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static int w_streq(const char *a, const char *b)
{
    u32 i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return (a[i] == b[i]) ? 1 : 0;
}

static void w_put(const char *s)
{
    u32 n = w_strlen(s);

    if (n != 0) {
        (void)w_sys(W_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static u32 w_u32str(u32 v, char *out)
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

static void w_putdec(u32 v)
{
    char buf[12];

    (void)w_u32str(v, buf);
    w_put(buf);
}

static void w_puthex(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[9];
    u32 n = 0;
    u32 i;

    w_put("0x");
    if (v == 0) {
        w_put("0");
        return;
    }
    while (v != 0 && n < 8) {
        tmp[n++] = digits[v & 0xFu];
        v >>= 4;
    }
    for (i = 0; i < n; i++) {
        char c = tmp[n - 1u - i];

        (void)w_sys(W_SYS_WRITE, (i64)&c, 1, 0);
    }
}

/* 读 /run/display.port（十进制 ticket + 可选换行）。0 = 失败。 */
static u32 w_read_server_ticket(void)
{
    char buf[24];
    u32 n = 0;
    u32 v = 0;
    int fd;
    i64 r;

    fd = (int)w_sys(W_SYS_OPEN, (i64)DS_PORT_PATH, (i64)W_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    r = w_sys(W_SYS_READ, (i64)fd, (i64)buf, (i64)(sizeof(buf) - 1u));
    (void)w_sys(W_SYS_CLOSE, (i64)fd, 0, 0);
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
/* 全局状态                                                                   */
/* ------------------------------------------------------------------------- */

static u32 g_port;
static u32 g_server;
static u32 g_seq;
static u32 g_self_id;
static u32 g_session; /* sessiond 端口 ticket（0 = 直连兼容模式） */
static u32 g_token;   /* WM 的 role_token */
/*
 * WP3-5：S1 故障注入钩子。只有 sessiond 用 `fault=spin` 启动 WM 时才会
 * 打开（S1 场景 d），收到 F12（PS/2 scancode 0x58）后进入死循环，模拟
 * "WM 卡死"。正式启动路径没有这个参数，行为不变。
 */
static u32 g_fault_spin;
static u32 g_session_list_printed;
static u32 g_empty_idle;
static u32 g_rounds;
static u32 g_panel_va;
static u32 g_panel_px;
static struct ds_wm_layout g_layout;
static struct ds_wm_tasks g_tasks;
static struct ds_wm_launcher g_apps;
static struct ds_wm_hotkey g_hotkey;

static u32 g_tasks_printed;
static u32 g_printed_ids[DS_WM_TASKS_MAX];
static u32 g_printed_count;
static u32 g_last_focus;
static u32 g_drawn_tasks;
static u32 g_drawn_focus;

static u32 g_switch_pending;
static u32 g_switch_from;
static u32 g_switch_via; /* 0 = key, 1 = auto */
static u32 g_switches;
static u32 g_last_switch;
static u32 g_launches;
static u32 g_last_launch_pid;

static u32 g_seen_tasks;
static u32 g_idle_rounds;
static u32 g_launch_idle;
static u32 g_auto_switch_tried;
static u32 g_auto_launch_tried;

/* 上次通过 WM_STATUS 报给 displayd 的快照（只在变化时发，避免刷状态文件）。 */
static u32 g_rep_tasks;
static u32 g_rep_focus;
static u32 g_rep_switch;
static u32 g_rep_launch;
static u32 g_rep_valid;


static int w_recv_msg(struct ds_msg *m, u64 timeout);

/* ------------------------------------------------------------------------- */
/* 会话通道（WP3-3）                                                          */
/* ------------------------------------------------------------------------- */

/* 一条 IPC 帧：display 协议或 session 协议（按 magic 区分）。 */
struct w_frame {
    int kind; /* 0 = display，1 = session */
    struct ds_msg ds;
    struct session_msg ss;
};

static int w_recv_frame(struct w_frame *f, u64 timeout)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct ds_ipc_hdr h;
    u32 magic = 0;
    i64 r = w_sys4(W_SYS_PORT_RECV, (i64)g_port, (i64)buf, (i64)sizeof(buf),
                   (i64)timeout);

    if (r < 0) {
        return (int)r;
    }
    if ((u64)r < (u64)DS_IPC_HDR_SIZE || (u64)r > (u64)sizeof(buf)) {
        return DS_EINVAL;
    }
    ds_memcopy(&h, buf, DS_IPC_HDR_SIZE);
    if ((u64)h.len + (u64)DS_IPC_HDR_SIZE != (u64)r) {
        return DS_EINVAL;
    }
    ds_memcopy(&magic, buf + DS_IPC_HDR_SIZE, 4);
    if (magic == SESSION_MAGIC) {
        if (h.len > (u32)SESSION_MSG_MAX) {
            return DS_EINVAL;
        }
        if (session_msg_decode(buf + DS_IPC_HDR_SIZE, h.len, &f->ss) != DS_OK) {
            return DS_EINVAL;
        }
        f->kind = 1;
        return DS_OK;
    }
    if (magic == DS_PROTO_MAGIC) {
        if (h.len > (u32)DS_MSG_MAX) {
            return DS_EINVAL;
        }
        if (ds_msg_decode(buf + DS_IPC_HDR_SIZE, h.len, &f->ds) != DS_OK) {
            return DS_EINVAL;
        }
        f->kind = 0;
        return DS_OK;
    }
    return DS_EINVAL;
}

/* 只等 display 协议消息（面板建立阶段用；session 帧直接丢弃）。 */
static int w_recv_ds(struct ds_msg *m, u64 timeout)
{
    for (;;) {
        struct w_frame f;
        int rc = w_recv_frame(&f, timeout);

        if (rc != 0) {
            return rc;
        }
        if (f.kind == 0) {
            ds_memcopy(m, &f.ds, (u64)sizeof(*m));
            return (int)m->type;
        }
    }
}

static int w_send_session(const struct session_msg *m)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct session_ipc_hdr *h = (struct session_ipc_hdr *)buf;
    int n = session_msg_encode(m, buf + SESSION_IPC_HDR_SIZE, SESSION_MSG_MAX);

    if (n <= 0) {
        return DS_EINVAL;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = g_port;
    h->flags = 0;
    return (int)w_sys4(W_SYS_PORT_SEND, (i64)g_session, (i64)buf,
                       (i64)(SESSION_IPC_HDR_SIZE + (u32)n), 0);
}

static void wm_session_register(void)
{
    struct session_msg m;

    if (g_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_REGISTER;
    m.seq = ++g_seq;
    m.u.reg.token = g_token;
    m.u.reg.port = g_port;
    m.u.reg.surface_id = g_self_id;
    ds_title_set(m.u.reg.title, "Aster Panel");
    (void)w_send_session(&m);
    w_put("ng: wm session ok (session=");
    w_putdec(g_session);
    w_put(" token=");
    w_puthex(g_token);
    w_put(")\r\n");

    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_LIST;
    m.seq = ++g_seq;
    (void)w_send_session(&m);
}

static void wm_session_heartbeat(void)
{
    struct session_msg m;

    if (g_session == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = SESSION_MSG_HEARTBEAT;
    m.seq = ++g_seq;
    m.u.hb.token = g_token;
    m.u.hb.rounds = g_rounds;
    (void)w_send_session(&m);
}

/* sessiond 的注册表快照（LOOKUP/LIST 通路证据）。 */
static void wm_handle_session_msg(const struct session_msg *sm)
{
    if (sm->type != SESSION_MSG_LIST_REPLY) {
        return;
    }
    if (g_session_list_printed < 4u) {
        g_session_list_printed++;
        w_put("ng: wm registry ok (apps=");
        w_putdec(sm->u.list_reply.apps);
        w_put(" windows=");
        w_putdec(sm->u.list_reply.windows);
        w_put(" procs=");
        w_putdec(sm->u.list_reply.procs);
        w_put(")\r\n");
    }
}

/* ------------------------------------------------------------------------- */
/* IPC 消息                                                                   */
/* ------------------------------------------------------------------------- */

static int w_send_msg(const struct ds_msg *m)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct ds_ipc_hdr *h = (struct ds_ipc_hdr *)buf;
    int n = ds_msg_encode(m, buf + DS_IPC_HDR_SIZE, DS_MSG_MAX);

    if (n <= 0) {
        return DS_EINVAL;
    }
    h->type = m->type;
    h->len = (u32)n;
    h->from = g_port;
    h->flags = 0;
    return (int)w_sys4(W_SYS_PORT_SEND, (i64)g_server, (i64)buf,
                       (i64)(DS_IPC_HDR_SIZE + (u32)n), 0);
}

static int w_recv_msg(struct ds_msg *m, u64 timeout)
{
    return w_recv_ds(m, timeout);
}

/*
 * 把 WM 的自治状态报给 displayd（WM_STATUS）。displayd 汇总进已有的
 * /run/display.status，内核 wp32 自检读同一份文件 —— 不新建 ramfs 节点。
 */
static void wm_report_if_changed(void)
{
    struct ds_msg m;
    u32 tasks = ds_wm_tasks_apps(&g_tasks, g_self_id);

    if (g_rep_valid != 0u && tasks == g_rep_tasks &&
        g_tasks.focus_id == g_rep_focus && g_switches == g_rep_switch &&
        g_launches == g_rep_launch) {
        return;
    }
    g_rep_valid = 1;
    g_rep_tasks = tasks;
    g_rep_focus = g_tasks.focus_id;
    g_rep_switch = g_switches;
    g_rep_launch = g_launches;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_WM_STATUS;
    m.seq = ++g_seq;
    m.u.wm_status.surface_id = g_self_id;
    m.u.wm_status.tasks = tasks;
    m.u.wm_status.focus = g_tasks.focus_id;
    m.u.wm_status.switches = g_switches;
    m.u.wm_status.lastswitch = g_last_switch;
    m.u.wm_status.launches = g_launches;
    m.u.wm_status.lastpid = g_last_launch_pid;
    (void)w_send_msg(&m);
}

/* ------------------------------------------------------------------------- */
/* 面板                                                                       */
/* ------------------------------------------------------------------------- */

static u32 wm_panel_value(void)
{
    if (g_panel_va == 0) {
        return 0;
    }
    return ds_wm_panel_pixel((const u32 *)(usize)g_panel_va, DS_WM_PANEL_W,
                             DS_WM_PROBE_X, DS_WM_PROBE_Y) &
           0x00FFFFFFu;
}

static void wm_commit_panel(void)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_COMMIT;
    m.u.commit.surface_id = g_self_id;
    m.u.commit.x = 0;
    m.u.commit.y = 0;
    m.u.commit.w = DS_WM_PANEL_W;
    m.u.commit.h = DS_WM_PANEL_H;
    (void)w_send_msg(&m);
}

static void wm_draw_panel(void)
{
    struct ds_buffer b;

    if (g_panel_va == 0) {
        return;
    }
    ds_buffer_init(&b, (u32 *)(usize)g_panel_va, DS_WM_PANEL_W,
                   DS_WM_PANEL_H);
    ds_wm_draw_panel(&b, &g_layout, &g_tasks, g_self_id, &g_apps, 0);
}

/* ------------------------------------------------------------------------- */
/* 启动器                                                                     */
/* ------------------------------------------------------------------------- */

static void wm_scan_dir(const char *dir)
{
    struct wm_dirent ents[DS_LIST_MAX];
    i64 n;
    i64 i;

    n = w_sys(W_SYS_LISTDIR, (i64)dir, (i64)ents, (i64)DS_LIST_MAX);
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        u32 len = ents[i].name_len;
        u32 is_dir = (ents[i].type == W_NODE_DIR) ? 1u : 0u;

        if (len >= (u32)sizeof(ents[i].name)) {
            len = (u32)sizeof(ents[i].name) - 1u;
        }
        (void)ds_wm_launcher_add(&g_apps, dir, ents[i].name, len, is_dir);
    }
}

static void wm_launcher_scan(void)
{
    u32 i;

    ds_wm_launcher_init(&g_apps);
    wm_scan_dir("/bin");
    wm_scan_dir("/System/Packages");
    ds_wm_launcher_finalize(&g_apps);

    w_put("ng: wm launcher ok (apps=");
    w_putdec(g_apps.launchable);
    w_put(" pkgs=");
    w_putdec(g_apps.pkgs);
    w_put(")\r\n");
    for (i = 0; i < g_apps.launchable && i < 9u; i++) {
        const struct ds_wm_app *a = ds_wm_launcher_at(&g_apps, i);

        if (a == NULL) {
            break;
        }
        w_put("ng: wm app ok (n=");
        w_putdec(i);
        w_put(" path=");
        w_put(a->path);
        w_put(")\r\n");
    }
    for (i = 0; i < g_apps.count; i++) {
        if (g_apps.app[i].kind == DS_WM_APP_PKG) {
            w_put("ng: wm pkg ok (path=");
            w_put(g_apps.app[i].path);
            w_put(")\r\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 协议动作                                                                   */
/* ------------------------------------------------------------------------- */

static void wm_send_list(void)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_LIST;
    m.seq = ++g_seq;
    (void)w_send_msg(&m);
}

static void wm_focus(u32 id)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.seq = ++g_seq;
    m.u.focus.surface_id = id;
    m.u.focus.token = g_token;
    (void)w_send_msg(&m);
}

static void wm_print_task_lines(void)
{
    u32 i;

    for (i = 0; i < g_tasks.count && i < DS_WM_TASKS_MAX; i++) {
        const struct ds_wm_task *t = &g_tasks.task[i];
        u32 k;
        int seen = 0;

        if (t->id == 0u || t->id == g_self_id) {
            continue;
        }
        for (k = 0; k < g_printed_count; k++) {
            if (g_printed_ids[k] == t->id) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (g_printed_count < DS_WM_TASKS_MAX) {
            g_printed_ids[g_printed_count++] = t->id;
        }
        w_put("ng: wm task ok (id=");
        w_putdec(t->id);
        w_put(" x=");
        w_putdec(t->x);
        w_put(" y=");
        w_putdec(t->y);
        w_put(" title=");
        w_put((t->title[0] != '\0') ? t->title : "app");
        w_put(")\r\n");
    }
}

static void wm_handle_list(const struct ds_msg *m)
{
    u32 before;
    u32 apps;

    (void)ds_wm_tasks_from_msg(m, &g_tasks);
    apps = ds_wm_tasks_apps(&g_tasks, g_self_id);
    before = g_drawn_tasks * 131u + g_drawn_focus;

    if (!g_tasks_printed && apps >= 1u) {
        g_tasks_printed = 1;
        g_seen_tasks = 1;
        g_idle_rounds = 0;
        g_empty_idle = 0;
        w_put("ng: wm tasks ok (n=");
        w_putdec(g_tasks.count);
        w_put(" apps=");
        w_putdec(apps);
        w_put(")\r\n");
        wm_print_task_lines();
        wm_report_if_changed();
    } else if (g_tasks_printed && g_printed_count < apps) {
        wm_print_task_lines();
    }

    if (g_tasks.focus_id != g_last_focus) {
        g_last_focus = g_tasks.focus_id;
        wm_report_if_changed();
    }

    /* 任务按钮/焦点变化时重画面板（避免每 200ms 一次 COMMIT 噪声）。 */
    if (before != g_tasks.count * 131u + g_tasks.focus_id) {
        g_drawn_tasks = g_tasks.count;
        g_drawn_focus = g_tasks.focus_id;
        wm_draw_panel();
        wm_commit_panel();
        g_panel_px = wm_panel_value();
    }

    /* FOCUS 确认：下一次 LIST 的 focus 就是目标 -> 任务切换完成。 */
    if (g_switch_pending != 0u && g_tasks.focus_id == g_switch_pending) {
        g_switch_pending = 0;
        g_switches++;
        g_last_switch = g_tasks.focus_id;
        g_launch_idle = 0;
        w_put("ng: wm task switch ok (focus=");
        w_putdec(g_tasks.focus_id);
        w_put(" from=");
        w_putdec(g_switch_from);
        w_put(" via=");
        w_put((g_switch_via == 0u) ? "key" : "auto");
        w_put(")\r\n");
        wm_report_if_changed();
    }
}

static void wm_do_switch(u32 via)
{
    u32 target = 0;

    if (g_switch_pending != 0u) {
        return;
    }
    if (ds_wm_tasks_next(&g_tasks, g_self_id, &target) != DS_OK) {
        w_put("ng: wm task switch FAIL (no task)\r\n");
        return;
    }
    g_switch_from = g_tasks.focus_id;
    g_switch_pending = target;
    g_switch_via = via;
    wm_focus(target);
}

static void wm_do_launch(u32 index, u32 via)
{
    const struct ds_wm_app *app = ds_wm_launcher_at(&g_apps, index);
    const char *argv[2];
    u32 pid = 0;
    i64 rc;

    if (app == NULL) {
        w_put("ng: wm launch FAIL (index)\r\n");
        return;
    }
    if (g_session != 0) {
        /*
         * WP3-3：应用生命周期归 sessiond —— 启动器只发 LAUNCH，不 spawn。
         * sessiond 校验 WM 令牌后 spawn，并把 pid/窗口记进注册表。
         */
        struct session_msg m;

        ds_memzero(&m, (u64)sizeof(m));
        m.type = SESSION_MSG_LAUNCH;
        m.seq = ++g_seq;
        m.u.launch.token = g_token;
        session_path_set(m.u.launch.path, app->path);
        (void)w_send_session(&m);
        g_launches++;
        g_last_launch_pid = 0;
        w_put("ng: wm launch req ok (app=");
        w_put(app->path);
        w_put(" via=");
        w_put((via == 0u) ? "key" : "auto");
        w_put(")\r\n");
        wm_report_if_changed();
        return;
    }
    /* 直连兼容路径（只有 WP3-2 的内核自检会走到；真实启动有会话通道）。 */
    argv[0] = app->path;
    argv[1] = NULL;
    rc = w_sys4(W_SYS_SPAWN, (i64)app->path, (i64)argv, (i64)&pid, 0);
    if (rc < 0 || pid == 0) {
        w_put("ng: wm launch FAIL (spawn rc=");
        w_putdec((u32)(-rc));
        w_put(")\r\n");
        return;
    }
    g_launches++;
    g_last_launch_pid = pid;
    w_put("ng: wm launch ok (app=");
    w_put(app->path);
    w_put(" pid=");
    w_putdec(pid);
    w_put(" via=");
    w_put((via == 0u) ? "key" : "auto");
    w_put(")\r\n");
    wm_report_if_changed();
}

static void wm_handle_input(const struct ds_msg *m)
{
    u32 key = m->u.input.key;
    u32 flags = m->u.input.flags;
    u32 index = DS_WM_LAUNCH_INDEX_NONE;
    int action;

    /* 只处理全局热键副本；焦点转发的普通按键归客户端。 */
    if (!ds_wm_input_is_global(flags)) {
        return;
    }
    /*
     * WP3-5：S1 场景 d 的故障注入钩子。F12 在正常 WM 里是"其它键"，这里
     * 只在 fault=spin 模式下把它变成"WM 死循环"：协作式调度没有抢占，
     * 自旋后 sessiond/displayd 都再也拿不到 CPU（已知限制，见 claims）。
     */
    if (g_fault_spin != 0u && m->u.input.scancode == 0x58u) {
        w_put("ng: wm fault spin (scancode=0x58)\r\n");
        for (;;) {
            __asm__ volatile("pause");
        }
    }
    action = ds_wm_hotkey_consume(&g_hotkey, key, flags, &index);
    if (action == DS_WM_HOTKEY_TASK_SWITCH) {
        wm_do_switch(0);
    } else if (action == DS_WM_HOTKEY_LAUNCH) {
        wm_do_launch(index, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

#define W_OPAQUE(rgb) (0xFF000000u | (rgb))

static int wm_setup_panel(void)
{
    struct ds_msg m;
    u32 shm_handle = 0;
    u32 pages = 0;
    u32 va = 0;
    u32 tries;
    i64 rc;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_CREATE;
    m.seq = ++g_seq;
    m.u.create.w = DS_WM_PANEL_W;
    m.u.create.h = DS_WM_PANEL_H;
    m.u.create.flags = DS_CREATE_F_POS;
    m.u.create.x = 0;
    m.u.create.y = 0;
    m.u.create.owner_pid = (u32)w_sys(W_SYS_GETPID, 0, 0, 0);
    m.u.create.token = g_token; /* v3：会话 manager 签发的 WM 令牌 */
    ds_title_set(m.u.create.title, "Aster Panel");
    if (w_send_msg(&m) != 0) {
        w_put("ng: wm FAIL (create send)\r\n");
        return 1;
    }
    for (tries = 0; tries < DS_REPLY_WAIT_TRIES; tries++) {
        int type = w_recv_msg(&m, DS_REPLY_WAIT_TICKS);

        if (type == (int)DS_MSG_CREATE_REPLY) {
            break;
        }
        if (type == W_ETIMEDOUT) {
            continue;
        }
        if (type < 0) {
            w_put("ng: wm FAIL (create reply)\r\n");
            return 1;
        }
    }
    if (m.type != DS_MSG_CREATE_REPLY || m.status != 0 ||
        m.u.create_reply.surface_id == 0) {
        w_put("ng: wm FAIL (create status)\r\n");
        return 1;
    }
    g_self_id = m.u.create_reply.surface_id;
    shm_handle = m.u.create_reply.shm_handle;
    pages = m.u.create_reply.pages;

    rc = w_sys(W_SYS_SHM_MAP, (i64)shm_handle, (i64)&va, 0);
    if (rc != 0 || va == 0) {
        w_put("ng: wm FAIL (shm map rc=");
        w_putdec((u32)(-rc));
        w_put(")\r\n");
        return 1;
    }
    g_panel_va = va;

    ds_wm_layout_init(&g_layout, DS_WM_PANEL_W);
    wm_draw_panel();

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_MAP;
    m.seq = ++g_seq;
    m.u.map.surface_id = g_self_id;
    (void)w_send_msg(&m);

    wm_commit_panel();
    g_panel_px = wm_panel_value();

    w_put("ng: wm up (pid=");
    w_putdec((u32)w_sys(W_SYS_GETPID, 0, 0, 0));
    w_put(" panel=1280x32)\r\n");
    w_put("ng: wm panel ok (px=");
    w_puthex(g_panel_px);
    w_put(" at=0,0)\r\n");
    (void)pages;
    return 0;
}

static void wm_register_hotkey(void)
{
    struct ds_msg m;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_REGISTER_HOTKEY;
    m.seq = ++g_seq;
    m.u.register_hotkey.surface_id = g_self_id;
    m.u.register_hotkey.key = DS_HOTKEY_ANY;
    if (w_send_msg(&m) != 0) {
        w_put("ng: wm FAIL (hotkey send)\r\n");
        return;
    }
    g_hotkey.registered = 1;
    wm_report_if_changed();
}

static void wm_run(void)
{
    u32 rounds;

    wm_send_list();
    if (g_session != 0) {
        wm_session_heartbeat();
    }
    for (rounds = 0; rounds < W_ROUNDS; rounds++) {
        struct w_frame f;
        int rc = w_recv_frame(&f, W_POLL_TICKS);

        g_rounds = rounds;
        if (rc == 0) {
            if (f.kind == 1) {
                wm_handle_session_msg(&f.ss);
            } else if (f.ds.type == (u16)DS_MSG_LIST_REPLY) {
                wm_handle_list(&f.ds);
            } else if (f.ds.type == (u16)DS_MSG_INPUT) {
                wm_handle_input(&f.ds);
            }
        } else if (rc != W_ETIMEDOUT) {
            w_put("ng: wm display gone (rc=");
            w_putdec((u32)(-rc));
            w_put(")\r\n");
            break;
        }

        if ((rounds % W_LIST_EVERY) == (W_LIST_EVERY - 1u)) {
            wm_send_list();
            if (g_session != 0) {
                wm_session_heartbeat();
            }
        }

        /* 自检 fallback：没有 QMP 输入时也能产出切换/启动证据。 */
        if (g_seen_tasks != 0u && g_switches == 0u && g_switch_pending == 0u) {
            g_idle_rounds++;
            if (g_idle_rounds >= W_AUTO_ROUNDS && g_auto_switch_tried == 0u) {
                g_auto_switch_tried = 1;
                wm_do_switch(1);
            }
        }
        /*
         * 会话自检：一个应用窗口都没有时（WP3-3 每次会话从零开始），
         * 3s 后自动启动第一个应用 —— QMP 模式下通常先收到 '1'。
         */
        if (g_seen_tasks == 0u && g_launches == 0u) {
            g_empty_idle++;
            if (g_empty_idle >= W_AUTO_ROUNDS && g_auto_launch_tried == 0u) {
                g_auto_launch_tried = 1;
                wm_do_launch(0, 1);
            }
        }
        if (g_switches > 0u && g_launches == 0u) {
            g_launch_idle++;
            if (g_launch_idle >= W_AUTO_ROUNDS && g_auto_launch_tried == 0u) {
                g_auto_launch_tried = 1;
                wm_do_launch(0, 1);
            }
        }
    }
}

int w_main(long argc, char **argv)
{
    u32 pid = (u32)w_sys(W_SYS_GETPID, 0, 0, 0);
    u32 i;
    i64 rc;

    /* 会话参数：argv 里的 `session=<ticket>` / `token=<hex>`（WP3-3）。 */
    g_session = 0;
    g_token = 0;
    g_fault_spin = 0;
    for (i = 1; argv != NULL && i < (u32)argc; i++) {
        u32 v = 0;

        if (session_arg_parse_u32(argv[i], "session=", &v) != 0) {
            g_session = v;
        } else if (session_arg_parse_u32(argv[i], "token=", &v) != 0) {
            g_token = v;
        } else if (w_streq(argv[i], "fault=spin") != 0) {
            g_fault_spin = 1; /* WP3-5：S1 场景 d 才出现 */
        }
    }
    rc = w_sys(W_SYS_MKDIR, (i64)"/run", 0755, 0);
    if (rc != 0 && rc != W_EEXIST) {
        /* displayd 通常已经建好 /run；失败不致命（WM 状态经 WM_STATUS 上报）。 */
    }
    g_server = w_read_server_ticket();
    if (g_server == 0) {
        w_put("ng: wm FAIL (no display port, pid=");
        w_putdec(pid);
        w_put(")\r\n");
        (void)w_sys(W_SYS_EXIT, 1, 0, 0);
    }
    rc = w_sys(W_SYS_PORT_CREATE, (i64)DS_CLIENT_PORT_DEPTH, 0, 0);
    if (rc <= 0) {
        w_put("ng: wm FAIL (port)\r\n");
        (void)w_sys(W_SYS_EXIT, 1, 0, 0);
    }
    g_port = (u32)rc;

    ds_memzero(&g_tasks, (u64)sizeof(g_tasks));
    ds_wm_hotkey_init(&g_hotkey);
    ds_wm_launcher_init(&g_apps);
    if (wm_setup_panel() != 0) {
        (void)w_sys(W_SYS_PORT_CLOSE, (i64)g_port, 0, 0);
        (void)w_sys(W_SYS_EXIT, 1, 0, 0);
    }
    wm_register_hotkey();
    wm_launcher_scan();
    wm_report_if_changed();
    wm_session_register();
    wm_run();

    (void)w_sys(W_SYS_PORT_CLOSE, (i64)g_port, 0, 0);
    w_put("ng: wm exit (switch=");
    w_putdec(g_switches);
    w_put(" launch=");
    w_putdec(g_launches);
    w_put(")\r\n");
    (void)w_sys(W_SYS_EXIT, 0, 0, 0);
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call w_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
