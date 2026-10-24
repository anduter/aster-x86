#include "include/mm.h"
#include "../io.h"
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

/* 堆使用统计：遍历块链表累计已用/空闲字节（诊断用，不在热路径） */
void heap_stats(unsigned int *used, unsigned int *total)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    block_t *b = first_block;
    unsigned int u = 0, t = 0;

    while (b != NULL && b->magic == BLOCK_MAGIC) {
        t += (unsigned int)b->size;
        if (b->used) u += (unsigned int)b->size;
        b = b->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);

    if (used != 0) *used = u;
    if (total != 0) *total = t;
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
    if (!ptr) return;

    block_t* block = (block_t*)((uint8_t*)ptr - HEADER_SIZE);

    /* 校验魔数，拦住野指针和重复释放 */
    if (block->magic != BLOCK_MAGIC || !block->used) {
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
