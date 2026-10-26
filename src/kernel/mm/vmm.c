/* =============================================================================
 * vmm.c - PAE 分页、每进程地址空间、COW 与 vmalloc
 *
 * 32 位 PAE：
 *   - CR3 指向 4 项 PDPT（每项 1GB）；
 *   - 恒等映射使用 2MB 页（PDE.PS=1）；
 *   - 用户区 0x40000000.. 使用 4KB PTE；
 *   - 用户可写页设置 PTE.NX=1，代码页 NX=0；
 *   - 内核高地址（framebuffer/LAPIC/vmalloc）使用 4KB 映射。
 *
 * 说明：PAE 是 NX 的前置条件。没有 PAE 的 32 位分页无法表达 NX 位。
 * ============================================================================= */
#include "vmm.h"
#include "../include/types.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/kernel.h"

/* vmalloc 块头校验失败、重复释放等异常累计次数，自检时用来判断 */
unsigned int vmm_faults;

/* PAE 下恒等映射使用 2MB 页 */
int vmm_large_pages = 0;
int vmm_nx_enabled = 0;

/* vmalloc 区放在 3GB 以上的内核高地址，避开恒等映射的 2MB 大页。
 * 0xC0000000..0xC2000000 = 512MB 虚拟区，物理页仍由 PMM 分配。 */
#define VMALLOC_BASE  0xC0000000U
#define VMALLOC_SIZE  (32 * 1024 * 1024)

#define PAGE_SHIFT   12
#define PAGE_BYTES   (1 << PAGE_SHIFT)

/* PAE 64 位页表项 */
#define PAE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PDE_2MB_MASK    0x000FFFFFFFE00000ULL
#define PTE_NX_BIT      (1ULL << 63)

/* vmalloc 块头魔数（小端 'KBLV'） */
#define VBLOCK_MAGIC  0x56424C4BU

typedef struct {
    uint32_t magic;
    uint32_t pages;
    uint32_t size;
    uint32_t checksum;
} vblock_t;

static uint8_t* vm_bitmap = 0;
static size_t vm_total_pages;

/* 内核自己的 PDPT（vmm_init 建立、只含内核映射，用户区 PDPT[1]=0）。
 * CPU 没有可运行进程时切回它再 idle，避免继续用“即将被父进程回收”的进程
 * 地址空间——否则另一颗 CPU 的 proc_reap → vmm_destroy_dir 会把本 CPU 仍在
 * 用的 PDPT/页表释放掉（PMM 毒化后变成 0xAA...），表现为 eip/cr2 在
 * 0x0/0x18/0x1b 附近的内核 #PF。 */
static pde_t *kernel_pdpt;

/* ==================== 内部辅助函数 ==================== */
static inline uint32_t pdpt_index(uintptr_t addr) { return (uint32_t)(addr >> 30); }
static inline uint32_t pd_index(uintptr_t addr)   { return (uint32_t)((addr >> 21) & 0x1FFU); }
static inline uint32_t pt_index(uintptr_t addr)   { return (uint32_t)((addr >> 12) & 0x1FFU); }

static void vmm_warn(const char *what, uintptr_t addr)
{
    vmm_faults++;
    serial_puts("vmm: WARN ");
    serial_puts(what);
    serial_puts(" at ");
    serial_puthex32((unsigned int)addr);
    serial_puts("\r\n");
}

static uint32_t vm_checksum(const vblock_t *b)
{
    return ~(b->magic ^ b->pages ^ b->size);
}

static void cpuid(uint32_t leaf, uint32_t sub,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t eax = leaf, ebx = 0, ecx = sub, edx = 0;

    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    *a = eax; *b = ebx; *c = ecx; *d = edx;
}

static int cpu_has_pae(void)
{
    uint32_t a, b, c, d;

    cpuid(0U, 0U, &a, &b, &c, &d);
    if (a < 1U) return 0;
    cpuid(1U, 0U, &a, &b, &c, &d);
    return (d & (1U << 6)) != 0U;      /* CPUID.01H:EDX.PAE */
}

static int cpu_has_nx(void)
{
    uint32_t a, b, c, d;

    cpuid(0x80000000U, 0U, &a, &b, &c, &d);
    if (a < 0x80000001U) return 0;
    cpuid(0x80000001U, 0U, &a, &b, &c, &d);
    return (d & (1U << 20)) != 0U;     /* CPUID.80000001H:EDX.NX */
}

static void enable_nxe(void)
{
    uint32_t lo, hi;

    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
    lo |= (1U << 11);                   /* EFER.NXE */
    __asm__ volatile ("wrmsr" : : "c"(0xC0000080U), "a"(lo), "d"(hi));
}

