/*
 * ng WP3-1: 软件合成器（L2a，纯函数）。
 *
 * 输入：`struct ds_screen`（一块 ARGB/XRGB 像素内存 + 宽高/pitch，displayd 传
 * 映射后的 LFB，host 单测传普通数组）+ surface 表；输出：按 z-order 把
 * surface 可见区域 source-over 混合到屏幕，以及一块"基础光标"。
 *
 * 约定：
 *   - 屏幕/surface 像素都是 u32 = 0xAARRGGBB；A=0 全透明、A=255 不透明。
 *   - 混合公式是整数 source-over（/255 截断），A=255 走直接拷贝的快路径，
 *     保证 demo 的实心色像素读回**逐位相等**（QMP screendump 断言依赖它）。
 *   - 所有越界都由裁剪处理，绝不写屏幕外/读 surface 外。
 */
#ifndef NG_DS_COMPOSITE_H
#define NG_DS_COMPOSITE_H

#include "ds_surface.h"
#include "types.h"

struct ds_screen {
    u32 *px;
    u32 w;
    u32 h;
    u32 pitch_px; /* 每行像素数（QEMU stdvga 5120 字节 -> 1280） */
};

struct ds_composite_stats {
    u32 surfaces; /* 实际参与合成的 surface 数 */
    u32 pixels;   /* 写入的像素数 */
    u32 clipped;  /* 需要裁剪（矩形不等于 surface 全尺寸）的次数 */
};

#define DS_CURSOR_W 8u
#define DS_CURSOR_H 12u
#define DS_CURSOR_FG 0x00FFFFFFu
#define DS_CURSOR_BG 0x00000000u

/* 整数 source-over：返回 0x00RRGGBB。 */
u32 ds_pixel_blend(u32 dst, u32 src);

void ds_screen_pixel(struct ds_screen *s, i64 x, i64 y, u32 argb);
u32 ds_screen_read(const struct ds_screen *s, i64 x, i64 y);

/*
 * 把一个 surface 的 clip（surface 局部坐标）合成到屏幕。返回写入像素数。
 * clip == NULL 表示整块 surface。
 */
u32 ds_surface_blit(struct ds_screen *s, const struct ds_surface *sf,
                    const struct ds_rect *clip);

/*
 * 按 z-order（后 -> 前）合成屏幕 region（屏幕坐标；NULL = 全屏）。
 * 每个 surface 取它与 region 的交集再 blit；stats 可空。
 */
u32 ds_composite_region(struct ds_screen *s, struct ds_table *t,
                        const struct ds_rect *region,
                        struct ds_composite_stats *stats);
u32 ds_composite_all(struct ds_screen *s, struct ds_table *t,
                     struct ds_composite_stats *stats);

/* 基础光标：8x12 白色描边 + 黑色填充；越界自动裁剪。 */
void ds_cursor_draw(struct ds_screen *s, u32 x, u32 y);

#endif /* NG_DS_COMPOSITE_H */
