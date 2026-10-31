/*
 * ng WP1-2: 4 级页表 + W^X/NX + SMEP/SMAP。
 *
 * 分工：entry.S 只建「引导用」的 1 GiB 恒等映射（2 MiB 大页），把 CPU 带进
 * 长模式并跳到 kmain；真正的 4 级页表在这里建好后写 CR3 切换。两张表对当前
 * 代码/栈/页表池的映射完全一致（都是恒等映射），所以切换 CR3 后不需要再来
 * 一次 far jump。
 *
 * 关键顺序（踩坑点）：
 *   1. EFER.NXE 必须在任何带 NX 位的页表被 CR3 装载之前置位，否则 NX=1 是
 *      保留位 -> #PF；entry.S 在开 PG 前就置了，这里只做确认/兜底。
 *   2. 新表自己的页（.bss 里的 pt_pool）必须被新表映射成 RW+NX，否则写表就
 *      会踩到只读页/不可执行假设。
 *   3. CR0.WP=1 让只读页在 ring0 也真正只读；CR4.SMEP/SMAP 最后开，并重写
 *      一次 CR3 刷 TLB（CR4 变更的保守做法）。
 */
#include "paging.h"

#include "io.h"
#include "serial.h"

#define PAGE_4K 0x1000ULL
#define PAGE_2M 0x200000ULL

#define MSR_EFER 0xC0000080u
#define EFER_NXE (1ULL << 11)
#define EFER_LMA (1ULL << 10)

#define CR0_PE (1ULL << 0)
#define CR0_WP (1ULL << 16)
#define CR0_PG (1ULL << 31)
#define CR4_PAE (1ULL << 5)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

/* 页表池：没有 PMM（WP1-4）之前，页表页从 .bss 的静态池里 bump 分配。 */
#define PT_POOL_PAGES 24

/* helper 自测用的虚拟地址：1 GiB，在恒等映射（0..1 GiB）之外。 */
#define SCRATCH_VA 0x40000000ULL

/* 链接脚本给的段边界（见 src/ng/link.ld）。 */
extern char __image_start[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];

/*
 * 页表池与 helper 的落点帧都放 .bss：内核镜像在第一个 2 MiB 里，已被
 * 4 KiB 页覆盖（RW+NX），所以恒等映射下「虚拟地址 == 物理地址」。
 * .bss 的页由 pt_alloc() 逐页清零，不依赖加载器对 p_memsz 的清零行为。
 */
static u64 pt_pool[PT_POOL_PAGES][512] __attribute__((aligned(4096)));
static u32 pt_pool_used;
static u64 helper_frame[512] __attribute__((aligned(4096)));

static u64 *kernel_pml4; /* 当前 CR3 指向的 PML4（恒等映射，虚拟 == 物理） */

/*
 * .data 段里的一个已初始化变量：让 .data 非空，自检能覆盖「已初始化数据页
 * 可读、可写、不可执行」，同时也给「只读 .rodata 对照」一个可写邻居。
 * volatile：否则 -O2 会把常量折叠掉、把变量降级进 .bss，.data 段就空了。
 */
#define PG_BOOT_MARKER 0x1A57E12A5EED0001ULL
static volatile u64 pg_boot_marker = PG_BOOT_MARKER;

static int cpu_nx;   /* CPUID.80000001h:EDX[20] */
static int cpu_smep; /* CPUID.(7,0):EBX[7] */
static int cpu_smap; /* CPUID.(7,0):EBX[20] */
static int nx_on;    /* EFER.NXE 已置位：数据页才允许/才敢写 NX 位 */
static int smep_on;  /* CR4.SMEP 生效 */
static int smap_on;  /* CR4.SMAP 生效 */

static int fails; /* 自检失败项数 */

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                   */
/* ------------------------------------------------------------------------- */

static inline u64 pa_of(const void *p)
{
    return (u64)(usize)p;
}

/* NX 只在 CPU 支持且 EFER.NXE 置位时才敢写进 PTE。 */
static inline u64 nx_bit(void)
{
    return nx_on ? PG_NX : 0;
}

