/*
 * ng WP2-3: Bochs VBE 扩展（QEMU -vga std 的 0x1CE/0x1CF 寄存器接口）。
 *
 * 为什么不走 BIOS/VBE INT 10h：ng 用 QEMU -kernel（PVH）直接进 64 位内核，
 * 没有 BIOS 环境；Bochs dispi 接口是纯 I/O 端口协议，内核自己就能设置
 * 线性帧缓冲模式。
 *
 * 寄存器访问是“索引/数据”两个 16 位端口：
 *   - 0x1CE：写寄存器索引；
 *   - 0x1CF：读写该索引的数据。
 * 端口 IO 通过 `struct vbe_io` 注入，host 单测用 fake 寄存器组验证编码。
 *
 * 本驱动只负责“探测 + 设模式 + 读回校验 + 关模式”，不碰 BAR/MMIO 映射。
 */
#ifndef NG_VBE_H
#define NG_VBE_H

#include "types.h"

#define VBE_PORT_INDEX 0x1CEu
#define VBE_PORT_DATA 0x1CFu

/* Bochs VBE dispi 索引。 */
#define VBE_DISPI_INDEX_ID 0u
#define VBE_DISPI_INDEX_XRES 1u
#define VBE_DISPI_INDEX_YRES 2u
#define VBE_DISPI_INDEX_BPP 3u
#define VBE_DISPI_INDEX_ENABLE 4u
#define VBE_DISPI_INDEX_BANK 5u
#define VBE_DISPI_INDEX_VIRT_WIDTH 6u
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7u
#define VBE_DISPI_INDEX_X_OFFSET 8u
#define VBE_DISPI_INDEX_Y_OFFSET 9u
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 10u

/* ENABLE 位。 */
#define VBE_DISPI_DISABLED 0x00u
#define VBE_DISPI_ENABLED 0x01u
#define VBE_DISPI_GETCAPS 0x02u
#define VBE_DISPI_8BIT_DAC 0x20u
#define VBE_DISPI_LFB_ENABLED 0x40u
#define VBE_DISPI_NOCLEARMEM 0x80u

/* ID 返回值的家族掩码：0xB0C0..0xB0CF 都是 Bochs/QEMU VBE。 */
#define VBE_ID_FAMILY 0xB0C0u
#define VBE_ID_MASK 0xFFF0u

/* 错误码（负）。 */
#define VBE_OK 0
#define VBE_ENODEV (-1)
#define VBE_EBADMODE (-2)
#define VBE_EVERIFY (-3)

/* 设置成功的模式描述（fb_init 用）。 */
struct vbe_mode {
    u16 width;
    u16 height;
    u16 bpp;
    u16 virt_width;
    u16 virt_height;
    u32 pitch;     /* width * bpp / 8 */
    u32 size_bytes;/* pitch * height */
};

struct vbe_io {
    void (*outw)(void *ctx, u16 port, u16 value);
    u16 (*inw)(void *ctx, u16 port);
    void *ctx;
};

/* 读/写一个 dispi 寄存器（index 先写 0x1CE，数据在 0x1CF）。 */
u16 vbe_read(const struct vbe_io *io, u16 index);
void vbe_write(const struct vbe_io *io, u16 index, u16 value);

/*
 * 探测：读 ID，返回 0xB0C0..0xB0CF 家族的完整 ID（如 0xB0C5）；
 * 不是 Bochs VBE 返回 VBE_ENODEV。
 */
int vbe_probe(const struct vbe_io *io, u16 *id_out);

/* 设备报告的显存大小（64 KiB 单位 -> 字节）；读不到返回 0。 */
u32 vbe_video_mem_bytes(const struct vbe_io *io);

/*
 * 设置模式：
 *   1. ENABLE=0（先关，避免改分辨率时撕裂）；
 *   2. 写 XRES/YRES/BPP/VIRT_WIDTH/VIRT_HEIGHT；
 *   3. ENABLE = ENABLED|LFB_ENABLED；
 *   4. 读回 ENABLE/XRES/YRES/BPP 校验。
 * 返回 VBE_OK 或 VBE_E*。宽度/高度/bpp 超出范围返回 VBE_EBADMODE。
 */
int vbe_set_mode(const struct vbe_io *io, u16 width, u16 height, u16 bpp,
                 struct vbe_mode *out);

/* 关模式（回到设备默认/文本模式）。 */
void vbe_disable(const struct vbe_io *io);

/* 由宽高/bpp 算 pitch/size；参数非法返回 -1。 */
int vbe_mode_info(u16 width, u16 height, u16 bpp, struct vbe_mode *out);

#endif /* NG_VBE_H */
