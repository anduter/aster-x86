/*
 * ng WP3-1: L2 内嵌 8x8 ASCII 字模（0x20..0x7E，95 个字形）。
 *
 * 用途：displayd/demo 在 ARGB surface 上画文字（软件绘制，不依赖 L1D 的
 * 8x16 帧缓冲字模，也不依赖 BIOS ROM）。数据由 src/drivers/video/font8x16.c
 * 的 8x16 字模按行对 OR 降采样生成（8x16 本来就是 8x8 纵向 2 倍展开），
 * 所以两套字模的笔画一致。
 *
 * 每字形 8 字节，每字节一行；bit0 = 最左列（与 font8x16 的约定一致）。
 */
#ifndef NG_DS_FONT8X8_H
#define NG_DS_FONT8X8_H

#include "types.h"

#define DS_FONT_FIRST 0x20u
#define DS_FONT_LAST 0x7Eu
#define DS_FONT_GLYPHS 95u
#define DS_FONT_W 8u
#define DS_FONT_H 8u

extern const u8 ds_font8x8[DS_FONT_GLYPHS][DS_FONT_H];

/* 取字形；越界返回 '?' 的字形（不返回 NULL）。 */
const u8 *ds_font_glyph(u8 ch);
/* 空格/全零字形返回 1。 */
int ds_font_is_blank(u8 ch);

#endif /* NG_DS_FONT8X8_H */
