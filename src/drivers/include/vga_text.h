/*
 * ng WP2-3: VGA 文本模式 0xB8000 兜底控制台（L1D）。
 *
 * 用途：stdvga / Bochs VBE 不可用时（-vga none、非 QEMU 显卡），系统仍然要
 * 能显示启动信息，而不是整块显示路径直接失败。80x25、每格 2 字节
 * （低字节 ASCII，高字节属性），默认属性 0x07（浅灰字/黑底）。
 *
 * 纯逻辑 + 调用方给的显存指针，host 单测用一块 80*25*2 的数组验证换行/滚屏。
 */
#ifndef NG_VGA_TEXT_H
#define NG_VGA_TEXT_H

#include "types.h"

#define VGA_TEXT_ADDR 0xB8000u
#define VGA_TEXT_COLS 80u
#define VGA_TEXT_ROWS 25u
#define VGA_TEXT_ATTR 0x07u

struct vga_text {
    volatile u16 *mem;
    u32 cols;
    u32 rows;
    u32 row;
    u32 col;
    u8 attr;
};

void vga_text_init(struct vga_text *t, void *mem);
void vga_text_clear(struct vga_text *t);
void vga_text_putc(struct vga_text *t, u8 ch);
void vga_text_puts(struct vga_text *t, const char *s);

/* 读回一个格子的低字节/属性（自检证据用）；越界返回 0。 */
u8 vga_text_char_at(const struct vga_text *t, u32 row, u32 col);
u8 vga_text_attr_at(const struct vga_text *t, u32 row, u32 col);

#endif /* NG_VGA_TEXT_H */
