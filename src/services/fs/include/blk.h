/*
 * ng WP2-2: L1 块设备抽象（扇区读写）。
 *
 * 目的：让文件系统只认识“扇区数组”，不认识内核设备模型 / MMIO / DMA：
 *   - 内核：绑到嵌入 .rodata 的 1.44MB FAT16 镜像（启动时复制到 PMM 页，
 *     恒等映射后直接按物理地址读写），见 src/ng/fat_ramdisk.c；
 *   - host 单测：绑到一块静态字节数组（tests/unit-ng/test_fat16.c）；
 *   - WP2-4 的 ATA/virtio-blk 驱动只要实现同样的三个字段即可复用 FAT16。
 *
 * 约定：
 *   - 一次调用读/写 count 个连续扇区，扇区号从 0 开始；
 *   - read/write 返回 0 = 成功，非 0 = 失败（FAT16 统一映射成 EIO）；
 *   - sectors 是设备总扇区数，mount 用它拒绝“几何超出设备”的 BPB；
 *   - ctx 由绑定方自己解释，FAT16 只原样透传。
 */
#ifndef BLK_H
#define BLK_H

#include "types.h"

struct blk_env {
    int (*read)(void *ctx, u64 sector, void *buf, u32 count);
    int (*write)(void *ctx, u64 sector, const void *buf, u32 count);
    u64 sectors;
    void *ctx;
};

#endif /* BLK_H */
