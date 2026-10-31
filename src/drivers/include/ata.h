/*
 * ng WP2-4: L1D ATA PIO 驱动（legacy primary/secondary 端口 0x1F0/0x3F6、0x170/0x376）。
 *
 * 定位：第一个真正的块设备驱动。它只认识“ATA 寄存器 + PIO 数据端口”，不认识
 * 内核页表/PMM/VFS：
 *   - 端口 IO 通过 `struct ata_io` 注入：内核（src/ng/wp24_selftest.c）注入
 *     inb/outb/inw/outw，host 单测注入 fake 寄存器文件 + fake 盘（tests/unit-ng/
 *     test_ata.c）；
 *   - `struct blk_env` 适配（ata_blk_env）放在 src/ng/ata_blk.c，因为 L1D
 *     **不能**向上依赖 L1 的 blk.h（check-domains: L1D 只允许 L0/A/B）。
 *
 * 能力（WP2-4 出口）：
 *   - IDENTIFY DEVICE（0xEC）：解析型号、LBA28 扇区数、LBA/LBA48 支持位；
 *   - READ SECTOR(S)（0x20）/ WRITE SECTOR(S)（0x30）：LBA28 编码，
 *     一次 1..8 个扇区（0x1F2 = 1..8；FAT16 每次调用只用 1）；
 *   - WRITE 末尾发 FLUSH CACHE（0xE7），保证数据落盘；
 *   - 所有等待都是**有界轮询**（`poll_limit` 次，每次 400ns 延时读），
 *     超时返回 ATA_ETIMEDOUT、ERR/DF 返回 ATA_EIO，不 panic、不无限等；
 *   - LBA/参数校验：count 0 或 > 8、跨越设备末尾、LBA > LBA28 上限 -> EINVAL。
 *
 * 明确不做（留给后续 WP）：
 *   - LBA48 / CHS / ATAPI / DMA / 中断（PIO 轮询足够 D0）；
 *   - PCI 资源分配/IRQ 路由：QEMU PIIX3 IDE 走兼容（legacy）端口，PCI 只用来
 *     确认 8086:7010 存在（见 src/ng/wp24_selftest.c）；
 *   - 分区表：镜像本身就是 FAT16 卷（无 MBR），FAT16 从 0 号扇区读 BPB。
 */
#ifndef NG_ATA_H
#define NG_ATA_H

#include "types.h"

/* ---- legacy 端口 --------------------------------------------------------- */
#define ATA_PRIMARY_IO_BASE 0x1F0u
#define ATA_PRIMARY_CTRL_BASE 0x3F6u
#define ATA_SECONDARY_IO_BASE 0x170u
#define ATA_SECONDARY_CTRL_BASE 0x376u

/* 命令块寄存器（相对 io_base）。 */
#define ATA_REG_DATA 0x00u     /* 16 位 PIO 数据口 */
#define ATA_REG_ERROR 0x01u    /* 读 */
#define ATA_REG_FEATURES 0x01u /* 写 */
#define ATA_REG_SECCOUNT0 0x02u
#define ATA_REG_LBA0 0x03u
#define ATA_REG_LBA1 0x04u
#define ATA_REG_LBA2 0x05u
#define ATA_REG_HDDEVSEL 0x06u
#define ATA_REG_COMMAND 0x07u
#define ATA_REG_STATUS 0x07u

/* 控制块寄存器（相对 ctrl_base）。 */
#define ATA_REG_ALTSTATUS 0x00u /* 读；不清理中断，用于 400ns 延时 */
#define ATA_REG_DEVCTL 0x00u    /* 写 */
#define ATA_REG_DRIVEADDR 0x01u

/* 状态寄存器位。 */
#define ATA_SR_ERR 0x01u
#define ATA_SR_DRQ 0x08u
#define ATA_SR_DF 0x20u
#define ATA_SR_DRDY 0x40u
#define ATA_SR_BSY 0x80u

/* 命令。 */
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_FLUSH_CACHE 0xE7u
#define ATA_CMD_IDENTIFY 0xECu

/* 设备选择（LBA 模式：0xE0 | drive<<4 | lba[27:24]）。 */
#define ATA_DRIVE_MASTER 0u
#define ATA_DRIVE_SLAVE 1u
#define ATA_LBA_SELECT 0xE0u
#define ATA_DEVCTL_NIEN 0x02u /* nIEN=1：不产生 IRQ（本驱动纯轮询） */

