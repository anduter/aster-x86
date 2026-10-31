/*
 * ng WP3-1: 共享内存对象表的纯状态机（见 shm.h）。
 *
 * 不 include 页表/PMM/进程/串口：页的分配与释放走 struct shm_env 注入，
 * 所以 host 单测（tests/unit-ng/test_shm.c）能把一块普通缓冲区当页池，
 * 逐条验证 refcount / 权限 / handle generation / 回滚。
 */
#include "shm.h"

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void shm_zero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    if (b == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static u32 shm_gen_next(struct shm_table *t)
{
    u32 gen = t->next_gen;

    if (gen == 0) {
        gen = 1;
    }
    t->next_gen = gen + 1u;
    if (t->next_gen == 0) {
        t->next_gen = 1;
    }
    return gen & 0xFFFFu;
}

static void shm_free_obj_pages(struct shm_table *t, struct shm_obj *o)
{
    u32 i;

    if (t == NULL || o == NULL || t->env == NULL) {
        return;
    }
    if (t->env->free_page != NULL && o->phys != NULL) {
        for (i = 0; i < o->pages; i++) {
            t->env->free_page(o->phys[i], t->env->ctx);
            t->stat.page_frees++;
        }
    }
    if (t->env->free != NULL && o->phys != NULL) {
        t->env->free(o->phys, t->env->ctx);
    }
    o->phys = NULL;
}

static void shm_clear_obj(struct shm_table *t, struct shm_obj *o)
{
    shm_zero(o, (u64)sizeof(*o));
    if (t->stat.live > 0) {
        t->stat.live--;
    }
}

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

void shm_table_init(struct shm_table *t, const struct shm_env *env)
{
    if (t == NULL) {
        return;
    }
    shm_zero(t, (u64)sizeof(*t));
    t->env = env;
    t->next_gen = 1;
}

/* ------------------------------------------------------------------------- */
/* 查找                                                                       */
/* ------------------------------------------------------------------------- */

struct shm_obj *shm_core_find(struct shm_table *t, u32 handle, int *err)
{
    u32 slot;
    u32 gen;

    if (err != NULL) {
        *err = SHM_EBADF;
    }
    if (t == NULL || handle == 0) {
        if (t != NULL) {
            t->stat.bad++;
        }
        return NULL;
    }
    slot = handle_slot_of(handle);
    gen = handle_gen_of(handle);
    if (slot >= SHM_MAX || gen == 0) {
        t->stat.bad++;
        return NULL;
    }
    {
        struct shm_obj *o = &t->objs[slot];

        if (!o->used || o->gen != gen) {
            t->stat.bad++;
            return NULL;
        }
        if (err != NULL) {
            *err = SHM_OK;
        }
        return o;
    }
}

struct shm_obj *shm_core_lookup(struct shm_table *t, u32 handle, u32 pid,
                                u32 uid, int *err)
{
    struct shm_obj *o = shm_core_find(t, handle, err);

    if (o == NULL) {
        return NULL;
    }
    if (o->owner_pid == pid || uid == 0) {
        return o;
    }
    t->stat.perms++;
    if (err != NULL) {
        *err = SHM_EPERM;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 分配                                                                       */
/* ------------------------------------------------------------------------- */

int shm_core_alloc(struct shm_table *t, u32 owner_pid, u32 owner_uid, u32 pages,
                   u64 owner_va, u32 *out_handle)
{
    struct shm_obj *o = NULL;
    u32 slot = SHM_MAX;
    u32 i;
    u32 gen;
    u32 handle;

    if (out_handle != NULL) {
        *out_handle = 0;
    }
    if (t == NULL || t->env == NULL || t->env->alloc_pages == NULL ||
        t->env->alloc == NULL) {
        return SHM_EINVAL;
    }
    if (pages == 0 || pages > SHM_PAGES_MAX) {
        t->stat.bad++;
        return SHM_EINVAL;
    }
    for (i = 0; i < SHM_MAX; i++) {
        if (!t->objs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == SHM_MAX) {
        t->stat.full++;
        return SHM_ENOSPC;
    }
    o = &t->objs[slot];
    shm_zero(o, (u64)sizeof(*o));
    o->phys = (u64 *)t->env->alloc((u64)pages * 8ULL, t->env->ctx);
    if (o->phys == NULL) {
        shm_zero(o, (u64)sizeof(*o));
        return SHM_ENOMEM;
    }
    if (t->env->alloc_pages(pages, o->phys, t->env->ctx) != 0) {
        t->env->free(o->phys, t->env->ctx);
        shm_zero(o, (u64)sizeof(*o));
        return SHM_ENOMEM;
    }
    t->stat.page_allocs += pages;

    gen = shm_gen_next(t);
    handle = handle_make(gen, slot);
    o->used = 1;
    o->slot = slot;
    o->gen = gen;
    o->owner_pid = owner_pid;
    o->owner_uid = owner_uid;
    o->pages = pages;
    o->refcnt = 1;
    o->maps[0].used = 1;
    o->maps[0].pid = owner_pid;
    o->maps[0].va = owner_va;
    o->map_count = 1;

    t->stat.live++;
    if (t->stat.live > t->stat.peak) {
        t->stat.peak = t->stat.live;
    }
    t->stat.allocs++;
    t->stat.maps++;
    if (out_handle != NULL) {
        *out_handle = handle;
    }
    return SHM_OK;
}

/* ------------------------------------------------------------------------- */
/* 映射 / 解除                                                                */
/* ------------------------------------------------------------------------- */

int shm_core_map(struct shm_table *t, u32 handle, u32 pid, u32 uid, u64 va,
                 struct shm_obj **out)
{
    struct shm_obj *o;
    u32 i;
    int err = SHM_EBADF;

    if (out != NULL) {
        *out = NULL;
    }
    o = shm_core_lookup(t, handle, pid, uid, &err);
    if (o == NULL) {
        return err;
    }
    for (i = 0; i < o->map_count; i++) {
        if (o->maps[i].used && o->maps[i].pid == pid && o->maps[i].va == va) {
            t->stat.busy++;
            return SHM_EBUSY;
        }
    }
    if (o->map_count >= SHM_MAPS_MAX) {
        t->stat.full++;
        return SHM_ENOSPC;
    }
    o->maps[o->map_count].used = 1;
    o->maps[o->map_count].pid = pid;
    o->maps[o->map_count].va = va;
    o->map_count++;
    o->refcnt++;
    t->stat.maps++;
    if (out != NULL) {
        *out = o;
    }
    return SHM_OK;
}

int shm_core_unmap(struct shm_table *t, u32 pid, u64 va)
{
    u32 i;
    u32 j;

    if (t == NULL) {
        return -1;
    }
    for (i = 0; i < SHM_MAX; i++) {
        struct shm_obj *o = &t->objs[i];

        if (!o->used) {
            continue;
        }
        for (j = 0; j < o->map_count; j++) {
            struct shm_map *m = &o->maps[j];

            if (!m->used || m->pid != pid || m->va != va) {
                continue;
            }
            m->used = 0;
            m->pid = 0;
            m->va = 0;
            /* 紧凑化，后面复用。 */
            {
                u32 k;

                for (k = j; k + 1u < o->map_count; k++) {
                    o->maps[k] = o->maps[k + 1u];
                }
                o->map_count--;
                shm_zero(&o->maps[o->map_count], (u64)sizeof(struct shm_map));
            }
            if (o->refcnt > 0) {
                o->refcnt--;
            }
            t->stat.unmaps++;
            if (o->refcnt == 0) {
                shm_free_obj_pages(t, o);
                shm_clear_obj(t, o);
                return 1;
            }
            return 0;
        }
    }
    return -1;
}

u32 shm_core_collect_pid(struct shm_table *t, u32 pid, u64 *out_va, u32 max)
{
    u32 i;
    u32 j;
    u32 n = 0;

    if (t == NULL || pid == 0) {
        return 0;
    }
    for (i = 0; i < SHM_MAX; i++) {
        struct shm_obj *o = &t->objs[i];

        if (!o->used) {
            continue;
        }
        for (j = 0; j < o->map_count; j++) {
            if (!o->maps[j].used || o->maps[j].pid != pid) {
                continue;
            }
            if (out_va != NULL && n < max) {
                out_va[n] = o->maps[j].va;
            }
            n++;
        }
    }
    return n;
}

u32 shm_core_release_pid(struct shm_table *t, u32 pid)
{
    u32 i;
    u32 j;
    u32 removed = 0;

    if (t == NULL || pid == 0) {
        return 0;
    }
    for (i = 0; i < SHM_MAX; i++) {
        struct shm_obj *o = &t->objs[i];

        if (!o->used) {
            continue;
        }
        for (j = 0; j < o->map_count;) {
            struct shm_map *m = &o->maps[j];

            if (!m->used || m->pid != pid) {
                j++;
                continue;
            }
            m->used = 0;
            m->pid = 0;
            m->va = 0;
            {
                u32 k;

                for (k = j; k + 1u < o->map_count; k++) {
                    o->maps[k] = o->maps[k + 1u];
                }
                o->map_count--;
                shm_zero(&o->maps[o->map_count],
                         (u64)sizeof(struct shm_map));
            }
            if (o->refcnt > 0) {
                o->refcnt--;
            }
            t->stat.unmaps++;
            removed++;
        }
        if (o->used && o->refcnt == 0) {
            shm_free_obj_pages(t, o);
            shm_clear_obj(t, o);
        }
    }
    return removed;
}

int shm_core_abort(struct shm_table *t, u32 handle)
{
    struct shm_obj *o;
    int err = SHM_EBADF;

    o = shm_core_find(t, handle, &err);
    if (o == NULL) {
        return err;
    }
    if (o->refcnt != 1 || o->map_count != 1) {
        t->stat.busy++;
        return SHM_EBUSY;
    }
    shm_free_obj_pages(t, o);
    shm_clear_obj(t, o);
    return SHM_OK;
}

/* ------------------------------------------------------------------------- */
/* 统计                                                                       */
/* ------------------------------------------------------------------------- */

u32 shm_core_live(const struct shm_table *t)
{
    return (t == NULL) ? 0 : t->stat.live;
}

u32 shm_core_refs(struct shm_table *t, u32 handle)
{
    int err = SHM_EBADF;
    struct shm_obj *o = shm_core_find(t, handle, &err);

    return (o == NULL) ? 0 : o->refcnt;
}

const struct shm_stats *shm_core_stat(const struct shm_table *t)
{
    static const struct shm_stats zero;
    return (t == NULL) ? &zero : &t->stat;
}
