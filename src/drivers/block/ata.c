/*
 * ng WP2-4: ATA PIO 驱动（L1D，见 include/ata.h）。
 *
 * 只做 legacy 端口 PIO：寄存器编程 + 有界轮询 + 16 位数据口搬 512 字节扇区。
 * 不碰页表/PMM/串口/VFS；端口访问全部经 struct ata_io 注入，因此同一份代码
 * 在内核（真端口）和 host 单测（fake 控制器）里跑。
 *
 * 轮询约定：
 *   - 每次状态检查后读 4 次 ALTSTATUS 做 ~400ns 延时（ATA 规范）；
 *   - 循环上限 dev->poll_limit（默认 1,000,000 ≈ 0.4s 硬件时间）；
 *   - 见到 BSY=0 且 ERR/DF=1 立刻返回 EIO（不把错误当超时）；
 *   - 超时只返回 ETIMEDOUT，不 panic。
 */
#include "ata.h"

static int ata_io_ok(const struct ata_io *io)
{
    return io != NULL && io->in8 != NULL && io->out8 != NULL &&
           io->in16 != NULL && io->out16 != NULL;
}

u8 ata_devsel(u8 drive, u32 lba)
{
    return (u8)(ATA_LBA_SELECT | (u8)((drive & 1u) << 4) |
                (u8)((lba >> 24) & 0x0Fu));
}

int ata_lba_range_ok(u32 lba, u32 count, u32 sectors)
{
    if (count == 0u || count > ATA_MAX_SECTORS_PER_CMD) {
        return 0;
    }
    if (sectors == 0u) {
        return 0;
    }
    if (lba > ATA_LBA28_MAX) {
        return 0;
    }
    if (lba >= sectors) {
        return 0;
    }
    if (count > sectors - lba) {
        return 0;
    }
    return 1;
}

int ata_status_class(u8 status)
{
    if ((status & ATA_SR_BSY) != 0u) {
        return 1;
    }
    if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0u) {
        return 2;
    }
    if ((status & ATA_SR_DRQ) != 0u) {
        return 3;
    }
    return 0;
}

void ata_dev_init(struct ata_dev *dev, const struct ata_io *io, u16 io_base,
                  u16 ctrl_base, u8 drive)
{
    u32 i;

    if (dev == NULL) {
        return;
    }
    dev->io = io;
    dev->io_base = (io_base != 0u) ? io_base : ATA_PRIMARY_IO_BASE;
    dev->ctrl_base = (ctrl_base != 0u) ? ctrl_base : ATA_PRIMARY_CTRL_BASE;
    dev->drive = (u8)(drive & 1u);
    dev->present = 0;
    dev->lba28 = 0;
    dev->lba48 = 0;
    dev->sectors = 0;
    dev->poll_limit = ATA_DEFAULT_POLL_LIMIT;
    dev->identify_loops = 0;
    dev->timeouts = 0;
    dev->errors = 0;
    dev->reads = 0;
    dev->writes = 0;
    for (i = 0; i < ATA_MODEL_LEN; i++) {
        dev->model[i] = 0;
    }
}

/* 读状态/交替状态；ctrl_base 为 0 时退回命令块状态口（单测/老机器）。 */
static u8 ata_read_status(struct ata_dev *dev, int alt)
{
    u16 port;

    if (alt && dev->ctrl_base != 0u) {
        port = (u16)(dev->ctrl_base + ATA_REG_ALTSTATUS);
    } else {
        port = (u16)(dev->io_base + ATA_REG_STATUS);
    }
    return dev->io->in8(dev->io->ctx, port);
}

/* ~400ns：读 4 次 ALTSTATUS（控制口读没有副作用）。 */
static void ata_delay_400ns(struct ata_dev *dev)
{
    u32 i;

    for (i = 0; i < 4u; i++) {
        (void)ata_read_status(dev, 1);
    }
}

/*
 * 有界等待：条件 = BSY=0 且 (status & want_set)==want_set 且
 * (status & want_clear)==0。BSY=0 且 ERR/DF=1 立刻 EIO；循环用完 ETIMEDOUT。
 */