/* 取（必要时创建）PDPT 项指向的页目录 */
static pde_t *get_pd(uint32_t pdpti, int create)
{
    pde_t entry = page_directory[pdpti];

    if (entry & PTE_PRESENT) {
        return (pde_t *)(uintptr_t)(entry & PAE_ADDR_MASK);
    }
    if (!create) return 0;

    void *pd = pmm_alloc_pages(1);
    if (pd == 0) return 0;
    memset(pd, 0, PAGE_BYTES);
    page_directory[pdpti] = (pde_t)(uintptr_t)pd | PTE_PRESENT | PTE_WRITE;
    return (pde_t *)pd;
}

/* 取（必要时创建）某地址对应的页表项 */
static pte_t *get_pt_entry(uintptr_t virt, int create)
{
    uint32_t pdpti = pdpt_index(virt);
    uint32_t pdi = pd_index(virt);
    uint32_t pti = pt_index(virt);
    pde_t *pd = get_pd(pdpti, create);
    pde_t pde;
    pte_t *pt;

    if (pd == 0) return 0;
    pde = pd[pdi];
    if (pde & PTE_PS) return 0;         /* 不为 4KB 请求拆分 2MB 页 */

    if (!(pde & PTE_PRESENT)) {
        if (!create) return 0;
        void *pt_page = pmm_alloc_pages(1);
        if (pt_page == 0) return 0;
        memset(pt_page, 0, PAGE_BYTES);
        pd[pdi] = (pde_t)(uintptr_t)pt_page | PTE_PRESENT | PTE_WRITE;
        pde = pd[pdi];
    }

    pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
    return &pt[pti];
}

/* ==================== 物理页分配（对 PMM 的封装） ==================== */
void* alloc_page(void) { return (void*)pmm_alloc_pages(1); }
void free_page(void* phys_addr) { pmm_free_pages(phys_addr, 1); }
void* alloc_pages(size_t count) { return (void*)pmm_alloc_pages(count); }
void free_pages(void* base_addr, size_t count) { pmm_free_pages(base_addr, count); }

