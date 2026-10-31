/*
 * ng WP2-4: ATA 块设备 -> L1 blk_env 适配（内核接线层，src/ng）。
 *
 * 为什么不在 src/drivers 里做：struct blk_env 属于 L1（src/services/fs），
 * 而 L1D 设备域只允许依赖 L0/A/B（tools/check-domains.ps1）。所以适配层放在
 * L0 的 src/ng，由它同时认识 ATA 驱动（L1D）和 blk.h（L1）。
 *
 * 本文件不 include src/ng/io.h，也不碰端口：只把 struct ata_dev 包装成
 * read/write/sectors/ctx 四个字段，因此 host 单测可以编译同一份代码
 * （tests/unit-ng/test_ata.c 用 fake ATA 控制器 + 同一套 ata.c）。
 *
 * 语义（与 fat_ramdisk 一致）：
 *   - read/write 返回 0 = 成功，-1 = 失败（FAT16 映射成 EIO）；
 *   - count 由调用方给，ATA PIO 驱动限制 1..8；FAT16 只用 1；
 *   - 扇区号是 u64，但 LBA28 只能寻址 0..0x0FFFFFFF，越界直接失败。
 */
#include "ata_blk.h"

#include "ata.h"
#include "blk.h"
#include "types.h"

static struct blk_env g_blk;
static u32 g_ready;

static int ata_blk_read(void *ctx, u64 sector, void *buf, u32 count)
{
    struct ata_dev *dev = (struct ata_dev *)ctx;

    if (dev == NULL || buf == NULL || count == 0u) {
        return -1;
    }
    if (sector > (u64)ATA_LBA28_MAX) {
        return -1;
    }
    if (ata_read28(dev, (u32)sector, buf, count) != ATA_OK) {
        return -1;
    }
    return 0;
}

static int ata_blk_write(void *ctx, u64 sector, const void *buf, u32 count)
{
    struct ata_dev *dev = (struct ata_dev *)ctx;

    if (dev == NULL || buf == NULL || count == 0u) {
        return -1;
    }
    if (sector > (u64)ATA_LBA28_MAX) {
        return -1;
    }
    if (ata_write28(dev, (u32)sector, buf, count) != ATA_OK) {
        return -1;
    }
    return 0;
}

int ata_blk_bind(struct ata_dev *dev)
{
    if (dev == NULL || dev->present == 0u || dev->sectors == 0u) {
        return -1;
    }
    g_blk.read = ata_blk_read;
    g_blk.write = ata_blk_write;
    g_blk.sectors = (u64)dev->sectors;
    g_blk.ctx = dev;
    g_ready = 1;
    return 0;
}

const struct blk_env *ata_blk_env(void)
{
    return (g_ready != 0u) ? &g_blk : NULL;
}

void ata_blk_reset(void)
{
    g_blk.read = NULL;
    g_blk.write = NULL;
    g_blk.sectors = 0;
    g_blk.ctx = NULL;
    g_ready = 0;
}
