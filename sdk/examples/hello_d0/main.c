/*
 * Aster SDK alpha 示例：hello_d0 —— 外部开发者测试（E4 / WP4-2）
 *
 * 这是一个"从文档/头文件出发、只用 sdk/include/aster 下的公共 API"写出来的
 * 普通 GUI 应用（不复制 hello_gui、不带任何 demo harness 角色）：
 *
 *   1. aster_session_connect() 解析 sessiond 经 argv 发来的
 *      `session=<ticket> token=0x<role_token>`；没有会话（例如 shell 里
 *      直接 spawn）返回 -ENOENT，应用按 token=0 继续跑；
 *   2. aster_app_open() 建 480x300 窗口 "D0 Demo"（v3 CREATE），
 *      aster_app_focus() 拿焦点，之后按键以 ASTER_EVENT_KEY_DOWN 到达；
 *   3. 画面：背景、标题栏、3 个彩色卡片（红/绿/蓝，各带一种几何图形）、
 *      计数器大字 + 10 格活动条、最后按键、页脚空闲进度条；
 *   4. 事件循环：任意键 count+1 并重绘；r 归零；q/Q/Esc 退出；
 *      **无输入累计 400 次 20ms poll 超时（≈8s）自动退出**；
 *   5. 退出：有会话时 EXIT + DESTROY + BYE，并打印证据行：
 *        ng: d0 boot ok (sdk=0.1.0-alpha abi=0x00010000)
 *        ng: d0 demo up (pid=.. id=.. x=.. y=.. w=480 h=300 token=0x.. session=0|1)
 *        ng: d0 draw ok (cards=3 count=0)
 *        ng: d0 key ok (key=0x20 count=1)
 *        ng: d0 quit ok (reason=q/esc|idle|display rounds=.. idle=..)
 *        ng: d0 demo bye (id=.. count=.. rounds=.. reason=..)
 *
 * 只依赖 sdk/include/aster/；不 include 内核/桌面头，不用 libc，不用 FPU。
 * 编译/链接/运行步骤见同目录 README.md。
 */
#include <aster/app.h>
#include <aster/session.h>

/* ---- 窗口与布局 ---------------------------------------------------------- */
#define D0_W 480
#define D0_H 300
#define D0_TITLE "D0 Demo"

/* 无定时器：ASTER_DISPLAY_POLL_TICKS ≈ 20ms，400 次超时 ≈ 8s 无输入。 */
#define D0_IDLE_LIMIT 400u
#define D0_FOOTER_Y 264
#define D0_HEADER_H 44
#define D0_CARD_X0 24
#define D0_CARD_Y 56
#define D0_CARD_W 136
#define D0_CARD_H 96
#define D0_CARD_GAP 12
#define D0_PANEL_X 24
#define D0_PANEL_Y 164
#define D0_PANEL_W 432
#define D0_PANEL_H 88

/* ---- 颜色（ARGB8888，全部不透明） ---------------------------------------- */
#define D0_BG ASTER_RGB(0x0E, 0x14, 0x1B)
#define D0_HEADER ASTER_RGB(0x18, 0x22, 0x2E)
#define D0_HEADER_LN ASTER_RGB(0x3A, 0x78, 0xC2)
#define D0_PANEL ASTER_RGB(0x16, 0x20, 0x2B)
#define D0_BORDER ASTER_RGB(0x2C, 0x3E, 0x50)
#define D0_TEXT ASTER_RGB(0xF0, 0xF4, 0xFF)
#define D0_DIM ASTER_RGB(0x8F, 0xA8, 0xC4)
#define D0_FAINT ASTER_RGB(0x5A, 0x70, 0x88)
#define D0_ACCENT ASTER_RGB(0x4A, 0x9A, 0xE0)
#define D0_TRACK ASTER_RGB(0x24, 0x32, 0x40)

#define D0_RED ASTER_RGB(0xC0, 0x50, 0x4A)
#define D0_RED_EDGE ASTER_RGB(0xE0, 0x78, 0x70)
#define D0_RED_DARK ASTER_RGB(0x8E, 0x38, 0x34)
#define D0_GREEN ASTER_RGB(0x3F, 0xA0, 0x60)
#define D0_GREEN_EDGE ASTER_RGB(0x64, 0xC8, 0x84)
#define D0_GREEN_DARK ASTER_RGB(0x2A, 0x6E, 0x42)
#define D0_GREEN_LIGHT ASTER_RGB(0x9C, 0xE0, 0xB0)
#define D0_BLUE ASTER_RGB(0x35, 0x70, 0xC0)
#define D0_BLUE_EDGE ASTER_RGB(0x64, 0x9C, 0xE8)
#define D0_BLUE_LIGHT ASTER_RGB(0x9C, 0xC8, 0xF0)

