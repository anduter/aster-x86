/*
 * ng WP1-4: 物理内存管理器（bitmap PMM）。
 *
 * 设计取舍：
 *   - 位图用两份静态数组（used / reserved），上限 PMM_MAX_PAGES = 1 GiB /
 *     4 KiB = 256 Ki 页，每个 32 KiB。位图不进 PMM 自己管理的内存，避免
 *     “位图页被分配出去再被清零”的 bootstrap 问题；代价是 .bss 多 64 KiB。
 *   - 页号相对 `base`（不是绝对物理地址），这样 host 单测可以把一块普通
 *     内存缓冲区当成“物理内存”，64 位宿主地址也能正常工作。
 *   - 所有对外地址都是“物理地址”；ng 内核的恒等映射让 phys == 虚拟地址，
 *     调用方直接解引用即可。
 *   - reserved 位图记录内核镜像、低 1 MiB、引导结构等永久保留页：free 时
 *     先查 reserved 再查 used，所以“释放内核页”不会被误判成普通释放。
 *   - 分配默认清零（PMM_ZERO_ON_ALLOC=1）：新页主看不到上一任的数据。
 */
#ifndef NG_PMM_H
#define NG_PMM_H

#include "phys.h"
#include "types.h"

#define PMM_MAX_PAGES ((u32)(NG_PHYS_LIMIT / NG_PAGE_SIZE))

/* 返回码 / 拒绝原因 */
#define PMM_OK 0
#define PMM_EINVAL (-1)   /* 参数非法（count=0、未对齐释放） */
#define PMM_ENOMEM (-2)   /* 没有足够空闲页 / 找不到连续段 */
#define PMM_EDOUBLE (-3)  /* double free（页本来就是空闲的） */
#define PMM_ERESERVED (-4) /* 释放保留页（内核镜像/低 1 MiB/引导结构） */
#define PMM_ERANGE (-5)   /* 地址不在 PMM 管理窗口内 */

typedef struct {
    u64 base;      /* 管理窗口起点（页对齐） */
    u64 limit;     /* 管理窗口终点（页对齐，不含） */
    u32 total_pages;
    u32 usable_pages; /* memmap 加进来的可用页（未扣保留区） */
    u32 free_pages;   /* 当前可分配页 */
    u32 rover;        /* first-fit 起点 */
    u64 alloc_count;  /* 成功分配的页数 */
    u64 free_count;   /* 成功归还的页数 */
    u64 fail_count;   /* 分配失败次数 */
    u32 reject_double;
    u32 reject_reserved;
    u32 reject_range;
    u32 reject_align;
} pmm_stats;

/*
 * 初始化管理窗口 [base, limit)。窗口内所有页初始都是 used+reserved；
 * 之后用 pmm_add_usable() 把 RAM 区间放开，用 pmm_reserve() 标记保留区。
 */
int pmm_init(u64 base, u64 limit);

/* 把 [base, base+size) 里的整页标成可用（重复调用幂等）。 */
int pmm_add_usable(u64 base, u64 size);

/* 把 [base, base+size) 里的整页标成 used+reserved（永久不可分配）。 */
int pmm_reserve(u64 base, u64 size);

u64 pmm_alloc_page(void);            /* 失败返回 0（0 号页永远保留） */
u64 pmm_alloc_pages(u32 count);      /* 连续 count 页；失败返回 0 */
int pmm_free_page(u64 phys);         /* PMM_OK / PMM_E* */
int pmm_free_pages(u64 phys, u32 count);

u32 pmm_free_count(void);
u32 pmm_total_pages(void);
u32 pmm_usable_pages(void);
int pmm_owns(u64 phys);       /* 在管理窗口内 + 整页 */
int pmm_is_reserved(u64 phys); /* 保留页（含未加入 usable 的页） */

const pmm_stats *pmm_stat(void);

#endif /* NG_PMM_H */
