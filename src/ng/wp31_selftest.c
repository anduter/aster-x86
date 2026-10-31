/*
 * ng WP3-1: display server 自检编排（见 wp31.h）。
 *
 * 证据行（stdvga 路径）：
 *   ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo)   [proc_init]
 *   ng: displayd spawn ok (pid=..)
 *   ng: display port file ok (ticket=..)
 *   ng: demo spawn ok (pid=..)
 *   <displayd> ng: displayd up (fb=0xfd000000 1280x800)
 *   <displayd> ng: display port ok (ticket=.. path=/run/display.port)
 *   <displayd> ng: surface create ok (id=1 w=400 h=300 ...)
 *   <demo>     ng: shm map ok (va=0x... pages=118)
 *   <displayd> ng: composite ok (px=0x3070c0 at=160,120)
 *   <demo>     ng: demo input ready (id=1)
 *   <displayd> ng: displayd input ok (key=..)
 *   <demo>     ng: input route ok (key=b)
 *   <demo>     ng: demo done (id=1 input=1)
 *   ng: demo exit ok (status=0)
 *   ng: shm refcount ok (live=1)          <- demo 退出后 displayd 还持有 1 个对象
 *   ng: displayd status ok (surface=1 commits=1 input=1 px=0x3070c0)
 *   ng: demo hold spawn ok (pid=..)
 *   ng: displayd kill ok (status=265 waiters=0 live=0)
 *   ng: demo hold done (status=0 alive=1)
 *   ng: WP3-1 selftest PASS               <- main.c
 *
 * 降级路径（-vga none）：
 *   ng: WP3-1 selftest skipped (no fb)
 */
#include "wp31.h"

#include "ds_protocol.h"
#include "fs_syscall.h"
#include "ipc.h"
#include "proc.h"
#include "serial.h"
#include "shm.h"
#include "thread.h"
#include "vfs.h"
#include "wp23.h"

#define WP31_DISPLAYD "/bin/displayd"
#define WP31_DEMO "/bin/demo"
#define WP31_POLL_SLEEP 1000000ULL
#define WP31_POLL_TRIES 3000u
#define WP31_HOLD_SETTLE 60u /* 60 * 1e6 tick ≈ 60ms（QEMU TCG ~1GHz） */

static int g_fails;