/* 分配一页 4 KiB 页表并清零；池耗尽返回 NULL。 */
static u64 *pt_alloc(void)
{
    u64 *page;
    u32 i;

    if (pt_pool_used >= PT_POOL_PAGES) {
        return NULL;
    }
    page = pt_pool[pt_pool_used];
    pt_pool_used++;
    for (i = 0; i < 512; i++) {
        page[i] = 0;
    }
    return page;
}

/* 一个断言：失败计数 + 一行串口证据（不依赖 #PF/trap，WP1-3 才做异常）。 */
static int expect(int cond, const char *what)
{
    if (!cond) {
        fails++;
        serial_puts("ng: CHECK FAIL ");
        serial_puts(what);
        serial_puts("\r\n");
    }
    return cond;
}

/* ------------------------------------------------------------------------- */
/* CPUID：NX / SMEP / SMAP                                                    */
/* ------------------------------------------------------------------------- */

static void detect_cpu(void)
{
    u32 a, b, c, d;
    u32 max_basic;
    u32 max_ext;

    cpuid(0, 0, &a, &b, &c, &d);
    max_basic = a;
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    max_ext = a;

    cpu_nx = 0;
    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        cpu_nx = (int)((d >> 20) & 1u);
    }

    cpu_smep = 0;
    cpu_smap = 0;
    if (max_basic >= 7u) {
        cpuid(7, 0, &a, &b, &c, &d);
        cpu_smep = (int)((b >> 7) & 1u);
        cpu_smap = (int)((b >> 20) & 1u);
    }

    serial_puts("ng: cpu nx=");
    serial_putdec((u64)cpu_nx);
    serial_puts(" smep=");
    serial_putdec((u64)cpu_smep);
    serial_puts(" smap=");
    serial_putdec((u64)cpu_smap);
    serial_puts(" (cpuid max=");
    serial_puthex((u64)max_basic);
    serial_puts(" ext=");
    serial_puthex((u64)max_ext);
    serial_puts(")\r\n");
}

/*
 * 置 EFER.NXE。entry.S 已经在开分页前置过；这里再读一次并兜底。
 * 注意：EFER.LME 在分页开启后不能改，NXE 可以。
 */
static void enable_nx(void)
{
    u64 efer;

    nx_on = 0;
    if (!cpu_nx) {
        return;
    }
    efer = rdmsr(MSR_EFER);
    if ((efer & EFER_NXE) == 0) {
        wrmsr(MSR_EFER, efer | EFER_NXE);
        efer = rdmsr(MSR_EFER);
    }
    nx_on = (efer & EFER_NXE) ? 1 : 0;
    expect(nx_on, "efer.nxe");
}

/* ------------------------------------------------------------------------- */
/* 建表                                                                       */
/* ------------------------------------------------------------------------- */

/* 恒等映射 [start, end) 的 4 KiB 页；区间必须页对齐且落在第一个 2 MiB 内。 */
static int map_4k_range(u64 *pt, u64 start, u64 end, u64 perms)
{
    u64 addr;
    u64 *entry;

    if (((start | end) & (PAGE_4K - 1)) != 0) {
        return PG_EINVAL;
    }
    if (end > PAGE_2M || start > end) {
        return PG_EINVAL;
    }
    for (addr = start; addr < end; addr += PAGE_4K) {
        entry = &pt[(addr >> 12) & 0x1FFu];
        if ((*entry & PG_PRESENT) && ((*entry & PG_ADDR_MASK) != addr)) {
            return PG_EEXIST; /* 同一个 4 KiB 页被要求映射到别处 */
        }
        *entry = addr | perms | PG_PRESENT;
    }
    return PG_OK;
}

/*
 * 建最终页表：
 *   - PD[1..511] 2 MiB 大页恒等映射 2 MiB..1 GiB（RW+NX）；
 *   - PD[0] 拆成 PT，0..2 MiB 全部 4 KiB 页，内核镜像按段改权限。
 * 返回 PG_OK 或错误码。
 */
