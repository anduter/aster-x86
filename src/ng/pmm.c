/*
 * ng WP1-4: bitmap PMM（used + reserved 两份位图，页号相对管理窗口起点）。
 */
#include "pmm.h"

#ifndef PMM_ZERO_ON_ALLOC
#define PMM_ZERO_ON_ALLOC 1
#endif

#define BITMAP_BYTES (PMM_MAX_PAGES / 8u)

static u8 used_bits[BITMAP_BYTES];
static u8 reserved_bits[BITMAP_BYTES];
static pmm_stats st;

/* ------------------------------------------------------------------------- */
/* 位图                                                                       */
/* ------------------------------------------------------------------------- */

static int bit_get(const u8 *map, u32 idx)
{
    return (map[idx >> 3] >> (idx & 7u)) & 1u;
}

static void bit_set(u8 *map, u32 idx)
{
    map[idx >> 3] |= (u8)(1u << (idx & 7u));
}

static void bit_clear(u8 *map, u32 idx)
{
    map[idx >> 3] &= (u8)~(1u << (idx & 7u));
}

static void map_fill(u8 *map, u32 bytes, u8 value)
{
    u32 i;

    for (i = 0; i < bytes; i++) {
        map[i] = value;
    }
}

/* 饱和加法：不把坏范围回绕成小地址。 */
static u64 sat_add(u64 a, u64 b)
{
    u64 max = ~(u64)0;
    return (a > max - b) ? max : a + b;
}

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

int pmm_init(u64 base, u64 limit)
{
    u64 span;
    u32 bytes;

    base = NG_PAGE_ALIGN_DOWN(base);
    limit = NG_PAGE_ALIGN_DOWN(limit);
    if (base >= limit) {
        return PMM_EINVAL;
    }
    span = limit - base;
    if (span > (u64)PMM_MAX_PAGES * NG_PAGE_SIZE) {
        limit = base + (u64)PMM_MAX_PAGES * NG_PAGE_SIZE;
        span = (u64)PMM_MAX_PAGES * NG_PAGE_SIZE;
    }

    st.base = base;
    st.limit = limit;
    st.total_pages = (u32)(span >> NG_PAGE_SHIFT);
    st.usable_pages = 0;
    st.free_pages = 0;
    st.rover = 0;
    st.alloc_count = 0;
    st.free_count = 0;
    st.fail_count = 0;
    st.reject_double = 0;
    st.reject_reserved = 0;
    st.reject_range = 0;
    st.reject_align = 0;

    bytes = (st.total_pages + 7u) / 8u;
    map_fill(used_bits, bytes, 0xFFu);
    map_fill(reserved_bits, bytes, 0xFFu);
    return PMM_OK;
}

int pmm_add_usable(u64 base, u64 size)
{
    u64 end;
    u64 start;
    u32 i;
    u32 i0;
    u32 i1;

    if (size == 0) {
        return PMM_EINVAL;
    }
    if (base >= st.limit) {
        return PMM_OK;
    }
    start = NG_PAGE_ALIGN_UP(base);
    end = NG_PAGE_ALIGN_DOWN(sat_add(base, size));
    if (end <= start) {
        return PMM_OK; /* 不足一整页 */
    }
    if (start < st.base) {
        start = st.base;
    }
    if (end > st.limit) {
        end = st.limit;
    }
    if (end <= start) {
        return PMM_OK;
    }
    i0 = (u32)((start - st.base) >> NG_PAGE_SHIFT);
    i1 = (u32)((end - st.base) >> NG_PAGE_SHIFT);
    for (i = i0; i < i1; i++) {
        if (bit_get(used_bits, i)) {
            bit_clear(used_bits, i);
            bit_clear(reserved_bits, i);
            st.free_pages++;
            st.usable_pages++;
        }
    }
    return PMM_OK;
}

int pmm_reserve(u64 base, u64 size)
{
    u64 end;
    u64 start;
    u32 i;
    u32 i0;
    u32 i1;

    if (size == 0) {
        return PMM_OK;
    }
    if (base >= st.limit) {
        return PMM_OK;
    }
    start = NG_PAGE_ALIGN_DOWN(base);
    end = sat_add(base, size);
    if (end > st.limit) {
        end = st.limit;
    }
    end = NG_PAGE_ALIGN_UP(end);
    if (start < st.base) {
        start = st.base;
    }
    if (end > st.limit) {
        end = st.limit;
    }
    if (end <= start) {
        return PMM_OK;
    }
    i0 = (u32)((start - st.base) >> NG_PAGE_SHIFT);
    i1 = (u32)((end - st.base) >> NG_PAGE_SHIFT);
    for (i = i0; i < i1; i++) {
        if (!bit_get(used_bits, i)) {
            bit_set(used_bits, i);
            st.free_pages--;
        }
        bit_set(reserved_bits, i);
    }
    return PMM_OK;
}

