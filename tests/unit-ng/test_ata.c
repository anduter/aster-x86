/*
 * ng WP2-4 host 单测：ATA PIO 驱动 + ata_blk_env 适配 + fake ATA 盘上的 FAT16。
 *
 * 覆盖：
 *   1. 纯函数：HDDEVSEL 编码（drive/LBA[27:24]）、LBA28 范围校验、
 *      状态分类（BSY/ERR/DF/DRQ/DRDY）；
 *   2. fake ATA 控制器：寄存器文件 + BSY/DRQ 状态机 + PIO 数据口 +
 *      IDENTIFY 数据 + ERR/DF/超时/ATAPI/浮空总线注入；
 *   3. IDENTIFY：型号（word27..46 高字节在前、去尾空格）、扇区数、LBA/LBA48
 *      位；无设备/ATAPI/不支持 LBA/全 0 数据 -> ENODEV；BSY 不落 -> ETIMEDOUT；
 *      ERR -> EIO；
 *   4. 读：单扇区/8 扇区/末扇区、LBA 寄存器与 HDDEVSEL 高位、参数越界
 *      （count 0/9、跨末尾、buf NULL、缺 io）、错误后恢复、超时后恢复；
 *   5. 写：单扇区/多扇区、数据真的写进 fake 盘、末尾 FLUSH（0xE7）、
 *      错误注入时盘内容不变；
 *   6. ata_blk_env：sectors 传播、read/write 走 ATA、u64 扇区号 > LBA28 拒绝、
 *      未绑定/未 IDENTIFY 拒绝；
 *   7. FAT16 on ATA：合成 4096 扇区（2 扇区/簇、2029 簇）FAT16 卷，挂载、
 *      读 HELLO.TXT / DIR/A.TXT、创建/写/读回/删除 NG.TXT、重新 mount 验证
 *      写入真的落在 fake 盘上；设备扇区数不足 -> EINVAL，读错误 -> EIO。
 *
 * 所有负向用例断言**具体错误码**（ATA_EIO / ATA_ETIMEDOUT / ATA_ENODEV /
 * ATA_EINVAL），不按“负值即失败”放过。
 */
#include <string.h>

#include "ata.h"
#include "ata_blk.h"
#include "fat16.h"
#include "test_util.h"
#include "vfs.h"

#define FK_IO_BASE ATA_PRIMARY_IO_BASE
#define FK_CTRL_BASE ATA_PRIMARY_CTRL_BASE
#define FK_SECTORS 4096u
#define FK_BYTES (FK_SECTORS * ATA_SECTOR_SIZE)

static u8 g_disk[FK_BYTES];

/* ------------------------------------------------------------------------- */
/* fake ATA 控制器                                                           */
/* ------------------------------------------------------------------------- */

enum {
    FK_PHASE_IDLE = 0,
    FK_PHASE_IDENTIFY = 1,
    FK_PHASE_READ = 2,
    FK_PHASE_WRITE = 3,
    FK_PHASE_FLUSH = 4
};

struct fake_ctrl {
    u8 regs[8]; /* 命令块寄存器（下标 = 端口 - io_base） */
    u8 ctrl;    /* 0x3F6 写入（nIEN 等） */
    u8 err;
    u8 no_device;    /* 读状态永远 0（浮空总线） */
    u8 busy_forever; /* 状态永远 BSY（超时用例） */
    u32 busy_reads;  /* 还剩几次状态读返回 BSY */
    u32 cmd_busy;    /* 每条命令初始的 BSY 次数 */
    u32 phase;

    u32 cmd;
    u32 lba;
    u32 count;
    u32 sector_idx;
    u32 word_idx;

    u8 fail_cmd; /* 该命令 -> 状态带 ERR */
    u8 cur_fail;
    u8 fail_df;
    u8 zero_id;
    u8 no_lba;
    u8 atapi;
    u8 lba48;
    u16 id[ATA_IDENTIFY_WORDS];

    u8 *disk;
    u32 sectors;

    /* 诊断/断言 */
    u32 status_reads;
    u32 data_in_words;
    u32 data_out_words;
    u8 cmd_log[16];
    u32 cmd_log_n;
    u8 last_secount;
    u8 last_lba0;
    u8 last_lba1;
    u8 last_lba2;
    u8 last_devsel;
};

static struct fake_ctrl fc;

static void fake_build_id(void)
{
    u8 model[40];
    u32 i;

    memset(fc.id, 0, sizeof(fc.id));
    if (fc.zero_id) {
        return;
    }
    fc.id[0] = 0x0040u;
    fc.id[49] = fc.no_lba ? 0x0000u : 0x0200u;
    fc.id[60] = (u16)(fc.sectors & 0xFFFFu);
    fc.id[61] = (u16)((fc.sectors >> 16) & 0xFFFFu);
    if (fc.lba48) {
        fc.id[83] = 0x0400u;
    }
    memset(model, ' ', sizeof(model));
    memcpy(model, "QEMU HARDDISK", 13);
    for (i = 0; i < 20u; i++) {
        fc.id[27u + i] = (u16)(((u16)model[2u * i] << 8) |
                               (u16)model[2u * i + 1u]);
    }
}