static int build_identity(u64 *pml4)
{
    u64 *pdpt;
    u64 *pd;
    u64 *pt;
    u32 i;
    u64 image;
    u64 text;
    u64 rodata_end;
    u64 kernel_end;

    pdpt = pt_alloc();
    pd = pt_alloc();
    pt = pt_alloc();
    if (pdpt == NULL || pd == NULL || pt == NULL) {
        return PG_ENOMEM;
    }

    pml4[0] = pa_of(pdpt) | PG_PRESENT | PG_WRITE;
    pdpt[0] = pa_of(pd) | PG_PRESENT | PG_WRITE;

    /* 先给 0..2 MiB 全部 4 KiB 页一个安全默认：RW + NX。 */
    for (i = 0; i < 512; i++) {
        pt[i] = ((u64)i * PAGE_4K) | PG_PRESENT | PG_WRITE | nx_bit();
    }

    /* PD[0] 指向 PT（4 KiB 粒度），其余 2 MiB 大页。 */
    pd[0] = pa_of(pt) | PG_PRESENT | PG_WRITE;
    for (i = 1; i < 512; i++) {
        pd[i] = ((u64)i * PAGE_2M) | PG_PRESENT | PG_WRITE | PG_PS | nx_bit();
    }

    /* 按段覆盖权限：启动头 RO+NX、.text RX、.rodata RO+NX、.data/.bss RW+NX。 */
    image = (u64)(usize)__image_start;
    text = (u64)(usize)__text_start;
    rodata_end = (u64)(usize)__rodata_end;
    kernel_end = (u64)(usize)__kernel_end;

    if (map_4k_range(pt, image, text, nx_bit()) != PG_OK) {
        return PG_EINVAL;
    }
    if (map_4k_range(pt, text, (u64)(usize)__text_end, 0) != PG_OK) {
        return PG_EINVAL;
    }
    if (map_4k_range(pt, (u64)(usize)__text_end, rodata_end, nx_bit()) != PG_OK) {
        return PG_EINVAL;
    }
    if (map_4k_range(pt, rodata_end, kernel_end, PG_WRITE | nx_bit()) != PG_OK) {
        return PG_EINVAL;
    }
    /* .data 单独标一次，便于证据行区分（.data 可能为空）。 */
    if (map_4k_range(pt, (u64)(usize)__data_start, (u64)(usize)__data_end,
                     PG_WRITE | nx_bit()) != PG_OK) {
        return PG_EINVAL;
    }
    /* .bss 单独标一次（页表池、栈都在这里）。 */
    if (map_4k_range(pt, (u64)(usize)__bss_start, (u64)(usize)__bss_end,
                     PG_WRITE | nx_bit()) != PG_OK) {
        return PG_EINVAL;
    }
    return PG_OK;
}

/* ------------------------------------------------------------------------- */
/* 安装：CR3 / CR0.WP / CR4.SMEP/SMAP                                         */
/* ------------------------------------------------------------------------- */

static void install_tables(void)
{
    u64 cr4;

    /*
     * 写 CR3 即切换页表。新表与引导表对当前代码页、栈、页表池的映射一致，
     * 且都在 0..1 GiB 恒等映射内，所以这条指令之后取指/访存都仍然有效。
     */
    write_cr3(pa_of(kernel_pml4));

    /* W^X 的 W 侧：CR0.WP=1，让 W=0 的页在 ring0 也真正只读。 */
    write_cr0(read_cr0() | CR0_WP);

    cr4 = read_cr4();
    if (cpu_smep) {
        cr4 |= CR4_SMEP;
    }
    if (cpu_smap) {
        cr4 |= CR4_SMAP;
    }
    write_cr4(cr4);
    /* CR4 变更后保守刷一次 TLB（重写 CR3）。 */
    write_cr3(pa_of(kernel_pml4));

    cr4 = read_cr4();
    smep_on = (cpu_smep && (cr4 & CR4_SMEP)) ? 1 : 0;
    smap_on = (cpu_smap && (cr4 & CR4_SMAP)) ? 1 : 0;
    expect(smep_on == cpu_smep, "cr4.smep matches cpuid");
    expect(smap_on == cpu_smap, "cr4.smap matches cpuid");
}

/* ------------------------------------------------------------------------- */
/* 走表 + 显式 helper                                                         */
/* ------------------------------------------------------------------------- */

