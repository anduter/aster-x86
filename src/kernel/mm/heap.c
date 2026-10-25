#include "include/mm.h"
#include "arch/x86/io.h"
#include "../types.h"
#include "include/kernel.h"

/* 堆锁：块链表是全局共享状态（SMP 下必须串行化） */
static spinlock_t heap_lock = SPINLOCK_INIT;

/* 块头，放在每个已分配块的用户数据前面 */
typedef struct block_header {
    uint32_t magic;       /* 魔数，用于校验块头是否有效 */
    size_t   size;        /* 用户数据大小（不含块头） */
    uint8_t  used;        /* 1 = 已分配，0 = 空闲 */
    struct block_header* next;
    struct block_header* prev;
} block_t;

#define BLOCK_MAGIC  0x4B43424C  /* "KBCL" */
#define HEADER_SIZE  sizeof(block_t)
#define ALIGN_MASK  0x3  /* 4 字节对齐掩码 */

static block_t* first_block;

/* 异常释放的诊断计数：同一个错误最多打印 8 次，避免误导出的死循环刷屏 */
static unsigned int heap_bad_magic;
static unsigned int heap_double_free;

/* 堆使用统计：遍历块链表累计已用/空闲字节（诊断用，不在热路径） */
void heap_stats(unsigned int *used, unsigned int *total)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    block_t *b = first_block;
    uintptr_t base = (uintptr_t)HEAP_BASE;
    uintptr_t end  = base + (uintptr_t)HEAP_SIZE;
    unsigned int u = 0, t = 0;

    while (b != NULL && (uintptr_t)b >= base && (uintptr_t)b < end &&
           b->magic == BLOCK_MAGIC) {
        t += (unsigned int)b->size;
        if (b->used) u += (unsigned int)b->size;
        b = b->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);

    if (used != 0) *used = u;
    if (total != 0) *total = t;
}

/* 碎片统计：最大空闲块（用户字节）与空闲块个数 */
void heap_frag_stats(unsigned int *largest_free, unsigned int *free_blocks)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    block_t *b = first_block;
    uintptr_t base = (uintptr_t)HEAP_BASE;
    uintptr_t end  = base + (uintptr_t)HEAP_SIZE;
    unsigned int max_free = 0;
    unsigned int nfree = 0;

    while (b != NULL && (uintptr_t)b >= base && (uintptr_t)b < end &&
           b->magic == BLOCK_MAGIC) {
        if (!b->used) {
            nfree++;
            if ((unsigned int)b->size > max_free) max_free = (unsigned int)b->size;
        }
        b = b->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);

    if (largest_free != 0) *largest_free = max_free;
    if (free_blocks != 0) *free_blocks = nfree;
}

/* 全链表一致性检查：块头魔数、区间、首尾覆盖、next/prev 互指、
 * 连续块首尾相接、没有相邻两个空闲块（合并不变量）。
 * 返回 0 表示健康；返回负值是第一处发现的错误编号（同时打印地址）。 */
int heap_check(void)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    block_t *b = first_block;
    uintptr_t base = (uintptr_t)HEAP_BASE;
    uintptr_t end  = base + (uintptr_t)HEAP_SIZE;
    uintptr_t expect = base;
    block_t *prev = NULL;
    bool prev_free = false;
    int rc = 0;
    uintptr_t bad = 0;

    if (b == NULL) {
        rc = -1;
    } else if ((uintptr_t)b != base) {
        rc = -2;
        bad = (uintptr_t)b;
    }

    while (rc == 0 && b != NULL) {
        uintptr_t addr = (uintptr_t)b;

        if (addr != expect) { rc = -3; bad = addr; break; }
        if (b->magic != BLOCK_MAGIC) { rc = -4; bad = addr; break; }
        if (b->used > 1U) { rc = -5; bad = addr; break; }
        if (b->prev != prev) { rc = -6; bad = addr; break; }
        if ((uintptr_t)b->size > end - (addr + HEADER_SIZE)) { rc = -7; bad = addr; break; }
        if (prev_free && !b->used) { rc = -8; bad = addr; break; }

        prev = b;
        prev_free = (b->used == 0U);
        expect = addr + HEADER_SIZE + (uintptr_t)b->size;
        if (expect > end) { rc = -9; bad = addr; break; }
        b = b->next;
    }

    if (rc == 0 && expect != end) { rc = -10; bad = expect; }

    spin_unlock_irqrestore(&heap_lock, flags);

    if (rc != 0) {
        kprintf("heap: check FAILED rc=%d at 0x%08x\n", rc, (unsigned int)bad);
    }
    return rc;
}

