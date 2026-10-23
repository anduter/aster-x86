/* =============================================================================
 * memtest.c - 内存子系统自检
 *
 * 在分页开启、堆初始化之后调用，逐项验证：
 *   1. 单页分配/释放：地址页对齐、可读写、页数账目复原
 *   2. 连续 8 页：返回的地址确实连续、每页可独立读写
 *   3. 堆 malloc/free：两块互不覆盖、释放后可再次分配大块
 *   4. vmalloc 相邻块：释放前一块不会误伤后一块（这正是修复过的缺陷）
 *   5. 恒等映射最高页：顶部 2MB 不再落在映射之外（另一个修复过的缺陷）
 *   6. 连续申请 64 页：地址互不重复（校验位图“按字扫描”的正确性）
 *   7. 总账：页数与基线一致，且 PMM/VMM 的异常计数都为 0
 *
 * 结果以 "memtest: ... PASS/FAIL" 的形式打到串口，
 * 返回值是失败项数，0 表示全部通过。
 * ============================================================================= */
#include "memtest.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "../types.h"

/* 本轮的失败项数 */
static int failures;

/* 输出一行测试结果 */
static void report(const char *name, int ok)
{
    serial_puts("memtest: ");
    serial_puts(name);
    if (ok) {
        serial_puts(" ... PASS\r\n");
    } else {
        serial_puts(" ... FAIL\r\n");
        failures++;
    }
}

/* 1/5 单页分配与释放 */
static int test_page_alloc(void)
{
    unsigned int before = pmm.free_pages;
    unsigned char *p = (unsigned char *)pmm_alloc_page();
    int ok = 1;

    if (!p) return 0;

    if (((unsigned int)(unsigned long)p & 0xFFFU) != 0) ok = 0;   /* 必须页对齐 */

    p[0]    = 0xA5U;                    /* 首页字节 */
    p[4095] = 0x5AU;                    /* 末页字节 */
    if (p[0] != 0xA5U || p[4095] != 0x5AU) ok = 0;

    pmm_free_page(p);
    if (pmm.free_pages != before) ok = 0;   /* 页数必须复原 */

    return ok;
}

/* 2/5 连续多页分配 */
static int test_contig(void)
{
    unsigned int before = pmm.free_pages;
    unsigned char *p = (unsigned char *)pmm_alloc_pages(8);
    int ok = 1;

    if (!p) return 0;

    for (unsigned int i = 0; i < 8U; i++) {
        p[i * 4096U] = (unsigned char)(0x10U + i);
    }
    for (unsigned int i = 0; i < 8U; i++) {
        if (p[i * 4096U] != (unsigned char)(0x10U + i)) ok = 0;
    }

    pmm_free_pages(p, 8);
    if (pmm.free_pages != before) ok = 0;

    return ok;
}

/* 3/5 堆分配器 */
static int test_heap(void)
{
    unsigned char *a = (unsigned char *)malloc(100U);
    unsigned char *b = (unsigned char *)malloc(200U);
    int ok = 1;

    if (!a || !b || a == b) {
        if (a) free(a);
        if (b) free(b);
        return 0;
    }

    /* 两块都写满不同图案，检查没有互相覆盖 */
    for (unsigned int i = 0; i < 100U; i++) a[i] = 0x3CU;
    for (unsigned int i = 0; i < 200U; i++) b[i] = 0xC3U;
    for (unsigned int i = 0; i < 100U; i++) if (a[i] != 0x3CU) ok = 0;
    for (unsigned int i = 0; i < 200U; i++) if (b[i] != 0xC3U) ok = 0;

    free(a);
    free(b);

    /* 释放后应能重新分配一块更大的（说明合并正常） */
    unsigned char *c = (unsigned char *)malloc(400U);
    if (!c) ok = 0;
    else free(c);

    return ok;
}

/* 4/5 vmalloc 相邻块：释放前一块不能影响后一块 */
static int test_vmalloc_adjacent(void)
{
    unsigned char *a = (unsigned char *)vmalloc(1000U);
    unsigned char *b = (unsigned char *)vmalloc(1000U);
    int ok = 1;

    if (!a || !b || a == b) {
        if (a) vfree(a);
        if (b) vfree(b);
        return 0;
    }

    a[0]   = 0x11U;
    a[999] = 0x22U;
    b[0]   = 0x33U;
    b[999] = 0x44U;

    vfree(a);                           /* 只应释放 a 自己那一页 */

    /* 若 b 的页被误释放，它的虚拟地址已不可访问：
       此时既不能读 b 的数据，也不能再 vfree(b)，否则会直接缺页把内核打挂。
       所以先查映射，发现异常就立即判失败返回。 */
    if (vmm_get_phys((unsigned int)(unsigned long)b) == 0U) {
        return 0;
    }

    if (b[0] != 0x33U || b[999] != 0x44U) ok = 0;
    if (vmm_get_phys((unsigned int)(unsigned long)a) != 0U) ok = 0;  /* a 已取消映射 */

    vfree(b);
    return ok;
}