/* ==================== 单页映射管理 ==================== */
int vmm_map_page(uintptr_t virt, uintptr_t phys, uint32_t flags)
{
    pte_t* entry = get_pt_entry(virt, 1);
    if (!entry) return -1;

    *entry = (phys & PAE_ADDR_MASK) | (flags & 0xFFFU);
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

int vmm_unmap_page(uintptr_t virt)
{
    pte_t* entry = get_pt_entry(virt, 0);
    if (!entry || !(*entry & PTE_PRESENT)) return -1;

    *entry = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

uintptr_t vmm_get_phys(uintptr_t virt)
{
    uint32_t pdpti = pdpt_index(virt);
    uint32_t pdi = pd_index(virt);
    pde_t *pd;
    pde_t pde;
    pte_t *pt;
    pte_t pte;

    if (!(page_directory[pdpti] & PTE_PRESENT)) return 0;
    pd = (pde_t *)(uintptr_t)(page_directory[pdpti] & PAE_ADDR_MASK);
    pde = pd[pdi];
    if (!(pde & PTE_PRESENT)) return 0;

    if (pde & PTE_PS) {
        return (pde & PDE_2MB_MASK) | (virt & 0x001FFFFFU);
    }

    pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
    pte = pt[pt_index(virt)];
    if (!(pte & PTE_PRESENT)) return 0;
    return pte & PAE_ADDR_MASK;
}

uint32_t vmm_user_flags(uint32_t virt)
{
    uint32_t pdpti = pdpt_index(virt);
    uint32_t pdi = pd_index(virt);
    pde_t *pd;
    pde_t pde;
    pte_t *pt;
    pte_t pte;

    if (!(page_directory[pdpti] & PTE_PRESENT)) return 0;
    pd = (pde_t *)(uintptr_t)(page_directory[pdpti] & PAE_ADDR_MASK);
    pde = pd[pdi];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_PS) return (uint32_t)(pde & 0xFFFU);

    pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
    pte = pt[pt_index(virt)];
    if (!(pte & PTE_PRESENT)) return 0;
    return (uint32_t)(pte & 0xFFFU);
}

/* =============================================================================
 * vmm_user_page_count - 统计一个地址空间的用户常驻页数（RSS 口径）
 *
 * 只扫用户布局区 [USER_LAYOUT_CODE_BASE, USER_LAYOUT_CLONE_END)（每进程 5MB，
 * 最多 3 个 2MB PDE、1536 个 PTE），计入 PRESENT|USER 的 4KB 页。
 *
 * 可以传非当前进程的页目录：PAE 页表物理地址都在恒等映射区，内核可直接读。
 * 为此对 PDPT/PD/PT 的物理地址做 RAM 范围与页对齐校验——进程正在被另一颗
 * CPU reap 时可能读到毒化值（0xAA 填充），校验不过就按 0 页处理，绝不让
 * 诊断路径把内核读挂。
 * ============================================================================= */
uint32_t vmm_user_page_count(pde_t *dir)
{
    const uintptr_t lo = USER_LAYOUT_CODE_BASE;
    const uintptr_t hi = USER_LAYOUT_CLONE_END;
    const uintptr_t ram_lo = (uintptr_t)pmm.phys_base;
    const uintptr_t ram_hi = ram_lo + ((uintptr_t)pmm.total_pages << PAGE_SHIFT);
    uint32_t pdi_first, pdi_last, count = 0;
    pde_t *pd;

    if (dir == 0) return 0;
    if (((uintptr_t)dir & 0xFFFU) != 0U) return 0;
    if ((uintptr_t)dir < ram_lo || (uintptr_t)dir >= ram_hi) return 0;
    if (!(dir[1] & PTE_PRESENT)) return 0;

    pd = (pde_t *)(uintptr_t)(dir[1] & PAE_ADDR_MASK);
    if (((uintptr_t)pd & 0xFFFU) != 0U) return 0;
    if ((uintptr_t)pd < ram_lo || (uintptr_t)pd >= ram_hi) return 0;

    pdi_first = pd_index(lo);
    pdi_last  = pd_index(hi - 1U);

    for (uint32_t pdi = pdi_first; pdi <= pdi_last; pdi++) {
        pde_t pde = pd[pdi];
        pte_t *pt;

        if (!(pde & PTE_PRESENT)) continue;
        if (pde & PTE_PS) continue;             /* 用户区不应出现 2MB 页 */
        pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
        if (((uintptr_t)pt & 0xFFFU) != 0U) continue;
        if ((uintptr_t)pt < ram_lo || (uintptr_t)pt >= ram_hi) continue;

        for (uint32_t pti = 0; pti < 512U; pti++) {
            pte_t pte = pt[pti];
            uint32_t va;

            if ((pte & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
                continue;
            }
            /* pdi/pti 是 PDPT[1]（0x40000000..0x7FFFFFFF）内的索引，要加回区基址。 */
            va = (uint32_t)lo + (pdi << 21) + (pti << 12);
            if (va < lo || va >= hi) continue;
            count++;
        }
    }

    return count;
}

/* =============================================================================
 * vmm_protect_page - 修改当前地址空间一个用户页的保护位（mprotect 底层）
 *
 * prot: VMM_PROT_READ/WRITE/EXEC 组合，0 = PROT_NONE（本实现等价只读+NX）。
 *   - 只接受 PRESENT|USER 的 4KB 用户页；其它返回 -1（不拆大页、不建新页）；
 *   - 请求写且页是 COW、或物理页仍被多个页表引用：先私有化当前进程的页，
 *     避免两个进程写同一物理页；
 *   - 清写时同时清 PTE_COW：显式只读必须真的不可写（否则 #PF 的 COW 路径
 *     会把写静默放行）；共享页以后要写时，再走上面的“请求写先私有化”；
 *   - 执行位用 NX（bit 63）表达；NX 不可用时忽略执行位差异；
 *   - 改完 invlpg 当前 CPU。不做远端 shootdown（每个进程单线程 + CR3 切换
 *     会刷新非全局 TLB；未来多线程/PCID 的远端策略见 docs/process-memory.md）。
 * ============================================================================= */
int vmm_protect_page(uintptr_t virt, uint32_t prot)
{
    pte_t *entry;
    uint64_t e;
    uint32_t flags;
    uint32_t phys;

    if ((virt & 0xFFFU) != 0U) return -1;
    if ((prot & ~(uint32_t)(VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_EXEC)) != 0U) {
        return -1;
    }

    entry = get_pt_entry(virt, 0);
    if (entry == 0) return -1;
    e = *entry;
    flags = (uint32_t)(e & 0xFFFU);
    if ((flags & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return -1;

    phys = (uint32_t)(e & PAE_ADDR_MASK);

    if ((prot & VMM_PROT_WRITE) != 0U) {
        bool added_cow = false;

        /* “共享但没有 PTE_COW” 会出现在“先 mprotect 只读、再 fork”的路径，
         * 这里临时打上 PTE_COW，让 vmm_cow_fault() 用同一套复制逻辑。 */
        if ((flags & PTE_COW) == 0U && pmm_page_ref(phys) > 1U) {
            *entry |= PTE_COW;
            added_cow = true;
            flags |= PTE_COW;
        }

        if ((flags & PTE_COW) != 0U) {
            if (!vmm_cow_fault((uint32_t)virt)) {
                if (added_cow) *entry &= ~(uint64_t)PTE_COW;   /* 回滚临时标记 */
                return -1;
            }
            entry = get_pt_entry(virt, 0);
            if (entry == 0) return -1;
            e = *entry;
            flags = (uint32_t)(e & 0xFFFU);
            if ((flags & PTE_PRESENT) == 0U) return -1;
            phys = (uint32_t)(e & PAE_ADDR_MASK);
        }
    }

    /* 只改权限位，保留物理地址与 PWT/PCD/DIRTY 等既有位。
     * 显式清写时必须同时清 PTE_COW：#PF 的写时复制路径看到 COW 就会把页
     * 改成可写，那会让 mprotect(PROT_READ) 形同虚设。COW 只表示“fork 共享、
     * 写时复制”，不能用来表达用户显式要求的只读。 */
    e &= ~(uint64_t)(PTE_WRITE | PTE_COW);
    if ((prot & VMM_PROT_WRITE) != 0U) {
        e |= PTE_WRITE;
    }

    if (vmm_nx_enabled) {
        if ((prot & VMM_PROT_EXEC) != 0U) e &= ~PTE_NX_BIT;
        else e |= PTE_NX_BIT;
    }

    *entry = e;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

/* =============================================================================
 * vmm_make_writable - 确保一个用户页对当前进程可写（COW 先私有化）
 *
 * 只处理“fork 共享/COW”这一类只读页；如果页是用户显式 mprotect 成的只读
 * （没有 COW、引用计数为 1），返回 -1，绝不越权改权限。
 * 供内核写用户内存的路径使用：信号帧（deliver_signal）、copy_to_user 兜底。
 * ============================================================================= */
int vmm_make_writable(uintptr_t virt)
{
    pte_t *entry;
    uint64_t e;
    uint32_t flags;
    uint32_t phys;

    if ((virt & 0xFFFU) != 0U) return -1;

    entry = get_pt_entry(virt, 0);
    if (entry == 0) return -1;
    e = *entry;
    flags = (uint32_t)(e & 0xFFFU);
    if ((flags & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return -1;

    /* 已经可写且不是 COW：直接成功 */
    if ((flags & (PTE_WRITE | PTE_COW)) == PTE_WRITE) return 0;

    phys = (uint32_t)(e & PAE_ADDR_MASK);

    if ((flags & PTE_COW) == 0U) {
        /* 共享但没有 COW（先 mprotect 只读、再 fork 会这样）：
         * 引用计数为 1 说明是显式只读，不越权；>1 才临时标 COW 走复制。 */
        if (pmm_page_ref(phys) <= 1U) return -1;
        *entry |= PTE_COW;
    }

    if (!vmm_cow_fault((uint32_t)virt)) {
        /* 失败保留 COW 标记：它本来就是共享页，标 COW 与 fork 语义一致，
         * 下次写缺页仍会尝试复制。 */
        return -1;
    }
    return 0;
}

void vmm_map_range(uintptr_t virt, uintptr_t phys, size_t size, uint32_t flags)
{
    if (size == 0) return;

    uintptr_t v = virt & 0xFFFFF000;
    uintptr_t p = phys & 0xFFFFF000;
    size_t pages = (size + 0xFFF) >> PAGE_SHIFT;

    for (size_t i = 0; i < pages; i++) {
        pte_t* pte = get_pt_entry(v + (i << PAGE_SHIFT), 1);
        if (pte) {
            int was_present = (*pte & PTE_PRESENT) != 0;
            *pte = (p + (i << PAGE_SHIFT)) | flags;
            if (was_present) {
                __asm__ volatile ("invlpg (%0)" : : "r"(v + (i << PAGE_SHIFT)) : "memory");
            }
        }
    }
}

static void identity_map_2mb(uint32_t phys_end)
{
    uint32_t end = (phys_end + 0x1FFFFFU) & ~0x1FFFFFU;
    pde_t *pd = get_pd(0, 1);

    if (pd == 0) panic("vmm: cannot allocate identity page directory");
    for (uint32_t addr = 0; addr < end; addr += 0x200000U) {
        pd[addr >> 21] = addr | PTE_PRESENT | PTE_WRITE | PTE_PS;
    }
}

void vmm_init(void)
{
    pde_t *dir;

    if (!cpu_has_pae()) {
        panic("vmm: CPU has no PAE; NX paging not possible");
    }
    vmm_nx_enabled = cpu_has_nx();

    dir = (pde_t *)pmm_alloc_pages(1);
    if (dir == 0) panic("vmm: no PDPT page");
    memset(dir, 0, PAGE_BYTES);

    /* 先装 CR3；identity_map_2mb 通过 current dir 写页表。 */
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)(uintptr_t)dir) : "memory");
    kernel_pdpt = dir;                  /* idle/无进程时切回的内核地址空间 */

    uint32_t phys_end = pmm.phys_base + (uint32_t)(pmm.total_pages << PAGE_SHIFT);
    identity_map_2mb(phys_end);
    vmm_large_pages = 1;

    vm_total_pages = VMALLOC_SIZE >> PAGE_SHIFT;
    size_t bitmap_pages = ((vm_total_pages + 7) / 8 + PAGE_BYTES - 1) >> PAGE_SHIFT;
    vm_bitmap = (uint8_t*)pmm_alloc_pages(bitmap_pages);
    if (vm_bitmap == 0) panic("vmm: no vmalloc bitmap");
    memset(vm_bitmap, 0, (vm_total_pages + 7) / 8);
}

void vmm_enable_paging(void)
{
    uint32_t cr4;
    uint32_t cr0;
    uint32_t cr3 = (uint32_t)(uintptr_t)page_directory;

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1U << 5);                   /* CR4.PAE */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    if (vmm_nx_enabled) enable_nxe();

    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000U;                 /* CR0.PG */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("jmp 1f\n1:");

    kprintf("vmm: PAE on, NX %s\n", vmm_nx_enabled ? "enabled" : "unsupported");
}

/* 内核 W^X：把 0..__kernel_end 的 2MB 恒等页拆成 4KB 页。
 * text=R+X，rodata=R+NX，data/bss=RW+NX，低内存/空洞=RW+NX；
 * __kernel_end 以上的 2MB 恒等页统一 NX。最后开启 CR0.WP。 */
void vmm_kernel_protect(void)
{
    extern uint8_t __kernel_text_start[], __kernel_text_end[];
    extern uint8_t __kernel_rodata_start[], __kernel_rodata_end[];
    extern uint8_t __kernel_data_start[], __kernel_data_end[];
    extern uint8_t __bss_start[], __bss_end[], __kernel_end[];

    uint32_t text_start = (uint32_t)(uintptr_t)__kernel_text_start;
    uint32_t text_end   = (uint32_t)(uintptr_t)__kernel_text_end;
    uint32_t ro_start   = (uint32_t)(uintptr_t)__kernel_rodata_start;
    uint32_t ro_end     = (uint32_t)(uintptr_t)__kernel_rodata_end;
    uint32_t data_start = (uint32_t)(uintptr_t)__kernel_data_start;
    uint32_t data_end   = (uint32_t)(uintptr_t)__kernel_data_end;
    uint32_t bss_start  = (uint32_t)(uintptr_t)__bss_start;
    uint32_t bss_end    = (uint32_t)(uintptr_t)__bss_end;
    uint32_t kend       = (uint32_t)(uintptr_t)__kernel_end;
    pde_t *pd = get_pd(0, 0);
    uint32_t regions;

    if (pd == 0) return;

    regions = (kend + 0x1FFFFFU) >> 21;
    if (regions > 512U) regions = 512U;

    for (uint32_t pdi = 0; pdi < regions; pdi++) {
        pde_t pde = pd[pdi];
        pte_t *pt;

        if ((pde & PTE_PRESENT) && (pde & PTE_PS)) {
            pt = (pte_t *)pmm_alloc_page();
            if (pt == 0) panic("vmm: no PT for kernel W^X");
            memset(pt, 0, PAGE_BYTES);
            for (uint32_t j = 0; j < 512U; j++) {
                uint32_t phys = (pdi << 21) | (j << 12);
                pt[j] = (uint64_t)phys | PTE_PRESENT | PTE_WRITE;
            }
            pd[pdi] = (uint64_t)(uintptr_t)pt | PTE_PRESENT | PTE_WRITE;
        } else if ((pde & PTE_PRESENT) != 0U) {
            pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
        } else {
            continue;
        }

        for (uint32_t j = 0; j < 512U; j++) {
            uint32_t addr = (pdi << 21) | (j << 12);
            uint64_t flags = PTE_PRESENT | PTE_WRITE | PTE_NOEXEC;

            if (addr >= text_start && addr < text_end) {
                flags = PTE_PRESENT;                        /* RX */
            } else if (addr >= ro_start && addr < ro_end) {
                flags = PTE_PRESENT | PTE_NOEXEC;           /* R+NX */
            } else if ((addr >= data_start && addr < data_end) ||
                       (addr >= bss_start && addr < bss_end)) {
                flags = PTE_PRESENT | PTE_WRITE | PTE_NOEXEC;
            }

            uint64_t entry = (uint64_t)addr | flags;
            if (vmm_nx_enabled && (flags & PTE_NOEXEC) != 0U) {
                entry |= PTE_NX_BIT;
            }
            pt[j] = entry;
        }
    }

    /* 内核映像之外的恒等映射只作为数据/MMIO 访问，不允许执行。 */
    if (vmm_nx_enabled) {
        for (uint32_t pdi = regions; pdi < 512U; pdi++) {
            if ((pd[pdi] & PTE_PRESENT) && (pd[pdi] & PTE_PS)) {
                pd[pdi] |= PTE_NX_BIT;
            }
        }
    }

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x10000U;                    /* CR0.WP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)(uintptr_t)page_directory) : "memory");

    kprintln("vmm: kernel W^X active (text RX, rodata R+NX, data RW+NX)");
}

/* 用户页：PDE/PTE 都带 U/S。可写页（栈/堆/数据）同时设置 NX；
 * 只读代码页保持可执行。 */
int vmm_map_user_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pdi = pd_index(virt);
    pde_t *pd = get_pd(pdpt_index(virt), 1);
    pte_t *pte;
    uint64_t entry;

    if (pd == 0) return -1;
    pte = get_pt_entry(virt, 1);
    if (pte == 0) return -1;

    pd[pdi] |= PTE_PRESENT | PTE_WRITE | PTE_USER;
    entry = (uint64_t)(phys & 0xFFFFF000U) | (uint64_t)(flags & 0xFFFU) | PTE_USER;
    if (vmm_nx_enabled && (flags & (PTE_WRITE | PTE_COW | PTE_NOEXEC)) != 0U) {
        entry |= PTE_NX_BIT;
    }
    *pte = entry;

    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

/* 内核态 4KB 映射（不带 USER 位）：LAPIC、framebuffer、vmalloc 用 */
int vmm_map_kernel_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    pte_t *pte = get_pt_entry(virt, 1);
    uint64_t entry;

    if (pte == 0) return -1;

    entry = (uint64_t)(phys & 0xFFFFF000U) | (uint64_t)(flags & 0xFFFU);
    if (vmm_nx_enabled && (flags & PTE_NOEXEC) != 0U) {
        entry |= PTE_NX_BIT;
    }
    *pte = entry;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

int vmm_map_mmio(uint32_t phys, uint32_t size)
{
    uint32_t pages = (size + PAGE_BYTES - 1U) / PAGE_BYTES;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t a = phys + i * PAGE_BYTES;

        if (vmm_map_kernel_page(a, a,
                                PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT |
                                PTE_NOEXEC) != 0) {
            return -1;
        }
    }
    return 0;
}

int vmm_kernel_set_exec(uint32_t virt)
{
    pte_t *pte = get_pt_entry(virt, 0);

    if (pte == 0 || (*pte & PTE_PRESENT) == 0U) return -1;
    *pte = (*pte & PAE_ADDR_MASK) | PTE_PRESENT;   /* RX */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

/* PAE 的 CR3 指向 PDPT；每进程复制内核 PDPT 项，清空用户项 1。 */
pde_t* vmm_new_dir(void)
{
    pde_t *dir = (pde_t *)pmm_alloc_pages(1);
    if (dir == 0) return 0;
    memset(dir, 0, PAGE_BYTES);

    for (uint32_t i = 0; i < 4U; i++) {
        dir[i] = page_directory[i];
    }
    dir[1] = 0;                         /* 0x40000000..0x7FFFFFFF 用户区 */

    return dir;
}

void vmm_switch_dir(pde_t *dir)
{
    if (dir == 0) return;

    uint32_t pd = (uint32_t)(uintptr_t)dir;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd) : "memory");
}

pde_t* vmm_current_dir(void)
{
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (pde_t *)(uintptr_t)(cr3 & 0xFFFFF000U);
}

/* 内核自己的地址空间（只含内核映射）。没有可运行进程的 CPU 切回它 idle，
 * 不要停留在即将被回收的进程页目录上。 */
pde_t* vmm_kernel_dir(void)
{
    return kernel_pdpt;
}

static void vmm_reload_cr3(void)
{
    uint32_t cr3 = (uint32_t)(uintptr_t)page_directory;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* 释放用户区（PDPT[1]）的全部物理页和页表页。
 * 只处理 4KB 映射；内核 PDPT[0]/[2]/[3] 是共享的，不能动。 */
void vmm_clear_user_space(pde_t *dir)
{
    pde_t *pd;
    pde_t pde;

    if (dir == 0) return;
    if (!(dir[1] & PTE_PRESENT)) {
        if (dir == page_directory) vmm_reload_cr3();
        return;
    }

    pd = (pde_t *)(uintptr_t)(dir[1] & PAE_ADDR_MASK);

    for (uint32_t pdi = 0; pdi < 512U; pdi++) {
        pde = pd[pdi];
        if (!(pde & PTE_PRESENT)) continue;

        /* 用户区不应出现 2MB 页；出现就只清项，避免误释放。 */
        if (pde & PTE_PS) {
            pd[pdi] = 0;
            continue;
        }

        pte_t *pt = (pte_t *)(uintptr_t)(pde & PAE_ADDR_MASK);
        for (uint32_t pti = 0; pti < 512U; pti++) {
            pte_t pte = pt[pti];
            if (!(pte & PTE_PRESENT)) continue;

            uintptr_t phys = (uintptr_t)(pte & PAE_ADDR_MASK);
            if (phys != 0U) pmm_free_pages((void *)phys, 1);
            pt[pti] = 0;
        }

        pmm_free_pages(pt, 1);          /* 释放页表页 */
        pd[pdi] = 0;
    }

    pmm_free_pages(pd, 1);              /* 释放用户 PD */
    dir[1] = 0;

    if (dir == page_directory) vmm_reload_cr3();
}

void vmm_destroy_dir(pde_t *dir)
{
    if (dir == 0) return;

    vmm_clear_user_space(dir);

    /* 当前 CR3 不能直接释放；调用方应先切到另一个地址空间。 */
    if (dir != page_directory) {
        pmm_free_pages(dir, 1);
    }
}

void vmm_teardown_selftest(void)
{
    uint32_t before = pmm.free_pages;
    pde_t *saved = vmm_current_dir();
    pde_t *dir = vmm_new_dir();
    void *page = 0;
    int ok = 1;

    if (dir == 0) {
        kprintln("vmm: address space teardown FAIL (no dir)");
        return;
    }

    vmm_switch_dir(dir);
    page = pmm_alloc_page();
    if (page == 0) {
        ok = 0;
    } else if (vmm_map_user_page(USER_LAYOUT_CODE_BASE,
                                 (uint32_t)(uintptr_t)page,
                                 PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
        pmm_free_pages(page, 1);
        ok = 0;
    }

    vmm_switch_dir(saved);
    vmm_destroy_dir(dir);

    if (ok && pmm.free_pages == before) {
        kprintln("vmm: address space teardown ok");
    } else {
        kprintf("vmm: address space teardown FAIL (free delta=%d)\n",
                (int)before - (int)pmm.free_pages);
    }
}

/* 用户地址布局由 user/user_layout.h 统一提供。 */
#define USER_CLONE_LO   USER_LAYOUT_CODE_BASE
#define USER_CLONE_HI   USER_LAYOUT_CLONE_END

static int cow_range(pde_t *dst_dir, pde_t *saved, uint32_t lo, uint32_t hi)
{
    for (uint32_t va = lo; va < hi; va += PAGE_BYTES) {
        uintptr_t phys = vmm_get_phys(va);
        int r;

        if (phys == 0U) continue;

        uint32_t old_flags = vmm_user_flags(va);
        uint32_t share_flags = PTE_PRESENT | PTE_USER;
        if ((old_flags & (PTE_WRITE | PTE_COW)) != 0U) {
            share_flags |= PTE_COW;
        }
        if ((old_flags & PTE_NOEXEC) != 0U) {
            share_flags |= PTE_NOEXEC;      /* rodata 在 fork 后仍保持 NX */
        }

        pmm_page_share((uint32_t)phys);

        r = vmm_map_user_page(va, (uint32_t)phys, share_flags);
        if (r != 0) {
            pmm_free_pages((void *)phys, 1);
            return -1;
        }

        vmm_switch_dir(dst_dir);
        r = vmm_map_user_page(va, (uint32_t)phys, share_flags);
        vmm_switch_dir(saved);
        if (r != 0) {
            pmm_free_pages((void *)phys, 1);
            return -1;
        }
    }
    return 0;
}

int vmm_clone_user_pages(pde_t *dst_dir)
{
    pde_t *saved = page_directory;

    return cow_range(dst_dir, saved, USER_CLONE_LO, USER_CLONE_HI);
}

unsigned int vmm_cow_copies;

int vmm_cow_fault(uint32_t addr)
{
    uint32_t va = addr & 0xFFFFF000U;
    uintptr_t phys = vmm_get_phys(va);
    uint32_t flags;
    void *copy;
    int r;

    if (phys == 0U) return 0;

    flags = vmm_user_flags(va);
    if ((flags & PTE_COW) == 0U) return 0;

    if (pmm_page_ref((uint32_t)phys) <= 1U) {
        return (vmm_map_user_page(va, (uint32_t)phys,
                                  PTE_PRESENT | PTE_WRITE | PTE_USER) == 0) ? 1 : 0;
    }

    copy = pmm_alloc_pages(1);
    if (copy == 0) return 0;
    memcpy(copy, (void *)phys, PAGE_BYTES);

    r = vmm_map_user_page(va, (uint32_t)(uintptr_t)copy,
                          PTE_PRESENT | PTE_WRITE | PTE_USER);
    if (r != 0) {
        pmm_free_pages(copy, 1);
        return 0;
    }

    pmm_free_pages((void *)phys, 1);
    vmm_cow_copies++;
    if (vmm_cow_copies == 1U) {
        kprintf("vmm: first COW copy at %p\n", (void *)(uintptr_t)va);
    }
    return 1;
}

/* ==================== vmalloc ==================== */
/* vmalloc 区是全局共享的内核虚拟区间（所有进程的 PDPT[3] 指向同一份页表），
 * vm_bitmap/映射/释放因此是跨 CPU 共享状态。旧实现完全无锁：两颗 CPU 同时
 * vmalloc 会从 vm_find_free() 拿到同一段 start_page，各自把不同的物理页映射
 * 到同一个虚拟地址（后者覆盖前者的 PTE），随后两次 vfree 还会重复释放/清位图，
 * 表现为块头校验失败、PMM double free、以及两个调用者互相踩数据。
 * audit-smp 用两个内核线程并发 vmalloc/vfree 复现（audit: mm vmalloc concurrent）。 */
static spinlock_t vm_lock = SPINLOCK_INIT;

static int vm_find_free(size_t count)
{
    size_t found = 0;
    size_t start = 0;

    for (size_t i = 0; i < vm_total_pages; i++) {
        if (!(vm_bitmap[i >> 3] & (1 << (i & 7)))) {
            if (found == 0) start = i;
            found++;
            if (found == count) return (int)start;
        } else {
            found = 0;
        }
    }
    return -1;
}

void* vmalloc(size_t size)
{
    size_t need;
    size_t pages;
    int start_page;
    uintptr_t virt;
    uint32_t flags;
    vblock_t* blk;

    if (size == 0) return 0;

    need  = size + sizeof(vblock_t);
    pages = (need + 0xFFF) >> PAGE_SHIFT;

    flags = spin_lock_irqsave(&vm_lock);
    start_page = vm_find_free(pages);
    if (start_page < 0) {
        spin_unlock_irqrestore(&vm_lock, flags);
        return 0;
    }

    virt = VMALLOC_BASE + ((uintptr_t)start_page << PAGE_SHIFT);

    for (size_t i = 0; i < pages; i++) {
        void* phys = alloc_page();
        if (!phys) {
            for (size_t j = 0; j < i; j++) {
                uintptr_t v = virt + (j << PAGE_SHIFT);
                uintptr_t p = vmm_get_phys(v);
                if (p) free_page((void*)p);
                vmm_unmap_page(v);
            }
            spin_unlock_irqrestore(&vm_lock, flags);
            return 0;
        }
        vmm_map_page(virt + (i << PAGE_SHIFT), (uintptr_t)phys, PTE_PRESENT | PTE_WRITE);
    }

    blk = (vblock_t*)virt;
    blk->magic    = VBLOCK_MAGIC;
    blk->pages    = (uint32_t)pages;
    blk->size     = (uint32_t)size;
    blk->checksum = vm_checksum(blk);

    for (size_t i = 0; i < pages; i++) {
        vm_bitmap[(start_page + i) >> 3] |= (1 << ((start_page + i) & 7));
    }
    spin_unlock_irqrestore(&vm_lock, flags);

    return (void*)(virt + sizeof(vblock_t));
}

void vfree(void* addr)
{
    uintptr_t ptr;
    vblock_t* blk;
    uintptr_t virt;
    size_t start_page;
    size_t pages;
    const char *warn = 0;
    uintptr_t warn_addr = 0;
    uint32_t flags;

    if (!addr) return;

    ptr = (uintptr_t)addr;
    flags = spin_lock_irqsave(&vm_lock);

    if (ptr < (uintptr_t)VMALLOC_BASE + sizeof(vblock_t)) {
        warn = "vfree on a non-vmalloc pointer";
        warn_addr = ptr;
        goto out;
    }

    blk = (vblock_t*)(ptr - sizeof(vblock_t));

    if (blk->magic != VBLOCK_MAGIC || blk->checksum != vm_checksum(blk)) {
        warn = "vfree on a corrupt block header";
        warn_addr = (uintptr_t)blk;
        goto out;
    }

    virt = (uintptr_t)blk;
    start_page = (virt - VMALLOC_BASE) >> PAGE_SHIFT;
    pages = blk->pages;

    if (start_page + pages > vm_total_pages) {
        warn = "vfree block out of vmalloc range";
        warn_addr = virt;
        goto out;
    }
    if (!(vm_bitmap[start_page >> 3] & (1 << (start_page & 7)))) {
        warn = "vfree double free";
        warn_addr = virt;
        goto out;
    }

    blk->magic = 0;
    blk->checksum = 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t v = virt + (i << PAGE_SHIFT);
        uintptr_t phys = vmm_get_phys(v);
        if (phys) free_page((void*)phys);
        vmm_unmap_page(v);
        vm_bitmap[(start_page + i) >> 3] &= ~(1 << ((start_page + i) & 7));
    }

out:
    spin_unlock_irqrestore(&vm_lock, flags);
    if (warn != 0) vmm_warn(warn, warn_addr);
}
