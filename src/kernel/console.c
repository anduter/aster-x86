/* =============================================================================
 * console.c - framebuffer 文字控制台（输出区 + 输入区）
 *
 * 布局：
 *   标题栏
 *   ┌──────────── 输出框（可滚动）────────────┐
 *   │ 日志/命令输出 ...                        │
 *   └──────────────────────────────────────────┘
 *   ┌──────────── 输入框（单行）──────────────┐
 *   │ shell> 正在输入的内容                    │
 *   └──────────────────────────────────────────┘
 *   状态栏（左：uptime，右：心跳小部件）
 *
 * 所有输出同时镜像到 COM1；输入区只在屏幕上画，不进串口（避免和日志混在一起）。
 * ============================================================================= */
#include "console.h"
#include "fb.h"
#include "io.h"
#include "lib/string.h"
#include "core/spinlock.h"

/* 字体缩放：1 = 标准 8x12 单元，2 = 放大一倍（更清晰但字符更大） */
#ifndef FONT_SCALE
#define FONT_SCALE 1U
#endif

#define CELL_W      (8U * FONT_SCALE)
#define CELL_H      (12U * FONT_SCALE)   /* 字模 8 行高，留 4 行行距 */
#define TITLE_H     (26U * FONT_SCALE)   /* 标题栏高度随字体缩放 */
#define STATUS_H    (26U * FONT_SCALE)   /* 状态栏高度 */
#define BORDER      10U
#define TAB_WIDTH   4U
#define INPUT_H     (16U * FONT_SCALE)   /* 输入框高度 */
#define BOX_GAP     6U    /* 输出框与输入框间距 */

/* 配色（0x00RRGGBB） */
#define COL_BG        0x102030U
#define COL_FG        0xE0E0E0U
#define COL_TITLE_BG  0x0040FFU
#define COL_TITLE_FG  0xFFFFFFU
#define COL_STATUS_BG 0x808080U
#define COL_STATUS_FG 0x102030U
#define COL_STATUS_RIGHT 0x004000U
#define COL_BORDER    0xFFFFFFU
#define COL_BOX       0x5A7A9AU   /* 内部两个框的边框色 */

/* 状态栏左右两段文字分开保存：任何一侧更新时整栏重画 */
#define STATUS_TEXT_MAX 64U
static char status_left[STATUS_TEXT_MAX] = "booting ...";
static char status_right[STATUS_TEXT_MAX];

/* 整个内容区、输出文字区、输入文字区 */
static uint32_t con_x0, con_y0, con_x1, con_y1;
static uint32_t out_x0, out_y0, out_x1, out_y1;
static uint32_t in_x0, in_y0, in_x1;

static uint32_t cols, rows;
static uint32_t cur_col, cur_row;
static uint32_t cur_fg = COL_FG;
static uint32_t cur_bg = COL_BG;
static bool     mirror_to_serial = true;

static uint8_t *fb_bytes(void)
{
    return (uint8_t *)(uintptr_t)fb_addr;
}

/* 画一个 2px 粗的空心框 */
static void draw_box(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color)
{
    fb_fill_rect(x0, y0, x1, y0 + 2U, color);
    fb_fill_rect(x0, y1 - 2U, x1, y1, color);
    fb_fill_rect(x0, y0, x0 + 2U, y1, color);
    fb_fill_rect(x1 - 2U, y0, x1, y1, color);
}

/* 把输出文字区整体上移一行，并清空最后一行 */
static void scroll_up(void)
{
    uint32_t bytes_pp = (uint32_t)fb_bpp / 8U;
    uint32_t row_bytes = (out_x1 - out_x0) * bytes_pp;
    uint32_t left_bytes = out_x0 * bytes_pp;
    uint8_t *fb = fb_bytes();
    uint32_t y;

    for (y = out_y0; y + CELL_H < out_y1; y++) {
        memmove(fb + y * fb_pitch + left_bytes,
                fb + (y + CELL_H) * fb_pitch + left_bytes,
                row_bytes);
    }
    fb_fill_rect(out_x0, out_y1 - CELL_H, out_x1, out_y1, cur_bg);
}