/* 初始化堆：整个堆作为唯一一个空闲块 */
void heap_init(void)
{
    first_block = (block_t*)HEAP_BASE;
    first_block->magic = BLOCK_MAGIC;
    first_block->size  = HEAP_SIZE - HEADER_SIZE;
    first_block->used  = 0;
    first_block->next  = NULL;
    first_block->prev  = NULL;
}

/* 与后面相邻的空闲块合并 */
static void merge_forward(block_t* block)
{
    block_t* next = block->next;
    if (next && !next->used && next->magic == BLOCK_MAGIC)
    {
        block->size += HEADER_SIZE + next->size;
        block->next = next->next;
        if (next->next) {
            next->next->prev = block;
        }
    }
}

/* 与前面相邻的空闲块合并 */
static void merge_backward(block_t* block)
{
    block_t* prev = block->prev;
    if (prev && !prev->used && prev->magic == BLOCK_MAGIC)
    {
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block->next) {
            block->next->prev = prev;
        }
    }
}

/* 申请内存（首次适配） */
static void* malloc_locked(size_t size)
{
    if (size == 0) return NULL;

    /* size + ALIGN_MASK 会回绕成小值，后面按小 size 分裂块就会把堆写穿。
     * 单个块不可能超过整个堆，先挡掉明显越界的请求。 */
    if (size > HEAP_SIZE - HEADER_SIZE) return NULL;

    /* 申请大小按 4 字节向上取整 */
    size = (size + ALIGN_MASK) & ~ALIGN_MASK;

    /* 首次适配搜索 */
    block_t* block = first_block;
    while (block)
    {
        if (block->magic != BLOCK_MAGIC) {
            /* 堆结构已被破坏 */
            return NULL;
        }

        if (!block->used && block->size >= size)
        {
            /* 剩余空间够放下一个新块头 + 至少 4 字节数据时才分裂 */
            if (block->size >= size + HEADER_SIZE + 4)
            {
                block_t* new_block = (block_t*)((uint8_t*)block + HEADER_SIZE + size);
                new_block->magic = BLOCK_MAGIC;
                new_block->size  = block->size - size - HEADER_SIZE;
                new_block->used  = 0;
                new_block->next  = block->next;
                new_block->prev  = block;

                if (block->next) {
                    block->next->prev = new_block;
                }

                block->size = size;
                block->next = new_block;
            }

            block->used = 1;
            /* 返回用户数据区指针（跳过块头） */
            return (void*)((uint8_t*)block + HEADER_SIZE);
        }

        block = block->next;
    }

    return NULL; /* 没有足够大的连续空间 */
}

/* 释放内存（相邻空闲块会被合并） */
static void free_locked(void* ptr)
{
    uintptr_t p;
    uintptr_t base = (uintptr_t)HEAP_BASE;
    block_t* block;

    if (!ptr) return;

    p = (uintptr_t)ptr;

    /* 先把指针限制在堆范围内再解引用块头：野指针直接读 block->magic 可能
     * 正好命中可读内存里的伪魔数，把无关数据当成块头改坏。 */
    if (p < base + HEADER_SIZE || p >= base + HEAP_SIZE) return;
    if (((p - base - HEADER_SIZE) & ALIGN_MASK) != 0U) return;  /* 不是 malloc 返回的地址 */

    block = (block_t*)((uint8_t*)ptr - HEADER_SIZE);

    /* 校验魔数，拦住野指针；魔数坏了说明块头被踩，打印地址后拒绝处理 */
    if (block->magic != BLOCK_MAGIC) {
        if (heap_bad_magic < 8U) {
            heap_bad_magic++;
            kprintf("heap: bad block magic %08x at %p\n",
                    block->magic, (void *)p);
        }
        return;
    }
    /* used==0：重复释放。只报告，不破坏链表 */
    if (!block->used) {
        if (heap_double_free < 8U) {
            heap_double_free++;
            kprintf("heap: double free at %p\n", (void *)p);
        }
        return;
    }

    block->used = 0;

    /* 与前后相邻空闲块合并 */
    merge_forward(block);
    merge_backward(block);
}

/* ==================== 对外的加锁接口 ==================== */

void* malloc(size_t size)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    void *p = malloc_locked(size);

    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}

void free(void* ptr)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);

    free_locked(ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}
