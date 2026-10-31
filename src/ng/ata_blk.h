/*
 * ng WP2-4: ATA 块设备 -> L1 blk_env 适配（内核接线层，src/ng）。
 *
 * 绑定一个已 IDENTIFY 成功的 struct ata_dev，暴露 FAT16 需要的
 * read/write/sectors/ctx。适配层只依赖 types.h / ata.h / blk.h，
 * 不依赖内核端口 IO，因此 host 单测可编译同一份实现。
 */
#ifndef NG_ATA_BLK_H
#define NG_ATA_BLK_H

#include "ata.h"
#include "blk.h"
#include "types.h"

/* 成功返回 0；dev 为空 / 未 IDENTIFY / sectors=0 返回 -1。 */
int ata_blk_bind(struct ata_dev *dev);

/* 已绑定的 blk_env；未绑定时返回 NULL。 */
const struct blk_env *ata_blk_env(void);

/* 解绑（host 单测的每个子用例之间用；内核不需要）。 */
void ata_blk_reset(void);

#endif /* NG_ATA_BLK_H */
