/*
 * ng WP2-3: L1D 帧缓冲控制台（32/24/16 bpp 线性帧缓冲 + 内嵌 8x16 字模）。
 *
 * 分工：
 *   - vbe.c 只负责把设备切到 1280x800x32 这类模式；
 *   - 本文件只认“一块线性像素内存 + 宽高/pitch/bpp”，所以 host 单测可以
 *     用一块普通数组验证绘制/读回，不需要 QEMU；
 *   - 内核把 BAR0 的物理地址（恒等映射后）直接传进来。
 *
 * 颜色格式统一用 0x00RRGGBB；写入时按 bpp 打包：
 *   32bpp -> 0x00RRGGBB（QEMU stdvga 是 BGRX8888，小端即低字节 B）
 *   24bpp -> 字节序 B,G,R
 *   16bpp -> RGB565
 * 光标是像素坐标；一个字符单元 8x16，'\n' 换行并滚屏。
 */
#ifndef NG_FB_H
#define NG_FB_H

#include "types.h"

#define FB_CELL_W 8u
#define FB_CELL_H 16u

#define FB_OK 0
#define FB_EINVAL (-1)

#define FB_BLACK 0x00000000u
#define FB_WHITE 0x00FFFFFFu
#define FB_RED 0x00FF0000u
#define FB_GREEN 0x0000FF00u
#define FB_BLUE 0x000000FFu
#define FB_GRAY 0x00808080u
#define FB_DARK 0x00101820u

struct fb_info {
    volatile u8 *base; /* 线性帧缓冲起点（内核恒等映射后的虚拟地址） */
    u32 width;         /* 可见宽（像素） */
    u32 height;        /* 可见高（像素） */
    u32 pitch;         /* 每行字节数 */
    u32 bpp;           /* 16/24/32 */
    u32 cursor_x;      /* 文本光标（像素，左上角） */
    u32 cursor_y;
    u32 fg;            /* fb_putc/fb_print 的前景色 */
    u32 bg;            /* 背景色 */
};

/* 初始化；bpp 只支持 16/24/32，pitch 必须 >= width*bpp/8。 */
int fb_init(struct fb_info *fb, void *base, u32 width, u32 height, u32 pitch,
            u32 bpp);

/* 纯函数：0x00RRGGBB -> 设备像素值（32bpp 时等于 RGB，16bpp 时 RGB565）。 */
u32 fb_pack(u32 rgb, u32 bpp);

/* 单个像素；越界忽略。 */
void fb_pixel(struct fb_info *fb, u32 x, u32 y, u32 rgb);

/* 读回一个像素，返回 0x00RRGGBB；越界/非法参数返回 0。 */
u32 fb_read_pixel(const struct fb_info *fb, u32 x, u32 y);

/* 整屏/矩形填充（矩形自动裁剪到屏幕内）。 */
void fb_fill(struct fb_info *fb, u32 rgb);
void fb_rect(struct fb_info *fb, u32 x, u32 y, u32 w, u32 h, u32 rgb);

/* 画一个 8x16 字符（fg 像素 + bg 底色），坐标是左上角。 */
void fb_glyph(struct fb_info *fb, u32 x, u32 y, u8 ch, u32 fg, u32 bg);

/* 棋盘格：cell 像素见方，两种颜色交替；用于启动自检的画图/读回验证。 */
void fb_checkerboard(struct fb_info *fb, u32 x, u32 y, u32 w, u32 h, u32 cell,
                     u32 c1, u32 c2);

/* 文本控制台：putc/print 自动换行、滚屏；set_colors 改默认前景/背景。 */
void fb_set_colors(struct fb_info *fb, u32 fg, u32 bg);
void fb_newline(struct fb_info *fb);
void fb_putc(struct fb_info *fb, u8 ch);
void fb_print(struct fb_info *fb, const char *s);

/* 文本网格大小（列/行）与光标位置（证据/调试用）。 */
void fb_grid(const struct fb_info *fb, u32 *cols, u32 *rows);
void fb_cursor(const struct fb_info *fb, u32 *x, u32 *y);

#endif /* NG_FB_H */
