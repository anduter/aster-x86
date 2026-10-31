/*
 * ng WP3-1: 共享内存内核胶水（见 shm.h）。
 *
 * 三件事：
 *   1. `struct shm_env` 绑到 PMM（每页独立分配，PMM 分配即清零）；
 *   2. 45/46 的实现：uaccess 校验用户指针 -> 选 VA -> 分配/映射 -> copyout；
 *   3. 进程退出/被 kill 时 `shm_release_proc()`：先把共享页从该进程 AS 里
 *      **unmap**（不 free！），再让 core 按 refcount 决定是否归还物理页。
 *      顺序不能反：`as_destroy()` 会 free 所有 present 叶子页。
 *
 * 共享页只映射成 RW+NX、U=1；没有 W^X 冲突（数据页不可执行）。VA 由每进程
 * bump 分配器给（proc->shm_next_va），对象之间留一页守护空洞。
 */
#include "shm.h"

#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "proc.h"
#include "uaccess.h"
#include "useras.h"

static struct shm_table g_shm;

/* ------------------------------------------------------------------------- */
/* 环境钩子                                                                   */
/* ------------------------------------------------------------------------- */

static int k_shm_alloc_pages(u32 count, u64 *out, void *ctx)
{
    u32 i;

    (void)ctx;
    if (out == NULL || count == 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        u64 phys = pmm_alloc_page();

        if (phys == 0) {
            while (i > 0) {
                i--;
                (void)pmm_free_page(out[i]);
            }
            return -1;
        }
        out[i] = phys;
    }
    return 0;
}

static void k_shm_free_page(u64 phys, void *ctx)
{
    (void)ctx;
    if (pmm_owns(phys)) {
        (void)pmm_free_page(phys);
    }
}

static void *k_shm_alloc(u64 size, void *ctx)
{
    (void)ctx;
    return kmalloc(size);
}

static void k_shm_free(void *p, void *ctx)
{
    (void)ctx;
    (void)kfree(p);
}

static const struct shm_env k_shm_env = {NULL,       k_shm_alloc_pages,
                                         k_shm_free_page, k_shm_alloc,
                                         k_shm_free};

void shm_init(void)
{
    shm_table_init(&g_shm, &k_shm_env);
}

struct shm_table *shm_global(void)
{
    return &g_shm;
}

u32 shm_live(void)
{
    return shm_core_live(&g_shm);
}

/* ------------------------------------------------------------------------- */
/* VA 分配（每进程 bump；对象之间留一页守护）                                 */
/* ------------------------------------------------------------------------- */

static int shm_va_alloc(struct proc *p, u32 pages, u64 *out_va)
{
    u64 va;
    u64 span;

    if (p == NULL || out_va == NULL || pages == 0) {
        return SHM_EINVAL;
    }
    va = (p->shm_next_va != 0) ? p->shm_next_va : SHM_VA_BASE;
    span = (u64)pages * SHM_PAGE_SIZE;
    if (va < SHM_VA_BASE || span == 0 || va + span + SHM_PAGE_SIZE > SHM_VA_LIMIT) {
        return SHM_ENOMEM;
    }
    if (va + span > NG_USER_VA_MAX) {
        return SHM_ENOMEM;
    }
    *out_va = va;
    /* 记录下一个候选；失败路径由调用方回滚。 */
    p->shm_next_va = va + span + SHM_PAGE_SIZE;
    return SHM_OK;
}

static void shm_va_rollback(struct proc *p, u64 va)
{
    if (p != NULL && p->shm_next_va > va) {
        p->shm_next_va = va;
    }
}

static u64 shm_page_flags(void)
{
    u64 flags = PG_USER | PG_WRITE;

    if (pg_nx_on()) {
        flags |= PG_NX;
    }
    return flags;
}

/* 把 obj 的页映射到 as；失败时把已映射的页从页表摘掉（不 free）。 */
static int shm_map_pages(struct user_as *as, struct shm_obj *obj, u64 va)
{
    u64 flags = shm_page_flags();
    u32 i;

    for (i = 0; i < obj->pages; i++) {
        if (as_map(as, va + (u64)i * SHM_PAGE_SIZE, obj->phys[i], flags) !=
            AS_OK) {
            u32 k;

            for (k = 0; k < i; k++) {
                (void)pg_unmap_in(as->pml4, va + (u64)k * SHM_PAGE_SIZE);
            }
            return SHM_ENOMEM;
        }
    }
    return SHM_OK;
}