static void fake_start_cmd(void)
{
    fc.cmd = fc.regs[ATA_REG_COMMAND];
    fc.lba = (u32)fc.regs[ATA_REG_LBA0] |
             ((u32)fc.regs[ATA_REG_LBA1] << 8) |
             ((u32)fc.regs[ATA_REG_LBA2] << 16) |
             ((u32)(fc.regs[ATA_REG_HDDEVSEL] & 0x0Fu) << 24);
    fc.count = (fc.regs[ATA_REG_SECCOUNT0] == 0u)
                   ? 256u
                   : (u32)fc.regs[ATA_REG_SECCOUNT0];
    fc.sector_idx = 0;
    fc.word_idx = 0;
    fc.busy_reads = fc.cmd_busy;
    fc.cur_fail = (fc.fail_cmd != 0u && fc.fail_cmd == fc.cmd) ? 1u : 0u;

    if (fc.cmd_log_n < 16u) {
        fc.cmd_log[fc.cmd_log_n++] = (u8)fc.cmd;
    }
    fc.last_secount = fc.regs[ATA_REG_SECCOUNT0];
    fc.last_lba0 = fc.regs[ATA_REG_LBA0];
    fc.last_lba1 = fc.regs[ATA_REG_LBA1];
    fc.last_lba2 = fc.regs[ATA_REG_LBA2];
    fc.last_devsel = fc.regs[ATA_REG_HDDEVSEL];

    if (fc.cmd == ATA_CMD_IDENTIFY) {
        if (fc.atapi) {
            fc.phase = FK_PHASE_IDLE;
            fc.cur_fail = 1;
            fc.regs[ATA_REG_LBA1] = 0x14; /* ATAPI 签名 */
            fc.regs[ATA_REG_LBA2] = 0xEB;
            return;
        }
        fake_build_id();
        fc.phase = FK_PHASE_IDENTIFY;
    } else if (fc.cmd == ATA_CMD_READ_SECTORS) {
        fc.phase = FK_PHASE_READ;
    } else if (fc.cmd == ATA_CMD_WRITE_SECTORS) {
        fc.phase = FK_PHASE_WRITE;
    } else if (fc.cmd == ATA_CMD_FLUSH_CACHE) {
        fc.phase = FK_PHASE_FLUSH;
    } else {
        fc.phase = FK_PHASE_IDLE;
    }
}

static u8 fake_status(void)
{
    fc.status_reads++;
    if (fc.no_device) {
        return 0x00u;
    }
    if (fc.busy_forever) {
        return ATA_SR_BSY;
    }
    if (fc.busy_reads > 0u) {
        fc.busy_reads--;
        return ATA_SR_BSY;
    }
    if (fc.cur_fail) {
        return (u8)(ATA_SR_DRDY | (fc.fail_df ? ATA_SR_DF : ATA_SR_ERR));
    }
    switch (fc.phase) {
    case FK_PHASE_IDENTIFY:
        return (u8)(ATA_SR_DRDY |
                    ((fc.word_idx < ATA_IDENTIFY_WORDS) ? ATA_SR_DRQ : 0u));
    case FK_PHASE_READ:
    case FK_PHASE_WRITE:
        return (u8)(ATA_SR_DRDY |
                    ((fc.sector_idx < fc.count) ? ATA_SR_DRQ : 0u));
    default:
        return ATA_SR_DRDY;
    }
}

static void fake_advance_data(void)
{
    fc.word_idx++;
    if (fc.word_idx >= ATA_SECTOR_SIZE / 2u) {
        fc.word_idx = 0;
        fc.sector_idx++;
        if (fc.sector_idx >= fc.count || fc.phase == FK_PHASE_IDENTIFY) {
            fc.phase = FK_PHASE_IDLE;
        }
    }
}

static u8 fake_in8(void *ctx, u16 port)
{
    u16 off;

    (void)ctx;
    if (port == FK_IO_BASE + ATA_REG_STATUS || port == FK_CTRL_BASE) {
        return fake_status();
    }
    if (port == FK_CTRL_BASE + 1u) {
        return 0;
    }
    if (port >= FK_IO_BASE && port < FK_IO_BASE + 8u) {
        off = (u16)(port - FK_IO_BASE);
        if (off == ATA_REG_ERROR) {
            return fc.err;
        }
        return fc.regs[off];
    }
    return 0;
}

static void fake_out8(void *ctx, u16 port, u8 value)
{
    u16 off;

    (void)ctx;
    if (port == FK_CTRL_BASE) {
        fc.ctrl = value;
        return;
    }
    if (port >= FK_IO_BASE && port < FK_IO_BASE + 8u) {
        off = (u16)(port - FK_IO_BASE);
        fc.regs[off] = value;
        if (off == ATA_REG_COMMAND) {
            fake_start_cmd();
        }
    }
}

static u16 fake_in16(void *ctx, u16 port)
{
    u32 base;
    u16 w;

    (void)ctx;
    if (port != FK_IO_BASE + ATA_REG_DATA) {
        return 0;
    }
    if (fc.phase == FK_PHASE_IDENTIFY) {
        if (fc.word_idx >= ATA_IDENTIFY_WORDS) {
            return 0;
        }
        fc.data_in_words++;
        w = fc.id[fc.word_idx];
        fake_advance_data();
        return w;
    }
    if (fc.phase == FK_PHASE_READ) {
        if (fc.sector_idx >= fc.count ||
            fc.lba + fc.sector_idx >= fc.sectors) {
            /* 越界读：丢弃但推进传输，让命令正常结束（测试 LBA 编码用）。 */
            fake_advance_data();
            return 0;
        }
        base = (fc.lba + fc.sector_idx) * ATA_SECTOR_SIZE + fc.word_idx * 2u;
        fc.data_in_words++;
        w = (u16)((u16)fc.disk[base] | ((u16)fc.disk[base + 1u] << 8));
        fake_advance_data();
        return w;
    }
    return 0;
}

