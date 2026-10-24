/* =============================================================================
 * block/ata.c - ATA PIO 驱动（主通道 master/slave，LBA28）
 *
 * 主通道两个设备：
 *   ata0 = 主盘（master，drive 0xA0）：XFS1 系统盘
 *   ata1 = 从盘（slave，  drive 0xB0）：FAT16 交换盘
 * 只做最必要的事：IDENTIFY 拿容量，然后按扇区 PIO 读写。
 * ============================================================================= */
#include "ata.h"
#include "block.h"
#include "../../io.h"
#include "../../core/error.h"
#include "../../lib/kprintf.h"
#include "../../lib/string.h"

#define ATA_IO      0x1F0
#define ATA_CTRL    0x3F6

#define ATA_REG_DATA     (ATA_IO + 0)
#define ATA_REG_SECCOUNT (ATA_IO + 2)
#define ATA_REG_LBA0     (ATA_IO + 3)
#define ATA_REG_LBA1     (ATA_IO + 4)
#define ATA_REG_LBA2     (ATA_IO + 5)
#define ATA_REG_DRIVE    (ATA_IO + 6)
#define ATA_REG_STATUS   (ATA_IO + 7)
#define ATA_REG_CMD      (ATA_IO + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

typedef struct {
    block_device_t dev;
    uint8_t        drive;      /* 0xA0 = master, 0xB0 = slave */
    uint16_t       id[256];
    bool           present;
} ata_dev_t;

static ata_dev_t ata_devs[2] = {
    { { "ata0", BLK_SECTOR, 0, 0, 0 }, 0xA0U, { 0 }, false },
    { { "ata1", BLK_SECTOR, 0, 0, 0 }, 0xB0U, { 0 }, false }
};

static void io_delay(void)
{
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL);
}

static int wait_ready(void)
{
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st == 0xFFU) return -1;                 /* 没有设备 */
        if ((st & ATA_SR_BSY) == 0U) return 0;
    }
    return -1;
}

static int wait_drq(void)
{
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint8_t st = inb(ATA_REG_STATUS);
        if ((st & ATA_SR_ERR) != 0U) return -1;
        if ((st & ATA_SR_BSY) == 0U && (st & ATA_SR_DRQ) != 0U) return 0;
    }
    return -1;
}

/* blk_register 会复制设备结构体，所以不能比较指针，只能按名字找上下文 */
static ata_dev_t *dev_ctx(block_device_t *dev)
{
    for (int i = 0; i < 2; i++) {
        const char *a = ata_devs[i].dev.name;
        const char *b = dev->name;

        while (*a != '\0' && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &ata_devs[i];
    }
    return 0;
}

static void ata_select(ata_dev_t *c, uint32_t lba)
{
    outb(ATA_REG_DRIVE, (uint8_t)(0xE0U | c->drive | ((lba >> 24) & 0x0FU)));
    io_delay();
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFFU));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFU));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFU));
}

static int ata_pio_read(block_device_t *dev, uint32_t lba, void *buf)
{
    ata_dev_t *c = dev_ctx(dev);
    uint16_t *p = (uint16_t *)buf;

    if (c == 0 || lba >= dev->sector_count) return -1;

    ata_select(c, lba);
    outb(ATA_REG_CMD, 0x20U);                       /* READ SECTORS */
    if (wait_drq() != 0) return -1;

    for (uint32_t i = 0; i < 256U; i++) p[i] = inw(ATA_REG_DATA);
    return 0;
}

static int ata_pio_write(block_device_t *dev, uint32_t lba, const void *buf)
{
    ata_dev_t *c = dev_ctx(dev);
    const uint16_t *p = (const uint16_t *)buf;

    if (c == 0 || lba >= dev->sector_count) return -1;

    ata_select(c, lba);
    outb(ATA_REG_CMD, 0x30U);                       /* WRITE SECTORS */
    if (wait_drq() != 0) return -1;

    for (uint32_t i = 0; i < 256U; i++) outw(ATA_REG_DATA, p[i]);

    outb(ATA_REG_CMD, 0xE7U);                       /* FLUSH CACHE */
    return (wait_ready() == 0) ? 0 : -1;
}

/* 探测一个设备：IDENTIFY 成功就注册成块设备，返回 E_OK */
static int ata_probe(ata_dev_t *c)
{
    uint32_t sectors;

    outb(ATA_REG_DRIVE, (uint8_t)(0xA0U | c->drive));
    io_delay();

    outb(ATA_REG_SECCOUNT, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);
    outb(ATA_REG_CMD, 0xECU);                       /* IDENTIFY */

    if (inb(ATA_REG_STATUS) == 0U) return E_NOENT;  /* 总线上没设备 */
    if (wait_ready() != 0) return E_NOENT;

    /* 不是 ATA 设备（例如 ATAPI）：LBA1/LBA2 非 0 */
    if (inb(ATA_REG_LBA1) != 0U || inb(ATA_REG_LBA2) != 0U) return E_NOENT;
    if (wait_drq() != 0) return E_NOENT;

    for (uint32_t i = 0; i < 256U; i++) c->id[i] = inw(ATA_REG_DATA);

    sectors = ((uint32_t)c->id[61] << 16) | (uint32_t)c->id[60];
    if (sectors == 0U) return E_NOENT;

    c->dev.sector_size = BLK_SECTOR;
    c->dev.sector_count = sectors;
    c->dev.read = ata_pio_read;
    c->dev.write = ata_pio_write;
    c->present = true;

    if (blk_register(&c->dev) != E_OK) {
        c->present = false;
        return E_NOENT;
    }

    kprintf("ata: %s ready (%u sectors, %u KB)\n", c->dev.name,
            (unsigned int)sectors, (unsigned int)(sectors / 2U));
    return E_OK;
}

int ata_init(void)
{
    int found = 0;

    for (int i = 0; i < 2; i++) {
        if (ata_probe(&ata_devs[i]) == E_OK) found++;
    }
    return (found > 0) ? E_OK : E_NOENT;
}