u64 pg_walk(u64 va)
{
    u64 e;
    u64 *pdpt;
    u64 *pd;
    u64 *pt;

    if (kernel_pml4 == NULL) {
        return 0;
    }
    e = kernel_pml4[(va >> 39) & 0x1FFu];
    if ((e & PG_PRESENT) == 0) {
        return 0;
    }
    if (e & PG_PS) { /* 1 GiB 页（本 WP 不建，防御性处理） */
        return e;
    }
    pdpt = (u64 *)(usize)(e & PG_ADDR_MASK);

    e = pdpt[(va >> 30) & 0x1FFu];
    if ((e & PG_PRESENT) == 0) {
        return 0;
    }
    if (e & PG_PS) {
        return e;
    }
    pd = (u64 *)(usize)(e & PG_ADDR_MASK);

    e = pd[(va >> 21) & 0x1FFu];
    if ((e & PG_PRESENT) == 0) {
        return 0;
    }
    if (e & PG_PS) {
        return e;
    }
    pt = (u64 *)(usize)(e & PG_ADDR_MASK);

    e = pt[(va >> 12) & 0x1FFu];
    return (e & PG_PRESENT) ? e : 0;
}

/*
 * 取下一级表的指针。alloc=1 时缺表就建（池耗尽返回 PG_ENOMEM）；
 * 大页（PS=1）不自动拆分，直接 PG_EBIGPAGE。
 */
static int descend(u64 *entry, int alloc, u64 **next)
{
    u64 *table;

    if (*entry & PG_PRESENT) {
        if (*entry & PG_PS) {
            return PG_EBIGPAGE;
        }
        *next = (u64 *)(usize)(*entry & PG_ADDR_MASK);
        return PG_OK;
    }
    if (!alloc) {
        *next = NULL;
        return PG_ENOENT;
    }
    table = pt_alloc();
    if (table == NULL) {
        return PG_ENOMEM;
    }
    *entry = pa_of(table) | PG_PRESENT | PG_WRITE;
    *next = table;
    return PG_OK;
}

int pg_map(u64 va, u64 phys, u64 flags)
{
    int rc;
    u64 *pdpt = NULL;
    u64 *pd = NULL;
    u64 *pt = NULL;
    u64 *entry;

    if (kernel_pml4 == NULL) {
        return PG_EINVAL;
    }
    if (((va | phys) & (PAGE_4K - 1)) != 0) {
        return PG_EINVAL;
    }
    if (va >> 48) { /* 只支持 4 级（48 位）地址空间 */
        return PG_EINVAL;
    }

    rc = descend(&kernel_pml4[(va >> 39) & 0x1FFu], 1, &pdpt);
    if (rc != PG_OK) {
        return rc;
    }
    rc = descend(&pdpt[(va >> 30) & 0x1FFu], 1, &pd);
    if (rc != PG_OK) {
        return rc;
    }
    if (pd[(va >> 21) & 0x1FFu] & PG_PS) {
        return PG_EBIGPAGE;
    }
    rc = descend(&pd[(va >> 21) & 0x1FFu], 1, &pt);
    if (rc != PG_OK) {
        return rc;
    }

    entry = &pt[(va >> 12) & 0x1FFu];
    if (*entry & PG_PRESENT) {
        return PG_EEXIST;
    }
    *entry = (phys & PG_ADDR_MASK) | (flags & ~PG_PRESENT) | PG_PRESENT;
    invlpg(va);
    return PG_OK;
}

int pg_unmap(u64 va)
{
    int rc;
    u64 *pdpt = NULL;
    u64 *pd = NULL;
    u64 *pt = NULL;
    u64 *entry;

    if (kernel_pml4 == NULL) {
        return PG_EINVAL;
    }
    if ((va & (PAGE_4K - 1)) != 0) {
        return PG_EINVAL;
    }

    rc = descend(&kernel_pml4[(va >> 39) & 0x1FFu], 0, &pdpt);
    if (rc != PG_OK) {
        return rc;
    }
    rc = descend(&pdpt[(va >> 30) & 0x1FFu], 0, &pd);
    if (rc != PG_OK) {
        return rc;
    }
    if (pd[(va >> 21) & 0x1FFu] & PG_PS) {
        return PG_EBIGPAGE; /* 2 MiB 大页不拆，避免把整段 2 MiB 打散 */
    }
    rc = descend(&pd[(va >> 21) & 0x1FFu], 0, &pt);
    if (rc != PG_OK) {
        return rc;
    }

    entry = &pt[(va >> 12) & 0x1FFu];
    if ((*entry & PG_PRESENT) == 0) {
        return PG_ENOENT;
    }
    *entry = 0;
    invlpg(va);
    return PG_OK;
}