static void fake_out16(void *ctx, u16 port, u16 value)
{
    u32 base;

    (void)ctx;
    if (port != FK_IO_BASE + ATA_REG_DATA || fc.phase != FK_PHASE_WRITE) {
        return;
    }
    if (fc.sector_idx >= fc.count || fc.lba + fc.sector_idx >= fc.sectors) {
        /* 越界写：丢弃但推进传输，让命令正常结束（测试 LBA 编码用）。 */
        fc.data_out_words++;
        fake_advance_data();
        return;
    }
    base = (fc.lba + fc.sector_idx) * ATA_SECTOR_SIZE + fc.word_idx * 2u;
    fc.disk[base] = (u8)(value & 0xFFu);
    fc.disk[base + 1u] = (u8)(value >> 8);
    fc.data_out_words++;
    fake_advance_data();
}

static const struct ata_io g_fake_io = {fake_in8, fake_out8, fake_in16,
                                        fake_out16, NULL};

static void fake_reset(void)
{
    memset(&fc, 0, sizeof(fc));
    fc.disk = g_disk;
    fc.sectors = FK_SECTORS;
    fc.cmd_busy = 6;
}

static void fake_setup(struct ata_dev *dev)
{
    fake_reset();
    ata_dev_init(dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE,
                 ATA_DRIVE_MASTER);
    CHECK(ata_identify(dev) == ATA_OK);
    CHECK(dev->present == 1);
}

/* ------------------------------------------------------------------------- */
/* 1. 纯函数                                                                 */
/* ------------------------------------------------------------------------- */

static void test_ata_pure(void)
{
    CHECK(ata_devsel(ATA_DRIVE_MASTER, 0) == 0xE0);
    CHECK(ata_devsel(ATA_DRIVE_SLAVE, 0) == 0xF0);
    CHECK(ata_devsel(ATA_DRIVE_MASTER, 0x0A000000u) == 0xEA);
    CHECK(ata_devsel(ATA_DRIVE_MASTER, 0x1234567u) == 0xE1);
    CHECK(ata_devsel(ATA_DRIVE_SLAVE, 0x1234567u) == 0xF1);
    CHECK(ata_devsel(2, 0) == 0xE0); /* drive 只看 bit0 */

    CHECK(ata_lba_range_ok(0, 1, 1) == 1);
    CHECK(ata_lba_range_ok(0, 8, 8) == 1);
    CHECK(ata_lba_range_ok(1, 8, 8) == 0);
    CHECK(ata_lba_range_ok(0, 9, 4096) == 0);
    CHECK(ata_lba_range_ok(0, 0, 4096) == 0);
    CHECK(ata_lba_range_ok(4096, 1, 4096) == 0);
    CHECK(ata_lba_range_ok(4095, 1, 4096) == 1);
    CHECK(ata_lba_range_ok(4088, 8, 4096) == 1);
    CHECK(ata_lba_range_ok(4089, 8, 4096) == 0);
    CHECK(ata_lba_range_ok(0, 1, 0) == 0);
    CHECK(ata_lba_range_ok(0x0FFFFFFFu, 1, 0x10000000u) == 1);
    CHECK(ata_lba_range_ok(0x10000000u, 1, 0x10000000u) == 0);

    CHECK(ata_status_class(ATA_SR_BSY) == 1);
    CHECK(ata_status_class(ATA_SR_BSY | ATA_SR_ERR) == 1);
    CHECK(ata_status_class(ATA_SR_ERR) == 2);
    CHECK(ata_status_class(ATA_SR_DF) == 2);
    CHECK(ata_status_class(ATA_SR_ERR | ATA_SR_DF) == 2);
    CHECK(ata_status_class(ATA_SR_DRQ) == 3);
    CHECK(ata_status_class(ATA_SR_DRDY | ATA_SR_DRQ) == 3);
    CHECK(ata_status_class(ATA_SR_DRDY) == 0);
    CHECK(ata_status_class(0) == 0);
}

/* ------------------------------------------------------------------------- */
/* 2. IDENTIFY                                                               */
/* ------------------------------------------------------------------------- */