static void newline(void)
{
    cur_col = 0;
    if (cur_row + 1U >= rows) {
        scroll_up();
    } else {
        cur_row++;
    }
}

static void erase_cell(uint32_t col, uint32_t row)
{
    fb_fill_rect(out_x0 + col * CELL_W, out_y0 + row * CELL_H,
                 out_x0 + (col + 1U) * CELL_W, out_y0 + (row + 1U) * CELL_H,
                 cur_bg);
}

/* 按当前字体缩放画一行文本（标题栏/输入框/状态栏共用） */
static void text_at(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg)
{
    while (s != NULL && *s != '\0') {
        fb_char_scaled(x, y, *s, fg, bg, FONT_SCALE);
        x += CELL_W;
        s++;
    }
}

/* 行内垂直居中：字符高 8*S，容器高 h */
static uint32_t center_y(uint32_t top, uint32_t h)
{
    uint32_t glyph = 8U * FONT_SCALE;

    return (h > glyph) ? (top + (h - glyph) / 2U) : top;
}

void console_init(void)
{
    uint32_t content_bottom = (uint32_t)fb_h - STATUS_H - BORDER - 10U;
    uint32_t input_top;

    /* 背景 */
    fb_fill_rect(0U, 0U, fb_w, fb_h, COL_BG);

    /* 标题栏 / 状态栏 */
    fb_fill_rect(0U, 0U, fb_w, TITLE_H, COL_TITLE_BG);
    fb_fill_rect(0U, (uint32_t)fb_h - STATUS_H, fb_w, fb_h, COL_STATUS_BG);

    /* 整个内容区 */
    con_x0 = BORDER + 10U;
    con_y0 = TITLE_H + BORDER + 10U;
    con_x1 = (uint32_t)fb_w - BORDER - 10U;
    con_y1 = content_bottom;

    /* 输入框贴底，输出框占其余部分 */
    input_top = con_y1 - INPUT_H;
    draw_box(con_x0, con_y0, con_x1, input_top - BOX_GAP, COL_BOX);
    draw_box(con_x0, input_top, con_x1, con_y1, COL_BOX);

    /* 两个框内部的文字区（留 10px 内边距） */
    out_x0 = con_x0 + 10U;
    out_y0 = con_y0 + 10U;
    out_x1 = con_x1 - 10U;
    out_y1 = input_top - BOX_GAP - 10U;

    in_x0 = con_x0 + 10U;
    in_y0 = input_top + ((INPUT_H - 8U) / 2U);
    in_x1 = con_x1 - 10U;

    cols = (out_x1 - out_x0) / CELL_W;
    rows = (out_y1 - out_y0) / CELL_H;

    cur_col = 0;
    cur_row = 0;
    cur_fg  = COL_FG;
    cur_bg  = COL_BG;

    fb_fill_rect(out_x0, out_y0, out_x1, out_y1, cur_bg);
    fb_fill_rect(in_x0, input_top + 4U, in_x1, con_y1 - 4U, cur_bg);
}

void console_clear(void)
{
    fb_fill_rect(out_x0, out_y0, out_x1, out_y1, cur_bg);
    fb_fill_rect(in_x0, con_y1 - INPUT_H + 4U, in_x1, con_y1 - 4U, cur_bg);
    cur_col = 0;
    cur_row = 0;
}

void console_set_color(uint32_t fg, uint32_t bg)
{
    cur_fg = fg;
    cur_bg = bg;
}

void console_set_mirror(bool enabled)
{
    mirror_to_serial = enabled;
}

/* 控制台输出锁：防止多 CPU/中断上下文同时画字符导致花屏。
 * 注意 console_putc 处理 '\t' 时会递归，所以锁只加在外层包装上。 */
static spinlock_t console_lock = SPINLOCK_INIT;

