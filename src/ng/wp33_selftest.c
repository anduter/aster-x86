/*
 * ng WP3-3: session manager 自检编排（L2c）。
 *
 * 证据行（stdvga 路径）：
 *   ng: sessiond spawn ok (pid=..)                    [wp33]
 *   <sessiond> ng: sessiond up (pid=.. displayd=.. wm=.. port=..)
 *   ng: session start ok (pid=.. displayd=.. wm=..)   [wp33]
 *   <displayd> ng: surface token ok (id=1 token=0x.. role=1)
 *   <wm>     ng: wm session ok (session=.. token=0x..)
 *   <sessiond> ng: session wm register ok (wm=.. id=1)
 *   <sessiond> ng: session focus ok (id=1 pid=..)
 *   ng: session wm ready ok (wm=..)                   [wp33]
 *   <wm>     ng: wm registry ok (apps=.. windows=.. procs=..)
 *   <wm>     ng: wm launch req ok (app=/bin/demo via=key|auto)
 *   <sessiond> ng: session app launch ok (pid=.. app=/bin/demo token=0x..)
 *   <demo>   ng: demo session ok (session=.. token=0x..)
 *   <demo>   ng: demo role probe ok (path=/bin/demo)     [越权 LAUNCH 被拒]
 *   <sessiond> ng: session launch reject ok (...)
 *   <demo>   ng: demo focus probe ok (id=1 token=0x..)   [越权 FOCUS 被拒]
 *   <displayd> ng: focus reject ok (id=1 token=0x..)
 *   <sessiond> ng: session window ok (pid=.. id=2 title=Demo)
 *   <sessiond> ng: session registry ok (apps=1 windows=2 procs=2)
 *   <sessiond> ng: role token ok (tokens=2 bound=2 mismatch=0)
 *   ng: session launch seen ok (pid=..)               [wp33]
 *   ng: session registry seen ok (apps=.. windows=..) [wp33]
 *   ng: role token seen ok (tokens=..)                [wp33]
 *   ng: session wm killed ok (pid=..)                 [wp33，kill WM]
 *   <sessiond> ng: session wm dead ok (pid=..)
 *   <sessiond> ng: session wm restart sched ok (...)
 *   <sessiond> ng: session wm restart ok (old=.. new=..)
 *   <sessiond> ng: session wm restart ready ok (wm=.. id=..)
 *   ng: session wm restart seen ok (old=.. new=..)    [wp33]
 *   ng: session survives wm ok (app=.. alive=1 ...)   [wp33]
 *   ng: session app reap ok (pid=..)                   [wp33 清理应用]
 *   ng: session cleanup ok (live=0)                    [wp33]
 *   ng: WP3-3 selftest PASS                            [main.c]
 *
 * 状态文件：`/run/session.status`（k=v 单行）
 *   up=1 pid= dpid= procs= apps= windows= tokens= launches= kills= exits=
 *   reaps= restarts= wmpid= wmstate= apppid= tokenfail= registry=
 * 内核只读这一份（不新建第二份；displayd 的 /run/display.status 仍是显示证据）。
 *
 * 降级路径（-vga none）：`ng: WP3-3 selftest skipped (no fb)`。
 */
#include "wp33.h"

#include "ds_protocol.h"
#include "fs_syscall.h"
#include "ipc.h"
#include "proc.h"
#include "serial.h"
#include "session_protocol.h"
#include "shm.h"
#include "thread.h"
#include "vfs.h"
#include "wp23.h"

#define WP33_SESSIOND "/bin/sessiond"
#define WP33_STATUS DS_STATUS_PATH /* 会话状态汇总在 displayd 的状态文件里 */
#define WP33_POLL_SLEEP 1000000ULL
#define WP33_START_TRIES 4000u  /* 4s：sessiond 启 displayd+wm */
#define WP33_LAUNCH_TRIES 8000u /* 8s：QMP '1' 或 WM 3s auto */
#define WP33_SYNC_TRIES 5000u   /* 5s：注册表同步 + 应用窗口 */
#define WP33_RESTART_TRIES 6000u
#define WP33_REAP_TRIES 4000u
#define WP33_SETTLE 80u

struct wp33_state {
    u64 up;
    u64 pid;
    u64 dpid;
    u64 wmpid;
    u64 wmstate;
    u64 procs;
    u64 apps;
    u64 windows;
    u64 tokens;
    u64 launches;
    u64 reaps;
    u64 restarts;
    u64 apppid;
    u64 tokenfail;
    u64 registry;
};

static int g_fails;

