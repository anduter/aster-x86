/*
 * Aster SDK alpha 示例：hello_gui（display 协议 v3 + session 通道）
 *
 * 只依赖 sdk/include/aster/ 下的头（不 include 任何内核/桌面头）：
 *   1. aster_session_connect() 解析 sessiond 经 argv 发来的
 *      `session=<ticket> token=0x<role_token>`（没有会话时返回 -ENOENT，
 *      应用仍可在 shell 里直接 spawn 跑起来）；
 *   2. aster_app_open() 向 displayd CREATE 一个 320x200 窗口并 MAP 共享内存，
 *      CREATE v3 带 token；随后 FOCUS（v3 令牌）并 COMMIT；
 *   3. 有会话通道时 REGISTER 窗口 + 每 ~200ms HEARTBEAT；再用一次 v3 LIST
 *      回读自己的 surface token，确认 sessiond 签发 → argv → displayd 的
 *      令牌链真的闭合；
 *   4. 事件循环：收到按键就重画状态行，q/Esc 退出；没有输入时 interactive
 *      模式约 5 秒、window 模式约 30 秒后自动退出（后者是 WP3-4 demo 工具
 *      用 sessiond/WM 拉起时的常驻角色）；
 *   5. 退出时 EXIT + DESTROY + BYE。
 *
 * 证据行（tools/sdk-demo.ps1 断言）：
 *   ng: sdk session ok (session=.. token=0x..)
 *   ng: sdk window ok (id=.. w=320 h=200)
 *   ng: sdk token ok (token=0x.. id=..)
 *   ng: sdk demo ok (pid=.. id=.. mode=interactive|window token=0x..)
 *   ng: sdk demo bye (id=.. mode=..)
 *
 * 编译（语法检查，不链接）：
 *   clang -fsyntax-only -std=c11 -I sdk/include sdk/examples/hello_gui/main.c
 * 完整构建/部署见 docs/sdk/getting-started.md；QEMU 内真跑见
 * tools/sdk-demo.ps1。
 */
#include <aster/app.h>
#include <aster/session.h>

#define GUI_W 320
#define GUI_H 200
#define GUI_TITLE "Aster SDK"
#define GUI_IDLE_INTERACTIVE 250u /* 250 * 20ms ≈ 5s */
#define GUI_IDLE_WINDOW 1500u     /* 1500 * 20ms ≈ 30s（内核会提前回收） */

#define COL_BG ASTER_RGB(0x10, 0x18, 0x20)
#define COL_BAR ASTER_RGB(0x28, 0x40, 0x60)
#define COL_PANEL ASTER_RGB(0x1C, 0x2A, 0x38)
#define COL_ACCENT ASTER_RGB(0x30, 0x70, 0xC0)
#define COL_TEXT ASTER_RGB(0xF0, 0xF4, 0xFF)
#define COL_DIM ASTER_RGB(0x9C, 0xB0, 0xC8)

/* ------------------------------------------------------------------------- */
/* 无 libc 的输出小工具                                                       */
/* ------------------------------------------------------------------------- */

static void gui_put(const char *s)
{
    (void)aster_puts(s);
}

static void gui_putdec(u32 v)
{
    char buf[11];
    u32 n = 0;
    u32 i;

    if (v == 0u) {
        gui_put("0");
        return;
    }
    while (v != 0u && n < 10u) {
        buf[n++] = (char)('0' + (i32)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++) {
        char c = buf[n - 1u - i];

        (void)aster_console_write(&c, 1);
    }
}

static void gui_puthex(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[8];
    u32 n = 0;
    u32 i;

    gui_put("0x");
    if (v == 0u) {
        gui_put("0");
        return;
    }
    while (v != 0u && n < 8u) {
        tmp[n++] = digits[v & 0xFu];
        v >>= 4;
    }
    for (i = 0; i < n; i++) {
        char c = tmp[n - 1u - i];

        (void)aster_console_write(&c, 1);
    }
}

/* 把 token 写成 "0x%08x"（窗口里对齐用）。 */
static void gui_token_text(char *out, u32 cap, u32 token)
{
    static const char digits[] = "0123456789abcdef";
    u32 i;

    if (out == NULL || cap < 11u) {
        return;
    }
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8u; i++) {
        out[2u + i] = digits[(token >> (28u - i * 4u)) & 0xFu];
    }
    out[10] = '\0';
}