static void shm_unmap_pages(struct user_as *as, u32 pages, u64 va)
{
    u32 i;

    if (as == NULL || as->pml4 == NULL) {
        return;
    }
    for (i = 0; i < pages; i++) {
        u64 page = va + (u64)i * SHM_PAGE_SIZE;

        if ((as_walk(as, page) & PG_PRESENT) != 0) {
            (void)pg_unmap_in(as->pml4, page);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* syscall 45 / 46                                                            */
/* ------------------------------------------------------------------------- */

u64 shm_sys_alloc(u64 pages, u64 uva_out, u64 uhandle_out)
{
    struct proc *p = proc_current();
    struct shm_obj *obj = NULL;
    u64 va = 0;
    u32 handle = 0;
    int rc;

    if (pages == 0 || pages > SHM_PAGES_MAX) {
        return (u64)(i64)SHM_EINVAL;
    }
    if (p == NULL || p->as == NULL) {
        return (u64)(i64)SHM_EPERM; /* 内核上下文没有用户 AS */
    }
    if (uaccess_range_ok(uva_out, 8, 1) == 0 ||
        uaccess_range_ok(uhandle_out, 8, 1) == 0) {
        return (u64)(i64)SHM_EFAULT;
    }
    rc = shm_va_alloc(p, (u32)pages, &va);
    if (rc != SHM_OK) {
        return (u64)(i64)rc;
    }
    rc = shm_core_alloc(&g_shm, p->pid, p->uid, (u32)pages, va, &handle);
    if (rc != SHM_OK) {
        shm_va_rollback(p, va);
        return (u64)(i64)rc;
    }
    obj = shm_core_find(&g_shm, handle, &rc);
    if (obj == NULL) {
        (void)shm_core_abort(&g_shm, handle);
        shm_va_rollback(p, va);
        return (u64)(i64)SHM_EBADF;
    }
    rc = shm_map_pages((struct user_as *)p->as, obj, va);
    if (rc != SHM_OK) {
        (void)shm_core_abort(&g_shm, handle);
        shm_va_rollback(p, va);
        return (u64)(i64)rc;
    }
    if (copyout(uva_out, &va, 8) != UACCESS_OK ||
        copyout(uhandle_out, &handle, 4) != UACCESS_OK) {
        shm_unmap_pages((struct user_as *)p->as, (u32)pages, va);
        (void)shm_core_abort(&g_shm, handle);
        shm_va_rollback(p, va);
        return (u64)(i64)SHM_EFAULT;
    }
    return 0;
}

u64 shm_sys_map(u64 handle, u64 uva_out)
{
    struct proc *p = proc_current();
    struct shm_obj *obj = NULL;
    u64 va = 0;
    int err = SHM_EBADF;
    int rc;

    if (handle == 0 || handle > 0xFFFFFFFFULL) {
        return (u64)(i64)SHM_EBADF;
    }
    if (p == NULL || p->as == NULL) {
        return (u64)(i64)SHM_EPERM;
    }
    if (uaccess_range_ok(uva_out, 8, 1) == 0) {
        return (u64)(i64)SHM_EFAULT;
    }
    /* 先做 handle + 权限检查，拿到页数才能选 VA。 */
    obj = shm_core_lookup(&g_shm, (u32)handle, p->pid, p->uid, &err);
    if (obj == NULL) {
        return (u64)(i64)err;
    }
    rc = shm_va_alloc(p, obj->pages, &va);
    if (rc != SHM_OK) {
        return (u64)(i64)rc;
    }
    rc = shm_core_map(&g_shm, (u32)handle, p->pid, p->uid, va, &obj);
    if (rc != SHM_OK) {
        shm_va_rollback(p, va);
        return (u64)(i64)rc;
    }
    rc = shm_map_pages((struct user_as *)p->as, obj, va);
    if (rc != SHM_OK) {
        (void)shm_core_unmap(&g_shm, p->pid, va);
        shm_va_rollback(p, va);
        return (u64)(i64)rc;
    }
    if (copyout(uva_out, &va, 8) != UACCESS_OK) {
        shm_unmap_pages((struct user_as *)p->as, obj->pages, va);
        (void)shm_core_unmap(&g_shm, p->pid, va);
        shm_va_rollback(p, va);
        return (u64)(i64)SHM_EFAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 进程回收                                                                   */
/* ------------------------------------------------------------------------- */

void shm_release_proc(struct proc *p)
{
    u64 vas[SHM_MAX * SHM_MAPS_MAX];
    struct user_as *as;
    u32 n;
    u32 i;

    if (p == NULL || p->as == NULL) {
        return;
    }
    as = (struct user_as *)p->as;
    n = shm_core_collect_pid(&g_shm, p->pid, vas, SHM_MAX * SHM_MAPS_MAX);
    for (i = 0; i < n && i < (SHM_MAX * SHM_MAPS_MAX); i++) {
        if ((as_walk(as, vas[i]) & PG_PRESENT) != 0) {
            (void)pg_unmap_in(as->pml4, vas[i]);
        }
    }
    (void)shm_core_release_pid(&g_shm, p->pid);
}
