/*
 * ng WP1-7: 用户地址空间实现（见 useras.h 的布局说明）。
 *
 * 所有页表页仍由 paging.c 的 pg_alloc_table() 从 PMM 申请；由于内核在
 * 0..1 GiB 恒等映射，页表页的物理地址可以直接解引用，CR3 切换后也一样
 * （AS 的 PDPT[0] 指向同一个内核 PD）。
 */
#include "useras.h"

#include "pmm.h"

#define USERAS_PAGE 0x1000ULL

static struct user_as *g_current_as;

static u64 pa_of_ptr(const void *p)
{
    return (u64)(usize)p;
}

static u64 *table_of(u64 entry)
{
    return (u64 *)(usize)(entry & PG_ADDR_MASK);
}

int as_create(struct user_as *as)
{
    u64 *kernel_pml4;
    u64 *kernel_pdpt;
    u64 *pml4;
    u64 *pdpt;
    u32 i;

    if (as == NULL) {
        return AS_EINVAL;
    }
    as->pml4 = NULL;
    as->pml4_phys = 0;
    as->table_pages = 0;
    as->user_pages = 0;
    as->map_lo = ~0ULL;
    as->map_hi = 0;
    as->active = 0;

    kernel_pml4 = pg_kernel_pml4();
    if (kernel_pml4 == NULL) {
        return AS_EINVAL;
    }
    if ((kernel_pml4[0] & PG_PRESENT) == 0 || (kernel_pml4[0] & PG_PS) != 0) {
        return AS_EINVAL;
    }
    kernel_pdpt = table_of(kernel_pml4[0]);
    if (kernel_pdpt == NULL || (kernel_pdpt[0] & PG_PRESENT) == 0 ||
        (kernel_pdpt[0] & PG_PS) != 0) {
        return AS_EINVAL;
    }

    pml4 = pg_alloc_table();
    if (pml4 == NULL) {
        return AS_ENOMEM;
    }
    pdpt = pg_alloc_table();
    if (pdpt == NULL) {
        pg_free_table(pml4);
        return AS_ENOMEM;
    }

    /* 自己的 PDPT：只共享内核 PD（0..1 GiB 恒等映射），其余从零开始。 */
    pdpt[0] = kernel_pdpt[0] & ~PG_USER;
    for (i = 1; i < 512; i++) {
        pdpt[i] = 0;
    }
    pml4[0] = pa_of_ptr(pdpt) | PG_PRESENT | PG_WRITE;
    for (i = 1; i < 256; i++) {
        pml4[i] = kernel_pml4[i];
    }

    as->pml4 = pml4;
    as->pml4_phys = pa_of_ptr(pml4);
    as->table_pages = 2;
    return AS_OK;
}

void as_destroy(struct user_as *as)
{
    u64 *pml4;
    u64 *pdpt;
    u32 i;
    u32 j;
    u32 k;

    if (as == NULL || as->pml4 == NULL) {
        return;
    }
    if (g_current_as == as || as->active) {
        as_switch_kernel();
    }
    pml4 = as->pml4;
    pdpt = table_of(pml4[0]);

    /* 只回收自己的子树：PDPT[0] 是内核 PD，必须跳过。 */
    if (pdpt != NULL && (pml4[0] & PG_PRESENT) != 0 && (pml4[0] & PG_PS) == 0) {
        for (i = 1; i < 512; i++) {
            u64 *pd;

            if ((pdpt[i] & PG_PRESENT) == 0 || (pdpt[i] & PG_PS) != 0) {
                continue;
            }
            pd = table_of(pdpt[i]);
            for (j = 0; j < 512; j++) {
                u64 *pt;

                if ((pd[j] & PG_PRESENT) == 0 || (pd[j] & PG_PS) != 0) {
                    continue;
                }
                pt = table_of(pd[j]);
                for (k = 0; k < 512; k++) {
                    if ((pt[k] & PG_PRESENT) != 0) {
                        (void)pmm_free_page(pt[k] & PG_ADDR_MASK);
                    }
                }
                pg_free_table(pt);
            }
            pg_free_table(pd);
        }
        pg_free_table(pdpt);
    }
    pg_free_table(pml4);

    as->pml4 = NULL;
    as->pml4_phys = 0;
    as->table_pages = 0;
    as->user_pages = 0;
    as->map_lo = ~0ULL;
    as->map_hi = 0;
    as->active = 0;
}

int as_map(struct user_as *as, u64 va, u64 phys, u64 flags)
{
    int rc;

    if (as == NULL || as->pml4 == NULL) {
        return AS_EINVAL;
    }
    if ((va & (USERAS_PAGE - 1)) != 0 || (phys & (USERAS_PAGE - 1)) != 0) {
        return AS_EINVAL;
    }
    rc = pg_map_in(as->pml4, va, phys, flags);
    if (rc != PG_OK) {
        return (rc == PG_ENOMEM) ? AS_ENOMEM : AS_EINVAL;
    }
    as->user_pages++;
    if (va < as->map_lo) {
        as->map_lo = va;
    }
    if (va + USERAS_PAGE > as->map_hi) {
        as->map_hi = va + USERAS_PAGE;
    }
    return AS_OK;
}

int as_unmap_free(struct user_as *as, u64 va)
{
    u64 pte;
    u64 phys;

    if (as == NULL || as->pml4 == NULL) {
        return AS_EINVAL;
    }
    pte = pg_walk_in(as->pml4, va);
    if ((pte & PG_PRESENT) == 0 || (pte & PG_PS) != 0) {
        return AS_ENOENT;
    }
    phys = pte & PG_ADDR_MASK;
    if (pg_unmap_in(as->pml4, va) != PG_OK) {
        return AS_EINVAL;
    }
    (void)pmm_free_page(phys);
    if (as->user_pages > 0) {
        as->user_pages--;
    }
    return AS_OK;
}

u64 as_walk(const struct user_as *as, u64 va)
{
    if (as == NULL || as->pml4 == NULL) {
        return 0;
    }
    return pg_walk_in(as->pml4, va);
}

int as_switch(struct user_as *as)
{
    if (as == NULL || as->pml4 == NULL) {
        return AS_EINVAL;
    }
    pg_switch_pml4(as->pml4);
    if (g_current_as != NULL && g_current_as != as) {
        g_current_as->active = 0;
    }
    g_current_as = as;
    as->active = 1;
    return AS_OK;
}

void as_switch_kernel(void)
{
    pg_switch_pml4(pg_kernel_pml4());
    if (g_current_as != NULL) {
        g_current_as->active = 0;
    }
    g_current_as = NULL;
}

struct user_as *as_current(void)
{
    return g_current_as;
}