static int ata_wait(struct ata_dev *dev, u8 want_set, u8 want_clear)
{
    u32 i;

    for (i = 0; i < dev->poll_limit; i++) {
        u8 st = ata_read_status(dev, 0);

        if ((st & ATA_SR_BSY) == 0u) {
            if ((st & (ATA_SR_ERR | ATA_SR_DF)) != 0u) {
                dev->errors++;
                return ATA_EIO;
            }
            if ((st & want_set) == want_set && (st & want_clear) == 0u) {
                return ATA_OK;
            }
        }
        ata_delay_400ns(dev);
    }
    dev->timeouts++;
    return ATA_ETIMEDOUT;
}

static void ata_select(struct ata_dev *dev, u32 lba)
{
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_HDDEVSEL),
                  ata_devsel(dev->drive, lba));
    ata_delay_400ns(dev);
}

/* 0x1F2 计数 + 0x1F3..0x1F5 的 LBA[23:0]；LBA[27:24] 在 HDDEVSEL。 */
static void ata_set_lba(struct ata_dev *dev, u32 lba, u32 count)
{
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_SECCOUNT0),
                  (u8)(count & 0xFFu));
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA0),
                  (u8)(lba & 0xFFu));
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA1),
                  (u8)((lba >> 8) & 0xFFu));
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA2),
                  (u8)((lba >> 16) & 0xFFu));
}

/* IDENTIFY 型号：word27..46，每个 word 高字节在前，去掉尾部空格。 */
static void ata_parse_model(struct ata_dev *dev, const u16 *id)
{
    u32 i;
    u32 m = 0;

    for (i = 0; i < 20u && m < ATA_MODEL_LEN - 1u; i++) {
        u8 hi = (u8)(id[27u + i] >> 8);
        u8 lo = (u8)(id[27u + i] & 0xFFu);

        if (hi >= 0x20u && hi <= 0x7Eu) {
            dev->model[m++] = (char)hi;
        }
        if (lo >= 0x20u && lo <= 0x7Eu && m < ATA_MODEL_LEN - 1u) {
            dev->model[m++] = (char)lo;
        }
    }
    dev->model[m] = '\0';
    while (m > 0u && dev->model[m - 1u] == ' ') {
        dev->model[--m] = '\0';
    }
}

int ata_identify(struct ata_dev *dev)
{
    u16 id[ATA_IDENTIFY_WORDS];
    u8 st;
    u32 i;
    int rc;

    if (dev == NULL || !ata_io_ok(dev->io) || dev->poll_limit == 0u) {
        return ATA_EINVAL;
    }
    dev->present = 0;
    dev->lba28 = 0;
    dev->lba48 = 0;
    dev->sectors = 0;
    dev->identify_loops = 0;
    for (i = 0; i < ATA_MODEL_LEN; i++) {
        dev->model[i] = 0;
    }

    ata_select(dev, 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_SECCOUNT0), 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA0), 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA1), 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_LBA2), 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_COMMAND),
                  ATA_CMD_IDENTIFY);
    ata_delay_400ns(dev);

    /* 浮空总线（0x00 或 0xFF）= 该槽位没有设备（QEMU 从盘就是这样）。 */
    st = ata_read_status(dev, 0);
    if (st == 0x00u || st == 0xFFu) {
        return ATA_ENODEV;
    }

    for (i = 0; i < dev->poll_limit; i++) {
        st = ata_read_status(dev, 0);
        if ((st & ATA_SR_BSY) == 0u) {
            break;
        }
        ata_delay_400ns(dev);
    }
    dev->identify_loops = i;
    if (i == dev->poll_limit) {
        dev->timeouts++;
        return ATA_ETIMEDOUT;
    }

    if ((st & (ATA_SR_ERR | ATA_SR_DF)) != 0u) {
        /* ATAPI/SATA 的 IDENTIFY 会报 ERR；从签名寄存器区分，不当作磁盘。 */
        u8 sig1 = dev->io->in8(dev->io->ctx,
                               (u16)(dev->io_base + ATA_REG_LBA1));
        u8 sig2 = dev->io->in8(dev->io->ctx,
                               (u16)(dev->io_base + ATA_REG_LBA2));

        if ((sig1 == 0x14u && sig2 == 0xEBu) ||
            (sig1 == 0x3Cu && sig2 == 0xC3u)) {
            return ATA_ENODEV;
        }
        dev->errors++;
        return ATA_EIO;
    }

    rc = ata_wait(dev, ATA_SR_DRQ, 0);
    if (rc != ATA_OK) {
        return rc;
    }

    for (i = 0; i < ATA_IDENTIFY_WORDS; i++) {
        id[i] = dev->io->in16(dev->io->ctx, (u16)(dev->io_base + ATA_REG_DATA));
    }

    if (id[0] == 0x0000u || id[0] == 0xFFFFu) {
        return ATA_ENODEV;
    }
    if ((id[49] & 0x0200u) == 0u) { /* bit9: LBA supported */
        return ATA_ENODEV;
    }
    if ((id[83] & 0x0400u) != 0u) { /* bit10: LBA48 */
        dev->lba48 = 1;
    }
    dev->sectors = (u32)id[60] | ((u32)id[61] << 16);
    if (dev->sectors == 0u) {
        return ATA_ENODEV;
    }
    dev->lba28 = 1;
    ata_parse_model(dev, id);
    dev->present = 1;
    return ATA_OK;
}

