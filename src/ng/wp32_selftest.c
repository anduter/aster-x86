/*
 * ng WP3-2: shell/WM 自检编排（见 wp32.h）。
 *
 * 证据行（stdvga 路径）：
 *   ng: wm displayd ok (pid=.. ticket=..)      [wp32]
 *   <wm>     ng: wm up (pid=.. panel=1280x32)
 *   <wm>     ng: wm panel ok (px=0x202838 at=0,0)
 *   <displayd> ng: hotkey registered ok (surface=1 key=any)
 *   <wm>     ng: wm launcher ok (apps=.. pkgs=..)
 *   <wm>     ng: wm app ok (n=0 path=/bin/demo)
 *   <wm>     ng: wm pkg ok (path=/System/Packages/hello)
 *   <displayd> ng: displayd wm status ok (switch=0 launch=0 tasks=0)
 *   ng: wm ready ok (wm=1 hotkey=1)            [wp32]
 *   ng: wm demo spawn ok (pid=..)              [wp32]
 *   <displayd> ng: surface title ok (id=2 title=Demo)
 *   <demo>   ng: demo window up (id=2 title=Demo)
 *   <wm>     ng: wm tasks ok (n=2 apps=1)
 *   <wm>     ng: wm task ok (id=2 x=.. y=.. title=Demo)
 *   <displayd> ng: displayd hotkey ok (key=0x9 scancode=0xf)   [QMP 注入 Tab]
 *   <wm>     ng: wm task switch ok (focus=2 from=1 via=key)     [QMP 注入 '1']
 *   <wm>     ng: wm launch ok (app=/bin/demo pid=.. via=key)
 *   ng: wm switch seen ok (focus=2 switch=1)   [wp32]
 *   ng: wm launch seen ok (pid=..)             [wp32]
 *   ng: wm kill ok (status=265 waiters=..)     [wp32]
 *   ng: demo survives wm ok (alive=1 id=2)     [wp32]
 *   ng: wm displayd kill ok (status=265 live=0)
 *   ng: WP3-2 selftest PASS                    [main.c]
 *
 * WM 的计数经 WM_STATUS 由 displayd 汇总进已有的 `/run/display.status`
 * （`wm=1 wmtasks=.. wmfocus=.. wmswitch=.. wmlast=.. wmlaunch=.. wmlastpid=..`），
 * 不新建 ramfs 文件/节点。
 *
 * 降级路径（-vga none）：
 *   ng: WP3-2 selftest skipped (no fb)
 */
#include "wp32.h"

#include "ds_protocol.h"
#include "fs_syscall.h"
#include "ipc.h"
#include "proc.h"
#include "serial.h"
#include "shm.h"
#include "thread.h"
#include "vfs.h"
#include "wp23.h"

#define WP32_DISPLAYD "/bin/displayd"
#define WP32_WM "/bin/wm"
#define WP32_DEMO "/bin/demo"
#define WP32_POLL_SLEEP 1000000ULL
#define WP32_READY_TRIES 3000u  /* 3s */
#define WP32_TASKS_TRIES 4000u  /* 4s */
#define WP32_SWITCH_TRIES 6000u /* 6s（WM 的 auto fallback 在 ~3s） */
#define WP32_LAUNCH_TRIES 6000u
#define WP32_SETTLE 60u /* 60 * 1e6 tick ≈ 60ms */

static int g_fails;

