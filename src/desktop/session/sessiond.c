/*
 * ng WP3-3: session manager（L2c，ring3 进程 /bin/sessiond）。
 *
 * 职责（应用生命周期归 session，不归 shell）：
 *   1. 启动：自己开主端口 -> spawn displayd（等 /run/display.port）-> spawn wm
 *      （带 WM 角色令牌）。**init 只 spawn sessiond；displayd/wm 由 sessiond 拉起。**
 *   2. 应用生命周期：只有 WM 能发 LAUNCH（令牌角色校验）；sessiond `spawn`
 *      并记录 {pid, token, role, 窗口}；EXIT/KILL 回收；心跳超时判断崩溃，
 *      回收窗口/进程；WM 崩溃按 restart_limit=3 + 指数退避（TSC tick）重启。
 *   3. 窗口注册表：{surface_id, app_pid, title, token}；LOOKUP/LIST 对外查询；
 *      每 ~200ms 与 displayd 的 LIST 同步（id/owner_pid/token 三字段校验）。
 *   4. 角色令牌：启动/LAUNCH 时签发 32 位 role_token（经 argv 交给进程）；
 *      displayd 的 CREATE v3 带 token、FOCUS v3 校验 token（见 displayd.c）。
 *   5. 焦点策略：新窗口 REGISTER 后由 sessiond 发 FOCUS（令牌为窗口自己的，
 *      displayd 校验通过）；WM 的任务切换仍直接 FOCUS（令牌角色 = WM）。
 *
 * 发现：sessiond 端口 ticket 经 argv（`session=<ticket>`）交给被启动进程，
 * 不新建 ramfs 发现文件；状态写 `/run/session.status`（内核 wp33 自检读）。
 *
 * 无 libc：syscall 内联；字符串/十进制/十六进制自己实现。
 */
#include "ds_protocol.h"
#include "session_protocol.h"
#include "session_table.h"
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- syscall 号（与 src/ng/syscall.h 一致） ------------------------------ */
#define S_SYS_WRITE 0
#define S_SYS_EXIT 1
#define S_SYS_PORT_CREATE 10
#define S_SYS_PORT_SEND 11
#define S_SYS_PORT_RECV 12
#define S_SYS_PORT_REPLY 13
#define S_SYS_PORT_CLOSE 14
#define S_SYS_OPEN 20
#define S_SYS_READ 21
#define S_SYS_WRITE_FD 22
#define S_SYS_CLOSE 23
#define S_SYS_MKDIR 26
#define S_SYS_UNLINK 27
#define S_SYS_SPAWN 40
#define S_SYS_WAIT 41
#define S_SYS_GETPID 42
#define S_SYS_KILL 43

/* ---- VFS 常量 ------------------------------------------------------------ */
#define S_O_RDONLY 0x000u
#define S_O_WRONLY 0x001u
#define S_O_CREAT 0x040u
#define S_O_TRUNC 0x200u
#define S_EEXIST (-17)

#define S_ETIMEDOUT (-110)

/* ---- 路径 / 时序 --------------------------------------------------------- */
#define S_WM_PATH "/bin/wm"
#define S_DISPLAYD_PATH "/bin/displayd"
#define S_DEMO_PATH "/bin/demo"

#define S_POLL_TICKS DS_POLL_TICKS      /* 20ms */
#define S_LIST_EVERY 10u                /* 10 * 20ms = 200ms 一次 LIST 同步 */
#define S_STALE_TICKS 800000000ULL      /* 800ms 没心跳 = 崩溃 */
#define S_STARTUP_TRIES 300u            /* 3s 等 displayd 的端口文件 */
#define S_STATUS_EVERY 25u              /* 500ms 兜底重写状态文件 */
#define S_MAX_PRINTS 8u                 /* 每类证据行最多打印次数 */

/* ------------------------------------------------------------------------- */
/* syscall / 字符串                                                           */
/* ------------------------------------------------------------------------- */

static i64 s_sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 s_sys(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return s_sys4(nr, a1, a2, a3, 0);
}

