/* =============================================================================
 * tests/unit/test_pmm.c - 物理页位图分配器 host 单元测试
 *
 * 被测真实源码：src/kernel/mm/pmm.c（pmm_init / pmm_alloc_page(s) /
 * pmm_free_page(s) / pmm_page_share / pmm_page_ref / pmm_mark_range /
 * pmm_init_e820 / pmm_is_used）。
 *
 * host shim：
 *   - tests/unit/shim/include/kernel.h：自旋锁退化为 no-op；
 *   - serial_puts / serial_puthex32 / serial_putdec：测试 TU 提供；
 *   - 用一块 4KB 对齐的静态 arena 同时充当"位图地址 + 管理内存起点"，
 *     pmm_init(arena, arena + size) 让页号 0 映射到 arena（和真机 2MB 基准一致）。
 *
 * 为什么用 i686：pmm.h 的 mem_size/物理地址都是 unsigned int（32 位），
 * 64 位 host 上指针会截断。测试 TU 放在源列表第一位，让 arena 落在低位
 * BSS（早于 pmm.c 的 4MB page_ref），避开 HEAP_BASE=8MB 保留区。
 * ========================================================================== */
#include "unit.h"
#include "include/mm.h"

#define ARENA_PAGES 256U

static unsigned char arena[ARENA_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

void serial_puts(const char *s) { (void)s; }
void serial_puthex32(unsigned int v) { (void)v; }
void serial_putdec(unsigned int v) { (void)v; }

static unsigned int phys_of(size_t page)
{
    return (unsigned int)(uintptr_t)arena + (unsigned int)(page << PAGE_SHIFT);
}

static void reset_counters(void)
{
    pmm_faults = 0;
    pmm_alloc_count = 0;
    pmm_alloc_fail = 0;
    pmm_free_count = 0;
    pmm_double_free = 0;
    pmm_ref_anomalies = 0;
}

int main(void)
{
    void *pages[ARENA_PAGES];
    void *p;
    unsigned int before;
    unsigned int used;
    uint32_t i;

    UNIT_BEGIN("pmm/bitmap");

    /* arena 必须落在 HEAP_BASE(8MB) 以下，否则 page_is_used() 会把整块当保留区 */
    CHECK((uintptr_t)arena + sizeof(arena) <= (uintptr_t)HEAP_BASE);
    CHECK((((uintptr_t)arena) & (PAGE_SIZE - 1U)) == 0U);

    pmm_init(arena, (unsigned int)(uintptr_t)arena + (unsigned int)sizeof(arena));
    reset_counters();

    /* ---- 初始化 ---- */
    CHECK_U32(pmm.total_pages, ARENA_PAGES);
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 1U);   /* 位图自身占 1 页 */
    CHECK_U32(pmm.phys_base, (unsigned int)(uintptr_t)arena);
    CHECK_INT(pmm_is_used(phys_of(0)), 1);         /* 页号 0 = 位图页，永久保留 */
    CHECK_INT(pmm_is_used(phys_of(1)), 0);
    CHECK_INT(pmm_is_used(0), 1);                  /* 基准以下视为已用 */
    CHECK_INT(pmm_is_used(phys_of(ARENA_PAGES)), 1); /* 管理区之外视为已用 */
    CHECK_U32(pmm_page_ref(phys_of(1)), 0);
    CHECK_U32(pmm_page_ref(0), 0);

    /* ---- 单页分配/释放 + 毒化 ---- */
    p = pmm_alloc_page();
    CHECK(p != 0);
    CHECK_U32((unsigned int)(uintptr_t)p, phys_of(1));   /* rover 从 0 开始，跳过位图页 */
    CHECK_U32(((uintptr_t)p) & (PAGE_SIZE - 1U), 0);
    CHECK_INT(pmm_is_used((unsigned int)(uintptr_t)p), 1);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 1);
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 2U);
    CHECK_U32(pmm_alloc_count, 1);
    CHECK_U32(((unsigned char *)p)[0], 0x00);            /* 分配清零 */
    CHECK_U32(((unsigned char *)p)[PAGE_SIZE - 1U], 0x00);

    pmm_free_page(p);
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 1U);
    CHECK_U32(pmm_free_count, 1);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 0);
    CHECK_INT(pmm_is_used((unsigned int)(uintptr_t)p), 0);
    CHECK_U32(((unsigned char *)p)[0], 0xAA);            /* 释放毒化 */

    /* ---- 连续多页 ---- */
    p = pmm_alloc_pages(4);
    CHECK(p != 0);
    CHECK_U32(((uintptr_t)p) & (PAGE_SIZE - 1U), 0);
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 1U - 4U);
    CHECK_U32(pmm_alloc_count, 5);
    for (i = 0; i < 4U; i++) {
        CHECK_INT(pmm_is_used((unsigned int)(uintptr_t)p + (int)(i << PAGE_SHIFT)), 1);
        CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p + (int)(i << PAGE_SHIFT)), 1);
    }
    pmm_free_pages(p, 4);
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 1U);
    CHECK_U32(pmm_free_count, 5);

    /* 非法 count：0 与超过空闲页数 */
    before = pmm.free_pages;
    CHECK(pmm_alloc_pages(0) == 0);
    CHECK_U32(pmm_alloc_fail, 1);
    CHECK(pmm_alloc_pages(ARENA_PAGES) == 0);
    CHECK_U32(pmm_alloc_fail, 2);
    CHECK_U32(pmm.free_pages, before);

    /* ---- 耗尽全部空闲页，再分配必须失败 ---- */
    for (i = 0; i < ARENA_PAGES; i++) pages[i] = 0;
    {
        unsigned char seen[ARENA_PAGES];
        int bad = 0;

        for (i = 0; i < ARENA_PAGES; i++) seen[i] = 0;
        for (i = 0; i < ARENA_PAGES - 1U; i++) {
            size_t idx;

            pages[i] = pmm_alloc_page();
            if (pages[i] == 0) { bad = 1; break; }
            idx = (size_t)(((uintptr_t)pages[i] - (uintptr_t)arena) >> PAGE_SHIFT);
            if (idx >= ARENA_PAGES || seen[idx]) bad = 1;
            else seen[idx] = 1;
            if (pmm_is_used((unsigned int)(uintptr_t)pages[i]) != 1) bad = 1;
            if (pmm_page_ref((unsigned int)(uintptr_t)pages[i]) != 1) bad = 1;
        }
        CHECK_INT(bad, 0);
        CHECK_INT(seen[0], 0);                          /* 位图页不会被分配 */
    }
    CHECK_U32(pmm.free_pages, 0);
    CHECK(pmm_alloc_page() == 0);
    CHECK_U32(pmm_alloc_fail, 3);
    for (i = 0; i < ARENA_PAGES - 1U; i++) {
        if (pages[i] != 0) pmm_free_page(pages[i]);
    }
    CHECK_U32(pmm.free_pages, ARENA_PAGES - 1U);
    CHECK_U32(pmm_double_free, 0);

    /* ---- 非法释放：对齐/保留区/越界 ---- */
    reset_counters();
    before = pmm.free_pages;
    pmm_free_page(0);                                    /* NULL 静默忽略 */
    CHECK_U32(pmm_faults, 0);
    pmm_free_page((void *)((uintptr_t)arena + 1U));      /* 未对齐 */
    CHECK_U32(pmm_faults, 1);
    pmm_free_page(arena);                                /* 位图页保留 */
    CHECK_U32(pmm_faults, 2);
    pmm_free_page((void *)((uintptr_t)arena - PAGE_SIZE)); /* 基准以下 */
    CHECK_U32(pmm_faults, 3);
    pmm_free_pages((void *)((uintptr_t)arena + ((uintptr_t)ARENA_PAGES << PAGE_SHIFT)), 1);
    CHECK_U32(pmm_faults, 4);                            /* 末尾之外 */
    CHECK_U32(pmm.free_pages, before);
    CHECK_U32(pmm_free_count, 0);

    /* double free 只记账 + WARN，不重复归还 */
    p = pmm_alloc_page();
    CHECK(p != 0);
    pmm_free_page(p);
    CHECK_U32(pmm.free_pages, before);
    pmm_free_page(p);
    CHECK_U32(pmm.free_pages, before);
    CHECK_U32(pmm_double_free, 1);
    CHECK_U32(pmm_faults, 5);

    /* ---- 引用计数（COW 语义） ---- */
    reset_counters();
    before = pmm.free_pages;
    p = pmm_alloc_page();
    CHECK(p != 0);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 1);
    pmm_page_share((unsigned int)(uintptr_t)p);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 2);
    pmm_page_share((unsigned int)(uintptr_t)p);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 3);
    pmm_free_page(p);                                    /* 3 -> 2 */
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 2);
    CHECK_U32(pmm.free_pages, before - 1U);
    CHECK_U32(pmm_free_count, 0);
    pmm_free_page(p);                                    /* 2 -> 1 */
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 1);
    CHECK_INT(pmm_is_used((unsigned int)(uintptr_t)p), 1);
    pmm_free_page(p);                                    /* 1 -> 0，真正归还 */
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 0);
    CHECK_INT(pmm_is_used((unsigned int)(uintptr_t)p), 0);
    CHECK_U32(pmm.free_pages, before);
    CHECK_U32(pmm_free_count, 1);

    /* 引用计数上限 255：再加不溢出 */
    p = pmm_alloc_page();
    for (i = 0; i < 300U; i++) pmm_page_share((unsigned int)(uintptr_t)p);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 255);
    pmm_free_pages(p, 1);                                /* 255 -> 254，仍不归还 */
    CHECK_U32(pmm.free_pages, before - 1U);
    for (i = 0; i < 254U; i++) pmm_free_page(p);
    CHECK_U32(pmm_page_ref((unsigned int)(uintptr_t)p), 0);
    CHECK_U32(pmm.free_pages, before);

    /* ---- pmm_mark_range / pmm_init_e820 ---- */
    reset_counters();
    before = pmm.free_pages;
    pmm_mark_range(phys_of(10), 3U * PAGE_SIZE);
    CHECK_INT(pmm_is_used(phys_of(10)), 1);
    CHECK_INT(pmm_is_used(phys_of(12)), 1);
    CHECK_INT(pmm_is_used(phys_of(13)), 0);
    CHECK_U32(pmm.free_pages, before - 3U);
    pmm_mark_range(phys_of(10), 3U * PAGE_SIZE);         /* 幂等：已用页不重复扣 */
    CHECK_U32(pmm.free_pages, before - 3U);

    /* 整段在基准以下：直接返回，不能把页号下溢成巨值 */
    pmm_mark_range(0, (uint64_t)(uintptr_t)arena);       /* end == 基准 */
    pmm_mark_range(0, 4096U);                            /* end < 基准 */
    CHECK_U32(pmm.free_pages, before - 3U);

    /* 跨末尾：end_page 被裁剪到 total_pages */
    pmm_mark_range(phys_of(250), 100U * PAGE_SIZE);
    CHECK_INT(pmm_is_used(phys_of(250)), 1);
    CHECK_INT(pmm_is_used(phys_of(255)), 1);
    CHECK_U32(pmm.free_pages, before - 3U - 6U);

    /* E820：只有非 FREE 区域被标记；FREE 不动 */
    {
        e820_entry_t e820[2];

        e820[0].base = phys_of(20);
        e820[0].length = PAGE_SIZE;
        e820[0].type = E820_TYPE_RESERVED;
        e820[0].acpi_attr = 0;
        e820[1].base = phys_of(21);
        e820[1].length = PAGE_SIZE;
        e820[1].type = E820_TYPE_FREE;
        e820[1].acpi_attr = 0;
        used = pmm.free_pages;
        pmm_init_e820(e820, 2);
        CHECK_INT(pmm_is_used(phys_of(20)), 1);
        CHECK_INT(pmm_is_used(phys_of(21)), 0);
        CHECK_U32(pmm.free_pages, used - 1U);
    }

    UNIT_END();
}