/* ------------------------------------------------------------------------- */
/* 无 libc 的小工具：输出 / 字符串拼装                                        */
/* ------------------------------------------------------------------------- */

static void d0_put(const char *s)
{
    (void)aster_puts(s);
}

static void d0_put_u32(u32 v)
{
    char tmp[10];
    u32 n = 0;

    if (v == 0u) {
        d0_put("0");
        return;
    }
    while (v != 0u && n < (u32)sizeof(tmp)) {
        tmp[n] = (char)('0' + (v % 10u));
        v /= 10u;
        n++;
    }
    while (n > 0u) {
        char c = tmp[--n];

        (void)aster_console_write(&c, 1u);
    }
}

static void d0_put_hex2(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char c[2];

    c[0] = digits[(v >> 4) & 0xFu];
    c[1] = digits[v & 0xFu];
    (void)aster_console_write(c, 2u);
}

static void d0_put_hex8(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char c;
    i32 shift;

    d0_put("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        c = digits[(v >> (u32)shift) & 0xFu];
        (void)aster_console_write(&c, 1u);
    }
}

/* 追加字符串（保证 NUL 结尾），返回新的长度。 */
static u32 d0_sput(char *buf, u32 n, u32 cap, const char *s)
{
    u32 i = 0;

    if (buf == NULL || s == NULL || n >= cap) {
        return n;
    }
    while (s[i] != '\0' && (n + 1u) < cap) {
        buf[n++] = s[i++];
    }
    buf[n] = '\0';
    return n;
}

/* 追加固定宽度小写十六进制。 */
static u32 d0_shex(char *buf, u32 n, u32 cap, u32 v, u32 digits)
{
    static const char hex[] = "0123456789abcdef";
    u32 i;

    for (i = 0; i < digits; i++) {
        if ((n + 1u) >= cap) {
            break;
        }
        buf[n++] = hex[(v >> (4u * (digits - 1u - i))) & 0xFu];
    }
    if (n < cap) {
        buf[n] = '\0';
    }
    return n;
}

static void d0_fmt_count(char out[6], u32 v)
{
    u32 x = v % 100000u;
    u32 div = 10000u;
    u32 i;

    for (i = 0; i < 5u; i++) {
        out[i] = (char)('0' + (x / div) % 10u);
        x %= div;
        div /= 10u;
    }
    out[5] = '\0';
}

/* 2 倍字号（16x16）文字：用公共字模 + 2x2 实心块放大。 */
static void d0_text2x(struct aster_surface *s, i32 x, i32 y, const char *text,
                      u32 fg, u32 bg)
{
    i32 cx = x;
    u64 i = 0;

    while (text[i] != '\0') {
        const u8 *glyph = aster_font_glyph((u8)text[i]);
        u32 row;
        u32 col;

        for (row = 0; row < ASTER_FONT_H; row++) {
            u8 bits = glyph[row];

            for (col = 0; col < ASTER_FONT_W; col++) {
                u32 color = ((bits >> col) & 1u) != 0u ? fg : bg;

                aster_draw_fill(s, cx + (i32)col * 2, y + (i32)row * 2, 2, 2,
                                color);
            }
        }
        cx += (i32)ASTER_FONT_W * 2;
        i++;
    }
}

/* ------------------------------------------------------------------------- */
/* 绘制                                                                      */
/* ------------------------------------------------------------------------- */

static void d0_draw_header(struct aster_surface *s)
{
    aster_draw_fill(s, 0, 0, D0_W, D0_HEADER_H, D0_HEADER);
    aster_draw_fill(s, 0, D0_HEADER_H - 2, D0_W, 2, D0_HEADER_LN);

    d0_text2x(s, 16, 12, D0_TITLE, D0_TEXT, D0_HEADER);
    aster_draw_text(s, 148, 16, "external developer test", D0_DIM, D0_HEADER);
    aster_draw_text(s, 408, 16, "WP4-2", D0_FAINT, D0_HEADER);
}

