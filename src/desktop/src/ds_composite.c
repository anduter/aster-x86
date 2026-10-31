/*
 * ng WP3-1: 软件合成器实现（见 ds_composite.h）。
 */
#include "ds_composite.h"

u32 ds_pixel_blend(u32 dst, u32 src)
{
    u32 a = (src >> 24) & 0xFFu;
    u32 sr;
    u32 sg;
    u32 sb;
    u32 r;
    u32 g;
    u32 b;

    if (a == 0) {
        return dst;
    }
    if (a == 0xFFu) {
        return src & 0x00FFFFFFu;
    }
    sr = (src >> 16) & 0xFFu;
    sg = (src >> 8) & 0xFFu;
    sb = src & 0xFFu;
    r = ((sr * a) + (((dst >> 16) & 0xFFu) * (0xFFu - a))) / 0xFFu;
    g = ((sg * a) + (((dst >> 8) & 0xFFu) * (0xFFu - a))) / 0xFFu;
    b = ((sb * a) + ((dst & 0xFFu) * (0xFFu - a))) / 0xFFu;
    return (r << 16) | (g << 8) | b;
}

void ds_screen_pixel(struct ds_screen *s, i64 x, i64 y, u32 argb)
{
    u32 ux;
    u32 uy;

    if (s == NULL || s->px == NULL || s->w == 0 || s->h == 0) {
        return;
    }
    if (x < 0 || y < 0) {
        return;
    }
    ux = (u32)x;
    uy = (u32)y;
    if (ux >= s->w || uy >= s->h) {
        return;
    }
    s->px[(u64)uy * (u64)s->pitch_px + (u64)ux] = argb & 0x00FFFFFFu;
}

u32 ds_screen_read(const struct ds_screen *s, i64 x, i64 y)
{
    if (s == NULL || s->px == NULL || x < 0 || y < 0) {
        return 0;
    }
    if ((u32)x >= s->w || (u32)y >= s->h) {
        return 0;
    }
    return s->px[(u64)(u32)y * (u64)s->pitch_px + (u64)(u32)x] & 0x00FFFFFFu;
}

/* ------------------------------------------------------------------------- */
/* surface -> screen                                                          */
/* ------------------------------------------------------------------------- */

u32 ds_surface_blit(struct ds_screen *s, const struct ds_surface *sf,
                    const struct ds_rect *clip)
{
    const u32 *src;
    struct ds_rect sr;
    struct ds_rect visible;
    u32 y;
    u32 pixels = 0;

    if (s == NULL || sf == NULL || s->px == NULL || sf->pixels == 0) {
        return 0;
    }
    if (sf->w == 0 || sf->h == 0) {
        return 0;
    }
    src = (const u32 *)(usize)sf->pixels;
    sr.x = 0;
    sr.y = 0;
    sr.w = sf->w;
    sr.h = sf->h;
    if (clip != NULL) {
        if (ds_rect_intersect(&sr, clip, &sr) == 0) {
            return 0;
        }
    }
    /* 映射到屏幕坐标。 */
    sr.x = sf->x + sr.x;
    sr.y = sf->y + sr.y;
    {
        struct ds_rect screen;
        struct ds_rect hit;

        screen.x = 0;
        screen.y = 0;
        screen.w = s->w;
        screen.h = s->h;
        if (ds_rect_intersect(&sr, &screen, &hit) == 0) {
            return 0;
        }
        visible = hit;
    }
    for (y = visible.y; y < visible.y + visible.h; y++) {
        u32 sy = y - sf->y;
        u32 x;
        u64 row = (u64)sy * (u64)sf->w;

        for (x = visible.x; x < visible.x + visible.w; x++) {
            u32 sx = x - sf->x;
            u32 dst = s->px[(u64)y * (u64)s->pitch_px + (u64)x];

            s->px[(u64)y * (u64)s->pitch_px + (u64)x] =
                ds_pixel_blend(dst, src[row + (u64)sx]);
            pixels++;
        }
    }
    return pixels;
}

u32 ds_composite_region(struct ds_screen *s, struct ds_table *t,
                        const struct ds_rect *region,
                        struct ds_composite_stats *stats)
{
    u32 i;
    u32 total = 0;
    u32 surfaces = 0;
    u32 clipped = 0;

    if (stats != NULL) {
        stats->surfaces = 0;
        stats->pixels = 0;
        stats->clipped = 0;
    }
    if (s == NULL || t == NULL || s->px == NULL) {
        return 0;
    }
    for (i = 0; i < t->count; i++) {
        struct ds_surface *sf = &t->slot[t->order[i]];
        struct ds_rect local;
        u32 n;

        if (!sf->used || !sf->mapped || sf->pixels == 0) {
            continue;
        }
        if (region == NULL) {
            local.x = 0;
            local.y = 0;
            local.w = sf->w;
            local.h = sf->h;
        } else {
            struct ds_rect screen_rect;
            struct ds_rect hit;

            /* region 是屏幕坐标；surface 的裁剪矩形是 surface 局部坐标。 */
            screen_rect.x = sf->x;
            screen_rect.y = sf->y;
            screen_rect.w = sf->w;
            screen_rect.h = sf->h;
            if (ds_rect_intersect(&screen_rect, region, &hit) == 0) {
                continue;
            }
            local.x = hit.x - sf->x;
            local.y = hit.y - sf->y;
            local.w = hit.w;
            local.h = hit.h;
        }
        if (local.w != sf->w || local.h != sf->h) {
            clipped++;
        }
        n = ds_surface_blit(s, sf, &local);
        total += n;
        surfaces++;
    }
    if (stats != NULL) {
        stats->surfaces = surfaces;
        stats->pixels = total;
        stats->clipped = clipped;
    }
    return total;
}

u32 ds_composite_all(struct ds_screen *s, struct ds_table *t,
                     struct ds_composite_stats *stats)
{
    return ds_composite_region(s, t, NULL, stats);
}

/* ------------------------------------------------------------------------- */
/* 基础光标                                                                   */
/* ------------------------------------------------------------------------- */

void ds_cursor_draw(struct ds_screen *s, u32 x, u32 y)
{
    u32 row;
    u32 col;

    if (s == NULL || s->px == NULL) {
        return;
    }
    for (row = 0; row < DS_CURSOR_H; row++) {
        for (col = 0; col < DS_CURSOR_W; col++) {
            u32 color;
            int border = (row == 0 || col == 0 || row == DS_CURSOR_H - 1u ||
                          col == DS_CURSOR_W - 1u);

            if (border) {
                color = DS_CURSOR_FG;
            } else if (col == 1 && row <= DS_CURSOR_H - 3u) {
                /* 一条竖直高亮，让块光标不像纯黑方块。 */
                color = DS_CURSOR_FG;
            } else {
                color = DS_CURSOR_BG;
            }
            ds_screen_pixel(s, (i64)x + (i64)col, (i64)y + (i64)row, color);
        }
    }
}