int pg_protect(u64 va, u64 flags)
{
    int rc;
    u64 *pdpt = NULL;
    u64 *pd = NULL;
    u64 *pt = NULL;
    u64 *entry;

    if (kernel_pml4 == NULL) {
        return PG_EINVAL;
    }
    if ((va & (PAGE_4K - 1)) != 0) {
        return PG_EINVAL;
    }

    rc = descend(&kernel_pml4[(va >> 39) & 0x1FFu], 0, &pdpt);
    if (rc != PG_OK) {
        return rc;
    }
    rc = descend(&pdpt[(va >> 30) & 0x1FFu], 0, &pd);
    if (rc != PG_OK) {
        return rc;
    }
    if (pd[(va >> 21) & 0x1FFu] & PG_PS) {
        return PG_EBIGPAGE;
    }
    rc = descend(&pd[(va >> 21) & 0x1FFu], 0, &pt);
    if (rc != PG_OK) {
        return rc;
    }

    entry = &pt[(va >> 12) & 0x1FFu];
    if ((*entry & PG_PRESENT) == 0) {
        return PG_ENOENT;
    }
    *entry = (*entry & PG_ADDR_MASK) | (flags & ~PG_PRESENT) | PG_PRESENT;
    invlpg(va);
    return PG_OK;
}

u64 pg_cr3(void)
{
    return read_cr3();
}

/* ------------------------------------------------------------------------- */
/* 自检：4 级结构 / 段权限 / 2 MiB 大页 / CR 与 EFER 读回 / helper 行为       */
/* ------------------------------------------------------------------------- */

static int check_4level(void)
{
    int ok = 1;
    int upper_clear = 1;
    u64 cr0 = read_cr0();
    u64 cr3 = read_cr3();
    u64 cr4 = read_cr4();
    u64 efer = rdmsr(MSR_EFER);
    u64 e;
    u64 *pdpt;
    u64 *pd;
    u32 i;

    ok &= expect((cr3 & PG_ADDR_MASK) == pa_of(kernel_pml4), "cr3 points at the new pml4");
    ok &= expect((cr3 & 0xFFF) == 0, "cr3 is 4 KiB aligned");
    ok &= expect((cr0 & CR0_PE) != 0, "cr0.pe");
    ok &= expect((cr0 & CR0_PG) != 0, "cr0.pg");
    ok &= expect((cr0 & CR0_WP) != 0, "cr0.wp");
    ok &= expect((cr4 & CR4_PAE) != 0, "cr4.pae");
    ok &= expect((efer & EFER_LMA) != 0, "efer.lma (long mode active)");
    ok &= expect((kernel_pml4[0] & PG_PRESENT) != 0, "pml4[0] present");

    for (i = 256; i < 512; i++) {
        if (kernel_pml4[i] != 0) {
            upper_clear = 0;
        }
    }
    ok &= expect(upper_clear != 0, "pml4 upper half (256..511) unused");

    e = kernel_pml4[0];
    if ((e & PG_PRESENT) == 0) {
        return 0;
    }
    pdpt = (u64 *)(usize)(e & PG_ADDR_MASK);
    ok &= expect((e & PG_PS) == 0, "pml4[0] is a table (not a huge page)");

    e = pdpt[0];
    ok &= expect((e & PG_PRESENT) != 0, "pdpt[0] present");
    ok &= expect((e & PG_PS) == 0, "pdpt[0] is a table (not a 1 GiB page)");
    if ((e & PG_PRESENT) == 0) {
        return 0;
    }
    pd = (u64 *)(usize)(e & PG_ADDR_MASK);

    ok &= expect((pd[0] & PG_PRESENT) != 0, "pd[0] present");
    ok &= expect((pd[0] & PG_PS) == 0, "pd[0] is a PT (4 KiB pages)");

    /* 读回三层表项原值：4 级结构 + 大页标志的直接证据。 */
    serial_puts("ng: tree pml4[0]=");
    serial_puthex(kernel_pml4[0]);
    serial_puts(" pdpt[0]=");
    serial_puthex(pdpt[0]);
    serial_puts(" pd[0]=");
    serial_puthex(pd[0]);
    serial_puts("\r\n");
    return ok;
}