static void d0_draw_cards(struct aster_surface *s)
{
    i32 red_x = D0_CARD_X0;
    i32 green_x = D0_CARD_X0 + D0_CARD_W + D0_CARD_GAP;
    i32 blue_x = D0_CARD_X0 + 2 * (D0_CARD_W + D0_CARD_GAP);
    u32 i;

    /* 卡片 1：红色，柱状图（5 根递增竖条）。 */
    aster_draw_fill(s, red_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H, D0_RED);
    aster_draw_rect(s, red_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H, D0_RED_EDGE);
    aster_draw_text(s, red_x + 12, D0_CARD_Y + 10, "RED", D0_TEXT, D0_RED);
    aster_draw_text(s, red_x + D0_CARD_W - 28, D0_CARD_Y + 10, "01", D0_TEXT,
                    D0_RED);
    for (i = 0; i < 5u; i++) {
        i32 h = 10 + (i32)i * 9;
        i32 x = red_x + 14 + (i32)i * 22;

        aster_draw_fill(s, x, D0_CARD_Y + 84 - h, 16, h, D0_RED_DARK);
    }

    /* 卡片 2：绿色，嵌套画框（rect + 实心内框）。 */
    aster_draw_fill(s, green_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H, D0_GREEN);
    aster_draw_rect(s, green_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H,
                    D0_GREEN_EDGE);
    aster_draw_text(s, green_x + 12, D0_CARD_Y + 10, "GREEN", D0_TEXT,
                    D0_GREEN);
    aster_draw_text(s, green_x + D0_CARD_W - 28, D0_CARD_Y + 10, "02", D0_TEXT,
                    D0_GREEN);
    aster_draw_rect(s, green_x + 14, D0_CARD_Y + 44, 108, 40, D0_GREEN_LIGHT);
    aster_draw_rect(s, green_x + 26, D0_CARD_Y + 54, 84, 20, D0_GREEN_DARK);
    aster_draw_fill(s, green_x + 38, D0_CARD_Y + 60, 60, 8, D0_GREEN_LIGHT);

    /* 卡片 3：蓝色，波形/均衡器（7 根不同高度的竖条）。 */
    aster_draw_fill(s, blue_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H, D0_BLUE);
    aster_draw_rect(s, blue_x, D0_CARD_Y, D0_CARD_W, D0_CARD_H, D0_BLUE_EDGE);
    aster_draw_text(s, blue_x + 12, D0_CARD_Y + 10, "BLUE", D0_TEXT, D0_BLUE);
    aster_draw_text(s, blue_x + D0_CARD_W - 28, D0_CARD_Y + 10, "03", D0_TEXT,
                    D0_BLUE);
    {
        static const u8 bars[7] = { 10, 22, 34, 46, 30, 18, 12 };

        for (i = 0; i < 7u; i++) {
            i32 h = (i32)bars[i];
            i32 x = blue_x + 13 + (i32)i * 16;

            aster_draw_fill(s, x, D0_CARD_Y + 84 - h, 10, h, D0_BLUE_LIGHT);
        }
    }
}

static void d0_draw_panel(struct aster_surface *s, u32 count, u32 last_key,
                          u32 last_scan)
{
    char digits[6];
    char line[64];
    u32 n = 0;
    u32 lit = count % 10u;
    u32 i;

    aster_draw_fill(s, D0_PANEL_X, D0_PANEL_Y, D0_PANEL_W, D0_PANEL_H,
                    D0_PANEL);
    aster_draw_rect(s, D0_PANEL_X, D0_PANEL_Y, D0_PANEL_W, D0_PANEL_H,
                    D0_BORDER);

    aster_draw_text(s, 40, 176, "COUNT", D0_DIM, D0_PANEL);
    aster_draw_text(s, 240, 176, "ACTIVITY", D0_DIM, D0_PANEL);

    d0_fmt_count(digits, count);
    d0_text2x(s, 40, 194, digits, D0_TEXT, D0_PANEL);
    aster_draw_text(s, 132, 194, "key presses", D0_DIM, D0_PANEL);
    aster_draw_text(s, 132, 208, "r = reset", D0_FAINT, D0_PANEL);

    /* 10 格活动条：亮 count % 10 格；计到 10 的整数倍时全灭，重新计数。 */
    for (i = 0; i < 10u; i++) {
        aster_draw_fill(s, 240 + (i32)i * 22, 196, 18, 16,
                        (i < lit) ? D0_ACCENT : D0_TRACK);
    }

    n = d0_sput(line, n, (u32)sizeof(line), "last key: ");
    if (last_key >= 0x20u && last_key <= 0x7Eu) {
        char ch[2];

        ch[0] = (char)last_key;
        ch[1] = '\0';
        n = d0_sput(line, n, (u32)sizeof(line), "'");
        n = d0_sput(line, n, (u32)sizeof(line), ch);
        n = d0_sput(line, n, (u32)sizeof(line), "' (0x");
    } else if (last_key == 0u) {
        n = d0_sput(line, n, (u32)sizeof(line), "(none) (0x");
    } else {
        n = d0_sput(line, n, (u32)sizeof(line), "(ctrl) (0x");
    }
    n = d0_shex(line, n, (u32)sizeof(line), last_key, 2u);
    n = d0_sput(line, n, (u32)sizeof(line), ") scan=0x");
    n = d0_shex(line, n, (u32)sizeof(line), last_scan, 2u);
    (void)n;
    aster_draw_text(s, 40, 230, line, D0_DIM, D0_PANEL);
}

