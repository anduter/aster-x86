/*
 * ng WP3-1: ARGB surface 上的软件绘制（L2，纯函数）。
 *
 * demo（L3）用它在共享内存 surface 上画渐变/矩形/文字；displayd 不需要它，
 * 但同一个模块的 host 单测（tests/unit-ng/test_desktop.c）会验证边界裁剪与
 * 字形渲染。绘制一律走 ds_pixel_blend（见 ds_composite.h），且画布始终写成
 * 不透明（A=0xFF，半透明颜色只影响 RGB 混合）——这样合成器的 A=255 快路径
 * 能保证 demo 的实心色像素逐位相等（QMP 截图断言依赖它）。
 */
#ifndef NG_DS_DRAW_H
#define NG_DS_DRAW_H

#include "types.h"

struct ds_buffer {
    u32 *px; /* 0xAARRGGBB */
    u32 w;
    u32 h;
    u32 pitch_px;
};

void ds_buffer_init(struct ds_buffer *b, u32 *px, u32 w, u32 h);

void ds_draw_pixel(struct ds_buffer *b, i64 x, i64 y, u32 argb);
void ds_draw_fill(struct ds_buffer *b, i64 x, i64 y, i64 w, i64 h, u32 argb);
/* 空心矩形（厚度 t 像素，向内画）；自动裁剪。 */
void ds_draw_outline(struct ds_buffer *b, i64 x, i64 y, u32 w, u32 h, u32 t,
                     u32 argb);
/* 横向 R 渐变 + 纵向 G 渐变 + 对角 B 渐变（整块 surface）。 */
void ds_draw_gradient(struct ds_buffer *b);
/* 8x8 字符：fg 像素 + bg 底色（bg 为覆盖色，通常不透明）。 */
void ds_draw_char(struct ds_buffer *b, i64 x, i64 y, char ch, u32 fg, u32 bg);
void ds_draw_text(struct ds_buffer *b, i64 x, i64 y, const char *s, u32 fg,
                  u32 bg);

/* 渐变单像素（host 单测直接验证公式）。 */
u32 ds_gradient_pixel(u32 x, u32 y, u32 w, u32 h);

#endif /* NG_DS_DRAW_H */
