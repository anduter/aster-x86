#ifndef MM_PMM_H
#define MM_PMM_H

#define PAGE_SIZE    4096
#define PAGE_SHIFT   12

/* E820 内存类型 */
#define E820_TYPE_FREE     1   /* 可用内存 */
#define E820_TYPE_RESERVED 2   /* 保留区域 */
#define E820_TYPE_ACPI     3   /* ACPI 表 */
#define E820_TYPE_NVS      4   /* ACPI NVS */

/* E820 条目结构（24 字节，必须与 loader/sysinfo.asm 保持一致） */
typedef struct {
    unsigned long long base;
    unsigned long long length;
    unsigned int type;
    unsigned int acpi_attr;
} __attribute__((packed)) e820_entry_t;

/* 物理内存管理器状态 */
typedef struct {
    unsigned char* bitmap;       /* 页位图，1 位对应 1 页 */
    unsigned int   phys_base;    /* 页号 0 对应的物理地址（即位图所在处，通常 2MB） */
    unsigned int   total_pages;  /* 管理的总页数 */
    unsigned int   free_pages;   /* 当前空闲页数 */
} PhysicalMemoryManager;

extern PhysicalMemoryManager pmm;

/* 非法释放等异常累计次数（供自检判断内存管理是否健康） */
extern unsigned int pmm_faults;

/* 分配/释放记账（诊断与自检）：成功分配的页数、失败次数、真正归还页数、
 * double-free 次数、位图已用但引用计数为 0 的次数。 */
extern unsigned int pmm_alloc_count;
extern unsigned int pmm_alloc_fail;
extern unsigned int pmm_free_count;
extern unsigned int pmm_double_free;
extern unsigned int pmm_ref_anomalies;

/* 毒化开关：默认开启（分配清零 0x00 / 释放填 0xAA），
 * 构建时定义 PMM_NO_POISON 可关闭。 */
#if !defined(PMM_NO_POISON)
#  ifndef PMM_POISON
#    define PMM_POISON 1
#  endif
#endif
#define PMM_POISON_ALLOC 0x00U
#define PMM_POISON_FREE  0xAAU

/* 基础初始化：bitmap_addr 是位图地址，mem_size 是总内存字节数 */
void pmm_init(unsigned char* bitmap_addr, unsigned int mem_size);

/* 把一段物理内存标记为已用 */
void pmm_mark_range(unsigned long long base, unsigned long long length);

/* 按 E820 表把保留区域标记为已用 */
void pmm_init_e820(e820_entry_t* e820_table, unsigned int entry_count);

/* 申请/释放单页 */
void* pmm_alloc_page(void);
void  pmm_free_page(void* phys_addr);

/* 申请/释放连续多页 */
void* pmm_alloc_pages(unsigned int count);
void  pmm_free_pages(void* base_addr, unsigned int count);

/* 物理页引用计数（COW fork 用）：share 加一，free 减一，归零才真正回收 */
unsigned int pmm_page_ref(unsigned int phys);
void         pmm_page_share(unsigned int phys);
int          pmm_is_used(unsigned int phys);

/* 该物理范围是否完整落在 PMM 管理区（= PMM 拥有的 RAM 页）。
 * 用于 vmm 拆除用户区时区分“PMM 页”和 MMIO 设备物理页：
 * 后者绝不能交给 pmm_free_pages()。phys 必须页对齐。 */
int          pmm_range_managed(unsigned int phys, unsigned int pages);

/* 线性 first-fit 自检：保留区不可分配、分配不重复、free 后账目复原。 */
int          pmm_selftest(void);

#endif
