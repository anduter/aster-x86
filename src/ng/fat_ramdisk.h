/*
 * ng WP2-2: 嵌入式 FAT16 镜像 -> PMM RAM 盘（L0 侧的 blk_env 绑定）。
 *
 * build-ng.ps1 用 tools/mkfat-ng.ps1 生成 1.44MB FAT16 镜像，再以 .incbin
 * 嵌进内核 .rodata（ng_fat16_img_start..end）。启动时把这份镜像复制到
 * PMM 页里（可写），并暴露一个 struct blk_env：
 *   - read/write 按扇区在页数组里定位（每页 4 KiB = 8 个扇区）；
 *   - ng 的恒等映射让 pmm_alloc_page() 返回的物理地址可以直接当指针用；
 *   - FAT16 只看到 blk_env，不依赖 PMM/内核头（L1 分层约束）。
 *
 * 失败：任一页分配失败会归还已分配的页并返回 -1；重复调用幂等。
 */
#ifndef NG_FAT_RAMDISK_H
#define NG_FAT_RAMDISK_H

#include "blk.h"
#include "types.h"

/* 把嵌入镜像复制到 PMM 页并绑定 blk_env；重复调用幂等（返回 0）。 */
int fat_ramdisk_init(void);

/* 初始化成功返回 blk_env（sectors = 2880，1.44MB）；否则 NULL。 */
const struct blk_env *fat_ramdisk_env(void);

/* 诊断：RAM 盘页数 / 扇区数 / 是否可写。 */
u32 fat_ramdisk_pages(void);
u64 fat_ramdisk_sectors(void);

#endif /* NG_FAT_RAMDISK_H */