/* 5/6 恒等映射的最高一页
 * 回归用：PMM 的页号从 phys_base 起算，恒等映射若只覆盖 total_pages*4096，
 * 顶部约 2MB 就会落在映射之外。这里先查页表（不会触发缺页），
 * 确认已映射后再实际读写，避免测试本身把内核打挂。 */
static int test_top_page(void)
{
    unsigned int top = pmm.phys_base + ((pmm.total_pages - 1U) << 12);

    if (vmm_get_phys(top) != top) return 0;     /* 未映射：判定失败，不触碰该地址 */

    *(volatile unsigned char *)top = 0x5AU;
    if (*(volatile unsigned char *)top != 0x5AU) return 0;

    return 1;
}

/* 6/7 连续申请 64 页
 * 回归用：位图改成“按 32 位字扫描 + rover 起点”之后，最容易出的错是
 * 把同一页重复发出去。这里分配 64 页、两两比对地址，并逐页写标记。 */
static int test_distinct_pages(void)
{
    unsigned char *pages[64];
    int ok = 1;

    for (unsigned int i = 0; i < 64U; i++) {
        pages[i] = (unsigned char *)pmm_alloc_page();
        if (!pages[i]) {
            /* 内存不足：把已分配的还回去，直接判失败 */
            for (unsigned int j = 0; j < i; j++) pmm_free_page(pages[j]);
            return 0;
        }
        if (((unsigned int)(unsigned long)pages[i] & 0xFFFU) != 0) ok = 0;
        pages[i][0] = (unsigned char)i;
    }

    /* 两两比较，确认没有重复地址 */
    for (unsigned int i = 0; i < 64U; i++) {
        for (unsigned int j = i + 1U; j < 64U; j++) {
            if (pages[i] == pages[j]) ok = 0;
        }
    }

    /* 每页写入的标记必须还在，说明没有互相覆盖 */
    for (unsigned int i = 0; i < 64U; i++) {
        if (pages[i][0] != (unsigned char)i) ok = 0;
    }

    for (unsigned int i = 0; i < 64U; i++) pmm_free_page(pages[i]);
    return ok;
}

/* 7/8 PAE 2MB 恒等映射 + 内核 W^X + 4KB 高地址映射 */
static int test_large_page(void)
{
    const unsigned int va = 0x02000000U;      /* 32MB：内核映像之外的 2MB 恒等映射 */
    const unsigned int high = 0xE0000000U;    /* PDPT[3] 空白 4KB 区 */
    void *frame = 0;
    int ok = 1;
    extern uint8_t __kernel_text_start[], __kernel_rodata_start[], __kernel_data_start[];

    if (vmm_get_phys(va) != va) return 0;
    if ((vmm_user_flags(va) & PTE_PS) == 0U) return 0;

    /* 内核 W^X：text RX、rodata R+NX、data RW+NX */
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_text_start) & PTE_WRITE) != 0U) ok = 0;
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_text_start) & PTE_NOEXEC) != 0U) ok = 0;
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_rodata_start) & PTE_WRITE) != 0U) ok = 0;
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_rodata_start) & PTE_NOEXEC) == 0U) ok = 0;
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_data_start) & PTE_WRITE) == 0U) ok = 0;
    if ((vmm_user_flags((uint32_t)(uintptr_t)__kernel_data_start) & PTE_NOEXEC) == 0U) ok = 0;

    frame = pmm_alloc_page();
    if (frame == 0) return 0;

    *(volatile unsigned int *)frame = 0x5A5A5A5AU;

    if (vmm_map_page(high, (unsigned int)(unsigned long)frame,
                     PTE_PRESENT | PTE_WRITE) != 0) {
        ok = 0;
    }
    if (ok) {
        if (vmm_get_phys(high) != (unsigned int)(unsigned long)frame) ok = 0;
        if (*(volatile unsigned int *)high != 0x5A5A5A5AU) ok = 0;
    }

    (void)vmm_unmap_page(high);
    if (vmm_get_phys(high) != 0U) ok = 0;
    pmm_free_page(frame);
    return ok;
}

int mm_selftest(void)
{
    /* 预热：先做一次 vmalloc/vfree。第一次 vmalloc 会顺带分配一张页表，
       那张页表释放时并不归还 PMM；先预热掉，后面的页数对比才不会误判。 */
    void *warm = vmalloc(64U);
    if (warm) vfree(warm);

    /* 大页测试会常驻页表，放在基线之前跑，避免污染账目 */
    int split_ok = test_large_page();

    unsigned int baseline = pmm.free_pages;
    failures = 0;

    serial_puts("memtest: start\r\n");
    report("1/8 single page alloc+free", test_page_alloc());
    report("2/8 8 contiguous pages", test_contig());
    report("3/8 heap malloc+free", test_heap());
    report("4/8 vmalloc adjacent blocks", test_vmalloc_adjacent());
    report("5/8 top page of RAM is mapped", test_top_page());
    report("6/8 64 pages are all distinct", test_distinct_pages());
    report("7/8 PAE 2MB identity", split_ok);
    report("8/8 page accounting and fault counters",
           (pmm.free_pages == baseline) && (pmm_faults == 0U) && (vmm_faults == 0U));

    serial_puts("memtest: done, failures = ");
    serial_putdec((unsigned int)failures);
    serial_puts("\r\n");

    return failures;
}
