/*
 * ng WP3-1: surface 表（纯状态机，见 ds_surface.h）。
 */
#include "ds_surface.h"

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void ds_table_zero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static int ds_slot_index(const struct ds_table *t, u32 id)
{
    u32 i;

    if (t == NULL || id == 0) {
        return -1;
    }
    for (i = 0; i < DS_SURFACES_MAX; i++) {
        if (t->slot[i].used && t->slot[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static int ds_order_pos(const struct ds_table *t, u32 slot_index)
{
    u32 i;

    for (i = 0; i < t->count; i++) {
        if (t->order[i] == slot_index) {
            return (int)i;
        }
    }
    return -1;
}

static void ds_order_remove(struct ds_table *t, u32 pos)
{
    u32 i;

    if (pos >= t->count) {
        return;
    }
    for (i = pos; i + 1u < t->count; i++) {
        t->order[i] = t->order[i + 1u];
    }
    t->order[t->count - 1u] = 0;
}

static void ds_order_push_back(struct ds_table *t, u32 slot_index)
{
    if (t->count >= DS_SURFACES_MAX) {
        return;
    }
    t->order[t->count] = slot_index;
}

/* 级联放置：第一个在 DS_FIRST_X/Y，之后每个 +DS_CASCADE；超出屏幕贴边。 */
static void ds_place(const struct ds_table *t, u32 w, u32 h, u32 *out_x,
                     u32 *out_y)
{
    u64 x = (u64)DS_FIRST_X + (u64)t->count * (u64)DS_CASCADE;
    u64 y = (u64)DS_FIRST_Y + (u64)t->count * (u64)DS_CASCADE;

    if (x + (u64)w > (u64)t->screen_w) {
        x = ((u64)t->screen_w > (u64)w) ? ((u64)t->screen_w - (u64)w) : 0;
    }
    if (y + (u64)h > (u64)t->screen_h) {
        y = ((u64)t->screen_h > (u64)h) ? ((u64)t->screen_h - (u64)h) : 0;
    }
    *out_x = (u32)x;
    *out_y = (u32)y;
}

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

void ds_table_init(struct ds_table *t, u32 screen_w, u32 screen_h)
{
    if (t == NULL) {
        return;
    }
    ds_table_zero(t, (u64)sizeof(*t));
    t->next_id = 1;
    t->focus_id = 0;
    t->screen_w = (screen_w != 0) ? screen_w : DS_SURFACE_MAX_W;
    t->screen_h = (screen_h != 0) ? screen_h : DS_SURFACE_MAX_H;
}

const char *ds_table_version(void)
{
    return "DS1";
}

int ds_table_create(struct ds_table *t, u32 w, u32 h, u32 owner_pid,
                    u32 client_ticket, u32 shm_handle, u32 shm_pages,
                    u64 pixels, u32 *out_id)
{
    u64 need_pages;
    u32 slot = DS_SURFACES_MAX;
    u32 i;
    u32 id;
    u32 x = 0;
    u32 y = 0;
    struct ds_surface *s;

    if (out_id != NULL) {
        *out_id = 0;
    }
    if (t == NULL) {
        return DS_EINVAL;
    }
    if (w == 0 || h == 0 || w > DS_SURFACE_MAX_W || h > DS_SURFACE_MAX_H) {
        t->rejects++;
        return DS_EINVAL;
    }
    if (t->count >= DS_SURFACES_MAX) {
        t->rejects++;
        return DS_ENOSPC;
    }
    need_pages = ((u64)w * (u64)h * 4ULL + (u64)DS_SHM_PAGE_SIZE - 1ULL) /
                 (u64)DS_SHM_PAGE_SIZE;
    if (shm_pages < (u32)need_pages || shm_pages > DS_SHM_PAGES_MAX) {
        t->rejects++;
        return DS_EINVAL;
    }
    for (i = 0; i < DS_SURFACES_MAX; i++) {
        if (!t->slot[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == DS_SURFACES_MAX) {
        t->rejects++;
        return DS_ENOSPC;
    }

    id = t->next_id;
    if (id == 0) {
        id = 1;
    }
    t->next_id = id + 1u;
    if (t->next_id == 0) {
        t->next_id = 2; /* 回绕：0 永远不是合法 id，1 留给下一个 */
    }

    ds_place(t, w, h, &x, &y);
    s = &t->slot[slot];
    ds_table_zero(s, (u64)sizeof(*s));
    s->used = 1;
    s->id = id;
    s->w = w;
    s->h = h;
    s->x = x;
    s->y = y;
    s->owner_pid = owner_pid;
    s->client_ticket = client_ticket;
    s->shm_handle = shm_handle;
    s->shm_pages = shm_pages;
    s->pixels = pixels;

    ds_order_push_back(t, slot);
    t->count++;
    t->created++;
    if (t->focus_id == 0) {
        t->focus_id = id; /* 第一个 surface 自动获得焦点 */
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return DS_OK;
}

struct ds_surface *ds_table_find(struct ds_table *t, u32 id)
{
    int i = ds_slot_index(t, id);

    return (i < 0) ? NULL : &t->slot[i];
}

struct ds_surface *ds_table_at(struct ds_table *t, u32 index)
{
    if (t == NULL || index >= t->count) {
        return NULL;
    }
    return &t->slot[t->order[index]];
}

u32 ds_table_count(const struct ds_table *t)
{
    return (t == NULL) ? 0 : t->count;
}

u32 ds_table_focus_id(const struct ds_table *t)
{
    return (t == NULL) ? 0 : t->focus_id;
}

int ds_table_mark_mapped(struct ds_table *t, u32 id)
{
    struct ds_surface *s = ds_table_find(t, id);

    if (s == NULL) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_ENOENT;
    }
    s->mapped = 1;
    return DS_OK;
}

/* ------------------------------------------------------------------------- */
/* COMMIT / FOCUS / DESTROY                                                   */
/* ------------------------------------------------------------------------- */

int ds_table_commit(struct ds_table *t, u32 id, const struct ds_rect *r,
                    struct ds_rect *out_clip)
{
    struct ds_surface *s = ds_table_find(t, id);
    struct ds_rect clip;

    if (out_clip != NULL) {
        out_clip->x = 0;
        out_clip->y = 0;
        out_clip->w = 0;
        out_clip->h = 0;
    }
    if (s == NULL) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_ENOENT;
    }
    if (!s->mapped || s->pixels == 0) {
        t->rejects++;
        return DS_EBUSY;
    }
    if (r == NULL || ds_rect_empty(r) != 0) {
        t->rejects++;
        return DS_EINVAL;
    }
    if (ds_rect_clip(r, s->w, s->h, &clip) == 0) {
        t->rejects++;
        return DS_EINVAL;
    }
    s->committed = 1;
    s->commits++;
    s->last_commit = clip;
    t->commit_total++;
    if (out_clip != NULL) {
        *out_clip = clip;
    }
    return DS_OK;
}

int ds_table_focus(struct ds_table *t, u32 id)
{
    int pos;
    u32 slot;

    if (t == NULL) {
        return DS_EINVAL;
    }
    pos = ds_slot_index(t, id);
    if (pos < 0) {
        t->rejects++;
        return DS_ENOENT;
    }
    slot = (u32)pos;
    pos = ds_order_pos(t, slot);
    if (pos >= 0) {
        ds_order_remove(t, (u32)pos);
        ds_order_push_back(t, slot);
    }
    t->focus_id = id;
    return DS_OK;
}

int ds_table_destroy(struct ds_table *t, u32 id)
{
    int pos;
    u32 slot;

    if (t == NULL) {
        return DS_EINVAL;
    }
    pos = ds_slot_index(t, id);
    if (pos < 0) {
        t->rejects++;
        return DS_ENOENT;
    }
    slot = (u32)pos;
    pos = ds_order_pos(t, slot);
    if (pos >= 0) {
        ds_order_remove(t, (u32)pos);
        t->count--;
    }
    ds_table_zero(&t->slot[slot], (u64)sizeof(t->slot[slot]));
    t->destroyed++;
    if (t->focus_id == id) {
        t->focus_id = (t->count > 0) ? t->slot[t->order[t->count - 1u]].id : 0;
    }
    return DS_OK;
}

/* ------------------------------------------------------------------------- */
/* 几何                                                                       */
/* ------------------------------------------------------------------------- */

int ds_rect_empty(const struct ds_rect *r)
{
    return (r == NULL || r->w == 0 || r->h == 0) ? 1 : 0;
}

u32 ds_rect_area(const struct ds_rect *r)
{
    if (r == NULL || r->w == 0 || r->h == 0) {
        return 0;
    }
    return r->w * r->h;
}

int ds_rect_clip(const struct ds_rect *r, u32 bw, u32 bh, struct ds_rect *out)
{
    u64 x0;
    u64 y0;
    u64 x1;
    u64 y1;

    if (out == NULL) {
        return 0;
    }
    out->x = 0;
    out->y = 0;
    out->w = 0;
    out->h = 0;
    if (r == NULL || r->w == 0 || r->h == 0 || bw == 0 || bh == 0) {
        return 0;
    }
    x0 = r->x;
    y0 = r->y;
    x1 = (u64)r->x + (u64)r->w;
    y1 = (u64)r->y + (u64)r->h;
    if (x0 > (u64)bw) {
        x0 = bw;
    }
    if (y0 > (u64)bh) {
        y0 = bh;
    }
    if (x1 > (u64)bw) {
        x1 = bw;
    }
    if (y1 > (u64)bh) {
        y1 = bh;
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    out->x = (u32)x0;
    out->y = (u32)y0;
    out->w = (u32)(x1 - x0);
    out->h = (u32)(y1 - y0);
    return 1;
}

int ds_rect_intersect(const struct ds_rect *a, const struct ds_rect *b,
                      struct ds_rect *out)
{
    u64 x0;
    u64 y0;
    u64 x1;
    u64 y1;

    if (a == NULL || b == NULL || out == NULL) {
        return 0;
    }
    x0 = (a->x > b->x) ? a->x : b->x;
    y0 = (a->y > b->y) ? a->y : b->y;
    x1 = ((u64)a->x + a->w < (u64)b->x + b->w)
             ? ((u64)a->x + a->w)
             : ((u64)b->x + b->w);
    y1 = ((u64)a->y + a->h < (u64)b->y + b->h)
             ? ((u64)a->y + a->h)
             : ((u64)b->y + b->h);
    if (x1 <= x0 || y1 <= y0) {
        out->x = 0;
        out->y = 0;
        out->w = 0;
        out->h = 0;
        return 0;
    }
    out->x = (u32)x0;
    out->y = (u32)y0;
    out->w = (u32)(x1 - x0);
    out->h = (u32)(y1 - y0);
    return 1;
}
