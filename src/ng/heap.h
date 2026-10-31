/*
 * ng WP1-4: 内核堆（first-fit + 相邻空闲块合并）。
 *
 * 结构：
 *   - 堆由若干个“region”组成，每个 region 是一段连续的物理页（恒等映射下
 *     直接可读写）。region 只能从 PMM 整页申请；第一次分配不够时按需扩展，
 *     至少 16 页（64 KiB）。
 *   - 每个 region 内部切成一串 heap_block（48 字节头 + payload），所有 region
 *     的块按地址串成一个双向链表；只在同一个 region 内做相邻合并（不同 region
 *     的块地址即使碰巧连续也不合并，避免“跨 region 块”带来的边界判断）。
 *   - 分配 = 首次适配；payload 16 字节对齐。kmalloc_aligned 在块内前移块头，
 *     把对齐前剩下的部分切成一个空闲块。
 *   - kfree/krealloc 先按 region 校验指针，再检查 magic/flags：野指针、重复
 *     释放、已合并掉的旧块头都会被拒绝并记账，而不是把堆搞坏。
 *
 * host 单测用 NG_HOST_TEST 把“向 PMM 要页”换成测试提供的 fake 分配器
 * （见 tests/unit-ng/test_heap.c），其余逻辑完全同一份。
 */
#ifndef NG_HEAP_H
#define NG_HEAP_H

#include "types.h"

#define HEAP_OK 0
#define HEAP_EINVAL (-1)  /* 野指针 / 未对齐 / 参数非法 */
#define HEAP_ENOMEM (-2)  /* 扩堆失败 */
#define HEAP_EDOUBLE (-3) /* 重复释放 */
#define HEAP_ECORRUPT (-4) /* 堆结构自检失败 */

#define HEAP_HDR_SIZE 48u
#define HEAP_MIN_PAYLOAD 16u
#define HEAP_GROW_MIN_PAGES 16u
#define HEAP_MAX_REGIONS 16u

/* 初始化/重置（不释放已经拿到的 region；内核启动时调用一次）。 */
void heap_init(void);

/* 把一段内存交给堆（对齐由内部处理）。返回 HEAP_OK / HEAP_E*。 */
int heap_add_region(void *base, u64 size);

void *kmalloc(u64 size);
void *kmalloc_aligned(u64 size, u64 align);
int kfree(void *ptr); /* HEAP_OK / HEAP_E*；kfree(NULL) 返回 HEAP_OK */
void *krealloc(void *ptr, u64 size);

/* 诊断/自检 */
u32 heap_check(void); /* 0 = 健康，非 0 = 结构错误编号 */
u64 heap_free_bytes(void);
u64 heap_live_bytes(void);
u32 heap_live_allocs(void);
u32 heap_block_count(void);
u32 heap_region_count(void);
u32 heap_double_free_count(void);
u32 heap_bad_free_count(void);

#endif /* NG_HEAP_H */