int ata_read28(struct ata_dev *dev, u32 lba, void *buf, u32 count)
{
    u8 *dst = (u8 *)buf;
    u32 s;
    u32 i;
    int rc;

    if (dev == NULL || !ata_io_ok(dev->io) || buf == NULL) {
        return ATA_EINVAL;
    }
    if (!dev->present) {
        return ATA_ENODEV;
    }
    if (!ata_lba_range_ok(lba, count, dev->sectors)) {
        return ATA_EINVAL;
    }

    ata_select(dev, lba);
    ata_set_lba(dev, lba, count);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_COMMAND),
                  ATA_CMD_READ_SECTORS);
    ata_delay_400ns(dev);

    for (s = 0; s < count; s++) {
        rc = ata_wait(dev, ATA_SR_DRQ, 0);
        if (rc != ATA_OK) {
            return rc;
        }
        for (i = 0; i < ATA_SECTOR_SIZE / 2u; i++) {
            u16 w = dev->io->in16(dev->io->ctx,
                                  (u16)(dev->io_base + ATA_REG_DATA));

            dst[0] = (u8)(w & 0xFFu);
            dst[1] = (u8)(w >> 8);
            dst += 2;
        }
    }

    rc = ata_wait(dev, 0, ATA_SR_DRQ);
    if (rc != ATA_OK) {
        return rc;
    }
    dev->reads++;
    return ATA_OK;
}

int ata_write28(struct ata_dev *dev, u32 lba, const void *buf, u32 count)
{
    const u8 *src = (const u8 *)buf;
    u32 s;
    u32 i;
    int rc;

    if (dev == NULL || !ata_io_ok(dev->io) || buf == NULL) {
        return ATA_EINVAL;
    }
    if (!dev->present) {
        return ATA_ENODEV;
    }
    if (!ata_lba_range_ok(lba, count, dev->sectors)) {
        return ATA_EINVAL;
    }

    ata_select(dev, lba);
    ata_set_lba(dev, lba, count);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_COMMAND),
                  ATA_CMD_WRITE_SECTORS);
    ata_delay_400ns(dev);

    for (s = 0; s < count; s++) {
        rc = ata_wait(dev, ATA_SR_DRQ, 0);
        if (rc != ATA_OK) {
            return rc;
        }
        for (i = 0; i < ATA_SECTOR_SIZE / 2u; i++) {
            u16 w = (u16)((u16)src[0] | ((u16)src[1] << 8));

            dev->io->out16(dev->io->ctx,
                           (u16)(dev->io_base + ATA_REG_DATA), w);
            src += 2;
        }
    }

    rc = ata_wait(dev, 0, ATA_SR_DRQ);
    if (rc != ATA_OK) {
        return rc;
    }
    rc = ata_flush(dev);
    if (rc != ATA_OK) {
        return rc;
    }
    dev->writes++;
    return ATA_OK;
}

int ata_flush(struct ata_dev *dev)
{
    if (dev == NULL || !ata_io_ok(dev->io)) {
        return ATA_EINVAL;
    }
    if (!dev->present) {
        return ATA_ENODEV;
    }
    ata_select(dev, 0);
    dev->io->out8(dev->io->ctx, (u16)(dev->io_base + ATA_REG_COMMAND),
                  ATA_CMD_FLUSH_CACHE);
    ata_delay_400ns(dev);
    return ata_wait(dev, 0, ATA_SR_DRQ);
}
