#ifndef BLOCK_ATA_H
#define BLOCK_ATA_H

/* ATA PIO 驱动：主通道 master（LBA28），注册成块设备 "ata0"。
 * 返回 E_OK 表示识别到磁盘；没有磁盘返回 E_NOENT。 */
int ata_init(void);

#endif