/* ------------------------------------------------------------------------- */
/* 分配                                                                       */
/* ------------------------------------------------------------------------- */

static int find_run(u32 count, u32 *start_out)
{
    u32 total = st.total_pages;
    u32 start = (st.rover < total) ? st.rover : 0u;
    u32 pass;

    for (pass = 0; pass < 2u; pass++) {
        u32 from = (pass == 0u) ? start : 0u;
        u32 to = (pass == 0u) ? total : start;
        u32 run = 0;
        u32 run_start = 0;
        u32 i;

        for (i = from; i < to; i++) {
            if (bit_get(used_bits, i)) {
                run = 0;
                continue;
            }
            if (run == 0) {
                run_start = i;
            }
            run++;
            if (run >= count) {
                *start_out = run_start;
                return 1;
            }
        }
    }
    return 0;
}

#if PMM_ZERO_ON_ALLOC
static void zero_pages(u64 phys, u32 count)
{
    volatile u64 *p = (volatile u64 *)(usize)phys;
    u32 words = (u32)(NG_PAGE_SIZE / 8u) * count;
    u32 i;

    for (i = 0; i < words; i++) {
        p[i] = 0;
    }
}
#endif

u64 pmm_alloc_pages(u32 count)
{
    u32 start;
    u32 j;
    u64 phys;

    if (count == 0u || count > st.free_pages) {
        st.fail_count++;
        return 0;
    }
    if (!find_run(count, &start)) {
        st.fail_count++;
        return 0;
    }
    for (j = start; j < start + count; j++) {
        bit_set(used_bits, j);
    }
    st.free_pages -= count;
    st.alloc_count += count;
    st.rover = start + count;
    if (st.rover >= st.total_pages) {
        st.rover = 0;
    }

    phys = st.base + (u64)start * NG_PAGE_SIZE;
#if PMM_ZERO_ON_ALLOC
    zero_pages(phys, count);
#endif
    return phys;
}

u64 pmm_alloc_page(void)
{
    return pmm_alloc_pages(1u);
}

/* ------------------------------------------------------------------------- */
/* 释放（先整体校验，再整体归还；不允许半释放）                               */
/* ------------------------------------------------------------------------- */

static int validate_free(u64 phys, u32 count, u32 *idx_out)
{
    u32 i0;
    u32 j;

    if (count == 0u) {
        return PMM_EINVAL;
    }
    if (!NG_IS_PAGE_ALIGNED(phys)) {
        st.reject_align++;
        return PMM_EINVAL;
    }
    if (st.limit < NG_PAGE_SIZE || phys < st.base ||
        phys > st.limit - NG_PAGE_SIZE) {
        st.reject_range++;
        return PMM_ERANGE;
    }
    i0 = (u32)((phys - st.base) >> NG_PAGE_SHIFT);
    if ((u64)i0 + count > st.total_pages) {
        st.reject_range++;
        return PMM_ERANGE;
    }
    for (j = i0; j < i0 + count; j++) {
        if (bit_get(reserved_bits, j)) {
            st.reject_reserved++;
            return PMM_ERESERVED;
        }
        if (!bit_get(used_bits, j)) {
            st.reject_double++;
            return PMM_EDOUBLE;
        }
    }
    *idx_out = i0;
    return PMM_OK;
}

int pmm_free_pages(u64 phys, u32 count)
{
    u32 i0 = 0;
    int rc;
    u32 j;

    rc = validate_free(phys, count, &i0);
    if (rc != PMM_OK) {
        return rc;
    }
    for (j = i0; j < i0 + count; j++) {
        bit_clear(used_bits, j);
        st.free_pages++;
        st.free_count++;
    }
    return PMM_OK;
}

int pmm_free_page(u64 phys)
{
    return pmm_free_pages(phys, 1u);
}

/* ------------------------------------------------------------------------- */
/* 查询                                                                       */
/* ------------------------------------------------------------------------- */

u32 pmm_free_count(void)
{
    return st.free_pages;
}

u32 pmm_total_pages(void)
{
    return st.total_pages;
}

u32 pmm_usable_pages(void)
{
    return st.usable_pages;
}

int pmm_owns(u64 phys)
{
    if (!NG_IS_PAGE_ALIGNED(phys)) {
        return 0;
    }
    if (st.limit < NG_PAGE_SIZE || phys < st.base ||
        phys > st.limit - NG_PAGE_SIZE) {
        return 0;
    }
    return 1;
}

int pmm_is_reserved(u64 phys)
{
    u32 idx;

    if (!pmm_owns(phys)) {
        return 1;
    }
    idx = (u32)((phys - st.base) >> NG_PAGE_SHIFT);
    return bit_get(reserved_bits, idx);
}

const pmm_stats *pmm_stat(void)
{
    return &st;
}
