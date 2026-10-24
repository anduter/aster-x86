/* =============================================================================
 * block.c - 块设备层 + ram0
 *
 * ram0：256KB 内存盘（512 个 512 字节扇区）。它不是文件系统的一部分，
 * 只是“会读写的扇区数组”；文件系统（fs/ramfs.c 的 sync/mount）把树
 * 序列化到这些扇区上，从而获得一个真正的磁盘格式。
 * ============================================================================= */
#include "block.h"
#include "../../lib/string.h"
#include "../../core/device.h"
#include "../../core/error.h"

static block_device_t blk_table[BLK_MAX];
static uint32_t blk_count_used;

/* ---- ram0 ---- */
#define RAM0_SECTORS 512U
static uint8_t ram0_data[RAM0_SECTORS * BLK_SECTOR];

static int ram0_read(block_device_t *dev, uint32_t lba, void *buf)
{
    if (lba >= dev->sector_count) return -1;
    memcpy(buf, ram0_data + (lba * BLK_SECTOR), BLK_SECTOR);
    return 0;
}

static int ram0_write(block_device_t *dev, uint32_t lba, const void *buf)
{
    if (lba >= dev->sector_count) return -1;
    memcpy(ram0_data + (lba * BLK_SECTOR), buf, BLK_SECTOR);
    return 0;
}

static block_device_t ram0 = {
    "ram0",
    BLK_SECTOR,
    RAM0_SECTORS,
    ram0_read,
    ram0_write
};

int blk_register(block_device_t *dev)
{
    if (dev == 0 || dev->read == 0 || dev->write == 0) return E_INVAL;
    if (blk_lookup(dev->name) != 0) return E_EXIST;
    if (blk_count_used >= BLK_MAX) return E_FULL;

    blk_table[blk_count_used++] = *dev;
    dev_register(dev->name, DEV_BLOCK, 0, 0);   /* 也让设备表能看到 */
    return E_OK;
}

block_device_t *blk_lookup(const char *name)
{
    if (name == 0) return 0;

    for (uint32_t i = 0; i < blk_count_used; i++) {
        const char *a = blk_table[i].name;
        const char *b = name;
        while (*a != '\0' && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &blk_table[i];
    }
    return 0;
}

uint32_t blk_count(void)
{
    return blk_count_used;
}

block_device_t *blk_at(uint32_t index)
{
    return (index < blk_count_used) ? &blk_table[index] : 0;
}

/* ---- 扇区读缓存：8 项写穿透（写时失效对应项） ---- */
#define BLK_CACHE_ENTRIES 8

typedef struct {
    bool            valid;
    block_device_t *dev;
    uint32_t        lba;
    uint8_t         data[BLK_SECTOR];
} blk_cache_t;

static blk_cache_t blk_cache[BLK_CACHE_ENTRIES];
static uint32_t    blk_cache_next;

static int cache_find(block_device_t *dev, uint32_t lba)
{
    for (int i = 0; i < BLK_CACHE_ENTRIES; i++) {
        if (blk_cache[i].valid && blk_cache[i].dev == dev && blk_cache[i].lba == lba) {
            return i;
        }
    }
    return -1;
}

static void cache_store(block_device_t *dev, uint32_t lba, const void *buf)
{
    int i = cache_find(dev, lba);

    if (i < 0) {
        i = (int)blk_cache_next;
        blk_cache_next = (blk_cache_next + 1U) % BLK_CACHE_ENTRIES;
    }
    blk_cache[i].valid = true;
    blk_cache[i].dev = dev;
    blk_cache[i].lba = lba;
    memcpy(blk_cache[i].data, buf, BLK_SECTOR);
}

static void cache_invalidate(block_device_t *dev, uint32_t lba)
{
    int i = cache_find(dev, lba);
    if (i >= 0) blk_cache[i].valid = false;
}

int blk_read(block_device_t *dev, uint32_t lba, void *buf)
{
    int i;

    if (dev == 0 || buf == 0) return E_INVAL;

    i = cache_find(dev, lba);
    if (i >= 0) {
        memcpy(buf, blk_cache[i].data, BLK_SECTOR);
        return E_OK;
    }

    if (dev->read(dev, lba, buf) != 0) return E_INVAL;
    cache_store(dev, lba, buf);
    return E_OK;
}

int blk_write(block_device_t *dev, uint32_t lba, const void *buf)
{
    if (dev == 0 || buf == 0) return E_INVAL;

    if (dev->write(dev, lba, buf) != 0) return E_INVAL;
    cache_invalidate(dev, lba);
    return E_OK;
}

void blk_init(void)
{
    blk_count_used = 0;
    memset(ram0_data, 0, sizeof(ram0_data));
    memset(blk_cache, 0, sizeof(blk_cache));
    blk_cache_next = 0;
    blk_register(&ram0);
}
