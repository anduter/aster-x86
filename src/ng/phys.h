/*
 * ng WP1-4: 物理内存常量（PMM / memmap / heap 共用）。
 *
 * NG_PHYS_LIMIT 是当前内核恒等映射的上界（PDPT[0] 覆盖 1 GiB）。PMM 只管理
 * 这一段：返回超过它的物理页没有任何映射可用。以后扩到 4 GiB 时，改这里 +
 * PMM 位图大小 + 页表建表范围三处。
 */
#ifndef NG_PHYS_H
#define NG_PHYS_H

#include "types.h"

#define NG_PAGE_SIZE 4096ULL
#define NG_PAGE_SHIFT 12
#define NG_PHYS_LIMIT 0x40000000ULL /* 1 GiB */

#define NG_PAGE_ALIGN_DOWN(x) ((u64)(x) & ~(NG_PAGE_SIZE - 1ULL))
#define NG_PAGE_ALIGN_UP(x) (((u64)(x) + NG_PAGE_SIZE - 1ULL) & ~(NG_PAGE_SIZE - 1ULL))
#define NG_IS_PAGE_ALIGNED(x) (((u64)(x) & (NG_PAGE_SIZE - 1ULL)) == 0)

#endif /* NG_PHYS_H */