static void wp33_fail(const char *what)
{
    g_fails++;
    serial_puts("ng: WP3-3 ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* ------------------------------------------------------------------------- */
/* 状态文件解析（与 wp31/wp32 同一套 "k=v k=v" 语义）                        */
/* ------------------------------------------------------------------------- */

static int wp33_key_match(const char *text, u32 pos, const char *key)
{
    u32 i = 0;

    while (key[i] != '\0') {
        if (text[pos + i] != key[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int wp33_status_u64(const char *text, u32 len, const char *key,
                           u64 *out)
{
    u32 key_len = 0;
    u32 i;

    while (key[key_len] != '\0') {
        key_len++;
    }
    for (i = 0; i + key_len <= len; i++) {
        u32 p;
        u64 v = 0;
        u32 digits = 0;
        int hex = 0;

        if (i > 0 && text[i - 1] != ' ') {
            continue;
        }
        if (i + key_len > len || !wp33_key_match(text, i, key)) {
            continue;
        }
        p = i + key_len;
        if (p + 1 < len && text[p] == '0' &&
            (text[p + 1] == 'x' || text[p + 1] == 'X')) {
            hex = 1;
            p += 2;
        }
        while (p < len) {
            u32 c = (u32)(u8)text[p];
            u32 d;

            if (hex) {
                if (c >= '0' && c <= '9') {
                    d = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    d = c - 'a' + 10u;
                } else if (c >= 'A' && c <= 'F') {
                    d = c - 'A' + 10u;
                } else {
                    break;
                }
                v = (v << 4) | (u64)d;
            } else {
                if (c < '0' || c > '9') {
                    break;
                }
                v = v * 10u + (u64)(c - '0');
            }
            digits++;
            p++;
        }
        if (digits == 0) {
            continue;
        }
        if (out != NULL) {
            *out = v;
        }
        return 1;
    }
    return 0;
}

static int wp33_read_file(const char *path, char *buf, u32 cap, u32 *out_len)
{
    u64 size = 0;
    int rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    buf[0] = '\0';
    rc = fs_sys_read_file(path, buf, (u64)(cap - 1u), &size);
    if (rc != VFS_OK || size == 0 || size >= (u64)cap) {
        return 0;
    }
    buf[size] = '\0';
    if (out_len != NULL) {
        *out_len = (u32)size;
    }
    return 1;
}

static int wp33_read_state(struct wp33_state *st)
{
    char text[512];
    u32 len = 0;

    if (!wp33_read_file(WP33_STATUS, text, (u32)sizeof(text), &len)) {
        return 0;
    }
    if (st == NULL) {
        return 1;
    }
    st->up = st->pid = st->dpid = st->wmpid = st->wmstate = 0;
    st->procs = st->apps = st->windows = st->tokens = 0;
    st->launches = st->reaps = st->restarts = st->apppid = 0;
    st->tokenfail = st->registry = 0;
    /*
     * 字段前缀 s：displayd 把 sessiond 的 SESSION_STATUS 追加到同一份
     * /run/display.status（不新建 ramfs 节点）。
     */
    (void)wp33_status_u64(text, len, "sess=", &st->up);
    (void)wp33_status_u64(text, len, "spid=", &st->pid);
    (void)wp33_status_u64(text, len, "sdpid=", &st->dpid);
    (void)wp33_status_u64(text, len, "swmpid=", &st->wmpid);
    (void)wp33_status_u64(text, len, "swmstate=", &st->wmstate);
    (void)wp33_status_u64(text, len, "sprocs=", &st->procs);
    (void)wp33_status_u64(text, len, "sapps=", &st->apps);
    (void)wp33_status_u64(text, len, "swindows=", &st->windows);
    (void)wp33_status_u64(text, len, "stokens=", &st->tokens);
    (void)wp33_status_u64(text, len, "slaunches=", &st->launches);
    (void)wp33_status_u64(text, len, "sreaps=", &st->reaps);
    (void)wp33_status_u64(text, len, "srestarts=", &st->restarts);
    (void)wp33_status_u64(text, len, "sapppid=", &st->apppid);
    (void)wp33_status_u64(text, len, "stokenfail=", &st->tokenfail);
    (void)wp33_status_u64(text, len, "sregistry=", &st->registry);
    return 1;
}

static void wp33_putdec(u64 v)
{
    serial_putdec(v);
}

static void wp33_ensure_run_dir(void)
{
    struct vfs_ctx *ctx = fs_sys_ctx();
    int rc;

    if (ctx == NULL) {
        return;
    }
    rc = vfs_mkdir(ctx, "/run", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        serial_puts("ng: WP3-3 dir mkdir rc=");
        wp33_putdec((u64)(-rc));
        serial_puts("\r\n");
    }
}

static u32 wp33_spawn(const char *path)
{
    const char *argv[2];
    u32 pid = 0;
    int rc;

    argv[0] = path;
    argv[1] = NULL;
    rc = proc_spawn(path, argv, &pid);
    if (rc != PROC_OK) {
        return 0;
    }
    return pid;
}

static void wp33_kill_reap(u32 pid)
{
    u32 i;

    if (pid == 0) {
        return;
    }
    (void)proc_kill(pid, 9);
    for (i = 0; i < 200u; i++) {
        const struct proc *p = proc_slot(pid);

        if (p == NULL) {
            return;
        }
        if (p->state == PROC_STATE_ZOMBIE) {
            (void)proc_reap_pid(pid);
            return;
        }
        (void)thread_sleep(WP33_POLL_SLEEP);
    }
}

static int wp33_alive(u32 pid)
{
    const struct proc *p;

    if (pid == 0) {
        return 0;
    }
    p = proc_slot(pid);
    if (p == NULL) {
        return 0;
    }
    return (p->state != PROC_STATE_FREE && p->state != PROC_STATE_ZOMBIE) ? 1
                                                                          : 0;
}

/* 等状态文件满足 pred 条件；成功把最后一次快照写进 *out。 */
typedef int (*wp33_pred)(const struct wp33_state *st);

static int wp33_wait(wp33_pred pred, u32 tries, struct wp33_state *out,
                     struct wp33_state *last)
{
    struct wp33_state st;
    u32 i;

    for (i = 0; i < tries; i++) {
        st.up = 0;
        if (wp33_read_state(&st) && pred(&st)) {
            if (out != NULL) {
                *out = st;
            }
            return 1;
        }
        if (last != NULL && st.up != 0) {
            *last = st;
        }
        (void)thread_sleep(WP33_POLL_SLEEP);
    }
    return 0;
}

/* ---- 条件 ---------------------------------------------------------------- */

static int wp33_p_up(const struct wp33_state *st)
{
    return st->up != 0 && st->pid != 0;
}

static int wp33_p_wm(const struct wp33_state *st)
{
    return st->wmpid != 0 && st->wmstate == 1;
}

static int wp33_p_launch(const struct wp33_state *st)
{
    return st->launches >= 1 && st->apppid != 0;
}

static int wp33_p_registry(const struct wp33_state *st)
{
    return st->apps >= 1 && st->windows >= 2 && st->registry != 0;
}

static int wp33_p_token(const struct wp33_state *st)
{
    return st->tokens >= 2 && st->tokenfail == 0 && st->registry != 0;
}

static int wp33_p_reap(const struct wp33_state *st)
{
    return st->reaps >= 1;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int wp33_selftest(void)
{
    struct wp33_state st;
    struct wp33_state last;
    u32 sessiond = 0;
    u32 old_wm = 0;
    u32 new_wm = 0;
    u32 app = 0;
    u32 dpid = 0;
    u32 i;
    int ok;

    g_fails = 0;
    if (ng_display_fb() == NULL) {
        serial_puts("ng: WP3-3 selftest skipped (no fb)\r\n");
        return 0;
    }
    wp33_ensure_run_dir();

    /* ---- 1. init 只 spawn sessiond；displayd/wm 由它拉起 ---------------- */
    sessiond = wp33_spawn(WP33_SESSIOND);
    if (sessiond == 0) {
        wp33_fail("sessiond spawn");
        return g_fails;
    }
    serial_puts("ng: sessiond spawn ok (pid=");
    wp33_putdec(sessiond);
    serial_puts(")\r\n");

    ok = wp33_wait(wp33_p_up, WP33_START_TRIES, &st, &last);
    if (!ok) {
        wp33_fail("sessiond up");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    dpid = (u32)st.dpid;
    old_wm = (u32)st.wmpid;
    serial_puts("ng: session start ok (pid=");
    wp33_putdec(st.pid);
    serial_puts(" displayd=");
    wp33_putdec(st.dpid);
    serial_puts(" wm=");
    wp33_putdec(st.wmpid);
    serial_puts(")\r\n");

    /* ---- 2. WM 由 sessiond 启动并注册（面板 surface） ------------------- */
    if (!wp33_wait(wp33_p_wm, WP33_START_TRIES, &st, &last)) {
        wp33_fail("session wm ready");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    serial_puts("ng: session wm ready ok (wm=");
    wp33_putdec(st.wmpid);
    serial_puts(")\r\n");

    /* ---- 3. WM 的 LAUNCH -> sessiond spawn 应用（QMP '1' 或 auto） ------ */
    if (!wp33_wait(wp33_p_launch, WP33_LAUNCH_TRIES, &st, &last)) {
        wp33_fail("session app launch");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    app = (u32)st.apppid;
    serial_puts("ng: session launch seen ok (pid=");
    wp33_putdec(st.apppid);
    serial_puts(")\r\n");

    /* ---- 4. 注册表 + 令牌（应用窗口 + 面板都在 displayd LIST 里） ------- */
    if (!wp33_wait(wp33_p_registry, WP33_SYNC_TRIES, &st, &last)) {
        wp33_fail("session registry");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    serial_puts("ng: session registry seen ok (apps=");
    wp33_putdec(st.apps);
    serial_puts(" windows=");
    wp33_putdec(st.windows);
    serial_puts(")\r\n");

    if (!wp33_wait(wp33_p_token, WP33_SYNC_TRIES, &st, &last)) {
        wp33_fail("role token");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    serial_puts("ng: role token seen ok (tokens=");
    wp33_putdec(st.tokens);
    serial_puts(" tokenfail=");
    wp33_putdec(st.tokenfail);
    serial_puts(")\r\n");

    /* ---- 5. kill WM：应用必须活着，sessiond 按退避重启 WM -------------- */
    old_wm = (u32)st.wmpid;
    if (proc_kill(old_wm, 9) != PROC_OK) {
        wp33_fail("wm kill");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    serial_puts("ng: session wm killed ok (pid=");
    wp33_putdec(old_wm);
    serial_puts(")\r\n");

    for (i = 0; i < WP33_RESTART_TRIES; i++) {
        if (wp33_read_state(&st) && st.restarts >= 1 && st.wmstate == 1 &&
            st.wmpid != 0 && st.wmpid != old_wm) {
            break;
        }
        (void)thread_sleep(WP33_POLL_SLEEP);
    }
    if (st.restarts < 1 || st.wmstate != 1 || st.wmpid == old_wm) {
        wp33_fail("session wm restart");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    new_wm = (u32)st.wmpid;
    serial_puts("ng: session wm restart seen ok (old=");
    wp33_putdec(old_wm);
    serial_puts(" new=");
    wp33_putdec(new_wm);
    serial_puts(" restarts=");
    wp33_putdec(st.restarts);
    serial_puts(")\r\n");

    if (!wp33_alive(app)) {
        wp33_fail("session survives wm");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    serial_puts("ng: session survives wm ok (app=");
    wp33_putdec(app);
    serial_puts(" alive=1 wm=");
    wp33_putdec(new_wm);
    serial_puts(" restarts=");
    wp33_putdec(st.restarts);
    serial_puts(")\r\n");

    /* ---- 6. 回收应用：kill 之后 sessiond 心跳超时 -> wait + DESTROY ----- */
    if (proc_kill(app, 9) != PROC_OK) {
        wp33_fail("app kill");
        wp33_kill_reap(sessiond);
        return g_fails;
    }
    if (wp33_wait(wp33_p_reap, WP33_REAP_TRIES, &st, &last)) {
        serial_puts("ng: session app reap ok (pid=");
        wp33_putdec(app);
        serial_puts(" reaps=");
        wp33_putdec(st.reaps);
        serial_puts(")\r\n");
    } else {
        wp33_fail("session app reap");
    }

    /* ---- 7. 清理：sessiond -> WM -> displayd（应用已回收） ------------ */
    (void)wp33_read_state(&st);
    if (st.wmpid != 0) {
        wp33_kill_reap((u32)st.wmpid);
    }
    wp33_kill_reap(sessiond);
    if (st.dpid != 0) {
        wp33_kill_reap((u32)st.dpid);
    } else {
        wp33_kill_reap(dpid);
    }
    (void)thread_sleep(WP33_SETTLE);
    /*
     * 归还两个 ramfs 节点（/run/display.port、/run/display.status）：
     * ramfs 节点池只有 32 个，WP3-3 又种了 /bin/sessiond；后续的 ring3
     * shell 会话（WP2-6）还要创建 /tmp/s.txt，不还就会 ENOSPC。
     */
    {
        struct vfs_ctx *ctx = fs_sys_ctx();

        if (ctx != NULL) {
            (void)vfs_unlink(ctx, DS_PORT_PATH);
            (void)vfs_unlink(ctx, DS_STATUS_PATH);
        }
    }
    serial_puts("ng: session cleanup ok (live=");
    wp33_putdec(shm_live());
    serial_puts(" nodes=");
    wp33_putdec((u64)fs_sys_ramfs()->st.nodes_used);
    serial_puts(")\r\n");
    if (shm_live() != 0) {
        wp33_fail("session cleanup");
    }
    return g_fails;
}