static void wp31_fail(const char *what)
{
    g_fails++;
    serial_puts("ng: WP3-1 ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* ------------------------------------------------------------------------- */
/* 状态文件解析（"k=v k=v ..."；值支持十进制与 0x 前缀十六进制）              */
/* ------------------------------------------------------------------------- */

static int wp31_key_match(const char *text, u32 pos, const char *key)
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

static int wp31_status_u64(const char *text, u32 len, const char *key,
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
        if (i + key_len > len || !wp31_key_match(text, i, key)) {
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

static void wp31_put(const char *s)
{
    serial_puts(s);
}

static void wp31_putdec(u64 v)
{
    serial_putdec(v);
}

static void wp31_puthex(u64 v)
{
    serial_puthex(v);
}

/*
 * 读一个 ramfs 文本文件。返回 1 = 读到（*out_len 是字节数），0 = 还没出现。
 * 状态文件可能正在被 displayd 重写（open(O_TRUNC) 后 write 前），读失败就
 * 当"还没出现"，下一轮再试。
 */
static int wp31_read_file(const char *path, char *buf, u32 cap, u32 *out_len)
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

static int wp31_wait_file(const char *path, char *buf, u32 cap, u32 *out_len)
{
    u32 i;

    for (i = 0; i < WP31_POLL_TRIES; i++) {
        if (wp31_read_file(path, buf, cap, out_len)) {
            return 1;
        }
        (void)thread_sleep(WP31_POLL_SLEEP);
    }
    return 0;
}

static u32 wp31_parse_ticket(const char *text, u32 len)
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

static void wp31_ensure_run_dir(void)
{
    struct vfs_ctx *ctx = fs_sys_ctx();
    int rc;

    if (ctx == NULL) {
        return;
    }
    rc = vfs_mkdir(ctx, "/run", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        serial_puts("ng: display dir mkdir rc=");
        wp31_putdec((u64)(-rc));
        serial_puts("\r\n");
    }
}

/* ------------------------------------------------------------------------- */
/* 进程小工具                                                                 */
/* ------------------------------------------------------------------------- */

static u32 wp31_spawn(const char *path, const char *arg1)
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

static void wp31_kill_and_wait(u32 pid)
{
    if (pid == 0) {
        return;
    }
    (void)proc_kill(pid, 9);
    (void)proc_wait_pid(pid, NULL, NULL);
    (void)proc_reap_pid(pid);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int wp31_selftest(void)
{
    char text[128];
    u32 len = 0;
    u32 ticket = 0;
    u32 dispd = 0;
    u32 demo = 0;
    u32 hold = 0;
    int status = -1;
    u64 v_surface = 0;
    u64 v_commits = 0;
    u64 v_input = 0;
    u64 v_focus = 0;
    u64 v_px = 0;
    u64 v_mapped = 0;
    u32 i;

    g_fails = 0;
    if (ng_display_fb() == NULL) {
        wp31_put("ng: WP3-1 selftest skipped (no fb)\r\n");
        return 0;
    }
    wp31_ensure_run_dir();

    /* ---- 1. 父进程 spawn display server -------------------------------- */
    dispd = wp31_spawn(WP31_DISPLAYD, NULL);
    if (dispd == 0) {
        wp31_fail("displayd spawn");
        return g_fails;
    }
    wp31_put("ng: displayd spawn ok (pid=");
    wp31_putdec(dispd);
    wp31_put(")\r\n");

    /* ---- 2. 等 /run/display.port（displayd 自己 mkdir /run 并写 ticket） */
    if (!wp31_wait_file(DS_PORT_PATH, text, (u32)sizeof(text), &len)) {
        wp31_fail("display port");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    ticket = wp31_parse_ticket(text, len);
    if (ticket == 0) {
        wp31_fail("display port");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    wp31_put("ng: display port file ok (ticket=");
    wp31_putdec(ticket);
    wp31_put(")\r\n");

    /* ---- 3. spawn demo：CREATE -> SHM_MAP -> 画 -> COMMIT -> 等 INPUT --- */
    demo = wp31_spawn(WP31_DEMO, NULL);
    if (demo == 0) {
        wp31_fail("demo spawn");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    wp31_put("ng: demo spawn ok (pid=");
    wp31_putdec(demo);
    wp31_put(")\r\n");

    status = -1;
    if (proc_wait_pid(demo, &status, NULL) != PROC_OK || status != 0) {
        wp31_fail("demo exit");
        (void)proc_reap_pid(demo);
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    (void)proc_reap_pid(demo);
    wp31_put("ng: demo exit ok (status=0)\r\n");

    /* ---- 4. 读 displayd 的状态文件（surface/commit/input/像素证据） ---- */
    if (!wp31_read_file(DS_STATUS_PATH, text, (u32)sizeof(text), &len)) {
        wp31_fail("display status");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    if (!wp31_status_u64(text, len, "surface=", &v_surface) ||
        !wp31_status_u64(text, len, "mapped=", &v_mapped) ||
        !wp31_status_u64(text, len, "commits=", &v_commits) ||
        !wp31_status_u64(text, len, "focus=", &v_focus) ||
        !wp31_status_u64(text, len, "input=", &v_input) ||
        !wp31_status_u64(text, len, "px=", &v_px)) {
        wp31_fail("display status");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    if (v_surface < 1 || v_mapped < 1 || v_commits < 1 || v_focus < 1 ||
        v_px != (u64)DS_DEMO_ACCENT) {
        wp31_fail("display status");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }

    /* demo 退出后：共享内存对象还在（displayd 的映射），refcount 没有归零。 */
    if (shm_live() < 1) {
        wp31_fail("shm refcount");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    wp31_put("ng: shm refcount ok (live=");
    wp31_putdec((u64)shm_live());
    wp31_put(")\r\n");

    wp31_put("ng: displayd status ok (surface=");
    wp31_putdec(v_surface);
    wp31_put(" commits=");
    wp31_putdec(v_commits);
    wp31_put(" input=");
    wp31_putdec(v_input);
    wp31_put(" px=");
    wp31_puthex(v_px);
    wp31_put(")\r\n");

    /* ---- 5. 崩溃隔离：另一个客户端在跑时 kill display server ----------- */
    hold = wp31_spawn(WP31_DEMO, "hold");
    if (hold == 0) {
        wp31_fail("demo hold spawn");
        wp31_kill_and_wait(dispd);
        return g_fails;
    }
    wp31_put("ng: demo hold spawn ok (pid=");
    wp31_putdec(hold);
    wp31_put(")\r\n");
    for (i = 0; i < WP31_HOLD_SETTLE; i++) {
        (void)thread_sleep(WP31_POLL_SLEEP);
    }

    status = -1;
    if (proc_kill(dispd, 9) != PROC_OK ||
        proc_wait_pid(dispd, &status, NULL) != PROC_OK ||
        status != PROC_STATUS_KILLED(9)) {
        wp31_fail("displayd kill");
        wp31_kill_and_wait(dispd);
        wp31_kill_and_wait(hold);
        return g_fails;
    }
    (void)proc_reap_pid(dispd);
    wp31_put("ng: displayd kill ok (status=");
    wp31_putdec((u64)status);
    wp31_put(" live=");
    wp31_putdec((u64)shm_live());
    wp31_put(")\r\n");
    if (shm_live() != 0) {
        /* displayd 退出后它的 surface 缓冲应随 refcount 归零释放。 */
        wp31_fail("shm refcount");
    }

    /* ---- 6. 客户端没被 displayd 的死拖垮：自己超时后干净退出 ----------- */
    status = -1;
    if (proc_wait_pid(hold, &status, NULL) != PROC_OK || status != 0) {
        wp31_fail("demo hold");
        wp31_kill_and_wait(hold);
        return g_fails;
    }
    (void)proc_reap_pid(hold);
    wp31_put("ng: demo hold wait ok (status=0 waiters=");
    wp31_putdec((u64)ipc_core_waiter_live(ipc_core_global()));
    wp31_put(")\r\n");

    return g_fails;
}