static void wp32_fail(const char *what)
{
    g_fails++;
    serial_puts("ng: WP3-2 ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* ------------------------------------------------------------------------- */
/* 状态文件解析（与 wp31 同一套 "k=v k=v" 语义）                             */
/* ------------------------------------------------------------------------- */

static int wp32_key_match(const char *text, u32 pos, const char *key)
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

static int wp32_status_u64(const char *text, u32 len, const char *key,
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
        if (i + key_len > len || !wp32_key_match(text, i, key)) {
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

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void wp32_putdec(u64 v)
{
    serial_putdec(v);
}


static int wp32_read_file(const char *path, char *buf, u32 cap, u32 *out_len)
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

static u32 wp32_parse_ticket(const char *text, u32 len)
{
    u64 v = 0;
    u32 i;

    for (i = 0; i < len; i++) {
        char c = text[i];

        if (c >= '0' && c <= '9') {
            v = v * 10u + (u64)(c - '0');
        } else if (c == '\r' || c == '\n' || c == ' ') {
            break;
        } else {
            return 0;
        }
    }
    return (v != 0 && v <= 0xFFFFFFFFULL) ? (u32)v : 0;
}

static void wp32_ensure_run_dir(void)
{
    struct vfs_ctx *ctx = fs_sys_ctx();
    int rc;

    if (ctx == NULL) {
        return;
    }
    rc = vfs_mkdir(ctx, "/run", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        serial_puts("ng: WP3-2 dir mkdir rc=");
        wp32_putdec((u64)(-rc));
        serial_puts("\r\n");
    }
}

static u32 wp32_spawn(const char *path, const char *arg1)
{
    const char *argv[3];
    u32 pid = 0;
    int rc;

    argv[0] = path;
    argv[1] = arg1;
    argv[2] = NULL;
    rc = proc_spawn(path, argv, &pid);
    if (rc != PROC_OK) {
        return 0;
    }
    return pid;
}

static void wp32_kill_and_wait(u32 pid)
{
    int status = -1;

    if (pid == 0) {
        return;
    }
    (void)proc_kill(pid, 9);
    (void)proc_wait_pid(pid, &status, NULL);
    (void)proc_reap_pid(pid);
}

static int wp32_alive(u32 pid)
{
    const struct proc *p;

    if (pid == 0) {
        return 0;
    }
    p = proc_slot(pid);
    if (p == NULL) {
        return 0;
    }
    return (p->state != PROC_STATE_FREE && p->state != PROC_STATE_ZOMBIE)
               ? 1
               : 0;
}

/* 等 /run/display.status 里某个字段 >= min；成功写 *out。 */
static int wp32_wait_key(const char *key, u64 min, u32 tries, u64 *out)
{
    char text[256];
    u32 len = 0;
    u32 i;

    for (i = 0; i < tries; i++) {
        if (wp32_read_file(DS_STATUS_PATH, text, (u32)sizeof(text), &len)) {
            u64 v = 0;

            if (wp32_status_u64(text, len, key, &v) && v >= min) {
                if (out != NULL) {
                    *out = v;
                }
                return 1;
            }
        }
        (void)thread_sleep(WP32_POLL_SLEEP);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int wp32_selftest(void)
{
    char text[256];
    u32 len = 0;
    u32 ticket = 0;
    u32 dispd = 0;
    u32 wm = 0;
    u32 demo = 0;
    u32 launched = 0;
    int status = -1;
    u64 v_apps = 0;
    u64 v_hotkey = 0;
    u64 v_tasks = 0;
    u64 v_switch = 0;
    u64 v_lastswitch = 0;
    u64 v_launch = 0;
    u64 v_lastpid = 0;
    u64 v_surface = 0;
    u64 v_up = 0;
    u32 i;

    g_fails = 0;
    if (ng_display_fb() == NULL) {
        serial_puts("ng: WP3-2 selftest skipped (no fb)\r\n");
        return 0;
    }
    wp32_ensure_run_dir();

    /* ---- 1. 新起一个 display server（WP3-1 的那个已在上一段被 kill） --- */
    dispd = wp32_spawn(WP32_DISPLAYD, NULL);
    if (dispd == 0) {
        wp32_fail("displayd spawn");
        return g_fails;
    }
    for (i = 0; i < WP32_READY_TRIES; i++) {
        if (wp32_read_file(DS_PORT_PATH, text, (u32)sizeof(text), &len)) {
            ticket = wp32_parse_ticket(text, len);
            if (ticket != 0) {
                break;
            }
        }
        (void)thread_sleep(WP32_POLL_SLEEP);
    }
    if (ticket == 0) {
        wp32_fail("display port");
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm displayd ok (pid=");
    wp32_putdec(dispd);
    serial_puts(" ticket=");
    wp32_putdec(ticket);
    serial_puts(")\r\n");

    /* ---- 2. WM：面板 + 启动器 + 全局热键 ------------------------------- */
    wm = wp32_spawn(WP32_WM, NULL);
    if (wm == 0) {
        wp32_fail("wm spawn");
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm spawn ok (pid=");
    wp32_putdec(wm);
    serial_puts(")\r\n");

    if (!wp32_wait_key("hotkey=", 1, WP32_READY_TRIES, &v_hotkey) ||
        !wp32_wait_key("wm=", 1, WP32_READY_TRIES, &v_apps)) {
        wp32_fail("wm up");
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm ready ok (wm=");
    wp32_putdec(v_apps);
    serial_puts(" hotkey=");
    wp32_putdec(v_hotkey);
    serial_puts(")\r\n");

    /* ---- 3. L3 窗口（title=Demo，常驻；不抢焦点） ----------------------- */
    demo = wp32_spawn(WP32_DEMO, "window");
    if (demo == 0) {
        wp32_fail("demo window spawn");
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm demo spawn ok (pid=");
    wp32_putdec(demo);
    serial_puts(")\r\n");

    /* ---- 4. WM 用 LIST 看到窗口，Tab 切换焦点（QMP 注入） --------------- */
    if (!wp32_wait_key("wmtasks=", 1, WP32_TASKS_TRIES, &v_tasks)) {
        wp32_fail("wm tasks");
        wp32_kill_and_wait(demo);
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm tasks seen ok (n=");
    wp32_putdec(v_tasks);
    serial_puts(")\r\n");

    if (!wp32_wait_key("wmswitch=", 1, WP32_SWITCH_TRIES, &v_switch) ||
        !wp32_wait_key("wmlast=", 1, 32u, &v_lastswitch)) {
        wp32_fail("wm task switch");
        wp32_kill_and_wait(demo);
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm switch seen ok (focus=");
    wp32_putdec(v_lastswitch);
    serial_puts(" switch=");
    wp32_putdec(v_switch);
    serial_puts(")\r\n");

    /* ---- 5. 启动器 1..9 -> spawn（QMP 注入 '1'） ------------------------ */
    if (!wp32_wait_key("wmlaunch=", 1, WP32_LAUNCH_TRIES, &v_launch) ||
        !wp32_wait_key("wmlastpid=", 1, 32u, &v_lastpid)) {
        wp32_fail("wm launch");
        wp32_kill_and_wait(demo);
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm launch seen ok (pid=");
    wp32_putdec(v_lastpid);
    serial_puts(")\r\n");

    /* ---- 6. displayd 的表里至少有面板 + Demo 窗口 ----------------------- */
    if (!wp32_read_file(DS_STATUS_PATH, text, (u32)sizeof(text), &len) ||
        !wp32_status_u64(text, len, "up=", &v_up) ||
        !wp32_status_u64(text, len, "surface=", &v_surface) || v_up < 1 ||
        v_surface < 2) {
        wp32_fail("display status");
        wp32_kill_and_wait(demo);
        wp32_kill_and_wait(wm);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: wm display status ok (surface=");
    wp32_putdec(v_surface);
    serial_puts(")\r\n");

    /* ---- 7. WM 是独立进程：kill 它，应用窗口仍然活着 -------------------- */
    if (proc_kill(wm, 9) != PROC_OK ||
        proc_wait_pid(wm, &status, NULL) != PROC_OK ||
        status != PROC_STATUS_KILLED(9)) {
        wp32_fail("wm kill");
        wp32_kill_and_wait(demo);
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    (void)proc_reap_pid(wm);
    serial_puts("ng: wm kill ok (status=");
    wp32_putdec((u64)status);
    serial_puts(" waiters=");
    wp32_putdec((u64)ipc_core_waiter_live(ipc_core_global()));
    serial_puts(")\r\n");

    if (!wp32_alive(demo)) {
        wp32_fail("demo survives wm");
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    serial_puts("ng: demo survives wm ok (alive=1 id=");
    wp32_putdec(v_lastswitch);
    serial_puts(")\r\n");

    /* ---- 8. 清理：WM 启动的那个 demo，再是窗口 demo + displayd --------- */
    launched = (u32)v_lastpid;
    if (launched != 0 && launched != demo) {
        wp32_kill_and_wait(launched);
    }
    (void)thread_sleep(WP32_SETTLE);
    status = -1;
    if (proc_kill(demo, 9) != PROC_OK ||
        proc_wait_pid(demo, &status, NULL) != PROC_OK ||
        status != PROC_STATUS_KILLED(9)) {
        wp32_fail("demo kill");
        wp32_kill_and_wait(dispd);
        return g_fails;
    }
    (void)proc_reap_pid(demo);

    status = -1;
    if (proc_kill(dispd, 9) != PROC_OK ||
        proc_wait_pid(dispd, &status, NULL) != PROC_OK ||
        status != PROC_STATUS_KILLED(9)) {
        wp32_fail("displayd kill");
        return g_fails;
    }
    (void)proc_reap_pid(dispd);
    serial_puts("ng: wm displayd kill ok (status=");
    wp32_putdec((u64)status);
    serial_puts(" live=");
    wp32_putdec((u64)shm_live());
    serial_puts(")\r\n");
    if (shm_live() != 0) {
        wp32_fail("shm refcount");
    }

    return g_fails;
}
