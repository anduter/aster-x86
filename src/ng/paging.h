/*
 * ng WP1-2: 4 级页表（PML4/PDPT/PD/PT）+ W^X/NX + SMEP/SMAP。
 *
 * 布局（恒等映射，虚拟 == 物理）：
 *
 *   PML4[0] -> PDPT
 *              PDPT[0] -> PD
 *                          PD[0]   -> PT   [0x000000..0x200000) 4 KiB 页
 *                          PD[1..511]       [0x200000..0x40000000) 2 MiB 大页
 *
 *   - [0x000000, 0x100000)  普通 RAM / VGA / 空洞：RW + NX
 *   - [0x100000, __text_start)  .multiboot + .note.Xen：RO + NX
 *   - [__text_start, __text_end)      .text：R + X（W=0, NX=0）
 *   - [__rodata_end 之前) .rodata：RO + NX
 *   - [__data_start, __kernel_end)   .data/.bss：RW + NX（含栈）
 *   - 上半区 PML4[256..511] 保持为空（不做 higher-half 映射）。
 *
 * 段边界由链接脚本给定，见 src/ng/link.ld 的 __text_start/__text_end 等符号。
 */
#ifndef NG_PAGING_H
#define NG_PAGING_H

#include "types.h"

/* ---- PTE / PDE / PDPTE / PML4E 标志位 ---- */
#define PG_PRESENT (1ULL << 0)  /* P：存在 */
#define PG_WRITE (1ULL << 1)    /* R/W：0=只读，1=可写 */
#define PG_USER (1ULL << 2)     /* U/S：0=内核（本 WP 全部内核页） */
#define PG_PWT (1ULL << 3)      /* write-through */
#define PG_PCD (1ULL << 4)      /* cache disable */
#define PG_ACCESSED (1ULL << 5) /* A */
#define PG_DIRTY (1ULL << 6)    /* D（仅 4 KiB PTE 有效） */
#define PG_PS (1ULL << 7)       /* PS：PD 项为 2 MiB 大页 */
#define PG_GLOBAL (1ULL << 8)   /* G */
#define PG_NX (1ULL << 63)      /* NX：需要 EFER.NXE=1 + 64 位模式 */

/* 物理地址位 12..51；其余位是标志/保留位。 */
#define PG_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ---- helper 返回码 ---- */
#define PG_OK 0
#define PG_EINVAL (-1)  /* 地址/长度不合法（未按 4 KiB 对齐） */
#define PG_ENOMEM (-2)  /* 页表池耗尽 */
#define PG_EEXIST (-3)  /* 目标页已映射 */
#define PG_ENOENT (-4)  /* 目标页未映射 */
#define PG_EBIGPAGE (-5) /* 需要拆分 2 MiB 大页（WP1-4 再做） */

/*
 * 建表 -> 安装（CR3/CR0.WP/CR4.SMEP/SMAP）-> 自检并打印证据行。
 * 返回失败检查项数（0 = 全部通过）。入口要求：长模式、分页已开（引导表）。
 */
int pg_init(void);

/* 走一遍 4 级页表，返回叶项（PD 大页项或 PTE）；未映射返回 0。 */
u64 pg_walk(u64 va);

/*
 * WP1-4：页表页统一从 PMM 申请（4 KiB、清零、物理地址 == 虚拟地址）。
 * 返回 NULL 表示 PMM 耗尽；pg_table_count() 是累计分配页数（证据用）。
 */
void *pg_alloc_table(void);
u32 pg_table_count(void);

/*
 * 显式 helper。flags 只传权限位（PG_WRITE/PG_NX/...），PG_PRESENT 由 helper 加。
 * 只支持 4 KiB 粒度；目标落在 2 MiB 大页里返回 PG_EBIGPAGE。
 */
int pg_map(u64 va, u64 phys, u64 flags);
int pg_unmap(u64 va);
int pg_protect(u64 va, u64 flags);

u64 pg_cr3(void);

/*
 * WP1-3：把 CR0/CR4/EFER 的实际状态暴露给 trap/syscall 初始化（clac 是否可用、
 * 缺页报告里的 SMAP 上下文）。都在 pg_init() 之后有效。
 */
int pg_nx_on(void);
int pg_smep_on(void);
int pg_smap_on(void);

#endif /* NG_PAGING_H */
