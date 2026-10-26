/* tests/unit/shim/include/dev.h - host 单元测试用的最小 dev.h */
#ifndef DEV_H_MODULE_H
#define DEV_H_MODULE_H

#include "../../../../src/kernel/include/types.h"

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

int blk_register(block_device_t *dev);
block_device_t *blk_lookup(const char *name);
uint32_t blk_count(void);
block_device_t *blk_at(uint32_t index);
int blk_read(block_device_t *dev, uint32_t lba, void *buf);
int blk_write(block_device_t *dev, uint32_t lba, const void *buf);

#endif /* DEV_H_MODULE_H */