static void d0_draw_footer(struct aster_surface *s, u32 idle)
{
    char line[24];
    u32 n = 0;
    u32 bar = (idle * 120u) / D0_IDLE_LIMIT;
    u32 tenths = idle / 5u; /* idle * 20ms = idle/50 s = idle/5 个 0.1s */
    u32 whole = tenths / 10u;

    aster_draw_fill(s, 0, D0_FOOTER_Y, D0_W, D0_H - D0_FOOTER_Y, D0_HEADER);
    aster_draw_fill(s, 0, D0_FOOTER_Y, D0_W, 1, D0_BORDER);

    aster_draw_text(s, 16, 272, "any key +1   r reset   q/ESC quit", D0_DIM,
                    D0_HEADER);
    aster_draw_text(s, 408, 272, "8s idle", D0_FAINT, D0_HEADER);

    aster_draw_fill(s, 16, 286, 120, 6, D0_TRACK);
    if (bar > 0u) {
        aster_draw_fill(s, 16, 286, (i32)bar, 6,
                        (idle >= D0_IDLE_LIMIT - 80u) ? D0_RED : D0_ACCENT);
    }

    n = d0_sput(line, n, (u32)sizeof(line), "idle ");
    if (whole > 9u) {
        whole = 9u;
    }
    line[n++] = (char)('0' + whole);
    line[n++] = '.';
    line[n++] = (char)('0' + (tenths % 10u));
    line[n++] = 's';
    line[n++] = '\0';
    (void)n;
    aster_draw_text(s, 148, 284, line, D0_DIM, D0_HEADER);
}

