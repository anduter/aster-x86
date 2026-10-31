/*
 * ng WP1-4: 内核堆（first-fit + 相邻空闲块合并）。
 */
#include "heap.h"

#include "phys.h"

#ifdef NG_HOST_TEST
/* host 单测提供：返回 count 个连续、4 KiB 对齐、可读写的“物理页”。 */
extern void *ng_host_alloc_pages(u32 count);
#define HEAP_ALLOC_PAGES(n) (ng_host_alloc_pages(n))
#define HEAP_FREE_PAGES(p, n) ((void)(p), (void)(n))
#else
#include "pmm.h"
#define HEAP_ALLOC_PAGES(n) ((void *)(usize)pmm_alloc_pages(n))
#define HEAP_FREE_PAGES(p, n) ((void)pmm_free_pages((u64)(usize)(p), (n)))
#endif

#define HEAP_MAGIC_LIVE 0x4E47484D4C495645ULL /* "NGHMLIVE" */
#define HEAP_MAGIC_FREE 0x4E47484D46524545ULL /* "NGHMFREE" */
#define HEAP_MAGIC_DEAD 0x4E47484D44454144ULL /* "NGHMDEAD" */

#define HEAP_BLOCK_FREE 0x1u

typedef struct heap_block {
    u64 magic;
    u64 size; /* payload 字节数，16 字节对齐 */
    u32 flags;
    u32 pad;
    struct heap_block *prev;
    struct heap_block *next;
    u64 spare; /* 凑满 48 字节：payload 相对块头保持 16 字节对齐 */
} heap_block;

_Static_assert(sizeof(heap_block) == HEAP_HDR_SIZE, "heap_block header must be 48 bytes");

typedef struct {
    u64 base;
    u64 end;
} heap_region;

static heap_block *heap_head;
static heap_region heap_regions[HEAP_MAX_REGIONS];
static u32 heap_regions_used;

static u64 heap_live_bytes_v;
static u32 heap_live_allocs_v;
static u32 heap_double_free_v;
static u32 heap_bad_free_v;

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static u64 round16(u64 v)
{
    return (v + 15ULL) & ~15ULL;
}

static u64 block_end(const heap_block *b)
{
    return (u64)(usize)b + HEAP_HDR_SIZE + b->size;
}

static heap_region *region_of(u64 addr)
{
    u32 i;

    for (i = 0; i < heap_regions_used; i++) {
        if (addr >= heap_regions[i].base && addr < heap_regions[i].end) {
            return &heap_regions[i];
        }
    }
    return NULL;
}

static int same_region(const heap_block *a, const heap_block *b)
{
    heap_region *ra = region_of((u64)(usize)a);
    heap_region *rb = region_of((u64)(usize)b);

    return (ra != NULL && ra == rb) ? 1 : 0;
}

static void insert_sorted(heap_block *b)
{
    heap_block *cur = heap_head;
    heap_block *prev = NULL;

    while (cur != NULL && (u64)(usize)cur < (u64)(usize)b) {
        prev = cur;
        cur = cur->next;
    }
    b->prev = prev;
    b->next = cur;
    if (prev != NULL) {
        prev->next = b;
    } else {
        heap_head = b;
    }
    if (cur != NULL) {
        cur->prev = b;
    }
}

/* 与同 region 的相邻空闲块合并；被吸收的块头标 DEAD，迟到的 double free
 * 会被 magic 检查拒绝。 */