static void test_ata_identify(void)
{
    struct ata_dev dev;
    u8 status;

    /* 正常：型号/扇区数/LBA28/LBA48=0 */
    fake_setup(&dev);
    CHECK(dev.lba28 == 1);
    CHECK(dev.lba48 == 0);
    CHECK(dev.sectors == FK_SECTORS);
    CHECK(strcmp(dev.model, "QEMU HARDDISK") == 0);
    CHECK(fc.cmd == ATA_CMD_IDENTIFY);
    CHECK(fc.last_secount == 0);
    CHECK(fc.last_lba0 == 0);
    CHECK(fc.last_lba1 == 0);
    CHECK(fc.last_lba2 == 0);
    CHECK(fc.last_devsel == 0xE0);
    CHECK(fc.data_in_words == ATA_IDENTIFY_WORDS);
    CHECK(dev.identify_loops <= dev.poll_limit);
    CHECK(dev.timeouts == 0);
    CHECK(dev.errors == 0);
    status = fake_status();
    CHECK(status != 0); /* 命令结束后 DRDY，不是浮空 */

    /* 从盘 select：HDDEVSEL 的 drive 位 */
    fake_reset();
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, ATA_DRIVE_SLAVE);
    CHECK(ata_identify(&dev) == ATA_OK);
    CHECK(fc.last_devsel == 0xF0);

    /* LBA48 能力位（只诊断，不传输） */
    fake_reset();
    fc.lba48 = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_OK);
    CHECK(dev.lba48 == 1);
    CHECK(dev.lba28 == 1);

    /* 浮空总线：状态 0 -> 没有设备 */
    fake_reset();
    fc.no_device = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_ENODEV);
    CHECK(dev.present == 0);
    CHECK(dev.sectors == 0);

    /* 全 0 IDENTIFY 数据 -> 非 ATA */
    fake_reset();
    fc.zero_id = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_ENODEV);
    CHECK(dev.present == 0);

    /* 不支持 LBA（word49 bit9=0） */
    fake_reset();
    fc.no_lba = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_ENODEV);

    /* ATAPI 签名 -> 当作“不是磁盘”，不 panic */
    fake_reset();
    fc.atapi = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_ENODEV);
    CHECK(dev.present == 0);

    /* ERR（非 ATAPI 签名）-> EIO */
    fake_reset();
    fc.fail_cmd = ATA_CMD_IDENTIFY;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_EIO);
    CHECK(dev.errors == 1);
    CHECK(dev.present == 0);

    /* DF -> EIO */
    fake_reset();
    fc.fail_cmd = ATA_CMD_IDENTIFY;
    fc.fail_df = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_EIO);

    /* BSY 永不落地 -> 有界超时 */
    fake_reset();
    fc.busy_forever = 1;
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    dev.poll_limit = 12;
    CHECK(ata_identify(&dev) == ATA_ETIMEDOUT);
    CHECK(dev.timeouts == 1);
    CHECK(dev.present == 0);
    CHECK(fc.status_reads >= 12u);

    /* 超时后恢复：坏设备不影响下一次正常 identify */
    fake_reset();
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_OK);
    CHECK(dev.present == 1);

    /* 参数/IO 缺失 */
    CHECK(ata_identify(NULL) == ATA_EINVAL);
    fake_reset();
    ata_dev_init(&dev, NULL, FK_IO_BASE, FK_CTRL_BASE, 0);
    CHECK(ata_identify(&dev) == ATA_EINVAL);
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
    dev.poll_limit = 0;
    CHECK(ata_identify(&dev) == ATA_EINVAL);
}

/* ------------------------------------------------------------------------- */
/* 3. 读                                                                     */
/* ------------------------------------------------------------------------- */

static u8 pat_byte(u32 sector, u32 i)
{
    return (u8)((sector * 31u + i * 7u + 5u) & 0xFFu);
}

static void fill_disk_pattern(void)
{
    u32 s;
    u32 i;

    for (s = 0; s < FK_SECTORS; s++) {
        for (i = 0; i < ATA_SECTOR_SIZE; i++) {
            g_disk[s * ATA_SECTOR_SIZE + i] = pat_byte(s, i);
        }
    }
}

static int sector_matches(u32 sector, const u8 *buf)
{
    u32 i;

    for (i = 0; i < ATA_SECTOR_SIZE; i++) {
        if (buf[i] != pat_byte(sector, i)) {
            return 0;
        }
    }
    return 1;
}