static void d0_draw_all(struct aster_surface *s, u32 count, u32 last_key,
                        u32 last_scan, u32 idle)
{
    aster_surface_clear(s, D0_BG);
    d0_draw_header(s);
    d0_draw_cards(s);
    d0_draw_panel(s, count, last_key, last_scan);
    d0_draw_footer(s, idle);
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                      */
/* ------------------------------------------------------------------------- */

static void d0_fail(const char *what, i32 rc)
{
    d0_put("ng: d0 FAIL (");
    d0_put(what);
    d0_put(" rc=-");
    d0_put_u32((u32)(-rc));
    d0_put(")\r\n");
}

ASTER_APP_MAIN
{
    struct aster_app app;
    struct aster_app_config cfg;
    struct aster_event ev;
    struct aster_session sess;
    const char *reason = "idle";
    u32 count = 0;
    u32 last_key = 0;
    u32 last_scan = 0;
    u32 rounds = 0;
    u32 idle = 0;
    u32 surface_id = 0;
    int have_session = 0;
    i32 rc;

    d0_put("ng: d0 boot ok (sdk=" ASTER_SDK_VERSION " abi=");
    d0_put_hex8(ASTER_ABI_VERSION);
    d0_put(")\r\n");

    rc = aster_session_connect(&sess, argc, argv);
    if (rc == 0) {
        have_session = 1;
        d0_put("ng: d0 session ok (session=");
        d0_put_u32(sess.server_ticket);
        d0_put(" token=");
        d0_put_hex8(sess.token);
        d0_put(")\r\n");
    } else if (rc != ASTER_SESSION_ENOENT) {
        d0_fail("session connect", rc);
        return 1;
    }

    cfg.width = (u32)D0_W;
    cfg.height = (u32)D0_H;
    cfg.title = D0_TITLE;
    cfg.flags = 0; /* 0 = 由 displayd 级联放置 */
    cfg.x = 0;
    cfg.y = 0;
    cfg.token = (have_session != 0) ? sess.token : 0u;

    rc = aster_app_open(&app, &cfg);
    if (rc < 0) {
        d0_fail("app open", rc);
        if (have_session != 0) {
            (void)aster_session_exit(&sess, 1);
            aster_session_close(&sess);
        }
        return 1;
    }

    rc = aster_app_focus(&app);
    if (rc < 0) {
        /* 焦点失败仍把窗口画出来（没有 INPUT 时会走 8s 空闲退出）。 */
        d0_put("ng: d0 focus warn (rc=-");
        d0_put_u32((u32)(-rc));
        d0_put(")\r\n");
    }

    surface_id = app.surface.id;
    d0_draw_all(&app.surface, count, last_key, last_scan, idle);
    rc = aster_app_commit(&app);
    if (rc < 0) {
        d0_fail("commit", rc);
        aster_app_close(&app);
        if (have_session != 0) {
            (void)aster_session_exit(&sess, 1);
            aster_session_close(&sess);
        }
        return 1;
    }

    d0_put("ng: d0 demo up (pid=");
    d0_put_u32(aster_getpid());
    d0_put(" id=");
    d0_put_u32(surface_id);
    d0_put(" x=");
    d0_put_u32(app.surface.x);
    d0_put(" y=");
    d0_put_u32(app.surface.y);
    d0_put(" w=");
    d0_put_u32(app.surface.width);
    d0_put(" h=");
    d0_put_u32(app.surface.height);
    d0_put(" token=");
    d0_put_hex8(cfg.token);
    d0_put(" session=");
    d0_put_u32((have_session != 0) ? 1u : 0u);
    d0_put(")\r\n");

    if (have_session != 0) {
        rc = aster_session_register(&sess, surface_id, D0_TITLE);
        if (rc < 0) {
            d0_put("ng: d0 register warn (rc=-");
            d0_put_u32((u32)(-rc));
            d0_put(")\r\n");
        }
    }
    d0_put("ng: d0 draw ok (cards=3 count=0)\r\n");

    /*
     * 事件循环：20ms 一轮。rc==0 = 本轮没有输入 -> idle++（≈8s 自动退出）；
     * 任意键 -> count++ 重绘；r -> 归零；q/Q/Esc -> 退出。每 10 轮发心跳，
     * 每 25 轮（~0.5s）只重绘/提交页脚的空闲进度条（脏矩形）。
     */
    while (aster_app_running(&app)) {
        rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
        if (rc < 0) {
            reason = "display";
            break;
        }
        rounds++;
        if (rc == 0) {
            idle++;
            if (idle >= D0_IDLE_LIMIT) {
                reason = "idle";
                break;
            }
            if ((rounds % 25u) == 0u) {
                d0_draw_footer(&app.surface, idle);
                (void)aster_display_commit(&app.display, &app.surface, 0,
                                           D0_FOOTER_Y, D0_W,
                                           D0_H - D0_FOOTER_Y);
            }
        } else if (ev.type == ASTER_EVENT_KEY_DOWN &&
                   (ev.surface_id == 0u || ev.surface_id == surface_id)) {
            idle = 0;
            last_key = ev.key;
            last_scan = ev.scancode;

            if (ev.key == 27u || ev.key == (u32)'q' ||
                ev.key == (u32)'Q') {
                reason = "q/esc";
                break;
            }

            if (ev.key == (u32)'r' || ev.key == (u32)'R') {
                count = 0;
                d0_put("ng: d0 reset ok (count=0)\r\n");
            } else {
                count++;
                d0_put("ng: d0 key ok (key=0x");
                d0_put_hex2(ev.key);
                d0_put(" count=");
                d0_put_u32(count);
                d0_put(")\r\n");
            }

            d0_draw_all(&app.surface, count, last_key, last_scan, idle);
            rc = aster_app_commit(&app);
            if (rc < 0) {
                reason = "commit";
                break;
            }
        }

        if (have_session != 0 &&
            (rounds % ASTER_SESSION_HEARTBEAT_ROUNDS) == 0u) {
            (void)aster_session_heartbeat(&sess, rounds);
        }
    }

    d0_put("ng: d0 quit ok (reason=");
    d0_put(reason);
    d0_put(" rounds=");
    d0_put_u32(rounds);
    d0_put(" idle=");
    d0_put_u32(idle);
    d0_put(")\r\n");

    if (have_session != 0) {
        (void)aster_session_exit(&sess, 0);
    }
    aster_app_close(&app);
    if (have_session != 0) {
        aster_session_close(&sess);
    }

    d0_put("ng: d0 demo bye (id=");
    d0_put_u32(surface_id);
    d0_put(" count=");
    d0_put_u32(count);
    d0_put(" rounds=");
    d0_put_u32(rounds);
    d0_put(" reason=");
    d0_put(reason);
    d0_put(")\r\n");
    return 0;
}
