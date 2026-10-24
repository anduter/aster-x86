#ifndef BLOCK_H
#define BLOCK_H

#include "../../types.h"

/* 块设备：按扇区读写。设备表里用 DEV_BLOCK 类别注册，
 * 但读写接口是强类型的（不走字符设备的 putc/getc）。 */
#define BLK_NAME_MAX 16
#define BLK_MAX      4
#define BLK_SECTOR   512U

typedef struct block_device {
    char     name[BLK_NAME_MAX];
    uint32_t sector_size;
    uint32_t sector_count;
    int    (*read)(struct block_device *dev, uint32_t lba, void *buf);
    int    (*write)(struct block_device *dev, uint32_t lba, const void *buf);
} block_device_t;

/* 注册内建块设备（ram0 是 256KB 内存盘；ata0 由 ata_init 注册） */
void blk_init(void);

#include "ata.h"

int blk_register(block_device_t *dev);
block_device_t *blk_lookup(const char *name);
uint32_t blk_count(void);
block_device_t *blk_at(uint32_t index);

/* 带边界检查的读写包装 */
int blk_read(block_device_t *dev, uint32_t lba, void *buf);
int blk_write(block_device_t *dev, uint32_t lba, const void *buf);

#endif
