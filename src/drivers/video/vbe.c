/*
 * ng WP2-3: Bochs VBE（QEMU stdvga）模式设置，见 include/vbe.h。
 *
 * 只做寄存器编码/读回校验，端口访问由调用方注入。QEMU 的 stdvga 在
 * -kernel（无 BIOS）启动时也保持 0x1CE/0x1CF 可用，所以这条路不依赖
 * VBE BIOS 调用，也不依赖 INT 10h。
 */
#include "vbe.h"

#define VBE_WIDTH_MIN 320u
#define VBE_WIDTH_MAX 2560u
#define VBE_HEIGHT_MIN 200u
#define VBE_HEIGHT_MAX 1600u

static int io_ok(const struct vbe_io *io)
{
    return io != NULL && io->outw != NULL && io->inw != NULL;
}

u16 vbe_read(const struct vbe_io *io, u16 index)
{
    if (!io_ok(io)) {
        return 0xFFFFu;
    }
    io->outw(io->ctx, VBE_PORT_INDEX, index);
    return io->inw(io->ctx, VBE_PORT_DATA);
}

void vbe_write(const struct vbe_io *io, u16 index, u16 value)
{
    if (!io_ok(io)) {
        return;
    }
    io->outw(io->ctx, VBE_PORT_INDEX, index);
    io->outw(io->ctx, VBE_PORT_DATA, value);
}

int vbe_probe(const struct vbe_io *io, u16 *id_out)
{
    u16 id;

    if (!io_ok(io)) {
        return VBE_ENODEV;
    }
    id = vbe_read(io, VBE_DISPI_INDEX_ID);
    if ((id & VBE_ID_MASK) != VBE_ID_FAMILY) {
        return VBE_ENODEV;
    }
    if (id_out != NULL) {
        *id_out = id;
    }
    return VBE_OK;
}

u32 vbe_video_mem_bytes(const struct vbe_io *io)
{
    u16 blocks;

    if (!io_ok(io)) {
        return 0;
    }
    blocks = vbe_read(io, VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    return (u32)blocks * 64u * 1024u;
}

int vbe_mode_info(u16 width, u16 height, u16 bpp, struct vbe_mode *out)
{
    u32 pitch;

    if (out == NULL) {
        return VBE_EBADMODE;
    }
    if (width < VBE_WIDTH_MIN || width > VBE_WIDTH_MAX ||
        height < VBE_HEIGHT_MIN || height > VBE_HEIGHT_MAX) {
        return VBE_EBADMODE;
    }
    if (bpp != 8u && bpp != 15u && bpp != 16u && bpp != 24u && bpp != 32u) {
        return VBE_EBADMODE;
    }
    pitch = (u32)width * ((u32)bpp / 8u);
    out->width = width;
    out->height = height;
    out->bpp = bpp;
    out->virt_width = width;
    out->virt_height = height;
    out->pitch = pitch;
    out->size_bytes = pitch * (u32)height;
    return VBE_OK;
}

void vbe_disable(const struct vbe_io *io)
{
    if (!io_ok(io)) {
        return;
    }
    vbe_write(io, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

int vbe_set_mode(const struct vbe_io *io, u16 width, u16 height, u16 bpp,
                 struct vbe_mode *out)
{
    struct vbe_mode mode;
    u16 enable;
    u16 xres;
    u16 yres;
    u16 bpp_rd;

    if (!io_ok(io)) {
        return VBE_ENODEV;
    }
    if (vbe_mode_info(width, height, bpp, &mode) != VBE_OK) {
        return VBE_EBADMODE;
    }

    /* 1. 先关模式。QEMU 在 ENABLE=0 时回到文本/VGA 默认状态。 */
    vbe_disable(io);

    /* 2. 写几何。VIRT_* 显式等于可见尺寸，避免设备沿用上一个模式的行宽。 */
    vbe_write(io, VBE_DISPI_INDEX_XRES, width);
    vbe_write(io, VBE_DISPI_INDEX_YRES, height);
    vbe_write(io, VBE_DISPI_INDEX_BPP, bpp);
    vbe_write(io, VBE_DISPI_INDEX_VIRT_WIDTH, mode.virt_width);
    vbe_write(io, VBE_DISPI_INDEX_VIRT_HEIGHT, mode.virt_height);

    /* 3. 打开线性帧缓冲。 */
    vbe_write(io, VBE_DISPI_INDEX_ENABLE,
              (u16)(VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));

    /* 4. 读回校验：QEMU 对 XRES/YRES/BPP 是不变的直通寄存器。 */
    enable = vbe_read(io, VBE_DISPI_INDEX_ENABLE);
    xres = vbe_read(io, VBE_DISPI_INDEX_XRES);
    yres = vbe_read(io, VBE_DISPI_INDEX_YRES);
    bpp_rd = vbe_read(io, VBE_DISPI_INDEX_BPP);
    if ((enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) !=
            (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED) ||
        xres != width || yres != height || bpp_rd != bpp) {
        return VBE_EVERIFY;
    }
    if (out != NULL) {
        *out = mode;
    }
    return VBE_OK;
}
