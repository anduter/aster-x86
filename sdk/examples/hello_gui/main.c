/*
 * Aster SDK alpha 示例：hello_gui
 *
 * 只依赖 sdk/include/aster/ 下的头（不 include 任何内核/桌面头）：
 *   1. aster_app_open() 向 displayd CREATE 一个 320x200 窗口并 MAP 共享内存；
 *   2. 在 surface 上画底色/色块/8x8 文字；
 *   3. COMMIT 合成到屏幕；
 *   4. 事件循环：收到按键就重画一行状态，q/Esc 退出；无输入约 5 秒后自动退出；
 *   5. aster_app_close() 销毁 surface 并关闭端口。
 *
 * 编译（语法检查，不链接）：
 *   clang -fsyntax-only -std=c11 -I sdk/include sdk/examples/hello_gui/main.c
 * 完整构建/部署见 docs/sdk/getting-started.md。
 */
#include <aster/app.h>

#define GUI_W 320
#define GUI_H 200

#define COL_BG ASTER_RGB(0x10, 0x18, 0x20)
#define COL_BAR ASTER_RGB(0x28, 0x40, 0x60)
#define COL_PANEL ASTER_RGB(0x1C, 0x2A, 0x38)
#define COL_ACCENT ASTER_RGB(0x30, 0x70, 0xC0)
#define COL_TEXT ASTER_RGB(0xF0, 0xF4, 0xFF)
#define COL_DIM ASTER_RGB(0x9C, 0xB0, 0xC8)

static void gui_draw(struct aster_app *app, const char *status)
{
    struct aster_surface *s = &app->surface;

    aster_surface_clear(s, COL_BG);

    /* 顶部标题栏 */
    aster_draw_fill(s, 0, 0, GUI_W, 28, COL_BAR);
    aster_draw_text(s, 12, 10, "Aster SDK alpha", COL_TEXT, COL_BAR);

    /* 中央面板 + 强调色块 */
    aster_draw_fill(s, 20, 48, 280, 84, COL_PANEL);
    aster_draw_rect(s, 20, 48, 280, 84, COL_ACCENT);
    aster_draw_fill(s, 32, 60, 64, 28, COL_ACCENT);

    aster_draw_text(s, 108, 62, "Hello, GUI!", COL_TEXT, COL_PANEL);
    aster_draw_text(s, 108, 80, "display protocol v2", COL_DIM, COL_PANEL);
    aster_draw_text(s, 32, 104, "ARGB8888 surface via shared memory",
                    COL_DIM, COL_PANEL);

    /* 底部状态行 */
    aster_draw_fill(s, 0, GUI_H - 20, GUI_W, 20, COL_PANEL);
    aster_draw_text(s, 8, GUI_H - 15, status, COL_DIM, COL_PANEL);
}

ASTER_APP_MAIN
{
    struct aster_app app;
    struct aster_app_config cfg;
    struct aster_event ev;
    char status[32];
    u32 idle = 0;
    i32 rc;

    (void)argc;
    (void)argv;

    cfg.width = GUI_W;
    cfg.height = GUI_H;
    cfg.title = "Hello GUI";
    cfg.flags = 0; /* 0 = 让 displayd 级联放置 */
    cfg.x = 0;
    cfg.y = 0;

    rc = aster_app_open(&app, &cfg);
    if (rc < 0) {
        (void)aster_puts("hello_gui: open failed (is displayd running?)\n");
        return 1;
    }

    gui_draw(&app, "ready - press a key, q/Esc to quit");
    rc = aster_app_commit(&app);
    if (rc < 0) {
        (void)aster_puts("hello_gui: commit failed\n");
        aster_app_close(&app);
        return 1;
    }
    (void)aster_puts("hello_gui: window up\n");

    /*
     * 事件循环骨架：20ms 一轮；没有输入就累计 idle，约 250 轮（5s）后退出，
     * 这样在没有 QMP 键盘注入的环境里也能自己收尾（与内核 demo 的 timeout
     * 分支同一套路）。
     */
    while (aster_app_running(&app) && idle < 250u) {
        rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            idle++;
            continue;
        }
        idle = 0;
        if (ev.type != ASTER_EVENT_KEY_DOWN) {
            continue;
        }
        if (ev.key == 27u || ev.key == (u32)'q' || ev.key == (u32)'Q') {
            aster_app_quit(&app);
            continue;
        }

        /* 把按键画进状态行（可打印字符显示字符，否则显示 "key"）。 */
        if (ev.key >= 0x20u && ev.key < 0x7Fu) {
            status[0] = 'k';
            status[1] = 'e';
            status[2] = 'y';
            status[3] = ':';
            status[4] = ' ';
            status[5] = (char)ev.key;
            status[6] = '\0';
        } else {
            const char *plain = "key: (non-ascii)";

            aster_memcopy(status, plain, aster_strlen(plain) + 1u);
        }
        gui_draw(&app, status);
        (void)aster_app_commit(&app);
    }

    aster_app_close(&app);
    (void)aster_puts("hello_gui: bye\n");
    return 0;
}