/* 限制。 */
#define ATA_SECTOR_SIZE 512u
#define ATA_MAX_SECTORS_PER_CMD 8u
#define ATA_LBA28_MAX 0x0FFFFFFFu /* 可寻址的最后一个 LBA */
#define ATA_LBA28_SECTORS 0x10000000u /* LBA28 能表示的最大扇区数 */
#define ATA_IDENTIFY_WORDS 256u
#define ATA_MODEL_LEN 41u /* IDENTIFY 型号 40 字符 + NUL */
#define ATA_DEFAULT_POLL_LIMIT 1000000u

/* 返回值：0 = 成功；其余为负的驱动局部码（调用方决定映射成哪个 errno）。 */
#define ATA_OK 0
#define ATA_EIO (-1)      /* 设备报 ERR/DF，或 PIO 传输不完整 */
#define ATA_ETIMEDOUT (-2) /* 有界轮询用完（BSY 不落 / DRQ 不来） */
#define ATA_ENODEV (-3)    /* 无设备 / 非 ATA（ATAPI、浮空总线） */
#define ATA_EINVAL (-4)    /* 参数非法（空指针、count、LBA 越界） */

/*
 * 端口 IO 注入。ctx 由调用方解释（host 单测放 fake 控制器指针）。
 * 四个函数都必须非 NULL（ata_dev_init 之后由 ata_identify 检查）。
 */
struct ata_io {
    u8 (*in8)(void *ctx, u16 port);
    void (*out8)(void *ctx, u16 port, u8 value);
    u16 (*in16)(void *ctx, u16 port);
    void (*out16)(void *ctx, u16 port, u16 value);
    void *ctx;
};

/* 一个 ATA 设备（主/从）的驱动状态。调用方分配，驱动不分配内存。 */
struct ata_dev {
    const struct ata_io *io;
    u16 io_base;
    u16 ctrl_base;
    u8 drive;  /* ATA_DRIVE_MASTER / ATA_DRIVE_SLAVE */
    u8 present;
    u8 lba28;
    u8 lba48; /* IDENTIFY word83 bit10；仅诊断，不做 LBA48 传输 */

    u32 sectors; /* LBA28 总扇区数（IDENTIFY word60/61） */
    u32 poll_limit;

    /* 诊断/自检计数 */
    u32 identify_loops; /* IDENTIFY 等 BSY 落地的轮询次数 */
    u32 timeouts;
    u32 errors;
    u32 reads;
    u32 writes;

    char model[ATA_MODEL_LEN];
};

/* 纯函数：HDDEVSEL 字节（0xE0 | drive<<4 | lba[27:24]）。drive 只取 bit0。 */
u8 ata_devsel(u8 drive, u32 lba);

/* 纯函数：LBA28 范围校验（1..ATA_MAX_SECTORS_PER_CMD，且整段落在设备内）。 */
int ata_lba_range_ok(u32 lba, u32 count, u32 sectors);

/* 纯函数：状态分类，便于单测断言。1=BSY，2=ERR/DF，3=DRQ 就绪，0=其它。 */
int ata_status_class(u8 status);

/*
 * 初始化驱动状态（不访问硬件）。poll_limit 默认 ATA_DEFAULT_POLL_LIMIT，
 * 单测可调小；io_base/ctrl_base 传 0 表示用 primary legacy 端口。
 */
void ata_dev_init(struct ata_dev *dev, const struct ata_io *io, u16 io_base,
                  u16 ctrl_base, u8 drive);

/*
 * IDENTIFY DEVICE：成功 fill present/lba28/lba48/sectors/model。
 * 失败：ENODEV（无设备/ATAPI/浮空）、ETIMEDOUT（BSY 不落）、EIO（ERR/DF）、
 * EINVAL（空参数/io 不全）。失败时 present=0、sectors=0。
 */
int ata_identify(struct ata_dev *dev);

/* 读 count 个扇区（LBA28）；成功返回 ATA_OK，失败返回负码。 */
int ata_read28(struct ata_dev *dev, u32 lba, void *buf, u32 count);

/* 写 count 个扇区（LBA28），末尾 FLUSH CACHE；成功返回 ATA_OK。 */
int ata_write28(struct ata_dev *dev, u32 lba, const void *buf, u32 count);

/* 发 FLUSH CACHE 并等完成（0xE7）。 */
int ata_flush(struct ata_dev *dev);

#endif /* NG_ATA_H */
