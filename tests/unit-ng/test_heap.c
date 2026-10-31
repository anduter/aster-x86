/*
 * ng WP1-4 host 单测：内核堆（first-fit + 合并 + 对齐 + double-free）。
 *
 * heap.c 在 NG_HOST_TEST 下通过 ng_host_alloc_pages() 向这里要“物理页”；
 * fake 分配器从一块 4 MiB 的静态缓冲区里按页 bump，足够覆盖扩堆/大分配路径。
 */
#include "heap.h"
#include "phys.h"
#include "test_util.h"

#define FAKE_PAGES 1024u /* 4 MiB */

static u8 fake[FAKE_PAGES * 4096u] __attribute__((aligned(4096)));
static u32 fake_pages_used;

void *ng_host_alloc_pages(u32 count)
{
    u64 bytes;

    if (count == 0) {
        return NULL;
    }
    if ((u64)fake_pages_used + (u64)count > (u64)FAKE_PAGES) {
        return NULL;
    }
    bytes = (u64)fake_pages_used * NG_PAGE_SIZE;
    fake_pages_used += count;
    return (void *)(usize)((u64)(usize)fake + bytes);
}

static void heap_reset(void)
{
    u32 i;

    fake_pages_used = 0;
    for (i = 0; i < (u32)sizeof(fake); i++) {
        fake[i] = 0;
    }
    heap_init();
}

static void fill(u8 *p, u32 n, u8 seed)
{
    u32 i;

    for (i = 0; i < n; i++) {
        p[i] = (u8)(seed + i);
    }
}

static int check_fill(const u8 *p, u32 n, u8 seed)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (p[i] != (u8)(seed + i)) {
            return 0;
        }
    }
    return 1;
}

