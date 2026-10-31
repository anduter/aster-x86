/*
 * ng WP1-6: 句柄表（slot + generation）的纯逻辑实现。
 *
 * 不 include heap/serial/调度头，host 单测与内核共用同一份代码。
 */
#include "handle.h"

static void handle_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

void handle_table_init(struct handle_table *t)
{
    if (t == NULL) {
        return;
    }
    handle_memzero(t, sizeof(*t));
}

int handle_alloc(struct handle_table *t, u32 type, void *obj, u32 *out_handle)
{
    u32 i;

    if (t == NULL || !handle_type_valid(type) || obj == NULL) {
        return HANDLE_EINVAL;
    }
    for (i = 0; i < HANDLE_MAX; i++) {
        struct handle_slot *s = &t->slots[i];

        if (s->type != HANDLE_TYPE_NONE) {
            continue;
        }
        /* 首次使用 generation=1；关闭后的槽位用关闭时递增过的值。 */
        if (s->gen == 0) {
            s->gen = 1;
        }
        s->type = type;
        s->obj = obj;
        t->live++;
        if (t->live > t->peak) {
            t->peak = t->live;
        }
        t->allocs++;
        if (out_handle != NULL) {
            *out_handle = handle_make(s->gen, i);
        }
        return HANDLE_OK;
    }
    t->full_rejects++;
    return HANDLE_ENOSPC;
}

int handle_lookup(struct handle_table *t, u32 handle, u32 type, void **out_obj)
{
    u32 slot;
    u32 gen;
    struct handle_slot *s;

    if (t == NULL || !handle_type_valid(type)) {
        return HANDLE_EINVAL;
    }
    slot = handle_slot_of(handle);
    gen = handle_gen_of(handle);
    if (slot >= HANDLE_MAX || gen == 0) {
        t->bad_lookups++;
        return HANDLE_EBADF;
    }
    s = &t->slots[slot];
    if (s->type == HANDLE_TYPE_NONE || s->gen != gen) {
        t->bad_lookups++;
        return HANDLE_EBADF;
    }
    if (s->type != type) {
        t->type_errors++;
        return HANDLE_EBADF;
    }
    if (out_obj != NULL) {
        *out_obj = s->obj;
    }
    return HANDLE_OK;
}

int handle_lookup_any(struct handle_table *t, u32 handle, u32 *out_type,
                      void **out_obj)
{
    u32 slot;
    u32 gen;
    struct handle_slot *s;

    if (t == NULL) {
        return HANDLE_EINVAL;
    }
    slot = handle_slot_of(handle);
    gen = handle_gen_of(handle);
    if (slot >= HANDLE_MAX || gen == 0) {
        t->bad_lookups++;
        return HANDLE_EBADF;
    }
    s = &t->slots[slot];
    if (s->type == HANDLE_TYPE_NONE || s->gen != gen) {
        t->bad_lookups++;
        return HANDLE_EBADF;
    }
    if (out_type != NULL) {
        *out_type = s->type;
    }
    if (out_obj != NULL) {
        *out_obj = s->obj;
    }
    return HANDLE_OK;
}

int handle_close(struct handle_table *t, u32 handle, u32 *out_type,
                 void **out_obj)
{
    u32 type = HANDLE_TYPE_NONE;
    void *obj = NULL;
    u32 slot;
    struct handle_slot *s;
    u32 next;

    if (handle_lookup_any(t, handle, &type, &obj) != HANDLE_OK) {
        return HANDLE_EBADF;
    }
    slot = handle_slot_of(handle);
    if (slot >= HANDLE_MAX) {
        return HANDLE_EBADF;
    }
    s = &t->slots[slot];

    /* generation +1；16 位回绕时跳过 0（0 保留给“无效”）。 */
    next = (s->gen + 1) & HANDLE_SLOT_MASK;
    if (next == 0) {
        next = 1;
    }
    s->gen = next;
    s->type = HANDLE_TYPE_NONE;
    s->obj = NULL;
    if (t->live > 0) {
        t->live--;
    }
    t->closes++;

    if (out_type != NULL) {
        *out_type = type;
    }
    if (out_obj != NULL) {
        *out_obj = obj;
    }
    return HANDLE_OK;
}

u32 handle_live(const struct handle_table *t)
{
    return (t == NULL) ? 0 : t->live;
}

u32 handle_peak(const struct handle_table *t)
{
    return (t == NULL) ? 0 : t->peak;
}

int handle_is_valid(struct handle_table *t, u32 handle)
{
    u32 slot = handle_slot_of(handle);
    u32 gen = handle_gen_of(handle);

    if (t == NULL || slot >= HANDLE_MAX || gen == 0) {
        return 0;
    }
    return t->slots[slot].type != HANDLE_TYPE_NONE &&
           t->slots[slot].gen == gen;
}
