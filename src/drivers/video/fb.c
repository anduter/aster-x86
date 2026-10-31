/*
 * ng WP2-3: 帧缓冲控制台实现（见 include/fb.h）。
 *
 * 所有绘制都直接走 volatile 指针（MMIO 不能假设 write-back 缓存行为；
 * 页表里用 PCD/PWT 映射，见 src/ng/wp23_selftest.c）。没有依赖 libc，
 * 也不用浮点/SIMD（内核 -mgeneral-regs-only）。
 */
#include "fb.h"

#include "font8x16.h"

static int fb_valid(const struct fb_info *fb)
{
    return fb != NULL && fb->base != NULL && fb->width != 0 && fb->height != 0 &&
           (fb->bpp == 16u || fb->bpp == 24u || fb->bpp == 32u) &&
           fb->pitch >= fb->width * (fb->bpp / 8u);
}

u32 fb_pack(u32 rgb, u32 bpp)
{
    u32 r = (rgb >> 16) & 0xFFu;
    u32 g = (rgb >> 8) & 0xFFu;
    u32 b = rgb & 0xFFu;

    if (bpp == 32u) {
        return rgb & 0x00FFFFFFu;
    }
    if (bpp == 24u) {
        return rgb & 0x00FFFFFFu;
    }
    if (bpp == 16u) {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    return 0;
}

int fb_init(struct fb_info *fb, void *base, u32 width, u32 height, u32 pitch,
            u32 bpp)
{
    if (fb == NULL || base == NULL || width == 0 || height == 0) {
        return FB_EINVAL;
    }
    if (bpp != 16u && bpp != 24u && bpp != 32u) {
        return FB_EINVAL;
    }
    if (pitch < width * (bpp / 8u)) {
        return FB_EINVAL;
    }
    fb->base = (volatile u8 *)base;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = bpp;
    fb->cursor_x = 0;
    fb->cursor_y = 0;
    fb->fg = FB_WHITE;
    fb->bg = FB_BLACK;
    return FB_OK;
}

void fb_pixel(struct fb_info *fb, u32 x, u32 y, u32 rgb)
{
    u8 *p;

    if (!fb_valid(fb) || x >= fb->width || y >= fb->height) {
        return;
    }
    p = (u8 *)fb->base + (u64)y * fb->pitch + (u64)x * (fb->bpp / 8u);
    if (fb->bpp == 32u) {
        *(volatile u32 *)p = fb_pack(rgb, 32);
    } else if (fb->bpp == 24u) {
        p[0] = (u8)(rgb & 0xFFu);
        p[1] = (u8)((rgb >> 8) & 0xFFu);
        p[2] = (u8)((rgb >> 16) & 0xFFu);
    } else {
        *(volatile u16 *)p = (u16)fb_pack(rgb, 16);
    }
}

u32 fb_read_pixel(const struct fb_info *fb, u32 x, u32 y)
{
    const u8 *p;
    u32 v;

    if (!fb_valid(fb) || x >= fb->width || y >= fb->height) {
        return 0;
    }
    p = (const u8 *)fb->base + (u64)y * fb->pitch + (u64)x * (fb->bpp / 8u);
    if (fb->bpp == 32u) {
        return *(const volatile u32 *)p & 0x00FFFFFFu;
    }
    if (fb->bpp == 24u) {
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
    }
    v = *(const volatile u16 *)p;
    return (((v >> 11) & 0x1Fu) << 19) | (((v >> 5) & 0x3Fu) << 10) |
           ((v & 0x1Fu) << 3);
}

void fb_fill(struct fb_info *fb, u32 rgb)
{
    if (!fb_valid(fb)) {
        return;
    }
    fb_rect(fb, 0, 0, fb->width, fb->height, rgb);
}

void fb_rect(struct fb_info *fb, u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    u32 x1;
    u32 y1;
    u32 row;
    u32 col;
    u32 packed;

    if (!fb_valid(fb) || w == 0 || h == 0 || x >= fb->width || y >= fb->height) {
        return;
    }
    x1 = (x + w > fb->width) ? fb->width : x + w;
    y1 = (y + h > fb->height) ? fb->height : y + h;
    packed = fb_pack(rgb, fb->bpp);

    for (row = y; row < y1; row++) {
        u8 *p = (u8 *)fb->base + (u64)row * fb->pitch + (u64)x * (fb->bpp / 8u);

        if (fb->bpp == 32u) {
            volatile u32 *q = (volatile u32 *)p;

            for (col = x; col < x1; col++) {
                *q++ = packed;
            }
        } else if (fb->bpp == 24u) {
            for (col = x; col < x1; col++) {
                p[0] = (u8)(rgb & 0xFFu);
                p[1] = (u8)((rgb >> 8) & 0xFFu);
                p[2] = (u8)((rgb >> 16) & 0xFFu);
                p += 3;
            }
        } else {
            volatile u16 *q = (volatile u16 *)p;

            for (col = x; col < x1; col++) {
                *q++ = (u16)packed;
            }
        }
    }
}

void fb_glyph(struct fb_info *fb, u32 x, u32 y, u8 ch, u32 fg, u32 bg)
{
    const u8 *glyph;
    u32 gx;
    u32 gy;

    if (!fb_valid(fb)) {
        return;
    }
    glyph = font8x16_glyph(ch);
    for (gy = 0; gy < NG_FONT_H; gy++) {
        u8 bits = glyph[gy];

        for (gx = 0; gx < NG_FONT_W; gx++) {
            fb_pixel(fb, x + gx, y + gy,
                     (bits & (u8)(1u << gx)) ? fg : bg);
        }
    }
}

void fb_checkerboard(struct fb_info *fb, u32 x, u32 y, u32 w, u32 h, u32 cell,
                     u32 c1, u32 c2)
{
    u32 cy;
    u32 cx;

    if (!fb_valid(fb) || cell == 0) {
        return;
    }
    for (cy = 0; cy * cell < h; cy++) {
        for (cx = 0; cx * cell < w; cx++) {
            u32 cw = cell;
            u32 ch = cell;

            if ((cx + 1u) * cell > w) {
                cw = w - cx * cell;
            }
            if ((cy + 1u) * cell > h) {
                ch = h - cy * cell;
            }
            fb_rect(fb, x + cx * cell, y + cy * cell, cw, ch,
                    ((cx + cy) & 1u) ? c2 : c1);
        }
    }
}

void fb_set_colors(struct fb_info *fb, u32 fg, u32 bg)
{
    if (fb == NULL) {
        return;
    }
    fb->fg = fg;
    fb->bg = bg;
}

/* 向上滚 16 像素：逐字节前向拷贝（目标 < 源，重叠安全），再清底部一行。 */
static void fb_scroll(struct fb_info *fb)
{
    u32 y;
    u32 x;
    u32 drop = NG_FONT_H;

    if (fb->height <= drop) {
        fb_fill(fb, fb->bg);
        return;
    }
    for (y = 0; y + drop < fb->height; y++) {
        volatile u8 *dst = fb->base + (u64)y * fb->pitch;
        const volatile u8 *src = fb->base + (u64)(y + drop) * fb->pitch;

        for (x = 0; x < fb->pitch; x++) {
            dst[x] = src[x];
        }
    }
    fb_rect(fb, 0, fb->height - drop, fb->width, drop, fb->bg);
}

void fb_newline(struct fb_info *fb)
{
    if (fb == NULL) {
        return;
    }
    fb->cursor_x = 0;
    fb->cursor_y += NG_FONT_H;
    if (fb->cursor_y + NG_FONT_H > fb->height) {
        fb_scroll(fb);
        fb->cursor_y = (fb->height >= NG_FONT_H) ? fb->height - NG_FONT_H : 0;
    }
}

void fb_putc(struct fb_info *fb, u8 ch)
{
    if (fb == NULL) {
        return;
    }
    if (ch == '\n') {
        fb_newline(fb);
        return;
    }
    if (ch == '\r') {
        fb->cursor_x = 0;
        return;
    }
    if (fb->cursor_x + NG_FONT_W > fb->width) {
        fb_newline(fb);
    }
    if (fb->cursor_y + NG_FONT_H > fb->height) {
        fb_newline(fb);
    }
    fb_glyph(fb, fb->cursor_x, fb->cursor_y, ch, fb->fg, fb->bg);
    fb->cursor_x += NG_FONT_W;
}

void fb_print(struct fb_info *fb, const char *s)
{
    if (fb == NULL || s == NULL) {
        return;
    }
    while (*s != '\0') {
        fb_putc(fb, (u8)*s);
        s++;
    }
}

void fb_grid(const struct fb_info *fb, u32 *cols, u32 *rows)
{
    if (cols != NULL) {
        *cols = (fb != NULL) ? fb->width / NG_FONT_W : 0;
    }
    if (rows != NULL) {
        *rows = (fb != NULL) ? fb->height / NG_FONT_H : 0;
    }
}

void fb_cursor(const struct fb_info *fb, u32 *x, u32 *y)
{
    if (x != NULL) {
        *x = (fb != NULL) ? fb->cursor_x : 0;
    }
    if (y != NULL) {
        *y = (fb != NULL) ? fb->cursor_y : 0;
    }
}