static void coalesce(heap_block *b)
{
    heap_block *n = b->next;

    if (n != NULL && (n->flags & HEAP_BLOCK_FREE) != 0 && same_region(b, n) &&
        block_end(b) == (u64)(usize)n) {
        b->size += HEAP_HDR_SIZE + n->size;
        b->next = n->next;
        if (n->next != NULL) {
            n->next->prev = b;
        }
        n->magic = HEAP_MAGIC_DEAD;
        n->size = 0;
        n->flags = 0;
        n->prev = NULL;
        n->next = NULL;
    }

    n = b->prev;
    if (n != NULL && (n->flags & HEAP_BLOCK_FREE) != 0 && same_region(n, b) &&
        block_end(n) == (u64)(usize)b) {
        n->size += HEAP_HDR_SIZE + b->size;
        n->next = b->next;
        if (b->next != NULL) {
            b->next->prev = n;
        }
        b->magic = HEAP_MAGIC_DEAD;
        b->size = 0;
        b->flags = 0;
        b->prev = NULL;
        b->next = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* 初始化 / 扩堆                                                              */
/* ------------------------------------------------------------------------- */

void heap_init(void)
{
    heap_head = NULL;
    heap_regions_used = 0;
    heap_live_bytes_v = 0;
    heap_live_allocs_v = 0;
    heap_double_free_v = 0;
    heap_bad_free_v = 0;
}

int heap_add_region(void *base, u64 size)
{
    u64 b;
    u64 e;
    heap_block *blk;

    if (base == NULL || size == 0) {
        return HEAP_EINVAL;
    }
    b = (u64)(usize)base;
    if (size > ~(u64)0 - b) {
        return HEAP_EINVAL;
    }
    e = b + size;
    b = round16(b);
    e &= ~15ULL;
    if (e <= b || e - b < HEAP_HDR_SIZE + HEAP_MIN_PAYLOAD) {
        return HEAP_EINVAL;
    }
    if (heap_regions_used >= HEAP_MAX_REGIONS) {
        return HEAP_ENOMEM;
    }
    heap_regions[heap_regions_used].base = b;
    heap_regions[heap_regions_used].end = e;
    heap_regions_used++;

    blk = (heap_block *)(usize)b;
    blk->magic = HEAP_MAGIC_FREE;
    blk->size = e - b - HEAP_HDR_SIZE;
    blk->flags = HEAP_BLOCK_FREE;
    blk->pad = 0;
    blk->prev = NULL;
    blk->next = NULL;
    insert_sorted(blk);
    coalesce(blk);
    return HEAP_OK;
}

static int heap_grow(u64 need)
{
    u64 pages;
    void *p;

    if (need > NG_PHYS_LIMIT) {
        return 0;
    }
    pages = (need + HEAP_HDR_SIZE + NG_PAGE_SIZE - 1ULL) / NG_PAGE_SIZE;
    if (pages < HEAP_GROW_MIN_PAGES) {
        pages = HEAP_GROW_MIN_PAGES;
    }
    if (pages > (u64)(~0u)) {
        return 0;
    }
    p = HEAP_ALLOC_PAGES((u32)pages);
    if (p == NULL) {
        return 0;
    }
    if (heap_add_region(p, pages * NG_PAGE_SIZE) != HEAP_OK) {
        HEAP_FREE_PAGES(p, (u32)pages);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* 块切分                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * 从 b 里切出 size 字节、按 align 对齐的 payload。
 * 对齐靠“前移块头”：把 [b, 新块头) 变成一个新的空闲块，剩余部分照常切尾。
 */
static void *block_take(heap_block *b, u64 size, u64 align, heap_block **out)
{
    u64 base = (u64)(usize)b;
    u64 payload = base + HEAP_HDR_SIZE;
    u64 have = b->size;
    u64 d = 0;
    u64 real_size;

    if (align > 16ULL) {
        u64 aligned = (payload + align - 1ULL) & ~(align - 1ULL);
        u64 gap = aligned - payload;

        if (gap != 0) {
            d = gap - HEAP_HDR_SIZE;
            while (d < HEAP_MIN_PAYLOAD) {
                d += align;
            }
        }
    }

    if (have < (d != 0 ? d + HEAP_HDR_SIZE : 0ULL) + size) {
        return NULL;
    }

    if (d != 0) {
        heap_block *front = b;
        heap_block *real = (heap_block *)(usize)(payload + d);

        front->size = d;
        real->magic = HEAP_MAGIC_LIVE;
        real->size = have - d - HEAP_HDR_SIZE;
        real->flags = 0;
        real->pad = 0;
        real->prev = front;
        real->next = front->next;
        if (real->next != NULL) {
            real->next->prev = real;
        }
        front->next = real;
        b = real;
        real_size = real->size;
    } else {
        real_size = have;
    }

    if (real_size - size >= HEAP_HDR_SIZE + HEAP_MIN_PAYLOAD) {
        heap_block *rest = (heap_block *)(usize)((u64)(usize)b + HEAP_HDR_SIZE + size);

        rest->magic = HEAP_MAGIC_FREE;
        rest->size = real_size - size - HEAP_HDR_SIZE;
        rest->flags = HEAP_BLOCK_FREE;
        rest->pad = 0;
        rest->prev = b;
        rest->next = b->next;
        if (rest->next != NULL) {
            rest->next->prev = rest;
        }
        b->next = rest;
        b->size = size;
    }

    b->flags &= ~HEAP_BLOCK_FREE;
    b->magic = HEAP_MAGIC_LIVE;
    *out = b;
    return (void *)(usize)((u64)(usize)b + HEAP_HDR_SIZE);
}

static void *heap_alloc(u64 size, u64 align)
{
    heap_block *b;
    heap_block *out = NULL;
    void *p;
    u64 grow_need;

    if (size == 0 || size > NG_PHYS_LIMIT) {
        return NULL;
    }
    size = round16(size);
    if (size == 0) {
        return NULL;
    }
    if (align < 16ULL) {
        align = 16ULL;
    }
    grow_need = size + 2ULL * align + 2ULL * HEAP_HDR_SIZE;

    for (;;) {
        for (b = heap_head; b != NULL; b = b->next) {
            if ((b->flags & HEAP_BLOCK_FREE) == 0 || b->size < size) {
                continue;
            }
            p = block_take(b, size, align, &out);
            if (p != NULL) {
                heap_live_bytes_v += out->size;
                heap_live_allocs_v++;
                return p;
            }
        }
        if (!heap_grow(grow_need)) {
            return NULL;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 对外 API                                                                   */
/* ------------------------------------------------------------------------- */

void *kmalloc(u64 size)
{
    return heap_alloc(size, 16ULL);
}

void *kmalloc_aligned(u64 size, u64 align)
{
    if (align < 16ULL) {
        align = 16ULL;
    }
    if ((align & (align - 1ULL)) != 0 || align > (1ULL << 20)) {
        return NULL;
    }
    return heap_alloc(size, align);
}

int kfree(void *ptr)
{
    u64 addr;
    heap_block *b;
    heap_region *r;
    u64 hdr_end;

    if (ptr == NULL) {
        return HEAP_OK;
    }
    addr = (u64)(usize)ptr;
    if (addr < HEAP_HDR_SIZE) {
        heap_bad_free_v++;
        return HEAP_EINVAL;
    }
    b = (heap_block *)(usize)(addr - HEAP_HDR_SIZE);
    r = region_of((u64)(usize)b);
    if (r == NULL) {
        heap_bad_free_v++;
        return HEAP_EINVAL;
    }
    hdr_end = (u64)(usize)b + HEAP_HDR_SIZE;
    if (hdr_end > r->end ||
        (b->magic != HEAP_MAGIC_LIVE && b->magic != HEAP_MAGIC_FREE) ||
        b->size > r->end - hdr_end) {
        heap_bad_free_v++;
        return HEAP_EINVAL;
    }
    if ((b->flags & HEAP_BLOCK_FREE) != 0) {
        heap_double_free_v++;
        return HEAP_EDOUBLE;
    }

    b->flags |= HEAP_BLOCK_FREE;
    b->magic = HEAP_MAGIC_FREE;
    if (heap_live_bytes_v >= b->size) {
        heap_live_bytes_v -= b->size;
    } else {
        heap_live_bytes_v = 0;
    }
    if (heap_live_allocs_v > 0) {
        heap_live_allocs_v--;
    }
    coalesce(b);
    return HEAP_OK;
}

void *krealloc(void *ptr, u64 size)
{
    u64 addr;
    heap_block *b;
    heap_region *r;
    u64 hdr_end;
    u64 nsize;

    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        (void)kfree(ptr);
        return NULL;
    }
    addr = (u64)(usize)ptr;
    if (addr < HEAP_HDR_SIZE) {
        return NULL;
    }
    b = (heap_block *)(usize)(addr - HEAP_HDR_SIZE);
    r = region_of((u64)(usize)b);
    if (r == NULL) {
        return NULL;
    }
    hdr_end = (u64)(usize)b + HEAP_HDR_SIZE;
    if (hdr_end > r->end || b->magic != HEAP_MAGIC_LIVE ||
        (b->flags & HEAP_BLOCK_FREE) != 0 || b->size > r->end - hdr_end) {
        return NULL;
    }
    if (size > NG_PHYS_LIMIT) {
        return NULL;
    }
    nsize = round16(size);
    if (nsize == 0) {
        return NULL;
    }

    if (b->size >= nsize) {
        u64 rem = b->size - nsize;

        if (rem >= HEAP_HDR_SIZE + HEAP_MIN_PAYLOAD) {
            heap_block *rest = (heap_block *)(usize)((u64)(usize)b + HEAP_HDR_SIZE + nsize);

            rest->magic = HEAP_MAGIC_FREE;
            rest->size = rem - HEAP_HDR_SIZE;
            rest->flags = HEAP_BLOCK_FREE;
            rest->pad = 0;
            rest->prev = b;
            rest->next = b->next;
            if (rest->next != NULL) {
                rest->next->prev = rest;
            }
            b->next = rest;
            b->size = nsize;
            if (heap_live_bytes_v >= rem) {
                heap_live_bytes_v -= rem;
            } else {
                heap_live_bytes_v = 0;
            }
            coalesce(rest);
        }
        return ptr;
    }

    {
        void *np = kmalloc(size);
        u64 copy = b->size;
        u8 *dst;
        const u8 *src;
        u64 i;

        if (np == NULL) {
            return NULL;
        }
        if (copy > nsize) {
            copy = nsize;
        }
        dst = (u8 *)np;
        src = (const u8 *)ptr;
        for (i = 0; i < copy; i++) {
            dst[i] = src[i];
        }
        (void)kfree(ptr);
        return np;
    }
}

/* ------------------------------------------------------------------------- */
/* 诊断                                                                       */
/* ------------------------------------------------------------------------- */

u64 heap_free_bytes(void)
{
    heap_block *b;
    u64 total = 0;

    for (b = heap_head; b != NULL; b = b->next) {
        if ((b->flags & HEAP_BLOCK_FREE) != 0) {
            total += b->size;
        }
    }
    return total;
}

u64 heap_live_bytes(void)
{
    return heap_live_bytes_v;
}

u32 heap_live_allocs(void)
{
    return heap_live_allocs_v;
}

u32 heap_block_count(void)
{
    heap_block *b;
    u32 n = 0;

    for (b = heap_head; b != NULL; b = b->next) {
        n++;
    }
    return n;
}

u32 heap_region_count(void)
{
    return heap_regions_used;
}

u32 heap_double_free_count(void)
{
    return heap_double_free_v;
}

u32 heap_bad_free_count(void)
{
    return heap_bad_free_v;
}

u32 heap_check(void)
{
    heap_block *b;
    heap_block *prev = NULL;
    u64 prev_addr = 0;
    u64 live = 0;

    for (b = heap_head; b != NULL; b = b->next) {
        u64 addr = (u64)(usize)b;
        heap_region *r = region_of(addr);
        u64 hdr_end = addr + HEAP_HDR_SIZE;

        if (r == NULL || hdr_end > r->end) {
            return 1;
        }
        if (b->magic != HEAP_MAGIC_LIVE && b->magic != HEAP_MAGIC_FREE) {
            return 2;
        }
        if (b->size > r->end - hdr_end) {
            return 3;
        }
        if (prev != NULL) {
            if (addr <= prev_addr || b->prev != prev) {
                return 4;
            }
            if ((prev->flags & HEAP_BLOCK_FREE) != 0 &&
                (b->flags & HEAP_BLOCK_FREE) != 0 && same_region(prev, b) &&
                block_end(prev) == addr) {
                return 5; /* 两个相邻空闲块没有合并 */
            }
        } else if (b->prev != NULL) {
            return 6;
        }
        if ((b->flags & HEAP_BLOCK_FREE) == 0) {
            live += b->size;
        }
        prev = b;
        prev_addr = addr;
    }
    if (live != heap_live_bytes_v) {
        return 7;
    }
    return 0;
}
