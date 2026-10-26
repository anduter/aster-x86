#ifndef MM_VMM_H
#define MM_VMM_H

#include "../include/types.h"
#include "arch/x86/io.h"
#include "pmm.h"

/* 页表项/页目录项标志位 */
#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_USER     0x004
#define PTE_PWT      0x008   /* 写通（Write-Through），映射 MMIO 时用 */
#define PTE_PCD      0x010   /* 禁止缓存（Cache Disable），映射显存时用 */
#define PTE_DIRTY    0x040
#define PTE_PS       0x080
#define PTE_COW      0x200   /* 软件位：该页是 fork COW 页，写入时允许复制 */
#define PTE_NOEXEC   0x800   /* 软件位：映射时设置 NX（用于只读 rodata） */

/* mprotect 的 prot 位：数值与 include/abi.h 的 PROT_* 保持一致。
 * PROT_NONE=0 在本实现里等价“只读 + NX”（见 docs/process-memory.md §5.5）。 */
#define VMM_PROT_NONE   0x0U
#define VMM_PROT_READ   0x1U
#define VMM_PROT_WRITE  0x2U
#define VMM_PROT_EXEC   0x4U

typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* 当前 CPU 的地址空间根；直接来自 CR3，避免 SMP 下全局指针被别的 CPU 改掉。 */
pde_t *vmm_current_dir(void);
#define page_directory (vmm_current_dir())

/* 内核自己的地址空间（只含内核映射，无用户区）。CPU 没有可运行进程时切回它
 * 再 idle，避免停留在即将被 proc_reap 回收的进程页目录上。 */
pde_t *vmm_kernel_dir(void);

/* 当前是否启用了 PAE/NX（由 vmm_init 探测） */
extern int vmm_nx_enabled;

/* vmalloc 块头校验失败、重复释放等异常累计次数（供自检判断） */
extern unsigned int vmm_faults;

/* 恒等映射是否使用 4MB 大页（1=大页，0=退回 4KB 页） */
extern int vmm_large_pages;

/* ========== 核心接口 ========== */
void vmm_init(void);
void vmm_map_range(unsigned int virt, unsigned int phys, unsigned int size, unsigned int flags);
void vmm_enable_paging(void);
/* 把内核覆盖的恒等映射拆成 4KB 页并设置 W^X：text RX、rodata R+NX、data/bss RW+NX。 */
void vmm_kernel_protect(void);

/* ========== 物理页分配（对 PMM 的封装） ========== */
/* 申请一页物理内存，返回其物理地址 */
void* alloc_page(void);
/* 释放一页物理内存 */
void free_page(void* phys_addr);
/* 申请若干连续物理页 */
void* alloc_pages(unsigned int count);
/* 释放若干连续物理页 */
void free_pages(void* base_addr, unsigned int count);

/* ========== 单页映射管理 ========== */
/* 把一个虚拟页映射到物理页 */
int vmm_map_page(unsigned int virt, unsigned int phys, unsigned int flags);
/* 取消一个虚拟页的映射 */
int vmm_unmap_page(unsigned int virt);
/* 查询虚拟页对应的物理地址 */
unsigned int vmm_get_phys(unsigned int virt);
/* 查询虚拟页的低 12 位页表标志（用于区分只读代码页与 COW 页） */
unsigned int vmm_user_flags(unsigned int virt);

/* 统计一个地址空间“用户布局区” [USER_LAYOUT_CODE_BASE,
 * USER_LAYOUT_CLONE_END) 内 PRESENT|USER 的 4KB 用户页数（RSS 口径）。
 * dir 可以是当前或非当前页目录（页表物理地址恒等映射，可直接遍历）；
 * dir==0 / 非 RAM 指针 / 已释放的 PDPT 返回 0。 */
uint32_t vmm_user_page_count(pde_t *dir);

/* 修改当前地址空间一个用户页的保护位（mprotect 的底层实现）：
 * prot 取 VMM_PROT_* 组合。请求写时若页是 COW/共享会先私有化；
 * 清写会同时清 PTE_COW（显式只读必须真的不可写）；执行位用 NX 表达。
 * 成功返回 0；非用户页/未映射/对齐或参数非法/COW 复制失败返回 -1。 */
int vmm_protect_page(uintptr_t virt, uint32_t prot);

/* 确保一个用户页对当前进程可写：COW/共享页先私有化；显式只读页返回 -1。
 * 内核写用户内存的路径（信号帧、copy_to_user 兜底）用它，避免内核态
 * 写 COW 只读页触发 #PF(err=3) → oops。 */
int vmm_make_writable(uintptr_t virt);

/* 把一块虚拟页映射成“用户可访问”（PDE 与 PTE 都带 U/S 位） */
int vmm_map_user_page(unsigned int virt, unsigned int phys, unsigned int flags);

/* 内核态 4KB 映射 / MMIO 映射（LAPIC 等），不带 USER 位 */
int vmm_map_kernel_page(unsigned int virt, unsigned int phys, unsigned int flags);
int vmm_map_mmio(unsigned int phys, unsigned int size);
/* 把已存在的内核 4KB 页改成 RX（AP trampoline 用）。 */
int vmm_kernel_set_exec(unsigned int virt);

/* 每进程页目录：新建（复制内核映射）、切换（改 CR3）、取当前 */
pde_t* vmm_new_dir(void);
void   vmm_switch_dir(pde_t *dir);
pde_t* vmm_current_dir(void);

/* 释放一个地址空间的用户区（0x40000000..0x7FFFFFFF）：
 * 用户物理页、PT/PD 页、PDPT 项都会回收；内核共享映射保留。
 * 当前 CR3 指向 dir 时只清用户区，不释放 PDPT 本身。 */
void   vmm_clear_user_space(pde_t *dir);
/* 非当前地址空间：clear_user_space + 释放 PDPT 页。 */
void   vmm_destroy_dir(pde_t *dir);

/* 回收回归：建临时地址空间、映射页面、销毁并核对 PMM 账目。 */
void   vmm_teardown_selftest(void);

/* 把用户页以 COW 方式共享到另一个页目录：fork 时用 */
int vmm_clone_user_pages(pde_t *dst_dir);

/* 写时复制：处理用户页写缺页，成功复制/恢复可写返回 1 */
int vmm_cow_fault(unsigned int addr);

/* COW 复制次数（诊断统计） */
extern unsigned int vmm_cow_copies;

/* ========== 虚拟内存区分配 ========== */
/* 申请 size 字节虚拟内存（自动映射物理页），返回虚拟地址 */
void* vmalloc(unsigned int size);
/* 释放 vmalloc 申请的内存（按块头记录的长度释放，不会误伤相邻块） */
void vfree(void* addr);

#endif