static void putc_locked(char c)
{
    if (mirror_to_serial) {
        if (c == '\n') {
            serial_putc('\r');
            serial_putc('\n');
        } else if (c != '\r') {
            serial_putc(c);
        }
    }

    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cur_col = 0;
        return;
    case '\t':
        do {
            putc_locked(' ');
        } while (cur_col % TAB_WIDTH != 0U && cur_col != 0U);
        return;
    case '\b':
        if (cur_col > 0U) {
            cur_col--;
            erase_cell(cur_col, cur_row);
        }
        return;
    default:
        break;
    }

    if (cur_col >= cols) {
        newline();
    }
    fb_char_scaled(out_x0 + cur_col * CELL_W, out_y0 + cur_row * CELL_H,
                   c, cur_fg, cur_bg, FONT_SCALE);
    cur_col++;
}

/* 带锁的对外接口 */
void console_putc(char c)
{
    uint32_t flags = spin_lock_irqsave(&console_lock);

    putc_locked(c);
    spin_unlock_irqrestore(&console_lock, flags);
}

static void console_write_raw(const char *s)
{
    while (s != NULL && *s != '\0') {
        console_putc(*s++);
    }
}

void console_write(const char *s)
{
    console_write_raw(s);
}

void console_write_atomic(const char *s)
{
    uint32_t flags = spin_lock_irqsave(&console_lock);

    while (s != NULL && *s != '\0') {
        putc_locked(*s++);
    }
    spin_unlock_irqrestore(&console_lock, flags);
}

/* 输入区：清空整行后画一行文本（不换行、不滚动、不进串口） */
void console_input_set(const char *text)
{
    char buf[160];
    uint32_t limit = (in_x1 - in_x0) / CELL_W;
    uint32_t n = 0;

    if (text == NULL) text = "";
    if (limit > sizeof(buf) - 1U) limit = (uint32_t)sizeof(buf) - 1U;

    while (text[n] != '\0' && n < limit) {
        buf[n] = text[n];
        n++;
    }
    buf[n] = '\0';

    fb_fill_rect(in_x0, con_y1 - INPUT_H + 4U, in_x1, con_y1 - 4U, COL_BG);
    text_at(in_x0, in_y0, buf, COL_FG, COL_BG);
}

void console_set_title(const char *text)
{
    fb_fill_rect(0U, 0U, fb_w, TITLE_H, COL_TITLE_BG);
    text_at(12U, center_y(0U, TITLE_H), text, COL_TITLE_FG, COL_TITLE_BG);
}

/* 重画整条状态栏：左侧左对齐、右侧右对齐 */
static void redraw_status(void)
{
    uint32_t y = (uint32_t)fb_h - STATUS_H;
    uint32_t text_y = center_y(y, STATUS_H);
    uint32_t len = (uint32_t)strlen(status_right);

    fb_fill_rect(0U, y, fb_w, fb_h, COL_STATUS_BG);
    text_at(12U, text_y, status_left, COL_STATUS_FG, COL_STATUS_BG);

    if (len > 0U) {
        uint32_t w = len * CELL_W;
        uint32_t x = ((uint32_t)fb_w > (w + 12U)) ? ((uint32_t)fb_w - 12U - w) : 0U;
        text_at(x, text_y, status_right, COL_STATUS_RIGHT, COL_STATUS_BG);
    }
}

void console_set_status(const char *text)
{
    if (text == NULL) text = "";
    strncpy(status_left, text, STATUS_TEXT_MAX - 1U);
    status_left[STATUS_TEXT_MAX - 1U] = '\0';
    redraw_status();
}

void console_set_status_right(const char *text)
{
    if (text == NULL) text = "";
    strncpy(status_right, text, STATUS_TEXT_MAX - 1U);
    status_right[STATUS_TEXT_MAX - 1U] = '\0';
    redraw_status();
}

uint32_t console_cols(void)
{
    return cols;
}

uint32_t console_rows(void)
{
    return rows;
}
