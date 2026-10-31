/*
 * ng WP2-3: VGA 文本兜底控制台实现（见 include/vga_text.h）。
 */
#include "vga_text.h"

static int valid(const struct vga_text *t)
{
    return t != NULL && t->mem != NULL && t->cols != 0 && t->rows != 0;
}

void vga_text_init(struct vga_text *t, void *mem)
{
    if (t == NULL) {
        return;
    }
    t->mem = (volatile u16 *)mem;
    t->cols = VGA_TEXT_COLS;
    t->rows = VGA_TEXT_ROWS;
    t->row = 0;
    t->col = 0;
    t->attr = VGA_TEXT_ATTR;
}

void vga_text_clear(struct vga_text *t)
{
    u32 i;
    u16 blank;

    if (!valid(t)) {
        return;
    }
    blank = (u16)(' ' | ((u16)t->attr << 8));
    for (i = 0; i < t->cols * t->rows; i++) {
        t->mem[i] = blank;
    }
    t->row = 0;
    t->col = 0;
}

static void scroll(struct vga_text *t)
{
    u32 i;

    for (i = 0; i + t->cols < t->cols * t->rows; i++) {
        t->mem[i] = t->mem[i + t->cols];
    }
    for (i = t->cols * (t->rows - 1); i < t->cols * t->rows; i++) {
        t->mem[i] = (u16)(' ' | ((u16)t->attr << 8));
    }
    if (t->rows > 0) {
        t->row = t->rows - 1;
    }
}

void vga_text_putc(struct vga_text *t, u8 ch)
{
    u32 idx;

    if (!valid(t)) {
        return;
    }
    if (ch == '\n') {
        t->col = 0;
        t->row++;
    } else if (ch == '\r') {
        t->col = 0;
        return;
    } else {
        if (t->col >= t->cols) {
            t->col = 0;
            t->row++;
        }
        idx = t->row * t->cols + t->col;
        if (idx < t->cols * t->rows) {
            t->mem[idx] = (u16)(ch | ((u16)t->attr << 8));
        }
        t->col++;
        if (t->col >= t->cols) {
            t->col = 0;
            t->row++;
        }
    }
    if (t->row >= t->rows) {
        scroll(t);
    }
}

void vga_text_puts(struct vga_text *t, const char *s)
{
    if (t == NULL || s == NULL) {
        return;
    }
    while (*s != '\0') {
        vga_text_putc(t, (u8)*s);
        s++;
    }
}

u8 vga_text_char_at(const struct vga_text *t, u32 row, u32 col)
{
    u32 idx;

    if (!valid(t) || row >= t->rows || col >= t->cols) {
        return 0;
    }
    idx = row * t->cols + col;
    return (u8)(t->mem[idx] & 0xFFu);
}

u8 vga_text_attr_at(const struct vga_text *t, u32 row, u32 col)
{
    u32 idx;

    if (!valid(t) || row >= t->rows || col >= t->cols) {
        return 0;
    }
    idx = row * t->cols + col;
    return (u8)(t->mem[idx] >> 8);
}
