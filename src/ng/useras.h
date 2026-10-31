/*
 * ng WP1-7: 用户地址空间（独立 PML4）。
 *
 * 布局（本 WP 只用一个用户地址空间，但接口按多 AS 设计）：
 *
 *   新 PML4
 *     [0]      -> 新的 PDPT（本 AS 私有）
 *                  [0]   -> 内核 PD（与内核共享，恒等映射 0..1 GiB，U=0）
 *                  [1..511] -> 按需分配（用户页；本 WP 的用户 VA 都落在
 *                              1..2 GiB 的 PDPT[1]）
 *     [1..255] -> 复制内核 PML4 的同名项（当前内核只用 [0]，复制是为了将来
 *                 内核映射增长时用户 AS 仍然一致）
 *     [256..511] 清零（不做 higher-half）
 *
 * 为什么 PDPT 要**新建**而不是直接共享内核的：WP1-3 在 0x40000000 映射过
 * 静态用户页，如果共享内核 PDPT，ring3 程序的段就会和 WP1-3 的页表项撞车；
 * 新建 PDPT 后，用户页只出现在用户 AS 里，PDPT[0] 仍然指向内核 PD（只读共
 * 享 0..1 GiB），所以切 CR3 之后内核代码/栈/页表页照常可访问。
 *
 * 用户窗口选 [0x40000000, 0x7FFFFFFFFF]（<512 GiB）：整段都在 PML4[0] 的
 * 子树里，销毁 AS 时可以只回收自己的子树（PDPT[1..511]），绝不会 free 到
 * 内核共享页表。ELF 加载器用同一个窗口（ELF_USER_VA_*）。
 *
 * 栈：16 KiB（4 页）+ 下方一个未映射的守护页（不显式建项，天然 EFAULT）。
 */
#ifndef NG_USERAS_H
#define NG_USERAS_H

#include "paging.h"
#include "types.h"

/* 用户加载窗口：PML4[0] 覆盖的低半区 [1 GiB, 512 GiB)。 */
#define NG_USER_VA_MIN 0x0000000040000000ULL
#define NG_USER_VA_MAX 0x0000007FFFFFFFFFULL

/* 用户栈：基址 1 GiB + 1 MiB；4 页 = 16 KiB；下面一页是守护页。 */
#define NG_USER_STACK_BASE 0x0000000040100000ULL
#define NG_USER_STACK_SIZE 0x4000ULL
#define NG_USER_STACK_TOP (NG_USER_STACK_BASE + NG_USER_STACK_SIZE)
#define NG_USER_STACK_GUARD (NG_USER_STACK_BASE - 0x1000ULL)

#define AS_OK 0
#define AS_EINVAL (-1)
#define AS_ENOMEM (-2)
#define AS_ENOENT (-3)

struct user_as {
    u64 *pml4;      /* 内核虚拟地址（恒等映射，虚拟 == 物理） */
    u64 pml4_phys;  /* CR3 用 */
    u32 table_pages;/* 本 AS 分配的页表页数（pml4 + pdpt + pd/pt） */
    u32 user_pages; /* 当前映射的用户 4 KiB 页数 */
    u64 map_lo;     /* 已映射范围（证据/自检用） */
    u64 map_hi;
    int active;     /* 是否正在被 CR3 使用 */
};

/* 建 AS：新 PML4 + 新 PDPT，PDPT[0] 复用内核 PD。失败时回滚已分配页。 */
int as_create(struct user_as *as);

/* 销毁 AS：先确保切回内核表，再回收自己的页表页与叶页。 */
void as_destroy(struct user_as *as);

/* 把一个 4 KiB 物理页映射到 va（flags 用 PG_*，PG_PRESENT 由 helper 加）。 */
int as_map(struct user_as *as, u64 va, u64 phys, u64 flags);

/* 解除 va 的映射并 free 它的物理页（ELF 回滚路径用）。 */
int as_unmap_free(struct user_as *as, u64 va);

u64 as_walk(const struct user_as *as, u64 va);

/* 切换 CR3 到该 AS / 回内核表。返回 AS_OK 或 AS_EINVAL。 */
int as_switch(struct user_as *as);
void as_switch_kernel(void);
struct user_as *as_current(void);

#endif /* NG_USERAS_H */
