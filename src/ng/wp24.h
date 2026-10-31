/*
 * ng WP2-4: 块设备（ATA PIO + 真实磁盘 FAT16）内核自检入口。
 */
#ifndef NG_WP24_H
#define NG_WP24_H

/* 跑 ATA identify/读/写/超时 + /mnt/disk FAT16 挂载与文件读写自检。
 * 返回失败项数（0 = 全部通过；证据行由本模块打印）。 */
int wp24_selftest(void);

#endif /* NG_WP24_H */
