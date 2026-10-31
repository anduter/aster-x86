/*
 * ng WP1-4 host 单测：bitmap PMM（fake memory = 一块 8 MiB 的静态缓冲区）。
 *
 * PMM 的“物理地址”就是缓冲区里的地址：pmm_init(base, limit) 把页号相对 base
 * 计算，所以 64 位宿主地址也能直接用；分配清零会真的写这块缓冲区。
 */
#include "phys.h"
#include "pmm.h"
#include "test_util.h"

#define ARENA_SIZE (8u * 1024u * 1024u)
#define LOW_RESERVE (64u * 1024u)      /* 16 页 */
#define KERNEL_RESERVE (32u * 1024u)   /* 8 页，模拟内核镜像 */
#define ARENA_PAGES (ARENA_SIZE / 4096u)

static u8 arena[ARENA_SIZE] __attribute__((aligned(4096)));

static u64 abase(void)
{
    return (u64)(usize)arena;
}

static void pmm_reset(void)
{
    u64 base = abase();

    pmm_init(base, base + (u64)sizeof(arena));
    pmm_add_usable(base, (u64)sizeof(arena));
    pmm_reserve(base, LOW_RESERVE);
    pmm_reserve(base + 0x100000ULL, KERNEL_RESERVE);
}

int test_pmm(void)
{
    u64 base = abase();
    const pmm_stats *st;
    u64 p;
    u64 q;
    u32 free0;
    u32 dbl0;
    u32 fail0;
    u32 i;

    pmm_reset();
    st = pmm_stat();
    CHECK(pmm_total_pages() == ARENA_PAGES);
    CHECK(pmm_usable_pages() == ARENA_PAGES);
    CHECK(pmm_free_count() == ARENA_PAGES - 16u - 8u);
    CHECK(st->base == base);
    CHECK(st->limit == base + (u64)sizeof(arena));

    /* 保留页：低 64 KiB + 模拟内核区。 */
    CHECK(pmm_is_reserved(base));
    CHECK(pmm_is_reserved(base + 0x100000ULL));
    CHECK(!pmm_is_reserved(base + 0x200000ULL));
    CHECK(pmm_owns(base + 0x200000ULL));
    CHECK(!pmm_owns(base + 8ULL));
    CHECK(!pmm_owns(0));

    free0 = pmm_free_count();

    /* 单页分配：对齐、窗口内、非保留、可用。 */
    p = pmm_alloc_page();
    CHECK(p != 0);
    CHECK(NG_IS_PAGE_ALIGNED(p));
    CHECK(pmm_owns(p));
    CHECK(!pmm_is_reserved(p));
    CHECK(p >= base && p + NG_PAGE_SIZE <= base + (u64)sizeof(arena));
    CHECK(pmm_free_count() == free0 - 1u);
    {
        volatile u64 *w = (volatile u64 *)(usize)p;
        CHECK(w[0] == 0);
        CHECK(w[(NG_PAGE_SIZE / 8u) - 1u] == 0);
    }
    CHECK(pmm_free_page(p) == PMM_OK);
    CHECK(pmm_free_count() == free0);

    /* 连续多页：地址连续、每页都归 PMM 所有、可整体归还。 */
    q = pmm_alloc_pages(8);
    CHECK(q != 0);
    CHECK(NG_IS_PAGE_ALIGNED(q));
    for (i = 0; i < 8; i++) {
        CHECK(pmm_owns(q + (u64)i * NG_PAGE_SIZE));
        CHECK(!pmm_is_reserved(q + (u64)i * NG_PAGE_SIZE));
    }
    CHECK(q + 7ULL * NG_PAGE_SIZE == q + (u64)(7u * 4096u));
    CHECK(pmm_free_pages(q, 8) == PMM_OK);
    CHECK(pmm_free_count() == free0);

    /* double free 必须被拒且记账。 */
    p = pmm_alloc_page();
    CHECK(p != 0);
    CHECK(pmm_free_page(p) == PMM_OK);
    dbl0 = st->reject_double;
    CHECK(pmm_free_page(p) == PMM_EDOUBLE);
    CHECK(st->reject_double == dbl0 + 1u);

    /* 保留页（低区、内核区）释放必须被拒。 */
    CHECK(pmm_free_page(base) == PMM_ERESERVED);
    CHECK(pmm_free_page(base + 0x100000ULL) == PMM_ERESERVED);

    /* 未对齐 / 窗口外 / 跨窗口末端。 */
    CHECK(pmm_free_page(base + 0x200000ULL + 8ULL) == PMM_EINVAL);
    CHECK(pmm_free_page(base - NG_PAGE_SIZE) == PMM_ERANGE);
    CHECK(pmm_free_page(base + (u64)sizeof(arena)) == PMM_ERANGE);
    CHECK(pmm_owns(base + (u64)sizeof(arena) - NG_PAGE_SIZE));

    /* 参数边界。 */
    CHECK(pmm_alloc_pages(0) == 0);
    CHECK(pmm_free_pages(base + 0x200000ULL, 0) == PMM_EINVAL);
    fail0 = st->fail_count;
    CHECK(pmm_alloc_pages(pmm_free_count() + 1u) == 0);
    CHECK(st->fail_count > fail0);

    CHECK(pmm_free_count() == free0);
    return 0;
}
