/*
 * ng WP3-1: surface 绘制实现（见 ds_draw.h）。
 */
#include "ds_draw.h"

#include "ds_composite.h"
#include "ds_font8x8.h"

void ds_buffer_init(struct ds_buffer *b, u32 *px, u32 w, u32 h)
{
    if (b == NULL) {
        return;
    }
    b->px = px;
    b->w = w;
    b->h = h;
    b->pitch_px = w;
}

void ds_draw_pixel(struct ds_buffer *b, i64 x, i64 y, u32 argb)
{
    u64 idx;

    if (b == NULL || b->px == NULL || x < 0 || y < 0) {
        return;
    }
    if ((u32)x >= b->w || (u32)y >= b->h) {
        return;
    }
    idx = (u64)(u32)y * (u64)b->pitch_px + (u64)(u32)x;
    /* surface 画布始终不透明（A=255）：半透明颜色只参与 RGB 混合。 */
    b->px[idx] = 0xFF000000u | ds_pixel_blend(b->px[idx], argb);
}

void ds_draw_fill(struct ds_buffer *b, i64 x, i64 y, i64 w, i64 h, u32 argb)
{
    i64 dy;
    i64 dx;

    if (b == NULL || b->px == NULL || w <= 0 || h <= 0) {
        return;
    }
    for (dy = 0; dy < h; dy++) {
        for (dx = 0; dx < w; dx++) {
            ds_draw_pixel(b, x + dx, y + dy, argb);
        }
    }
}

void ds_draw_outline(struct ds_buffer *b, i64 x, i64 y, u32 w, u32 h, u32 t,
                     u32 argb)
{
    u32 k;

    if (b == NULL || b->px == NULL || w == 0 || h == 0 || t == 0) {
        return;
    }
    for (k = 0; k < t; k++) {
        ds_draw_fill(b, x + (i64)k, y + (i64)k,
                     (i64)w - 2 * (i64)k, 1, argb);
        ds_draw_fill(b, x + (i64)k, y + (i64)h - 1 - (i64)k,
                     (i64)w - 2 * (i64)k, 1, argb);
        ds_draw_fill(b, x + (i64)k, y + (i64)k, 1,
                     (i64)h - 2 * (i64)k, argb);
        ds_draw_fill(b, x + (i64)w - 1 - (i64)k, y + (i64)k, 1,
                     (i64)h - 2 * (i64)k, argb);
    }
}

u32 ds_gradient_pixel(u32 x, u32 y, u32 w, u32 h)
{
    u32 r;
    u32 g;
    u32 b;

    if (w == 0 || h == 0) {
        return 0;
    }
    r = (x * 0xFFu) / w;
    g = (y * 0xFFu) / h;
    b = 0x40u + ((x + y) * 0x40u) / (w + h);
    if (r > 0xFFu) {
        r = 0xFFu;
    }
    if (g > 0xFFu) {
        g = 0xFFu;
    }
    if (b > 0xFFu) {
        b = 0xFFu;
    }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void ds_draw_gradient(struct ds_buffer *b)
{
    u32 y;

    if (b == NULL || b->px == NULL) {
        return;
    }
    for (y = 0; y < b->h; y++) {
        u32 x;

        for (x = 0; x < b->w; x++) {
            /* 画布不透明：保留 A=0xFF，合成时走直接拷贝快路径。 */
            b->px[(u64)y * (u64)b->pitch_px + (u64)x] =
                ds_gradient_pixel(x, y, b->w, b->h);
        }
    }
}

void ds_draw_char(struct ds_buffer *b, i64 x, i64 y, char ch, u32 fg, u32 bg)
{
    const u8 *glyph = ds_font_glyph((u8)(ch & 0x7F));
    u32 row;
    u32 col;

    for (row = 0; row < DS_FONT_H; row++) {
        u8 bits = glyph[row];

        for (col = 0; col < DS_FONT_W; col++) {
            u32 color = ((bits >> col) & 1u) ? fg : bg;

            ds_draw_pixel(b, x + (i64)col, y + (i64)row, color);
        }
    }
}

void ds_draw_text(struct ds_buffer *b, i64 x, i64 y, const char *s, u32 fg,
                  u32 bg)
{
    i64 cx = x;
    int i = 0;

    if (s == NULL) {
        return;
    }
    while (s[i] != '\0') {
        ds_draw_char(b, cx, y, s[i], fg, bg);
        cx += (i64)DS_FONT_W;
        i++;
    }
}