/* ------------------------------------------------------------------------- */
/* 绘制                                                                       */
/* ------------------------------------------------------------------------- */

static void gui_draw(struct aster_app *app, int have_session, u32 token,
                     const char *status)
{
    struct aster_surface *s = &app->surface;
    char token_line[24];

    aster_surface_clear(s, COL_BG);

    /* 顶部标题栏。 */
    aster_draw_fill(s, 0, 0, GUI_W, 28, COL_BAR);
    aster_draw_text(s, 12, 10, "Aster SDK alpha", COL_TEXT, COL_BAR);

    /*
     * 强调色 logo 覆盖 (20,20)..(83,47)：displayd 的合成探针固定在
     * surface 的 (25,25)，所以这里必须是 COL_ACCENT（与 WP3-1 的 demo
     * 契约一致，SdkDemo 内核跑 WP3-1 自检时读的就是它）。
     */
    aster_draw_fill(s, 20, 20, 64, 28, COL_ACCENT);
    aster_draw_text(s, 96, 30, "display protocol v3", COL_DIM, COL_BAR);

    /* 中央面板 + 令牌探针（QMP 截图像素断言打在 (x+40,y+80)）。 */
    aster_draw_fill(s, 20, 56, 280, 84, COL_PANEL);
    aster_draw_rect(s, 20, 56, 280, 84, COL_ACCENT);
    aster_draw_fill(s, 32, 70, 64, 28, COL_ACCENT);

    aster_draw_text(s, 108, 74, "Hello, GUI!", COL_TEXT, COL_PANEL);
    aster_draw_text(s, 108, 92, "ARGB8888 via shared memory", COL_DIM,
                    COL_PANEL);
    aster_draw_text(s, 32, 108, "session channel + role token", COL_DIM,
                    COL_PANEL);

    if (have_session != 0) {
        gui_token_text(token_line, (u32)sizeof(token_line), token);
        aster_draw_text(s, 32, 124, "token: ", COL_DIM, COL_PANEL);
        aster_draw_text(s, 80, 124, token_line, COL_TEXT, COL_PANEL);
    } else {
        aster_draw_text(s, 32, 124, "token: (no session)", COL_DIM, COL_PANEL);
    }

    /* 底部状态行。 */
    aster_draw_fill(s, 0, GUI_H - 20, GUI_W, 20, COL_PANEL);
    aster_draw_text(s, 8, GUI_H - 15, status, COL_DIM, COL_PANEL);
}

/* ------------------------------------------------------------------------- */
/* 会话：注册窗口 / v3 令牌回读 / 心跳                                        */
/* ------------------------------------------------------------------------- */

static int gui_session_register(struct aster_app *app,
                                struct aster_session *sess)
{
    u32 tries;
    i32 rc = ASTER_SESSION_EINVAL;

    for (tries = 0; tries < 8u; tries++) {
        struct aster_display_input in;

        rc = aster_session_register(sess, app->surface.id, GUI_TITLE);
        if (rc == 0) {
            return 0;
        }
        /* 20ms 的"睡眠"：顺便丢掉窗口建立阶段可能到达的按键。 */
        (void)aster_display_poll_input(&app->display, &in,
                                       ASTER_DISPLAY_POLL_TICKS);
    }
    return (int)rc;
}

/*
 * 用 display v3 的 LIST 回读自己的 surface，确认 displayd 记住的 token 与
 * sessiond 经 argv 发来的一致（sessiond 的 200ms 同步也是同一判据）。
 * 返回 1 = 一致，0 = 没找到，<0 = 协议/超时错误。
 */
