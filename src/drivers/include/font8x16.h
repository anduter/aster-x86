/*
 * ng WP2-3: L1D 内嵌 8x16 ASCII 字模（0x20..0x7E，95 个字形）。
 *
 * 每个字形 16 字节，每字节一行，bit0 = 最左列（与 fb.c 的绘制约定一致）。
 * 字模由旧树 src/kernel/dev/char/fb.c 的 8x8 基线字库纵向 2 倍展开生成
 * （见 docs/design/display-input-ng.md），ASCII 全覆盖且不依赖 BIOS ROM：
 * 这样即使没有 VBE/BIOS 字库，帧缓冲控制台也能画出可读文本。
 *
 * 纯数据 + 一个只读访问函数，host 单测和内核共用同一份表。
 */
#ifndef NG_FONT8X16_H
#define NG_FONT8X16_H

#include "types.h"

#define NG_FONT_FIRST 0x20u
#define NG_FONT_LAST 0x7Eu
#define NG_FONT_GLYPHS 95u
#define NG_FONT_W 8u
#define NG_FONT_H 16u

extern const u8 ng_font8x16[NG_FONT_GLYPHS][NG_FONT_H];

/*
 * 取字形：ch 不在 0x20..0x7E 时返回 '?' 的字形（不返回 NULL，调用方不用判空）。
 * 返回的指针指向 16 字节，只读。
 */
const u8 *font8x16_glyph(u8 ch);

/* 字形是否“空”（所有行为 0）：空格为 1，其余 ASCII 可见字符为 0。 */
int font8x16_is_blank(u8 ch);

#endif /* NG_FONT8X16_H */