/*
 * 逐页检查一个段：每页都必须存在、是 4 KiB 叶项，且 W/NX 与期望一致。
 * 打印一行证据（起始地址、页数、第一页的 PTE 原值）。
 */
static int check_seg(const char *name, u64 start, u64 end, int want_write,
                     int want_nx)
{
    int ok = 1;
    u64 pages = 0;
    u64 first = 0;
    u64 addr;
    int actual_write;
    int actual_nx;

    if (((start | end) & (PAGE_4K - 1)) != 0 || start > end) {
        ok = 0;
    }
    for (addr = start; addr < end; addr += PAGE_4K) {
        u64 e = pg_walk(addr);
        int w;
        int nx;

        if (e == 0 || (e & PG_PS)) {
            ok = 0;
            break;
        }
        w = (e & PG_WRITE) ? 1 : 0;
        nx = (e & PG_NX) ? 1 : 0;
        if (w != want_write || nx != want_nx) {
            ok = 0;
        }
        if (first == 0) {
            first = e;
        }
        pages++;
    }
    expect(ok, name);

    actual_write = (first & PG_WRITE) ? 1 : 0;
    actual_nx = (first & PG_NX) ? 1 : 0;
    serial_puts("ng: seg ");
    serial_puts(name);
    serial_puts(" ");
    serial_puthex(start);
    serial_puts("..");
    serial_puthex(end);
    serial_puts(" pages=");
    serial_putdec(pages);
    serial_puts(" pte=");
    serial_puthex(first);
    serial_puts(" W=");
    serial_putdec((u64)actual_write);
    serial_puts(" NX=");
    serial_putdec((u64)actual_nx);
    serial_puts("\r\n");
    return ok;
}

static int check_layout(void)
{
    int ok = 1;

    ok &= check_seg("note", (u64)(usize)__image_start, (u64)(usize)__text_start, 0,
                    nx_on);
    ok &= check_seg("text", (u64)(usize)__text_start, (u64)(usize)__text_end, 0, 0);
    ok &= check_seg("rodata", (u64)(usize)__rodata_start,
                    (u64)(usize)__rodata_end, 0, nx_on);
    ok &= check_seg("data", (u64)(usize)__data_start, (u64)(usize)__data_end, 1,
                    nx_on);
    ok &= check_seg("bss", (u64)(usize)__bss_start, (u64)(usize)__bss_end, 1,
                    nx_on);
    return ok;
}

/* PD[1..511] 必须是恒等映射的 2 MiB 大页：RW+NX，地址连续。 */
static int check_bigpages(void)
{
    int ok = 1;
    u64 count = 0;
    u64 *pd = NULL;
    u64 e;
    u32 i;

    e = kernel_pml4[0];
    if ((e & PG_PRESENT) == 0 || (e & PG_PS)) {
        return expect(0, "bigpage base");
    }
    e = ((u64 *)(usize)(e & PG_ADDR_MASK))[0];
    if ((e & PG_PRESENT) == 0 || (e & PG_PS)) {
        return expect(0, "bigpage pd");
    }
    pd = (u64 *)(usize)(e & PG_ADDR_MASK);

    for (i = 1; i < 512; i++) {
        e = pd[i];
        if ((e & PG_PRESENT) == 0 || (e & PG_PS) == 0) {
            ok = 0;
            continue;
        }
        if ((e & PG_WRITE) == 0) {
            ok = 0;
        }
        if (nx_on && (e & PG_NX) == 0) {
            ok = 0;
        }
        if ((e & PG_ADDR_MASK) != ((u64)i * PAGE_2M)) {
            ok = 0;
        }
        count++;
    }
    expect(ok, "2 MiB identity entries");
    expect(count == 511, "2 MiB entry count == 511");

    serial_puts("ng: 2m  ");
    serial_puthex(PAGE_2M);
    serial_puts("..");
    serial_puthex((u64)512 * PAGE_2M);
    serial_puts(" entries=");
    serial_putdec(count);
    serial_puts(" P=1 W=1 NX=");
    serial_putdec((u64)(nx_on ? 1 : 0));
    serial_puts("\r\n");
    return ok;
}

