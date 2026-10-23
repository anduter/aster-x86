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

/* 线性 first-fit 自检：保留区不可分配、分配不重复、free 后账目复原。 */
int          pmm_selftest(void);

#endif
