/*
 * ng WP3-2: ARGB8888 整数 source-over 混合（从 ds_composite.c 拆出的小对象）。
 *
 * 为什么单独一个文件：demo/wm 只用到 `ds_pixel_blend`（ds_draw 的每个像素），
 * 却不需要整个合成器。拆开之后用户 ELF 不用链 ds_composite.o —— 内核 .rodata
 * 里嵌了 6 个用户 ELF，2 MiB 映像窗口很紧，这一刀省掉约 6 KiB/程序。
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