/* 读回 CR0/CR4/EFER 与表基址，作为「保护真的装上了」的证据。 */
static void print_control_state(void)
{
    serial_puts("ng: cr0=");
    serial_puthex(read_cr0());
    serial_puts(" cr4=");
    serial_puthex(read_cr4());
    serial_puts(" efer=");
    serial_puthex(rdmsr(MSR_EFER));
    serial_puts(" cr3=");
    serial_puthex(read_cr3());
    serial_puts("\r\n");
}

/*
 * helper 行为自测：在恒等映射之外的 1 GiB 处 map 一页 4 KiB，
 * 写/读模式 -> 重复 map 必须被拒 -> protect 成只读（W=0）-> 再读 ->
 * unmap -> PTE 归零；顺带覆盖 PDPT/PD/PT 的按需分配路径。
 * 注意：这里不能「写只读页」来验 W^X（那需要 #PF，留给 WP1-3）。
 */
static void check_helpers(void)
{
    const u64 pattern = 0xA5A55A5A01234567ULL;
    volatile u64 *p = (volatile u64 *)(usize)SCRATCH_VA;
    int before = fails;
    u64 phys = pa_of(helper_frame);

    expect(pg_map(SCRATCH_VA, phys, PG_WRITE | nx_bit()) == PG_OK, "helper map");
    *p = pattern;
    expect(*p == pattern, "helper write/read via new mapping");

    expect(pg_map(SCRATCH_VA, phys, PG_WRITE | nx_bit()) == PG_EEXIST,
           "helper duplicate map rejected");

    expect(pg_protect(SCRATCH_VA, nx_bit()) == PG_OK, "helper protect");
    expect((pg_walk(SCRATCH_VA) & PG_WRITE) == 0, "helper protect cleared W");
    expect(*p == pattern, "helper read after protect");

    expect(pg_unmap(SCRATCH_VA) == PG_OK, "helper unmap");
    expect(pg_walk(SCRATCH_VA) == 0, "helper unmap cleared pte");
    expect(pg_unmap(SCRATCH_VA) == PG_ENOENT, "helper unmap missing page");

    if (fails == before) {
        serial_puts("ng: helper map/unmap/protect ok\r\n");
    } else {
        serial_puts("ng: helper map/unmap/protect FAIL\r\n");
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int pg_init(void)
{
    int rc;
    int ok4;
    int okwx;

    detect_cpu();
    enable_nx();

    kernel_pml4 = pt_alloc();
    if (kernel_pml4 == NULL) {
        expect(0, "alloc pml4");
        return fails;
    }
    rc = build_identity(kernel_pml4);
    if (rc != PG_OK) {
        expect(0, "build 4-level identity tables");
        serial_puts("ng: paging build FAIL (");
        serial_putdec((u64)(-rc));
        serial_puts(")\r\n");
        return fails;
    }

    serial_puts("ng: pml4=");
    serial_puthex(pa_of(kernel_pml4));
    serial_puts(" tables=");
    serial_putdec((u64)pt_pool_used);
    serial_puts(" pool=");
    serial_putdec((u64)PT_POOL_PAGES);
    serial_puts(" (4-level, .bss)\r\n");

    install_tables();

    ok4 = check_4level();
    okwx = check_layout();
    check_bigpages();
    print_control_state();

    /* .data：已初始化、可写、不可执行（段权限读回断言之外的读写抽查）。 */
    expect(pg_boot_marker == PG_BOOT_MARKER, "initialized .data readable");
    pg_boot_marker = PG_BOOT_MARKER + 1;
    expect(pg_boot_marker == PG_BOOT_MARKER + 1, "initialized .data writable");

    if (ok4) {
        serial_puts("ng: paging 4-level ok\r\n");
    } else {
        serial_puts("ng: paging 4-level FAIL\r\n");
    }

    if (okwx && nx_on) {
        serial_puts("ng: wx layout ok (text rx, rodata ro, data rw-nx)\r\n");
    } else {
        serial_puts("ng: wx layout FAIL (text rx, rodata ro, data rw-nx)\r\n");
    }

    if (nx_on) {
        serial_puts("ng: nx on\r\n");
    } else {
        serial_puts("ng: nx off (cpuid or efer.nxe unavailable)\r\n");
    }

    serial_puts("ng: smep=");
    serial_puts(smep_on ? "on" : "off");
    serial_puts(" smap=");
    serial_puts(smap_on ? "on" : "off");
    serial_puts("\r\n");

    check_helpers();
    return fails;
}
