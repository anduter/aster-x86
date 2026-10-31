/*
 * ng WP2-2: 嵌入 FAT16 镜像的 PMM RAM 盘实现。
 *
 * 镜像以 1 字节数组的形式在 .rodata（ng_fat16_img_start..ng_fat16_img_end），
 * 启动时复制到 4 KiB PMM 页；之后 FAT16 的 create/write 只改 PMM 副本，
 * 不改只读的 .rodata。页数组是静态的（镜像 1.44MB = 360 页）。
 */
#include "fat_ramdisk.h"

#include "pmm.h"
#include "types.h"

#define FAT_RAMDISK_MAX_PAGES 360u
#define FAT_RAMDISK_PAGE_SIZE 4096u
#define FAT_RAMDISK_SECTORS_PER_PAGE 8u

extern const u8 ng_fat16_img_start[];
extern const u8 ng_fat16_img_end[];

static u8 *g_pages[FAT_RAMDISK_MAX_PAGES];
static u32 g_page_count;
static u64 g_sectors;
static struct blk_env g_blk;
static u32 g_ready;

static void copy_bytes(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void zero_bytes(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

/* sector -> (页号, 页内偏移)；调用方保证 sector < g_sectors。 */
static u32 sector_page(u64 sector)
{
    return (u32)(sector / FAT_RAMDISK_SECTORS_PER_PAGE);
}

static u32 sector_offset(u64 sector)
{
    return (u32)((sector % FAT_RAMDISK_SECTORS_PER_PAGE) * 512u);
}

static int ram_read(void *ctx, u64 sector, void *buf, u32 count)
{
    u8 *dst = (u8 *)buf;
    u32 i;

    (void)ctx;
    if (buf == NULL || count == 0 || g_ready == 0 ||
        sector + (u64)count > g_sectors) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u64 s = sector + (u64)i;
        u32 page = sector_page(s);

        copy_bytes(dst + (u64)i * 512u, g_pages[page] + sector_offset(s), 512u);
    }
    return 0;
}

static int ram_write(void *ctx, u64 sector, const void *buf, u32 count)
{
    const u8 *src = (const u8 *)buf;
    u32 i;

    (void)ctx;
    if (buf == NULL || count == 0 || g_ready == 0 ||
        sector + (u64)count > g_sectors) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u64 s = sector + (u64)i;
        u32 page = sector_page(s);

        copy_bytes(g_pages[page] + sector_offset(s), src + (u64)i * 512u, 512u);
    }
    return 0;
}

int fat_ramdisk_init(void)
{
    u64 size;
    u32 pages;
    u32 i;

    if (g_ready != 0) {
        return 0;
    }
    size = (u64)(ng_fat16_img_end - ng_fat16_img_start);
    if (size == 0 || (size % 512u) != 0) {
        return -1;
    }
    pages = (u32)((size + FAT_RAMDISK_PAGE_SIZE - 1u) / FAT_RAMDISK_PAGE_SIZE);
    if (pages == 0 || pages > FAT_RAMDISK_MAX_PAGES) {
        return -1;
    }
    for (i = 0; i < pages; i++) {
        u64 phys = pmm_alloc_page();

        if (phys == 0) {
            u32 k;

            for (k = 0; k < i; k++) {
                (void)pmm_free_page((u64)(usize)g_pages[k]);
                g_pages[k] = NULL;
            }
            return -1;
        }
        g_pages[i] = (u8 *)(usize)phys;
        zero_bytes(g_pages[i], FAT_RAMDISK_PAGE_SIZE);
    }
    for (i = 0; i < pages; i++) {
        u64 off = (u64)i * FAT_RAMDISK_PAGE_SIZE;
        u64 left = size - off;
        u32 chunk = (left > FAT_RAMDISK_PAGE_SIZE)
                        ? (u32)FAT_RAMDISK_PAGE_SIZE
                        : (u32)left;

        copy_bytes(g_pages[i], ng_fat16_img_start + off, chunk);
    }
    g_page_count = pages;
    g_sectors = size / 512u;
    g_blk.read = ram_read;
    g_blk.write = ram_write;
    g_blk.sectors = g_sectors;
    g_blk.ctx = &g_blk;
    g_ready = 1;
    return 0;
}

const struct blk_env *fat_ramdisk_env(void)
{
    return (g_ready != 0) ? &g_blk : NULL;
}

u32 fat_ramdisk_pages(void)
{
    return g_page_count;
}

u64 fat_ramdisk_sectors(void)
{
    return g_sectors;
}