static void test_ata_read(void)
{
    struct ata_dev dev;
    u8 buf[8u * ATA_SECTOR_SIZE];
    u32 i;
    u32 s;
    int ok;

    fill_disk_pattern();
    fake_setup(&dev);

    /* 单扇区 */
    memset(buf, 0, sizeof(buf));
    CHECK(ata_read28(&dev, 0, buf, 1) == ATA_OK);
    CHECK(sector_matches(0, buf));
    CHECK(fc.last_secount == 1);
    CHECK(fc.last_lba0 == 0 && fc.last_lba1 == 0 && fc.last_lba2 == 0);
    CHECK(fc.last_devsel == 0xE0);
    CHECK(dev.reads == 1);

    /* 8 扇区（上限），一次命令 + 逐扇区 DRQ */
    memset(buf, 0, sizeof(buf));
    CHECK(ata_read28(&dev, 100, buf, 8) == ATA_OK);
    ok = 1;
    for (s = 0; s < 8u; s++) {
        if (!sector_matches(100u + s, buf + s * ATA_SECTOR_SIZE)) {
            ok = 0;
        }
    }
    CHECK(ok);
    CHECK(fc.last_secount == 8);
    CHECK(dev.reads == 2);

    /* 末扇区 */
    memset(buf, 0, 512);
    CHECK(ata_read28(&dev, FK_SECTORS - 1u, buf, 1) == ATA_OK);
    CHECK(sector_matches(FK_SECTORS - 1u, buf));

    /* LBA[23:16]/[27:24] 编码：0x1123456（高 4 位 = 1）。fake 盘只有 4096
     * 扇区，越界读返回 0 但命令正常结束；只断言寄存器编码。 */
    {
        struct ata_dev wide = dev;

        wide.sectors = 0x02000000u; /* 让范围校验通过，fake 负责不越界 */
        memset(buf, 0, 512);
        CHECK(ata_read28(&wide, 0x1123456u, buf, 1) == ATA_OK);
        CHECK(fc.last_lba0 == 0x56);
        CHECK(fc.last_lba1 == 0x34);
        CHECK(fc.last_lba2 == 0x12);
        CHECK(fc.last_devsel == 0xE1);
    }

    /* 同一 LBA 读两次结果一致（无隐藏状态） */
    memset(buf, 0, 512);
    CHECK(ata_read28(&dev, 42, buf, 1) == ATA_OK);
    CHECK(sector_matches(42, buf));
    for (i = 0; i < 512u; i++) {
        buf[i] = 0;
    }
    CHECK(ata_read28(&dev, 42, buf, 1) == ATA_OK);
    CHECK(sector_matches(42, buf));

    /* 参数非法 */
    CHECK(ata_read28(&dev, 0, NULL, 1) == ATA_EINVAL);
    CHECK(ata_read28(&dev, 0, buf, 0) == ATA_EINVAL);
    CHECK(ata_read28(&dev, 0, buf, 9) == ATA_EINVAL);
    CHECK(ata_read28(&dev, FK_SECTORS, buf, 1) == ATA_EINVAL);
    CHECK(ata_read28(&dev, FK_SECTORS - 1u, buf, 2) == ATA_EINVAL);
    CHECK(ata_read28(NULL, 0, buf, 1) == ATA_EINVAL);
    {
        struct ata_dev noio;

        ata_dev_init(&noio, NULL, FK_IO_BASE, FK_CTRL_BASE, 0);
        noio.present = 1;
        noio.sectors = FK_SECTORS;
        CHECK(ata_read28(&noio, 0, buf, 1) == ATA_EINVAL);
    }

    /* LBA28 上限：范围检查本身（不做数据搬运，避免 fake 盘越界） */
    {
        struct ata_dev big;

        ata_dev_init(&big, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
        big.present = 1;
        big.sectors = ATA_LBA28_SECTORS;
        CHECK(ata_lba_range_ok(ATA_LBA28_MAX, 1, big.sectors) == 1);
        CHECK(ata_lba_range_ok(ATA_LBA28_MAX, 2, big.sectors) == 0);
        CHECK(ata_lba_range_ok(ATA_LBA28_MAX + 1u, 1, big.sectors) == 0);
        /* sectors 超出 LBA28 时，LBA > 0x0FFFFFFF 必须被拒（不发命令）。 */
        big.sectors = ATA_LBA28_SECTORS + 1u;
        CHECK(ata_read28(&big, ATA_LBA28_MAX + 1u, buf, 1) == ATA_EINVAL);
    }

    /* 未 IDENTIFY 的设备 */
    {
        struct ata_dev fresh;

        ata_dev_init(&fresh, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
        CHECK(ata_read28(&fresh, 0, buf, 1) == ATA_ENODEV);
    }

    /* ERR -> EIO，错误后恢复 */
    fake_setup(&dev);
    fc.fail_cmd = ATA_CMD_READ_SECTORS;
    CHECK(ata_read28(&dev, 5, buf, 1) == ATA_EIO);
    CHECK(dev.errors == 1);
    CHECK(dev.reads == 0);
    fc.fail_cmd = 0;
    CHECK(ata_read28(&dev, 5, buf, 1) == ATA_OK);
    CHECK(sector_matches(5, buf));
    CHECK(dev.reads == 1);

    /* 超时 -> ETIMEDOUT，恢复后仍可读 */
    fake_setup(&dev);
    fc.busy_forever = 1;
    dev.poll_limit = 8;
    CHECK(ata_read28(&dev, 6, buf, 1) == ATA_ETIMEDOUT);
    CHECK(dev.timeouts == 1);
    fc.busy_forever = 0;
    CHECK(ata_read28(&dev, 6, buf, 1) == ATA_OK);
    CHECK(dev.reads == 1);
}

/* ------------------------------------------------------------------------- */
/* 4. 写                                                                     */
/* ------------------------------------------------------------------------- */

static int disk_sector_is(u32 sector, u8 value)
{
    u32 i;

    for (i = 0; i < ATA_SECTOR_SIZE; i++) {
        if (g_disk[sector * ATA_SECTOR_SIZE + i] != value) {
            return 0;
        }
    }
    return 1;
}

static void test_ata_write(void)
{
    struct ata_dev dev;
    u8 buf[8u * ATA_SECTOR_SIZE];
    u8 back[8u * ATA_SECTOR_SIZE];
    u32 i;
    u32 s;
    int ok;

    fill_disk_pattern();
    fake_setup(&dev);

    /* 单扇区写：盘内容真的变了，读回一致 */
    for (i = 0; i < 512u; i++) {
        buf[i] = (u8)(i ^ 0x5Au);
    }
    CHECK(ata_write28(&dev, 10, buf, 1) == ATA_OK);
    CHECK(dev.writes == 1);
    CHECK(memcmp(g_disk + 10u * ATA_SECTOR_SIZE, buf, 512) == 0);

    /* FLUSH CACHE 紧跟 WRITE（此时还没发 READ 命令） */
    CHECK(fc.cmd_log_n >= 3); /* IDENTIFY / WRITE / FLUSH */
    CHECK(fc.cmd_log[fc.cmd_log_n - 1u] == ATA_CMD_FLUSH_CACHE);
    CHECK(fc.cmd_log[fc.cmd_log_n - 2u] == ATA_CMD_WRITE_SECTORS);
    CHECK(dev.writes == 1); /* flush 不增加 writes */

    memset(back, 0, 512);
    CHECK(ata_read28(&dev, 10, back, 1) == ATA_OK);
    CHECK(memcmp(back, buf, 512) == 0);

    /* 8 扇区写 */
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (u8)(i * 3u + 0x11u);
    }
    CHECK(ata_write28(&dev, 20, buf, 8) == ATA_OK);
    CHECK(memcmp(g_disk + 20u * ATA_SECTOR_SIZE, buf, sizeof(buf)) == 0);
    memset(back, 0, sizeof(back));
    CHECK(ata_read28(&dev, 20, back, 8) == ATA_OK);
    CHECK(memcmp(back, buf, sizeof(buf)) == 0);
    CHECK(fc.last_secount == 8);
    CHECK(dev.writes == 2);

    /* LBA 寄存器：0x0A0B0C（fake 盘只有 4096 扇区：越界写被丢弃，只测编码） */
    for (i = 0; i < 512u; i++) {
        buf[i] = (u8)i;
    }
    {
        struct ata_dev wide = dev;

        wide.sectors = 0x02000000u;
        CHECK(ata_write28(&wide, 0x0A0B0Cu, buf, 1) == ATA_OK);
        CHECK(fc.last_lba0 == 0x0C);
        CHECK(fc.last_lba1 == 0x0B);
        CHECK(fc.last_lba2 == 0x0A);
        CHECK(fc.last_devsel == 0xE0);
    }

    /* 参数非法 */
    CHECK(ata_write28(&dev, 0, NULL, 1) == ATA_EINVAL);
    CHECK(ata_write28(&dev, 0, buf, 0) == ATA_EINVAL);
    CHECK(ata_write28(&dev, 0, buf, 9) == ATA_EINVAL);
    CHECK(ata_write28(&dev, FK_SECTORS, buf, 1) == ATA_EINVAL);
    CHECK(ata_write28(&dev, FK_SECTORS - 1u, buf, 2) == ATA_EINVAL);
    CHECK(ata_write28(NULL, 0, buf, 1) == ATA_EINVAL);

    /* ERR：命令在数据阶段前失败，盘内容不变 */
    fake_setup(&dev);
    for (i = 0; i < 512u; i++) {
        buf[i] = 0xCCu;
    }
    s = 30;
    for (i = 0; i < 512u; i++) {
        g_disk[s * ATA_SECTOR_SIZE + i] = (u8)(i + 1u);
    }
    fc.fail_cmd = ATA_CMD_WRITE_SECTORS;
    CHECK(ata_write28(&dev, s, buf, 1) == ATA_EIO);
    CHECK(dev.writes == 0);
    ok = 1;
    for (i = 0; i < 512u; i++) {
        if (g_disk[s * ATA_SECTOR_SIZE + i] != (u8)(i + 1u)) {
            ok = 0;
        }
    }
    CHECK(ok);
    fc.fail_cmd = 0;
    CHECK(ata_write28(&dev, s, buf, 1) == ATA_OK);
    CHECK(memcmp(g_disk + s * ATA_SECTOR_SIZE, buf, 512) == 0);

    /* 超时 */
    fake_setup(&dev);
    fc.busy_forever = 1;
    dev.poll_limit = 8;
    CHECK(ata_write28(&dev, 31, buf, 1) == ATA_ETIMEDOUT);
    CHECK(dev.timeouts == 1);
    fc.busy_forever = 0;
    CHECK(ata_write28(&dev, 31, buf, 1) == ATA_OK);
    CHECK(dev.writes == 1);

    /* 写末扇区（边界） */
    for (i = 0; i < 512u; i++) {
        buf[i] = (u8)(255u - (i & 0xFFu));
    }
    CHECK(ata_write28(&dev, FK_SECTORS - 1u, buf, 1) == ATA_OK);
    CHECK(memcmp(g_disk + (FK_SECTORS - 1u) * ATA_SECTOR_SIZE, buf, 512) == 0);

    /* 全 0xFF 扇区也是合法数据（不做值判断） */
    memset(buf, 0xFF, 512);
    CHECK(ata_write28(&dev, 32, buf, 1) == ATA_OK);
    CHECK(disk_sector_is(32, 0xFFu));
}

/* ------------------------------------------------------------------------- */
/* 5. ata_blk_env                                                            */
/* ------------------------------------------------------------------------- */

static void test_ata_blk_env(void)
{
    struct ata_dev dev;
    const struct blk_env *blk;
    u8 buf[512];
    u32 i;

    fill_disk_pattern();
    ata_blk_reset();
    CHECK(ata_blk_env() == NULL);
    CHECK(ata_blk_bind(NULL) == -1);
    {
        struct ata_dev absent;

        ata_dev_init(&absent, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE, 0);
        CHECK(ata_blk_bind(&absent) == -1); /* 未 IDENTIFY */
    }

    fake_setup(&dev);
    CHECK(ata_blk_bind(&dev) == 0);
    blk = ata_blk_env();
    CHECK(blk != NULL);
    CHECK(blk->read != NULL);
    CHECK(blk->write != NULL);
    CHECK(blk->sectors == FK_SECTORS);
    CHECK(blk->ctx == (void *)&dev);

    memset(buf, 0, sizeof(buf));
    CHECK(blk->read(blk->ctx, 7, buf, 1) == 0);
    for (i = 0; i < 512u; i++) {
        if (buf[i] != pat_byte(7, i)) {
            break;
        }
    }
    CHECK(i == 512u);

    for (i = 0; i < 512u; i++) {
        buf[i] = (u8)(0x80u + (i & 0x3Fu));
    }
    CHECK(blk->write(blk->ctx, 8, buf, 1) == 0);
    CHECK(memcmp(g_disk + 8u * ATA_SECTOR_SIZE, buf, 512) == 0);

    /* 越界/参数 */
    CHECK(blk->read(blk->ctx, FK_SECTORS, buf, 1) == -1);
    CHECK(blk->read(blk->ctx, 0, buf, 0) == -1);
    CHECK(blk->read(blk->ctx, 0, NULL, 1) == -1);
    CHECK(blk->read(blk->ctx, 0, buf, 9) == -1);
    CHECK(blk->write(blk->ctx, FK_SECTORS, buf, 1) == -1);
    CHECK(blk->read(blk->ctx, (u64)ATA_LBA28_MAX + 1u, buf, 1) == -1);
    CHECK(blk->read(NULL, 0, buf, 1) == -1);

    ata_blk_reset();
    CHECK(ata_blk_env() == NULL);
    CHECK(ata_blk_bind(&dev) == 0);
    CHECK(ata_blk_env() != NULL);
    ata_blk_reset();
}

/* ------------------------------------------------------------------------- */
/* 6. FAT16 on fake ATA                                                      */
/* ------------------------------------------------------------------------- */

#define IMG_TOTAL 4096u
#define IMG_SPC 2u
#define IMG_FATS 2u
#define IMG_FAT_SECTORS 16u
#define IMG_ROOT_ENTRIES 64u
#define IMG_RESERVED 1u
#define IMG_ROOT_START (IMG_RESERVED + IMG_FATS * IMG_FAT_SECTORS) /* 33 */
#define IMG_ROOT_DIR_SECTORS 4u
#define IMG_DATA_START (IMG_ROOT_START + IMG_ROOT_DIR_SECTORS) /* 37 */
#define IMG_CLUSTERS ((IMG_TOTAL - IMG_DATA_START) / IMG_SPC)  /* 2029 */

static void img_w16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

static void img_w32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void img_set_fat(u32 cluster, u32 value)
{
    u32 f;

    for (f = 0; f < IMG_FATS; f++) {
        u8 *fat = g_disk + (IMG_RESERVED + f * IMG_FAT_SECTORS) *
                               ATA_SECTOR_SIZE;

        img_w16(fat + cluster * 2u, value);
    }
}

static void img_entry(u32 sector, u32 index, const char *name11, u8 attr,
                      u32 cluster, u32 size)
{
    u8 *slot = g_disk + sector * ATA_SECTOR_SIZE + index * 32u;
    u32 i;

    memset(slot, 0, 32);
    for (i = 0; i < 11u; i++) {
        slot[i] = (u8)name11[i];
    }
    slot[11] = attr;
    img_w16(slot + 20, 0);
    img_w16(slot + 26, cluster & 0xFFFFu);
    img_w32(slot + 28, size);
}

static void img_cluster_data(u32 cluster, u32 offset, const void *data,
                             u32 len)
{
    u8 *p = g_disk + (IMG_DATA_START + (cluster - 2u) * IMG_SPC) *
                            ATA_SECTOR_SIZE +
             offset;

    memcpy(p, data, len);
}

/* 合成一个 4096 扇区（2 MiB？不，4096*512 = 2 MiB）的 FAT16 卷。 */
static void img_format(void)
{
    u8 *s = g_disk;
    u32 f;

    memset(g_disk, 0, FK_BYTES);
    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    memcpy(s + 3, "ASTER   ", 8);
    img_w16(s + 11, 512);
    s[13] = (u8)IMG_SPC;
    img_w16(s + 14, IMG_RESERVED);
    s[16] = (u8)IMG_FATS;
    img_w16(s + 17, IMG_ROOT_ENTRIES);
    img_w16(s + 19, IMG_TOTAL);
    s[21] = 0xF8;
    img_w16(s + 22, IMG_FAT_SECTORS);
    img_w16(s + 24, 18);
    img_w16(s + 26, 2);
    img_w32(s + 28, 0);
    img_w32(s + 32, 0);
    s[36] = 0x80;
    s[37] = 0;
    s[38] = 0x29;
    img_w32(s + 39, 0x4E473234u);
    memcpy(s + 43, "ASTERDISK  ", 11);
    memcpy(s + 54, "FAT16   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;

    img_set_fat(0, 0xFFF8);
    img_set_fat(1, 0xFFFF);
    for (f = 2; f <= 4; f++) {
        img_set_fat(f, 0xFFFF);
    }
    img_entry(IMG_ROOT_START, 0, "ASTERDISK ", 0x08, 0, 0);
    img_entry(IMG_ROOT_START, 1, "HELLO   TXT", 0x20, 2, 18);
    img_entry(IMG_ROOT_START, 2, "DIR        ", 0x10, 4, 0);
    img_entry(IMG_DATA_START + (4u - 2u) * IMG_SPC, 0, ".          ",
              0x10, 4, 0);
    img_entry(IMG_DATA_START + (4u - 2u) * IMG_SPC, 1, "..         ",
              0x10, 0, 0);
    img_entry(IMG_DATA_START + (4u - 2u) * IMG_SPC, 2, "A       TXT",
              0x20, 3, 6);
    img_cluster_data(2, 0, "hello from fat16\r\n", 18);
    img_cluster_data(3, 0, "dir a\n", 6);
}

static int read_all(void *ctx, struct vfs_node *node, u8 *buf, u32 cap,
                    u32 *out_len)
{
    int n;

    (void)ctx;
    n = fat16_ops()->read(ctx, node, 0, buf, cap);
    if (n < 0) {
        return n;
    }
    *out_len = (u32)n;
    return VFS_OK;
}

static void test_ata_fat16(void)
{
    struct ata_dev dev;
    struct ata_dev small;
    struct fat16 fs;
    struct fat16 fs2;
    struct vfs_node *root;
    struct vfs_node *node = NULL;
    struct blk_env blk_small;
    const struct blk_env *blk;
    struct vfs_stat st;
    u8 buf[1024];
    u8 wbuf[600];
    u32 len;
    u32 i;
    int n;
    int stat_ok;

    img_format();
    fake_reset();
    ata_dev_init(&dev, &g_fake_io, FK_IO_BASE, FK_CTRL_BASE,
                 ATA_DRIVE_MASTER);
    CHECK(ata_identify(&dev) == ATA_OK);
    ata_blk_reset();
    CHECK(ata_blk_bind(&dev) == 0);
    blk = ata_blk_env();
    CHECK(blk != NULL);

    fat16_init(&fs);
    CHECK(fat16_mount(&fs, blk) == VFS_OK);
    CHECK(fat16_cluster_count(&fs) == IMG_CLUSTERS);
    CHECK(fat16_bytes_per_cluster(&fs) == IMG_SPC * 512u);
    CHECK(fat16_total_sectors(&fs) == IMG_TOTAL);
    root = fat16_root(&fs);
    CHECK(root != NULL);

    /* 读 HELLO.TXT / DIR/A.TXT，全链路走 ATA PIO */
    CHECK(fat16_ops()->lookup(&fs, root, "HELLO.TXT", 9, &node) == VFS_OK);
    CHECK(node != NULL);
    if (node != NULL) {
        memset(buf, 0, sizeof(buf));
        CHECK(read_all(&fs, node, buf, sizeof(buf), &len) == VFS_OK);
        CHECK(len == 18);
        CHECK(memcmp(buf, "hello from fat16\r\n", 18) == 0);
        CHECK(fat16_ops()->release != NULL);
        fat16_ops()->release(&fs, node);
        node = NULL;
    }
    CHECK(fat16_ops()->lookup(&fs, root, "DIR", 3, &node) == VFS_OK);
    CHECK(node != NULL);
    if (node != NULL) {
        CHECK(fat16_ops()->lookup(&fs, node, "A.TXT", 5, &node) == VFS_OK);
        CHECK(node != NULL);
        memset(buf, 0, sizeof(buf));
        CHECK(read_all(&fs, node, buf, sizeof(buf), &len) == VFS_OK);
        CHECK(len == 6);
        CHECK(memcmp(buf, "dir a\n", 6) == 0);
        fat16_ops()->release(&fs, node);
        node = NULL;
    }

    /* 创建/写/读回/stat/删除 NG.TXT */
    CHECK(fat16_ops()->create(&fs, root, "NG.TXT", 6, VFS_NODE_FILE, 0644,
                              &node) == VFS_OK);
    CHECK(node != NULL);
    for (i = 0; i < sizeof(wbuf); i++) {
        wbuf[i] = (u8)(i * 7u + 0x21u);
    }
    if (node != NULL) {
        CHECK(fat16_ops()->write(&fs, node, 0, wbuf, 600) == 600);
        CHECK(node->size == 600);
        memset(buf, 0, 700);
        CHECK(fat16_ops()->read(&fs, node, 0, buf, 700) == 600);
        CHECK(memcmp(buf, wbuf, 600) == 0);
        fat16_ops()->release(&fs, node);
        node = NULL;
    }
    /* readdir 里读回 NG.TXT 的 stat（8.3 名字 + size=600）。 */
    n = 0;
    stat_ok = 0;
    for (i = 0; i < 4u; i++) {
        char nm[VFS_NAME_MAX];

        if (fat16_ops()->readdir(&fs, root, i, nm, sizeof(nm), &st) != 1) {
            break;
        }
        n++;
        if (nm[0] == 'N' && nm[1] == 'G' && st.type == VFS_NODE_FILE &&
            st.size == 600u) {
            stat_ok = 1;
        }
    }
    CHECK(n == 3); /* HELLO.TXT / DIR / NG.TXT（卷标跳过） */
    CHECK(stat_ok);

    CHECK(fat16_ops()->unlink(&fs, root, "NG.TXT", 6) == VFS_OK);
    CHECK(fat16_ops()->lookup(&fs, root, "NG.TXT", 6, &node) == VFS_ENOENT);
    CHECK(node == NULL);

    /* 写入真的落在 fake 盘上：重新 mount 后 HELLO.TXT 仍可读 */
    CHECK(fat16_mount(&fs2, blk) == VFS_OK);
    CHECK(fat16_ops()->lookup(&fs2, fat16_root(&fs2), "HELLO.TXT", 9,
                              &node) == VFS_OK);
    if (node != NULL) {
        memset(buf, 0, sizeof(buf));
        CHECK(read_all(&fs2, node, buf, sizeof(buf), &len) == VFS_OK);
        CHECK(len == 18);
        CHECK(memcmp(buf, "hello from fat16\r\n", 18) == 0);
        fat16_ops()->release(&fs2, node);
        node = NULL;
    }
    CHECK(g_disk[510] == 0x55 && g_disk[511] == 0xAA);
    CHECK(dev.reads > 0);
    CHECK(dev.writes > 0);

    /* 设备几何小于 BPB -> EINVAL（mount 校验不通过） */
    small = dev;
    small.sectors = 2000;
    blk_small.read = blk->read;
    blk_small.write = blk->write;
    blk_small.sectors = 2000;
    blk_small.ctx = &small;
    fat16_init(&fs2);
    CHECK(fat16_mount(&fs2, &blk_small) == VFS_EINVAL);

    /* 设备读错误 -> EIO */
    fc.fail_cmd = ATA_CMD_READ_SECTORS;
    fat16_init(&fs2);
    CHECK(fat16_mount(&fs2, blk) == VFS_EIO);
    fc.fail_cmd = 0;

    /* 恢复后仍能 mount */
    fat16_init(&fs2);
    CHECK(fat16_mount(&fs2, blk) == VFS_OK);
    ata_blk_reset();
}

/* ------------------------------------------------------------------------- */

int test_ata(void)
{
    test_ata_pure();
    test_ata_identify();
    test_ata_read();
    test_ata_write();
    test_ata_blk_env();
    test_ata_fat16();
    return 0;
}
