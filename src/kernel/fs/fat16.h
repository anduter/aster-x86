#ifndef FS_FAT16_H
#define FS_FAT16_H

#include "fs.h"

/* FAT16 交换盘：挂在第二块硬盘（ata1）上，以 "fat:" 驱动器暴露给 shell。
 * 只实现根目录 + 8.3 短名（不做 LFN），支持读、写、删除、列目录、stat。 */
int  fat16_mount(const char *blk_name, const char *drive);
bool fat16_mounted(void);
const char *fat16_drive(void);

/* 路径是 "fat:" 之后的相对路径（可带前导 '/'），只接受根目录 */
int  fat16_listdir(const char *rel, uint32_t index, char *name, uint32_t max,
                   fs_stat_t *out);
int  fat16_stat(const char *rel, fs_stat_t *out);
int  fat16_unlink(const char *rel);
int  fat16_mkdir(const char *rel);
/* 根目录内 8.3 改名；跨目录/长名/目标已存在分别返回 E_INVAL/E_EXIST */
int  fat16_rename(const char *oldrel, const char *newrel);

/* 打开/读写/关闭：返回 >=0 的 FAT 句柄下标 */
int  fat16_open(const char *rel, uint32_t flags);
int  fat16_read(int h, void *buf, uint32_t len);
int  fat16_write(int h, const void *buf, uint32_t len);
int  fat16_close(int h);

#endif