static u64 s_now(void)
{
    u32 lo;
    u32 hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

static u32 s_strlen(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static void s_put(const char *s)
{
    u32 n = s_strlen(s);

    if (n != 0) {
        (void)s_sys(S_SYS_WRITE, (i64)s, (i64)n, 0);
    }
}

static u32 s_u32str(u32 v, char *out)
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

static void s_putdec(u32 v)
{
    char buf[12];

    (void)s_u32str(v, buf);
    s_put(buf);
}

static u32 s_hexstr(u32 v, char *out)
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

static void s_puthex(u32 v)
{
    char buf[10];

    s_put("0x");
    (void)s_hexstr(v, buf);
    s_put(buf);
}

static int s_streq(const char *a, const char *b)
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
static u32 s_read_display_ticket(void)
{
    char buf[24];
    u32 n = 0;
    u32 v = 0;
    int fd;
    i64 r;

    fd = (int)s_sys(S_SYS_OPEN, (i64)DS_PORT_PATH, (i64)S_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    r = s_sys(S_SYS_READ, (i64)fd, (i64)buf, (i64)(sizeof(buf) - 1u));
    (void)s_sys(S_SYS_CLOSE, (i64)fd, 0, 0);
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

static u32 g_pid;
static u32 g_port;      /* session 主端口（客户端 LAUNCH/REGISTER/...） */
static u32 g_disp_port; /* display 客户端端口（LIST/DESTROY/FOCUS 的 from） */
static u32 g_disp_ticket;
static u32 g_disp_pid;
static u32 g_seq;
static u32 g_round;

static struct session_table g_tab;

#define S_WM_STATE_NONE 0u   /* 没有 WM（还没起来 / 已放弃） */
#define S_WM_STATE_RUNNING 1u
#define S_WM_STATE_WAIT 2u   /* 等退避到期后重启 */

static u32 g_wm_pid;
static u32 g_wm_state;
static u32 g_wm_restarts;     /* WM 重启次数（成功拉起即 +1） */
static u64 g_wm_seen;         /* 最近一次 WM REGISTER/HEARTBEAT 的 TSC */
static u64 g_restart_at;      /* 退避到期时间（TSC） */
static u32 g_restart_pending; /* 重启后等新 WM REGISTER（打印 restart ready） */
static u32 g_status_dirty;
static u32 g_registry_prints;
static u32 g_clamp_prints;
static u32 g_token_printed;
static u32 g_last_apps;
static u32 g_last_windows;
static u32 g_launch_prints;
static u32 g_reject_prints;
/*
 * WP3-5：S1 故障注入钩子。sessiond 自身行为不变；只有存在启动参数
 * `fault=spin` 时才把它透传给 WM，WM 收到 F12 后进入死循环（无抢占的
 * 已知限制，见 docs/design/session-ng.md 第 12 节）。正式启动（wp33 的
 * init -> sessiond）没有这个参数，不影响产品路径。
 */
static u32 g_fault_spin;

/* ------------------------------------------------------------------------- */
/* 状态文件（经 displayd 的 /run/display.status 汇总）                        */
/* ------------------------------------------------------------------------- */

static int s_send_ds(const struct ds_msg *m, u32 from);

static void s_status_write(void)
{
    struct ds_msg m;

    g_status_dirty = 0;
    if (g_disp_ticket == 0) {
        return;
    }
    /*
     * 状态汇总走 displayd 已有的 `/run/display.status`（SESSION_STATUS），
     * **不新建 ramfs 文件**：节点池只有 32 个，WP3-3 又种了一个 /bin/sessiond。
     * 内核 wp33 读同一份文件的 s* 字段。
     */
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_SESSION_STATUS;
    m.seq = ++g_seq;
    m.u.session_status.pid = g_pid;
    m.u.session_status.dpid = g_disp_pid;
    m.u.session_status.procs = session_procs(&g_tab);
    m.u.session_status.apps = session_apps(&g_tab);
    m.u.session_status.windows = session_windows(&g_tab);
    m.u.session_status.tokens = session_tokens(&g_tab);
    m.u.session_status.launches = (u32)g_tab.launches;
    m.u.session_status.kills = (u32)g_tab.kills;
    m.u.session_status.exits = (u32)g_tab.exits;
    m.u.session_status.reaps = (u32)g_tab.reaps;
    m.u.session_status.restarts = g_wm_restarts;
    m.u.session_status.wmpid = g_wm_pid;
    m.u.session_status.wmstate = g_wm_state;
    m.u.session_status.apppid = session_last_app_pid(&g_tab);
    m.u.session_status.tokenfail = (u32)g_tab.token_fail;
    m.u.session_status.registry = g_tab.registry_ok;
    (void)s_send_ds(&m, g_disp_port);
}

/* ------------------------------------------------------------------------- */
/* IPC：session 协议（主端口）                                                */
/* ------------------------------------------------------------------------- */

static int s_reply_sess(u32 to, const struct session_msg *m)
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
    return (int)s_sys4(S_SYS_PORT_REPLY, (i64)to, (i64)buf,
                       (i64)(SESSION_IPC_HDR_SIZE + (u32)n), 0);
}

static int s_recv_sess(u32 port, struct session_msg *m, u32 *out_from,
                       u64 timeout)
{
    u8 buf[SESSION_IPC_HDR_SIZE + SESSION_MSG_MAX];
    struct session_ipc_hdr h;
    i64 r = s_sys4(S_SYS_PORT_RECV, (i64)port, (i64)buf, (i64)sizeof(buf),
                   (i64)timeout);

    if (r < 0) {
        return (int)r;
    }
    if ((u64)r < (u64)SESSION_IPC_HDR_SIZE || (u64)r > (u64)sizeof(buf)) {
        return DS_EINVAL;
    }
    ds_memcopy(&h, buf, SESSION_IPC_HDR_SIZE);
    if ((u64)h.len + (u64)SESSION_IPC_HDR_SIZE != (u64)r ||
        h.len > (u32)SESSION_MSG_MAX) {
        return DS_EINVAL;
    }
    if (session_msg_decode(buf + SESSION_IPC_HDR_SIZE, h.len, m) != DS_OK) {
        return DS_EINVAL;
    }
    if (out_from != NULL) {
        *out_from = h.from;
    }
    return (int)m->type;
}

/* ------------------------------------------------------------------------- */
/* IPC：display 协议（LIST/DESTROY/FOCUS）                                    */
/* ------------------------------------------------------------------------- */

static int s_send_ds(const struct ds_msg *m, u32 from)
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
    return (int)s_sys4(S_SYS_PORT_SEND, (i64)g_disp_ticket, (i64)buf,
                       (i64)(DS_IPC_HDR_SIZE + (u32)n), 0);
}

static int s_recv_ds(struct ds_msg *m, u64 timeout)
{
    u8 buf[DS_IPC_HDR_SIZE + DS_MSG_MAX];
    struct ds_ipc_hdr h;
    i64 r = s_sys4(S_SYS_PORT_RECV, (i64)g_disp_port, (i64)buf,
                   (i64)sizeof(buf), (i64)timeout);

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
    return (int)m->type;
}

/* 让某个窗口获得焦点（令牌 = 窗口自己的令牌；displayd v3 校验）。 */
static void s_focus_surface(u32 surface_id, u32 token)
{
    struct ds_msg m;

    if (g_disp_ticket == 0 || surface_id == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_FOCUS;
    m.seq = ++g_seq;
    m.u.focus.surface_id = surface_id;
    m.u.focus.token = token;
    (void)s_send_ds(&m, g_disp_port);
}

/* 回收一个窗口（客户端已经死了/退出了；会话管理器有权清理）。
 * v3：带上窗口自己的 role_token —— displayd 的 DESTROY 现在做归属校验，
 * 不再接受"知道 id 就能销毁"。 */
static void s_destroy_surface(u32 surface_id, u32 token)
{
    struct ds_msg m;

    if (g_disp_ticket == 0 || surface_id == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_DESTROY;
    m.seq = ++g_seq;
    m.u.destroy.surface_id = surface_id;
    m.u.destroy.token = token;
    (void)s_send_ds(&m, g_disp_port);
}

/* ------------------------------------------------------------------------- */
/* 进程操作                                                                   */
/* ------------------------------------------------------------------------- */

static u32 s_spawn(const char *path, const char *const *argv)
{
    u32 pid = 0;
    i64 rc = s_sys4(S_SYS_SPAWN, (i64)path, (i64)argv, (i64)&pid, 0);

    if (rc < 0 || pid == 0) {
        return 0;
    }
    return pid;
}

/* kill + wait：让子进程变成僵尸并回收状态（wait 不重用槽位）。 */
static int s_kill_wait(u32 pid, int *out_status)
{
    int status = -1;

    if (pid == 0) {
        return -1;
    }
    (void)s_sys4(S_SYS_KILL, (i64)pid, 9, 0, 0);
    if (s_sys4(S_SYS_WAIT, (i64)pid, (i64)&status, 0, 0) < 0) {
        status = -1;
    }
    if (out_status != NULL) {
        *out_status = status;
    }
    return status;
}

/* ------------------------------------------------------------------------- */
/* 启动：spawn displayd + wm                                                  */
/* ------------------------------------------------------------------------- */

static u32 s_make_token(u32 role)
{
    u32 nonce = (u32)((s_now() >> 7) ^ ((u64)g_pid << 16));
    u32 seq = g_tab.next_seq + 1u;

    if (nonce == 0) {
        nonce = 0x5A5Au;
    }
    return session_token_make(role, seq, nonce);
}

static int s_send_ds(const struct ds_msg *m, u32 from);
static int s_recv_ds(struct ds_msg *m, u64 timeout);
static void s_on_list(const struct ds_msg *m);

/*
 * 端口发现要防"陈旧文件"：/run/display.port 是上一个 displayd（WP3-1/3-2 的
 * 自检）留下的，直接读会拿到死进程的 ticket。所以先 unlink，再等新 displayd
 * 写文件，并用 LIST/LIST_REPLY 握手确认对端活着（失败就重试）。
 */
static int s_display_ping(void)
{
    struct ds_msg m;
    u32 tries;

    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_LIST;
    m.seq = ++g_seq;
    if (s_send_ds(&m, g_disp_port) != 0) {
        return 0;
    }
    for (tries = 0; tries < 20u; tries++) {
        int type = s_recv_ds(&m, S_POLL_TICKS);

        if (type == (int)DS_MSG_LIST_REPLY) {
            s_on_list(&m);
            return 1;
        }
        if (type >= 0) {
            continue;
        }
        if (type != S_ETIMEDOUT) {
            break;
        }
    }
    return 0;
}

static int s_start_displayd(void)
{
    const char *argv[2];
    struct session_msg tmp;
    u32 pid;
    u32 tries;
    u32 ticket = 0;
    int port_ok = 0;

    /* 丢掉上一个会话/自检留下的发现文件（否则会读到死 displayd 的 ticket）。 */
    (void)s_sys(S_SYS_UNLINK, (i64)DS_PORT_PATH, 0, 0);
    argv[0] = S_DISPLAYD_PATH;
    argv[1] = NULL;
    pid = s_spawn(S_DISPLAYD_PATH, argv);
    if (pid == 0) {
        s_put("ng: sessiond FAIL (displayd spawn)\r\n");
        return 1;
    }
    g_disp_pid = pid;
    for (tries = 0; tries < S_STARTUP_TRIES; tries++) {
        ticket = s_read_display_ticket();
        if (ticket == 0) {
            (void)s_recv_sess(g_port, &tmp, NULL, S_POLL_TICKS);
            continue;
        }
        g_disp_ticket = ticket;
        if (!port_ok) {
            i64 rc = s_sys(S_SYS_PORT_CREATE, (i64)SESSION_PORT_DEPTH, 0, 0);

            if (rc <= 0) {
                s_put("ng: sessiond FAIL (display client port)\r\n");
                return 1;
            }
            g_disp_port = (u32)rc;
            port_ok = 1;
        }
        if (s_display_ping()) {
            return 0;
        }
        g_disp_ticket = 0;
        (void)s_recv_sess(g_port, &tmp, NULL, S_POLL_TICKS);
    }
    s_put("ng: sessiond FAIL (display port)\r\n");
    return 1;
}

/*
 * 拉起 WM：argv = {/bin/wm, "session=<ticket>", "token=0x<hex>"}。
 * 令牌每次重启都重新签发（旧令牌随旧进程一起作废）。
 */
static int s_start_wm(void)
{
    char ticket_arg[24];
    char token_arg[24];
    const char *argv[5];
    u32 pid;
    u32 token;
    int slot;
    int argc = 0;

    (void)s_u32str(g_port, ticket_arg + 8);
    ds_memcopy(ticket_arg, "session=", 8);
    ds_memcopy(token_arg, "token=0x", 8);
    token = s_make_token(SESSION_ROLE_WM);
    (void)s_hexstr(token, token_arg + 8);

    argv[argc++] = S_WM_PATH;
    argv[argc++] = ticket_arg;
    argv[argc++] = token_arg;
    if (g_fault_spin != 0u) {
        argv[argc++] = "fault=spin"; /* WP3-5：只在 S1 场景 d 出现 */
    }
    argv[argc] = NULL;
    pid = s_spawn(S_WM_PATH, argv);
    if (pid == 0) {
        s_put("ng: sessiond FAIL (wm spawn)\r\n");
        return 1;
    }
    slot = session_alloc_with_token(&g_tab, pid, SESSION_ROLE_WM, s_now(),
                                    token);
    if (slot < 0) {
        s_put("ng: sessiond FAIL (wm table)\r\n");
        (void)s_sys4(S_SYS_KILL, (i64)pid, 9, 0, 0);
        return 1;
    }
    g_wm_pid = pid;
    g_wm_state = S_WM_STATE_RUNNING;
    g_wm_seen = s_now();
    g_status_dirty = 1;
    return 0;
}

static void s_startup(void)
{
    (void)s_sys(S_SYS_MKDIR, (i64)"/run", 0755, 0);
    {
        i64 rc = s_sys(S_SYS_PORT_CREATE, (i64)SESSION_PORT_DEPTH, 0, 0);

        if (rc <= 0) {
            s_put("ng: sessiond FAIL (port)\r\n");
            (void)s_sys(S_SYS_EXIT, 1, 0, 0);
        }
        g_port = (u32)rc;
    }
    if (s_start_displayd() != 0) {
        (void)s_sys(S_SYS_EXIT, 1, 0, 0);
    }
    if (s_start_wm() != 0) {
        (void)s_sys(S_SYS_EXIT, 1, 0, 0);
    }
    s_put("ng: sessiond up (pid=");
    s_putdec(g_pid);
    s_put(" displayd=");
    s_putdec(g_disp_pid);
    s_put(" wm=");
    s_putdec(g_wm_pid);
    s_put(" port=");
    s_putdec(g_port);
    s_put(")\r\n");
    s_status_write();
}

/* ------------------------------------------------------------------------- */
/* 消息处理                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * v3 审计加固：会话请求除了 role_token，还要求来自**该会话项登记过的端口**。
 * 令牌经 argv 下发、也可能从 displayd 的 LIST 里被读到；只靠令牌的话，
 * 偷到令牌的客户端就能替别人 REGISTER/HEARTBEAT/EXIT/LAUNCH/KILL。端口
 * 绑定让"只偷到令牌"不够用（还要知道受害者的端口 ticket）。
 *
 * 局限：内核的 IPC `from` 只是端口 ticket，且发送方可以把它填成任意存活
 * 端口（syscall 审计会话的发现）；真正的 peer 身份校验需要内核在 syscall
 * 边界填 from，不在本会话范围。这里做的是协议层的最低限度加固。
 */
static int s_channel_ok(const struct session_entry *e, u32 from)
{
    if (e == NULL || from == 0) {
        return 0;
    }
    if (e->port != 0) {
        return (from == e->port) ? 1 : 0;
    }
    return 1; /* 首次 REGISTER：端口在本处理函数里绑定 */
}

static void s_channel_reject(const char *what, u32 from)
{
    g_tab.rejects++;
    if (g_reject_prints < S_MAX_PRINTS) {
        g_reject_prints++;
        s_put("ng: session channel reject ok (");
        s_put(what);
        s_put(" from=");
        s_puthex(from);
        s_put(" n=");
        s_putdec((u32)g_tab.rejects);
        s_put(")\r\n");
    }
    g_status_dirty = 1;
}

static void s_handle_register(const struct session_msg *m, u32 from)
{
    char title[DS_TITLE_MAX];
    struct session_entry *e;
    int rc;

    ds_title_get(m->u.reg.title, title, (u32)sizeof(title));
    e = session_by_token(&g_tab, m->u.reg.token);
    if (e == NULL) {
        g_tab.rejects++;
        if (g_reject_prints < S_MAX_PRINTS) {
            g_reject_prints++;
            s_put("ng: session register reject ok (token=");
            s_puthex(m->u.reg.token);
            s_put(" n=");
            s_putdec((u32)g_tab.rejects);
            s_put(")\r\n");
        }
        g_status_dirty = 1;
        return;
    }
    if (!s_channel_ok(e, from)) {
        s_channel_reject("register", from);
        return;
    }
    /* 上报的回复端口必须就是发送端口：否则 port=0 会让后面的通道绑定失效。 */
    if (m->u.reg.port == 0 || m->u.reg.port != from) {
        s_channel_reject("register-port", from);
        return;
    }
    rc = session_register_window(&g_tab, m->u.reg.token, m->u.reg.port,
                                 m->u.reg.surface_id, title, s_now());
    if (rc != DS_OK) {
        g_status_dirty = 1;
        return;
    }
    e = session_by_token(&g_tab, m->u.reg.token);
    if (e == NULL) {
        return;
    }
    e->restarts = (e->role == SESSION_ROLE_WM) ? g_wm_restarts : 0;
    if (e->role == SESSION_ROLE_WM) {
        g_wm_pid = e->pid;
        g_wm_seen = s_now();
        g_wm_state = S_WM_STATE_RUNNING;
        if (g_restart_pending != 0u) {
            g_restart_pending = 0;
            s_put("ng: session wm restart ready ok (wm=");
            s_putdec(e->pid);
            s_put(" id=");
            s_putdec(e->surface_id);
            s_put(")\r\n");
        } else {
            s_put("ng: session wm register ok (wm=");
            s_putdec(e->pid);
            s_put(" id=");
            s_putdec(e->surface_id);
            s_put(")\r\n");
        }
    } else {
        s_put("ng: session window ok (pid=");
        s_putdec(e->pid);
        s_put(" id=");
        s_putdec(e->surface_id);
        s_put(" title=");
        s_put((title[0] != '\0') ? title : "app");
        s_put(")\r\n");
    }
    /* 焦点策略：新窗口（重新）获得焦点（令牌 = 窗口自己的）。 */
    if (e->surface_id != 0) {
        s_focus_surface(e->surface_id, e->token);
        s_put("ng: session focus ok (id=");
        s_putdec(e->surface_id);
        s_put(" pid=");
        s_putdec(e->pid);
        s_put(")\r\n");
    }
    g_status_dirty = 1;
}

static void s_handle_heartbeat(const struct session_msg *m, u32 from)
{
    struct session_entry *e = session_by_token(&g_tab, m->u.hb.token);

    if (e == NULL) {
        g_tab.rejects++;
        return;
    }
    if (!s_channel_ok(e, from)) {
        s_channel_reject("heartbeat", from);
        return;
    }
    (void)session_heartbeat(&g_tab, m->u.hb.token, s_now());
    if (e->role == SESSION_ROLE_WM) {
        g_wm_seen = s_now();
    }
}

static void s_handle_launch(const struct session_msg *m, u32 from)
{
    char path[SESSION_PATH_MAX];
    char arg[SESSION_ARG_MAX];
    char ticket_arg[24];
    char token_arg[24];
    const char *argv[5];
    u32 pid;
    u32 token = 0;
    int slot;
    struct session_entry *e;
    struct session_entry *req;

    session_path_get(m->u.launch.path, path, (u32)sizeof(path));
    session_arg_get(m->u.launch.arg, arg, (u32)sizeof(arg));
    if (session_launch_allowed(&g_tab, m->u.launch.token) != DS_OK) {
        if (g_reject_prints < S_MAX_PRINTS) {
            g_reject_prints++;
            s_put("ng: session launch reject ok (token=");
            s_puthex(m->u.launch.token);
            s_put(" role=");
            s_putdec(SESSION_TOKEN_ROLE(m->u.launch.token));
            s_put(" n=");
            s_putdec((u32)g_tab.rejects);
            s_put(")\r\n");
        }
        g_status_dirty = 1;
        return;
    }
    req = session_by_token(&g_tab, m->u.launch.token);
    if (!s_channel_ok(req, from)) {
        s_channel_reject("launch", from);
        return;
    }
    if (path[0] == '\0') {
        return;
    }
    /* 会话策略：GUI demo 以常驻窗口角色启动；其它应用不带角色参数。 */
    if (s_streq(path, S_DEMO_PATH) && arg[0] == '\0') {
        ds_memcopy(arg, "window", 7);
    }
    (void)s_u32str(g_port, ticket_arg + 8);
    ds_memcopy(ticket_arg, "session=", 8);
    ds_memcopy(token_arg, "token=0x", 8);
    token = s_make_token(SESSION_ROLE_APP);
    (void)s_hexstr(token, token_arg + 8);

    argv[0] = path;
    argv[1] = (arg[0] != '\0') ? arg : NULL;
    argv[2] = ticket_arg;
    argv[3] = token_arg;
    argv[4] = NULL;
    pid = s_spawn(path, argv);
    if (pid == 0) {
        s_put("ng: session launch FAIL (spawn ");
        s_put(path);
        s_put(")\r\n");
        return;
    }
    slot = session_alloc_with_token(&g_tab, pid, SESSION_ROLE_APP, s_now(),
                                    token);
    if (slot < 0) {
        s_put("ng: session launch FAIL (table)\r\n");
        (void)s_sys4(S_SYS_KILL, (i64)pid, 9, 0, 0);
        return;
    }
    e = session_at(&g_tab, (u32)slot);
    g_tab.launches++;
    if (g_launch_prints < S_MAX_PRINTS) {
        g_launch_prints++;
        s_put("ng: session app launch ok (pid=");
        s_putdec(pid);
        s_put(" app=");
        s_put(path);
        s_put(" token=");
        s_puthex((e != NULL) ? e->token : 0);
        s_put(")\r\n");
    }
    g_status_dirty = 1;
}

static void s_reap_entry(struct session_entry *e, const char *why)
{
    u32 pid = e->pid;
    u32 token = e->token;
    u32 surface = e->surface_id;
    int status = -1;

    if (surface != 0) {
        s_destroy_surface(surface, e->token);
        e->surface_id = 0;
    }
    if (pid != 0) {
        (void)s_kill_wait(pid, &status);
    }
    (void)session_mark_dead(&g_tab, pid);
    session_retire_token(&g_tab, token);
    s_put("ng: session app reap ok (pid=");
    s_putdec(pid);
    s_put(" why=");
    s_put(why);
    s_put(")\r\n");
    (void)status;
    /* 释放槽位（DEAD 项不再占表；计数在 table 里累计）。 */
    {
        u32 slot = (u32)(e - g_tab.e);

        ds_memzero(e, (u64)sizeof(*e));
        if (g_tab.count > 0) {
            g_tab.count--;
        }
        if (g_tab.wm_slot == slot) {
            g_tab.wm_slot = SESSION_SLOT_NONE;
        }
    }
    g_status_dirty = 1;
}

static void s_handle_exit(const struct session_msg *m, u32 from)
{
    struct session_entry *e = session_by_token(&g_tab, m->u.exit.token);

    if (e == NULL) {
        g_tab.rejects++;
        return;
    }
    if (!s_channel_ok(e, from)) {
        s_channel_reject("exit", from);
        return;
    }
    if (e->role == SESSION_ROLE_APP) {
        (void)session_mark_exit(&g_tab, e->token, m->u.exit.code);
        s_reap_entry(e, "exit");
    } else if (e->role == SESSION_ROLE_WM) {
        /* WM 自己请求退出：按崩溃处理（重启策略一致）。 */
        g_wm_seen = 0;
    }
}

static void s_handle_kill(const struct session_msg *m, u32 from)
{
    struct session_entry *e;
    struct session_entry *req;

    if (session_kill_allowed(&g_tab, m->u.kill.token) != DS_OK) {
        if (g_reject_prints < S_MAX_PRINTS) {
            g_reject_prints++;
            s_put("ng: session kill reject ok (token=");
            s_puthex(m->u.kill.token);
            s_put(" n=");
            s_putdec((u32)g_tab.rejects);
            s_put(")\r\n");
        }
        return;
    }
    req = session_by_token(&g_tab, m->u.kill.token);
    if (!s_channel_ok(req, from)) {
        s_channel_reject("kill", from);
        return;
    }
    e = session_by_pid(&g_tab, m->u.kill.pid);
    if (e == NULL || e->role != SESSION_ROLE_APP) {
        return;
    }
    g_tab.kills++;
    s_reap_entry(e, "kill");
}

static void s_handle_lookup(const struct session_msg *m, u32 from)
{
    struct session_msg r;
    struct session_entry *e = NULL;

    if (m->u.lookup.surface_id != 0) {
        e = session_by_surface(&g_tab, m->u.lookup.surface_id);
    } else if (m->u.lookup.pid != 0) {
        e = session_by_pid(&g_tab, m->u.lookup.pid);
    }
    ds_memzero(&r, (u64)sizeof(r));
    r.type = SESSION_MSG_ENTRY;
    r.seq = m->seq;
    if (e != NULL) {
        session_entry_fill(&r.u.entry.info, e->pid, e->token, e->surface_id,
                           e->state, e->role, e->title);
        /*
         * v3 审计修复：role_token 是能力凭证，只在签发时经 argv 交给它的
         * 属主进程。LOOKUP 谁都能调，以前会把 WM/别的应用的令牌一起回出去
         * （拿到 WM 令牌就能 LAUNCH/KILL）。查询回复不导出令牌。
         */
        r.u.entry.info.token = 0;
        r.u.entry.found = 1;
    }
    (void)s_reply_sess(from, &r);
}

static void s_handle_list(const struct session_msg *m, u32 from)
{
    struct session_msg r;
    u32 i;
    u32 n = 0;

    ds_memzero(&r, (u64)sizeof(r));
    r.type = SESSION_MSG_LIST_REPLY;
    r.seq = m->seq;
    r.u.list_reply.procs = session_procs(&g_tab);
    r.u.list_reply.apps = session_apps(&g_tab);
    r.u.list_reply.windows = session_windows(&g_tab);
    for (i = 0; i < SESSION_MAX_ENTRIES && n < SESSION_MAX_ENTRIES; i++) {
        struct session_entry *e = &g_tab.e[i];

        if (!e->used) {
            continue;
        }
        session_entry_fill(&r.u.list_reply.entries[n], e->pid, e->token,
                           e->surface_id, e->state, e->role, e->title);
        /* 同上：注册表快照不导出 role_token（只有计数/身份/窗口是公开的）。 */
        r.u.list_reply.entries[n].token = 0;
        n++;
    }
    r.u.list_reply.count = n;
    (void)s_reply_sess(from, &r);
}

static void s_handle_msg(int type, const struct session_msg *m, u32 from)
{
    switch (type) {
    case SESSION_MSG_REGISTER:
        s_handle_register(m, from);
        break;
    case SESSION_MSG_HEARTBEAT:
        s_handle_heartbeat(m, from);
        break;
    case SESSION_MSG_LAUNCH:
        s_handle_launch(m, from);
        break;
    case SESSION_MSG_EXIT:
        s_handle_exit(m, from);
        break;
    case SESSION_MSG_KILL:
        s_handle_kill(m, from);
        break;
    case SESSION_MSG_LOOKUP:
        s_handle_lookup(m, from);
        break;
    case SESSION_MSG_LIST:
        s_handle_list(m, from);
        break;
    case SESSION_MSG_SESSION_STATUS:
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* displayd LIST 同步                                                         */
/* ------------------------------------------------------------------------- */

static void s_on_list(const struct ds_msg *m)
{
    u32 bound = 0;
    u32 mismatch;
    u32 apps;
    u32 windows;
    u32 count = m->u.list_reply.count;

    /*
     * 审计修复：count 来自 IPC 消息（body 里就是一个 u32），解码器不会校验
     * 它 <= entries[] 容量。以前直接传给 session_sync_list()，一个伪造的
     * LIST_REPLY（count=0xFFFFFFFF）就能让它越界读栈/崩溃。
     */
    if (count > DS_LIST_MAX) {
        if (g_clamp_prints < S_MAX_PRINTS) {
            g_clamp_prints++;
            s_put("ng: session registry count clamp ok (n=");
            s_putdec(count);
            s_put(" max=");
            s_putdec(DS_LIST_MAX);
            s_put(")\r\n");
        }
        count = DS_LIST_MAX;
    }
    mismatch = session_sync_list(&g_tab, m->u.list_reply.entries, count, &bound);
    apps = session_apps(&g_tab);
    windows = session_windows(&g_tab);
    if (mismatch == 0 && bound > 0) {
        if (g_registry_prints == 0u || apps != g_last_apps ||
            windows != g_last_windows) {
            g_registry_prints++;
            g_last_apps = apps;
            g_last_windows = windows;
            s_put("ng: session registry ok (apps=");
            s_putdec(apps);
            s_put(" windows=");
            s_putdec(windows);
            s_put(" procs=");
            s_putdec(session_procs(&g_tab));
            s_put(")\r\n");
        }
        if (g_token_printed == 0u) {
            g_token_printed = 1;
            s_put("ng: role token ok (tokens=");
            s_putdec(session_tokens(&g_tab));
            s_put(" bound=");
            s_putdec(bound);
            s_put(" mismatch=0)\r\n");
        }
    } else if (mismatch != 0 && g_registry_prints < S_MAX_PRINTS) {
        g_registry_prints++;
        s_put("ng: session registry mismatch (n=");
        s_putdec(mismatch);
        s_put(" tokenfail=");
        s_putdec((u32)g_tab.token_fail);
        s_put(")\r\n");
    }
    g_status_dirty = 1;
}

static void s_poll_display(void)
{
    struct ds_msg m;

    if (g_disp_ticket == 0) {
        return;
    }
    ds_memzero(&m, (u64)sizeof(m));
    m.type = DS_MSG_LIST;
    m.seq = ++g_seq;
    if (s_send_ds(&m, g_disp_port) != 0) {
        return;
    }
    /* 非阻塞收 reply（displayd 通常几轮内回复）。 */
    for (;;) {
        int type = s_recv_ds(&m, 0);

        if (type == (int)DS_MSG_LIST_REPLY) {
            s_on_list(&m);
            continue;
        }
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* 崩溃检测 / WM 重启                                                         */
/* ------------------------------------------------------------------------- */

static void s_wm_died(void)
{
    u32 pid = g_wm_pid;
    struct session_entry *e = session_by_pid(&g_tab, pid);
    u64 delay = 0;
    u32 old = pid;

    s_put("ng: session wm dead ok (pid=");
    s_putdec(pid);
    s_put(")\r\n");
    if (e != NULL) {
        u32 surface = e->surface_id;
        u32 token = e->token;
        u32 slot = (u32)(e - g_tab.e);

        if (surface != 0) {
            s_destroy_surface(surface, e->token);
        }
        if (pid != 0) {
            int status = -1;

            (void)s_kill_wait(pid, &status);
        }
        session_retire_token(&g_tab, token);
        ds_memzero(e, (u64)sizeof(*e));
        if (g_tab.count > 0) {
            g_tab.count--;
        }
        if (g_tab.wm_slot == slot) {
            g_tab.wm_slot = SESSION_SLOT_NONE;
        }
    }
    g_wm_pid = 0;
    if (session_should_restart(g_wm_restarts, SESSION_RESTART_LIMIT, &delay)) {
        g_wm_state = S_WM_STATE_WAIT;
        g_restart_at = s_now() + delay;
        s_put("ng: session wm restart sched ok (old=");
        s_putdec(old);
        s_put(" restarts=");
        s_putdec(g_wm_restarts);
        s_put(" backoff_ticks=");
        s_putdec((u32)delay);
        s_put(")\r\n");
    } else {
        g_wm_state = S_WM_STATE_NONE;
        s_put("ng: session wm restart limit ok (restarts=");
        s_putdec(g_wm_restarts);
        s_put(" limit=");
        s_putdec(SESSION_RESTART_LIMIT);
        s_put(")\r\n");
    }
    g_status_dirty = 1;
}

static void s_service_restart(u64 now)
{
    u32 old;

    if (g_wm_state != S_WM_STATE_WAIT || now < g_restart_at) {
        return;
    }
    old = g_wm_pid;
    if (s_start_wm() != 0) {
        /* 拉起失败也消耗一次重启额度，按退避再试（避免忙等）。 */
        g_wm_restarts++;
        if (g_wm_restarts >= SESSION_RESTART_LIMIT) {
            g_wm_state = S_WM_STATE_NONE;
            s_put("ng: session wm restart limit ok (restarts=");
            s_putdec(g_wm_restarts);
            s_put(" limit=");
            s_putdec(SESSION_RESTART_LIMIT);
            s_put(")\r\n");
        } else {
            g_restart_at = s_now() + session_backoff_ticks(g_wm_restarts + 1u);
        }
        g_status_dirty = 1;
        return;
    }
    g_wm_restarts++;
    g_restart_pending = 1;
    s_put("ng: session wm restart ok (old=");
    s_putdec(old);
    s_put(" new=");
    s_putdec(g_wm_pid);
    s_put(" restarts=");
    s_putdec(g_wm_restarts);
    s_put(")\r\n");
    g_status_dirty = 1;
}

static void s_check_deadlines(u64 now)
{
    u32 i;

    if (g_wm_state == S_WM_STATE_RUNNING) {
        if (now - g_wm_seen > S_STALE_TICKS) {
            s_wm_died();
        }
    } else if (g_wm_state == S_WM_STATE_WAIT && now >= g_restart_at) {
        s_service_restart(now);
    }
    /* 应用心跳超时 -> 回收（optioanl 应用崩溃不影响别人）。 */
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        struct session_entry *e = &g_tab.e[i];

        if (!e->used || e->state != SESSION_STATE_RUNNING) {
            continue;
        }
        if (e->role != SESSION_ROLE_APP) {
            continue;
        }
        if (now - e->last_seen > S_STALE_TICKS) {
            s_reap_entry(e, "stale");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int s_main(long argc, char **argv)
{
    long i;

    g_pid = (u32)s_sys(S_SYS_GETPID, 0, 0, 0);
    g_seq = 0;
    g_round = 0;
    g_fault_spin = 0;
    /* WP3-5：只认故障注入参数；正式启动（init）没有参数。 */
    for (i = 1; argv != NULL && i < argc; i++) {
        if (s_streq(argv[i], "fault=spin") != 0) {
            g_fault_spin = 1;
        }
    }
    session_table_init(&g_tab);
    g_wm_state = S_WM_STATE_NONE;
    s_startup();

    for (;;) {
        struct session_msg m;
        u32 from = 0;
        int type;
        u64 now = s_now();

        type = s_recv_sess(g_port, &m, &from, S_POLL_TICKS);
        if (type > 0) {
            s_handle_msg(type, &m, from);
        }
        g_round++;
        if ((g_round % S_LIST_EVERY) == 0u) {
            s_poll_display();
        }
        now = s_now();
        s_check_deadlines(now);
        if (g_status_dirty != 0u) {
            s_status_write();
        } else if ((g_round % S_STATUS_EVERY) == 0u) {
            s_status_write();
        }
    }
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call s_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
