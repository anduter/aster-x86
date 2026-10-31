/*
 * ng WP2-2: FAT16 内核自检入口。
 *
 * 返回失败项数（0 = PASS）；main.c 负责打印 "ng: WP2-2 selftest PASS/FAIL"。
 * 自检做三件事：
 *   1. 把嵌入 .rodata 的 1.44MB 镜像复制到 PMM RAM 盘，挂到 /mnt/fat；
 *   2. 读 HELLO.TXT / DIR/A.TXT、listdir、创建 NEW.TXT 写入/stat/改名/unlink；
 *   3. 用合成小镜像验证坏 BPB 拒绝与簇链循环/截断/坏簇/越界检测。
 * 自检结束后 /mnt/fat 保持挂载，ring3 demo 直接用它。
 */
#ifndef NG_FAT16_SELFTEST_H
#define NG_FAT16_SELFTEST_H

int fat16_selftest(void);

#endif /* NG_FAT16_SELFTEST_H */