static int gui_verify_token(struct aster_app *app, u32 token, u32 *out_id)
{
    struct aster_display_list list;
    i32 rc;
    u32 i;

    rc = aster_display_list(&app->display, &list);
    if (rc < 0) {
        return (int)rc;
    }
    for (i = 0; i < list.count; i++) {
        if (list.entries[i].id == app->surface.id) {
            if (out_id != NULL) {
                *out_id = list.entries[i].id;
            }
            return (list.entries[i].token == token) ? 1 : 0;
        }
    }
    return 0;
}

static void gui_session_fail(const char *what, i32 rc)
{
    gui_put("ng: sdk demo FAIL (");
    gui_put(what);
    gui_put(" rc=-");
    gui_putdec((u32)(-rc));
    gui_put(")\r\n");
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * demo harness 的 hold 角色（SdkDemo 构建里 /bin/demo 也承担 WP3-1 的
 * "displayd 被杀后客户端自己干净退出" 用例）：只建端口、不建 surface/不映射
 * 共享内存，等约 2 秒后关闭退出。这样 displayd 死时没有客户端映射残留。
 */
static int gui_hold(void)
{
    u8 buf[ASTER_IPC_HDR_SIZE + 64];
    i64 port;
    u32 i;

    port = aster_port_create(ASTER_PORT_DEPTH_DEFAULT);
    if (port <= 0) {
        gui_put("ng: sdk demo FAIL (hold port)\r\n");
        return 1;
    }
    gui_put("ng: sdk demo ok (pid=");
    gui_putdec(aster_getpid());
    gui_put(" id=0 mode=hold token=0x0)\r\n");

    for (i = 0; i < ASTER_DISPLAY_INPUT_WAIT_TRIES; i++) {
        i64 n = aster_port_recv((u32)port, buf, (u32)sizeof(buf),
                                ASTER_DISPLAY_POLL_TICKS);

        if (n < 0 && n != ASTER_ETIMEDOUT) {
            break;
        }
    }
    (void)aster_port_close((u32)port);
    gui_put("ng: sdk demo bye (id=0 mode=hold)\r\n");
    return 0;
}

ASTER_APP_MAIN
{
    struct aster_app app;
    struct aster_app_config cfg;
    struct aster_event ev;
    struct aster_session sess;
    char status[32];
    u32 idle = 0;
    u32 round = 0;
    u32 idle_limit = GUI_IDLE_INTERACTIVE;
    u32 mode_window = 0;
    u32 pid;
    u32 surface_id = 0;
    int have_session = 0;
    i32 rc;

    /* WP3-4 demo 工具用 sessiond/WM 拉起时 argv[1] == "window"（常驻）。 */
    if (argc >= 2 && argv != NULL && aster_streq(argv[1], "window")) {
        mode_window = 1u;
        idle_limit = GUI_IDLE_WINDOW;
    } else if (argc >= 2 && argv != NULL && aster_streq(argv[1], "hold")) {
        return gui_hold();
    }

    /*
     * 会话通道：session= / token= 来自 argv。没有会话参数是正常情况
     * （shell 里 `spawn /bin/demo` 直连），返回 -ENOENT 后 token 用 0。
     */
    rc = aster_session_connect(&sess, argc, argv);
    if (rc == 0) {
        have_session = 1;
        gui_put("ng: sdk session ok (session=");
        gui_putdec(sess.server_ticket);
        gui_put(" token=");
        gui_puthex(sess.token);
        gui_put(")\r\n");
    } else if (rc != ASTER_SESSION_ENOENT) {
        gui_session_fail("session connect", rc);
        return 1;
    }

    cfg.width = GUI_W;
    cfg.height = GUI_H;
    cfg.title = GUI_TITLE;
    cfg.flags = 0; /* 0 = 让 displayd 级联放置 */
    cfg.x = 0;
    cfg.y = 0;
    cfg.token = (have_session != 0) ? sess.token : 0u;

    rc = aster_app_open(&app, &cfg);
    if (rc < 0) {
        gui_session_fail("app open", rc);
        if (have_session != 0) {
            (void)aster_session_exit(&sess, 1);
            aster_session_close(&sess);
        }
        return 1;
    }

    /* 焦点：v3 带令牌；之后 displayd 才会把按键推给本窗口。 */
    rc = aster_app_focus(&app);
    if (rc < 0) {
        gui_session_fail("focus", rc);
    }

    pid = aster_getpid();
    surface_id = app.surface.id;
    gui_draw(&app, have_session, sess.token,
             "ready - press a key, q/Esc to quit");
    rc = aster_app_commit(&app);
    if (rc < 0) {
        gui_session_fail("commit", rc);
        aster_app_close(&app);
        if (have_session != 0) {
            (void)aster_session_exit(&sess, 1);
            aster_session_close(&sess);
        }
        return 1;
    }
    gui_put("ng: sdk window ok (id=");
    gui_putdec(app.surface.id);
    gui_put(" w=");
    gui_putdec(GUI_W);
    gui_put(" h=");
    gui_putdec(GUI_H);
    gui_put(")\r\n");

    if (have_session != 0) {
        u32 seen = 0;

        rc = gui_session_register(&app, &sess);
        if (rc < 0) {
            gui_session_fail("session register", rc);
        }
        rc = gui_verify_token(&app, sess.token, &seen);
        if (rc > 0) {
            gui_put("ng: sdk token ok (token=");
            gui_puthex(sess.token);
            gui_put(" id=");
            gui_putdec(seen);
            gui_put(")\r\n");
        } else if (rc == 0) {
            gui_put("ng: sdk token FAIL (token=");
            gui_puthex(sess.token);
            gui_put(" id=");
            gui_putdec(app.surface.id);
            gui_put(")\r\n");
        } else {
            gui_session_fail("token list", rc);
        }
    }

    gui_put("ng: sdk demo ok (pid=");
    gui_putdec(pid);
    gui_put(" id=");
    gui_putdec(app.surface.id);
    gui_put(" mode=");
    gui_put((mode_window != 0u) ? "window" : "interactive");
    gui_put(" token=");
    gui_puthex((have_session != 0) ? sess.token : 0u);
    gui_put(")\r\n");

    /*
     * 事件循环骨架：20ms 一轮；没有输入累计 idle，超过本角色上限自动退出
     * （这样在没有 QMP 键盘注入的环境里也能自己收尾）。有会话通道时每 10 轮
     * 发一次心跳（~200ms，sessiond 的 800ms 超时窗口）。
     */
    while (aster_app_running(&app) && idle < idle_limit) {
        rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
        if (rc < 0) {
            break;
        }
        round++;
        if (rc == 0) {
            idle++;
        } else {
            idle = 0;
            if (ev.type == ASTER_EVENT_KEY_DOWN) {
                if (ev.key == 27u || ev.key == (u32)'q' ||
                    ev.key == (u32)'Q') {
                    aster_app_quit(&app);
                } else if (ev.key >= 0x20u && ev.key < 0x7Fu) {
                    status[0] = 'k';
                    status[1] = 'e';
                    status[2] = 'y';
                    status[3] = ':';
                    status[4] = ' ';
                    status[5] = (char)ev.key;
                    status[6] = '\0';
                    gui_draw(&app, have_session, sess.token, status);
                    (void)aster_app_commit(&app);
                }
            }
        }
        if (have_session != 0 &&
            (round % ASTER_SESSION_HEARTBEAT_ROUNDS) == 0u) {
            (void)aster_session_heartbeat(&sess, round);
        }
    }

    if (have_session != 0) {
        (void)aster_session_exit(&sess, 0);
    }
    /*
     * WP3-1 兼容说明：SdkDemo 构建里本程序占据 /bin/demo 的位置，而 WP3-1
     * 自检在 demo 退出后仍要读 displayd 的 surface/mapped 计数（历史 demo
     * 不 DESTROY）。interactive 角色只关端口；window/会话角色走完整收尾。
     */
    if (mode_window != 0u || have_session != 0) {
        aster_app_close(&app);
    } else {
        aster_display_close(&app.display);
    }
    if (have_session != 0) {
        aster_session_close(&sess);
    }
    gui_put("ng: sdk demo bye (id=");
    gui_putdec(surface_id);
    gui_put(" mode=");
    gui_put((mode_window != 0u) ? "window" : "interactive");
    gui_put(")\r\n");
    return 0;
}