int test_heap(void)
{
    u8 *a;
    u8 *b;
    u8 *c;
    u8 *e;
    void *al;
    u64 base;
    u32 dbl0;
    u32 bad0;

    heap_reset();
    CHECK(heap_block_count() == 0);
    CHECK(heap_region_count() == 0);
    CHECK(heap_free_bytes() == 0);
    CHECK(heap_live_bytes() == 0);
    CHECK(heap_live_allocs() == 0);
    CHECK(heap_check() == 0);

    /* 基本分配：16 字节对齐、可写可读、记账。 */
    a = (u8 *)kmalloc(64);
    CHECK(a != NULL);
    CHECK(((u64)(usize)a & 15ULL) == 0);
    CHECK(heap_region_count() == 1);
    CHECK(heap_live_allocs() == 1);
    CHECK(heap_live_bytes() == 64);
    if (a != NULL) {
        fill(a, 64, 0x20);
        CHECK(check_fill(a, 64, 0x20));
    }

    b = (u8 *)kmalloc(128);
    c = (u8 *)kmalloc(64);
    CHECK(b != NULL && c != NULL);
    CHECK(a != b && b != c && a != c);
    CHECK(heap_live_allocs() == 3);
    CHECK(heap_live_bytes() == 64 + 128 + 64);
    if (b != NULL) {
        fill(b, 128, 0x40);
        CHECK(check_fill(b, 128, 0x40));
    }

    CHECK(kmalloc(0) == NULL);
    CHECK(kmalloc_aligned(16, 24) == NULL);   /* 非 2 的幂 */
    CHECK(kmalloc_aligned(16, 1ULL << 21) == NULL);

    /* 对齐分配：payload 地址按 align 对齐，且释放后堆仍健康。 */
    al = kmalloc_aligned(100, 64);
    CHECK(al != NULL && (((u64)(usize)al & 63ULL) == 0));
    if (al != NULL) {
        fill((u8 *)al, 100, 0x60);
        CHECK(check_fill((const u8 *)al, 100, 0x60));
        CHECK(kfree(al) == HEAP_OK);
    }
    al = kmalloc_aligned(100, 4096);
    CHECK(al != NULL && (((u64)(usize)al & 4095ULL) == 0));
    if (al != NULL) {
        fill((u8 *)al, 100, 0x70);
        CHECK(check_fill((const u8 *)al, 100, 0x70));
        CHECK(kfree(al) == HEAP_OK);
    }

    /* realloc：扩大/缩小都保留数据。 */
    if (a != NULL) {
        fill(a, 64, 0x11);
        a = (u8 *)krealloc(a, 256);
        CHECK(a != NULL);
        CHECK(check_fill(a, 64, 0x11));
        if (a != NULL) {
            fill(a, 256, 0x22);
            a = (u8 *)krealloc(a, 32);
            CHECK(a != NULL);
            CHECK(check_fill(a, 32, 0x22));
        }
        CHECK(kfree(a) == HEAP_OK);
    }
    if (b != NULL) {
        CHECK(kfree(b) == HEAP_OK);
    }
    if (c != NULL) {
        CHECK(kfree(c) == HEAP_OK);
    }

    /* 全部释放后：单个 region 只剩一个空闲块，且无泄漏记账。 */
    CHECK(heap_live_allocs() == 0);
    CHECK(heap_live_bytes() == 0);
    CHECK(heap_block_count() == 1);
    CHECK(heap_free_bytes() + HEAP_HDR_SIZE ==
          (u64)HEAP_GROW_MIN_PAGES * NG_PAGE_SIZE);
    CHECK(heap_check() == 0);

    /* 首次适配：reset 后第一个分配落在 region 起点（fake + 48）。 */
    heap_reset();
    base = (u64)(usize)fake;
    a = (u8 *)kmalloc(100);
    CHECK(a != NULL);
    CHECK((u64)(usize)a == base + HEAP_HDR_SIZE);
    CHECK(kfree(a) == HEAP_OK);
    a = (u8 *)kmalloc(32);
    CHECK(a != NULL);
    CHECK((u64)(usize)a == base + HEAP_HDR_SIZE);
    CHECK(kfree(a) == HEAP_OK);

    /* 合并：三个相邻块按乱序释放后必须并回一个。 */
    heap_reset();
    a = (u8 *)kmalloc(100);
    b = (u8 *)kmalloc(200);
    c = (u8 *)kmalloc(100);
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK(kfree(a) == HEAP_OK);
    CHECK(kfree(c) == HEAP_OK);
    CHECK(heap_block_count() == 3); /* used b + 两个空闲块（未相邻，暂不合并） */
    CHECK(kfree(b) == HEAP_OK);
    CHECK(heap_block_count() == 1);
    CHECK(heap_free_bytes() + HEAP_HDR_SIZE ==
          (u64)HEAP_GROW_MIN_PAGES * NG_PAGE_SIZE);
    CHECK(heap_live_allocs() == 0);
    CHECK(heap_live_bytes() == 0);
    CHECK(heap_check() == 0);

    /* double free：第一次释放后块头仍是自己（与右侧空闲块合并），
     * 第二次必须返回 EDOUBLE。 */
    heap_reset();
    e = (u8 *)kmalloc(16);
    CHECK(e != NULL);
    if (e != NULL) {
        dbl0 = heap_double_free_count();
        CHECK(kfree(e) == HEAP_OK);
        CHECK(kfree(e) == HEAP_EDOUBLE);
        CHECK(heap_double_free_count() == dbl0 + 1u);
    }

    /* 野指针 / 未对齐指针：不能把堆写坏。 */
    heap_reset();
    e = (u8 *)kmalloc(16);
    CHECK(e != NULL);
    if (e != NULL) {
        bad0 = heap_bad_free_count();
        CHECK(kfree((u8 *)e + 8) == HEAP_EINVAL);
        CHECK(heap_bad_free_count() == bad0 + 1u);
        CHECK(kfree(NULL) == HEAP_OK);
        CHECK(kfree(e) == HEAP_OK);
    }
    CHECK(heap_check() == 0);

    /* 扩堆：单次大分配 + 第二次分配触发第二个 region。 */
    heap_reset();
    a = (u8 *)kmalloc(70000);
    CHECK(a != NULL);
    CHECK(heap_region_count() == 1);
    b = (u8 *)kmalloc(60000);
    CHECK(b != NULL);
    CHECK(heap_region_count() == 2);
    if (a != NULL) {
        fill(a, 70000, 0x33);
        CHECK(check_fill(a, 70000, 0x33));
    }
    if (b != NULL) {
        fill(b, 60000, 0x44);
        CHECK(check_fill(b, 60000, 0x44));
    }
    CHECK(kfree(a) == HEAP_OK);
    CHECK(kfree(b) == HEAP_OK);
    CHECK(heap_block_count() == heap_region_count());
    CHECK(heap_live_allocs() == 0);
    CHECK(heap_check() == 0);

    /* 超出 fake 内存：扩堆失败返回 NULL，不破坏现有记账。 */
    heap_reset();
    al = kmalloc(8u * 1024u * 1024u);
    CHECK(al == NULL);
    CHECK(heap_region_count() == 0);
    CHECK(heap_live_allocs() == 0);
    CHECK(heap_check() == 0);

    return 0;
}
