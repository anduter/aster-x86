/* =============================================================================
 * proc/cap.c - 通用 per-process capability 表
 *
 * 槽位持有全局对象句柄 + 类型 + rights；transfer 是移动而不是复制，
 * close/revoke 释放一次对象引用。fd 表仍保留自己的 rights 字段。
 * ============================================================================= */
#include "cap.h"
#include "include/kernel.h"
#include "include/userland.h"
#include "include/kernel.h"
#include "include/kernel.h"

static uint32_t make_handle(uint32_t slot, uint16_t gen)
{
    return ((uint32_t)gen << 8) | (slot + 1U);
}

static int handle_slot(uint32_t handle, uint32_t *slot)
{
    uint32_t idx;

    if (handle == 0U) return E_BADH;
    idx = handle & 0xFFU;
    if (idx == 0U || idx > CAP_TABLE_MAX) return E_BADH;
    *slot = idx - 1U;
    return E_OK;
}

void cap_init(cap_table_t *t)
{
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < CAP_TABLE_MAX; i++) {
        t->slot[i].generation = 1;
    }
}

uint32_t cap_install(cap_table_t *t, uint32_t object, obj_type_t type,
                     uint32_t rights)
{
    if (t == 0 || object == 0U) return 0;
    if (obj_ref(object, type) != E_OK) return 0;

    for (uint32_t i = 0; i < CAP_TABLE_MAX; i++) {
        cap_slot_t *s = &t->slot[i];

        if (s->used) continue;
        s->used = true;
        s->type = type;
        s->object = object;
        s->rights = rights;
        if (s->generation == 0U) s->generation = 1;
        return make_handle(i, s->generation);
    }

    (void)obj_close(object, type);
    return 0;
}

int cap_lookup(cap_table_t *t, uint32_t handle, obj_type_t type,
               uint32_t need_rights, uint32_t *object)
{
    uint32_t slot;
    cap_slot_t *s;

    if (t == 0 || handle_slot(handle, &slot) != E_OK) return E_BADH;
    s = &t->slot[slot];
    if (!s->used) return E_BADH;
    if ((uint16_t)(handle >> 8) != s->generation) return E_BADH;
    if (s->type != type) return E_TYPE;
    if ((need_rights & ~s->rights) != 0U) return E_ACCES;
    if (object != 0) *object = s->object;
    return E_OK;
}

int cap_close(cap_table_t *t, uint32_t handle)
{
    uint32_t slot;
    cap_slot_t *s;
    int r;

    if (t == 0 || handle_slot(handle, &slot) != E_OK) return E_BADH;
    s = &t->slot[slot];
    if (!s->used || (uint16_t)(handle >> 8) != s->generation) return E_BADH;

    r = obj_close(s->object, s->type);
    s->used = false;
    s->object = 0;
    s->rights = 0;
    s->generation++;
    if (s->generation == 0U) s->generation = 1;
    return r;
}

int cap_transfer(cap_table_t *src, cap_table_t *dst, uint32_t handle,
                 uint32_t rights)
{
    uint32_t slot;
    cap_slot_t *s;
    uint32_t out;

    if (src == 0 || dst == 0 || handle_slot(handle, &slot) != E_OK) return -1;
    s = &src->slot[slot];
    if (!s->used || (uint16_t)(handle >> 8) != s->generation) return -1;
    if (rights == 0U) rights = s->rights;
    if ((rights & ~s->rights) != 0U) return -1;

    for (uint32_t i = 0; i < CAP_TABLE_MAX; i++) {
        cap_slot_t *d = &dst->slot[i];

        if (d->used) continue;
        d->used = true;
        d->type = s->type;
        d->object = s->object;
        d->rights = rights;
        out = make_handle(i, d->generation);

        s->used = false;
        s->object = 0;
        s->rights = 0;
        s->generation++;
        if (s->generation == 0U) s->generation = 1;
        return (int)out;
    }
    return -1;
}

void cap_fork(cap_table_t *dst, const cap_table_t *src)
{
    cap_init(dst);
    for (uint32_t i = 0; i < CAP_TABLE_MAX; i++) {
        const cap_slot_t *s = &src->slot[i];

        if (!s->used) continue;
        if (obj_ref(s->object, s->type) != E_OK) continue;
        dst->slot[i] = *s;
    }
}

void cap_release_all(cap_table_t *t)
{
    if (t == 0) return;
    for (uint32_t i = 0; i < CAP_TABLE_MAX; i++) {
        if (t->slot[i].used) {
            (void)obj_close(t->slot[i].object, t->slot[i].type);
            t->slot[i].used = false;
        }
    }
}

int cap_selftest(void)
{
    cap_table_t a;
    cap_table_t b;
    uint32_t ph;
    uint32_t h;
    uint32_t t;
    uint32_t obj = 0;
    int ok = 1;

    cap_init(&a);
    cap_init(&b);
    ph = prog_open("shell");
    if (ph == 0U) return 0;

    h = cap_install(&a, ph, OBJ_PROGRAM, 0xFU);
    if (h == 0U) ok = 0;
    if (ok && cap_lookup(&a, h, OBJ_PROGRAM, 0x1U, &obj) != E_OK) ok = 0;
    if (ok && obj != ph) ok = 0;

    t = (uint32_t)cap_transfer(&a, &b, h, 0x3U);
    if (t == 0U) ok = 0;
    if (ok && cap_lookup(&a, h, OBJ_PROGRAM, 0, 0) != E_BADH) ok = 0;
    if (ok && cap_lookup(&b, t, OBJ_PROGRAM, 0x1U, &obj) != E_OK) ok = 0;

    if (t != 0U) (void)cap_close(&b, t);
    (void)obj_close(ph, OBJ_PROGRAM);
    return ok;
}
