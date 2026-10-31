/*
 * ng WP3-2: shell/WM 纯逻辑（见 ds_wm.h）。
 *
 * 不依赖 libc / IPC / syscall：内核、ring3 WM、host 单测共用同一份代码。
 */
#include "ds_wm.h"

static u32 wm_strlen(const char *s)
{
    u32 n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void wm_strcpy(char *dst, u32 cap, const char *src)
{
    u32 i;

    if (dst == NULL || cap == 0) {
        return;
    }
    for (i = 0; i + 1u < cap; i++) {
        char c = (src != NULL) ? src[i] : '\0';

        dst[i] = c;
        if (c == '\0') {
            return;
        }
    }
    dst[cap - 1u] = '\0';
}

static int wm_name_cmp(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
        i++;
    }
    if (a[i] == b[i]) {
        return 0;
    }
    return (a[i] == '\0') ? -1 : 1;
}

static int wm_name_eq(const char *a, const char *b)
{
    u32 i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return (a[i] == b[i]) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 面板布局                                                                   */
/* ------------------------------------------------------------------------- */

void ds_wm_layout_init(struct ds_wm_layout *l, u32 panel_w)
{
    u32 end;
    u32 avail;

    if (l == NULL) {
        return;
    }
    ds_memzero(l, (u64)sizeof(*l));
    l->panel_w = panel_w;
    l->panel_h = DS_WM_PANEL_H;

    l->title.x = 0;
    l->title.y = 0;
    l->title.w = 64;
    l->title.h = DS_WM_PANEL_H;

    l->launcher.x = 64;
    l->launcher.y = 4;
    l->launcher.w = 64;
    l->launcher.h = 24;

    l->notif.w = 88;
    l->notif.h = 24;
    l->notif.y = 4;
    l->notif.x = (panel_w > (l->notif.w + 8u)) ? (panel_w - l->notif.w - 8u) : 0u;

    l->clock.w = 64;
    l->clock.h = DS_WM_PANEL_H;
    l->clock.y = 0;
    l->clock.x = (l->notif.x > (l->clock.w + 8u))
                     ? (l->notif.x - l->clock.w - 8u)
                     : 0u;

    l->task_x = 136;
    l->task_y = 4;
    l->task_w = DS_WM_TASK_W;
    l->task_h = DS_WM_TASK_H;
    l->task_gap = 6;
    end = (l->clock.x > (l->task_x + 8u)) ? (l->clock.x - 8u) : l->task_x;
    avail = (end > l->task_x) ? (end - l->task_x) : 0u;
    l->task_max = (avail + l->task_gap) / (l->task_w + l->task_gap);
    if (l->task_max > DS_LIST_MAX) {
        l->task_max = DS_LIST_MAX;
    }
}

int ds_wm_task_rect(const struct ds_wm_layout *l, u32 index,
                    struct ds_wm_rect *out)
{
    if (l == NULL || out == NULL || index >= l->task_max) {
        return 0;
    }
    out->x = l->task_x + index * (l->task_w + l->task_gap);
    out->y = l->task_y;
    out->w = l->task_w;
    out->h = l->task_h;
    return 1;
}

void ds_wm_text_fit(char *dst, u32 cap, const char *src)
{
    wm_strcpy(dst, cap, src);
}

/* ------------------------------------------------------------------------- */
/* 启动器                                                                     */
/* ------------------------------------------------------------------------- */

void ds_wm_launcher_init(struct ds_wm_launcher *l)
{
    if (l != NULL) {
        ds_memzero(l, (u64)sizeof(*l));
    }
}

static int wm_launcher_dup(const struct ds_wm_launcher *l, const char *path)
{
    u32 i;

    for (i = 0; i < l->count; i++) {
        if (wm_name_eq(l->app[i].path, path)) {
            return 1;
        }
    }
    return 0;
}

static int wm_bin_not_launchable(const char *name)
{
    /* 已经在跑/需要串口的程序不放进"可启动"集合（见 ds_wm.h 说明）。 */
    return wm_name_eq(name, "shell") || wm_name_eq(name, "displayd") ||
           wm_name_eq(name, "wm");
}

int ds_wm_launcher_add(struct ds_wm_launcher *l, const char *dir,
                       const char *name, u32 name_len, u32 is_dir)
{
    struct ds_wm_app *app;
    char path[DS_WM_PATH_MAX];
    char short_name[DS_WM_NAME_MAX];
    u32 dir_len;
    u32 i;

    if (l == NULL || dir == NULL || name == NULL) {
        return 0;
    }
    if (l->count >= DS_WM_APPS_MAX || name_len == 0) {
        return 0;
    }
    if (name_len >= (u32)DS_WM_NAME_MAX) {
        name_len = (u32)DS_WM_NAME_MAX - 1u;
    }
    for (i = 0; i < name_len; i++) {
        short_name[i] = name[i];
    }
    short_name[name_len] = '\0';
    if (short_name[0] == '.') {
        return 0; /* . / .. */
    }
    dir_len = wm_strlen(dir);
    if (dir_len + 1u + name_len + 1u > (u32)DS_WM_PATH_MAX) {
        return 0;
    }
    wm_strcpy(path, (u32)DS_WM_PATH_MAX, dir);
    path[dir_len] = '/';
    for (i = 0; i < name_len; i++) {
        path[dir_len + 1u + i] = short_name[i];
    }
    path[dir_len + 1u + name_len] = '\0';
    if (wm_launcher_dup(l, path)) {
        return 0;
    }
    app = &l->app[l->count];
    ds_memzero(app, (u64)sizeof(*app));
    wm_strcpy(app->path, (u32)DS_WM_PATH_MAX, path);
    wm_strcpy(app->name, (u32)DS_WM_NAME_MAX, short_name);
    if (is_dir != 0u) {
        if (!wm_name_eq(dir, "/System/Packages")) {
            return 0;
        }
        app->kind = DS_WM_APP_PKG;
        app->launchable = 0;
    } else {
        if (!wm_name_eq(dir, "/bin")) {
            return 0;
        }
        app->kind = DS_WM_APP_BIN;
        app->launchable = wm_bin_not_launchable(short_name) ? 0u : 1u;
    }
    l->count++;
    return 1;
}

static void wm_app_copy(struct ds_wm_app *dst, const struct ds_wm_app *src)
{
    u32 i;

    for (i = 0; i + 1u < (u32)DS_WM_PATH_MAX; i++) {
        dst->path[i] = src->path[i];
        if (dst->path[i] == '\0') {
            break;
        }
    }
    dst->path[DS_WM_PATH_MAX - 1u] = '\0';
    for (i = 0; i + 1u < (u32)DS_WM_NAME_MAX; i++) {
        dst->name[i] = src->name[i];
        if (dst->name[i] == '\0') {
            break;
        }
    }
    dst->name[DS_WM_NAME_MAX - 1u] = '\0';
    dst->kind = src->kind;
    dst->launchable = src->launchable;
}

void ds_wm_launcher_finalize(struct ds_wm_launcher *l)
{
    u32 i;
    u32 j;

    if (l == NULL) {
        return;
    }
    /* 插入排序：kind 优先（BIN 在前），同 kind 按 name 字典序。 */
    for (i = 1; i < l->count; i++) {
        struct ds_wm_app key;

        wm_app_copy(&key, &l->app[i]);
        j = i;
        while (j > 0) {
            const struct ds_wm_app *prev = &l->app[j - 1u];
            int swap;

            if (prev->kind != key.kind) {
                swap = (prev->kind > key.kind) ? 1 : 0;
            } else {
                swap = (wm_name_cmp(prev->name, key.name) > 0) ? 1 : 0;
            }
            if (!swap) {
                break;
            }
            wm_app_copy(&l->app[j], prev);
            j--;
        }
        wm_app_copy(&l->app[j], &key);
    }
    l->bins = 0;
    l->pkgs = 0;
    l->launchable = 0;
    for (i = 0; i < l->count; i++) {
        if (l->app[i].kind == DS_WM_APP_BIN) {
            l->bins++;
        } else if (l->app[i].kind == DS_WM_APP_PKG) {
            l->pkgs++;
        }
        if (l->app[i].launchable != 0u) {
            l->launchable++;
        }
    }
}

const struct ds_wm_app *ds_wm_launcher_at(const struct ds_wm_launcher *l,
                                           u32 index)
{
    u32 i;
    u32 seen = 0;

    if (l == NULL) {
        return NULL;
    }
    for (i = 0; i < l->count; i++) {
        if (l->app[i].launchable == 0u) {
            continue;
        }
        if (seen == index) {
            return &l->app[i];
        }
        seen++;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 任务列表                                                                   */
/* ------------------------------------------------------------------------- */

u32 ds_wm_tasks_from_msg(const struct ds_msg *m, struct ds_wm_tasks *out)
{
    u32 n;
    u32 i;

    if (m == NULL || out == NULL) {
        return 0;
    }
    ds_memzero(out, (u64)sizeof(*out));
    if (m->type != DS_MSG_LIST_REPLY || m->status != 0) {
        return 0;
    }
    n = m->u.list_reply.count;
    if (n > DS_WM_TASKS_MAX) {
        n = DS_WM_TASKS_MAX;
    }
    for (i = 0; i < n; i++) {
        const struct ds_list_entry *e = &m->u.list_reply.entries[i];
        struct ds_wm_task *t;

        if (e->id == 0) {
            continue;
        }
        t = &out->task[out->count];
        t->id = e->id;
        t->owner_pid = e->owner_pid;
        t->x = e->x;
        t->y = e->y;
        t->w = e->w;
        t->h = e->h;
        t->focused = (e->focused != 0u) ? 1u : 0u;
        ds_title_get(e->title, t->title, (u32)DS_TITLE_MAX);
        if (t->focused != 0u) {
            out->focus_id = e->id;
        }
        out->count++;
    }
    return out->count;
}

int ds_wm_tasks_find(const struct ds_wm_tasks *t, u32 id)
{
    u32 i;

    if (t == NULL || id == 0) {
        return -1;
    }
    for (i = 0; i < t->count; i++) {
        if (t->task[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

u32 ds_wm_tasks_apps(const struct ds_wm_tasks *t, u32 self_id)
{
    u32 i;
    u32 n = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < t->count; i++) {
        if (t->task[i].id != 0u && t->task[i].id != self_id) {
            n++;
        }
    }
    return n;
}

int ds_wm_tasks_next(const struct ds_wm_tasks *t, u32 self_id, u32 *out_id)
{
    u32 ids[DS_WM_TASKS_MAX];
    u32 n = 0;
    u32 i;
    u32 pos;
    u32 focus_pos = DS_WM_TASKS_MAX;

    if (out_id != NULL) {
        *out_id = 0;
    }
    if (t == NULL || out_id == NULL) {
        return DS_EINVAL;
    }
    for (i = 0; i < t->count; i++) {
        if (t->task[i].id == 0u || t->task[i].id == self_id) {
            continue;
        }
        ids[n++] = t->task[i].id;
    }
    if (n == 0) {
        return DS_ENOENT;
    }
    for (i = 0; i < n; i++) {
        if (ids[i] == t->focus_id) {
            focus_pos = i;
            break;
        }
    }
    pos = (focus_pos == DS_WM_TASKS_MAX) ? 0u : ((focus_pos + 1u) % n);
    *out_id = ids[pos];
    return DS_OK;
}

/* ------------------------------------------------------------------------- */
/* 热键状态机                                                                 */
/* ------------------------------------------------------------------------- */

void ds_wm_hotkey_init(struct ds_wm_hotkey *h)
{
    if (h != NULL) {
        ds_memzero(h, (u64)sizeof(*h));
    }
}

u32 ds_wm_hotkey_index(u32 key)
{
    if (key >= (u32)'1' && key <= (u32)'9') {
        return key - (u32)'1';
    }
    return DS_WM_LAUNCH_INDEX_NONE;
}

int ds_wm_hotkey_map(u32 key, u32 flags, u32 *out_index)
{
    (void)flags; /* Alt 没有单独的修饰位：Alt+Tab 也以 Tab 到达。 */
    if (out_index != NULL) {
        *out_index = DS_WM_LAUNCH_INDEX_NONE;
    }
    if (key == 9u) { /* Tab */
        return DS_WM_HOTKEY_TASK_SWITCH;
    }
    if (key >= (u32)'1' && key <= (u32)'9') {
        if (out_index != NULL) {
            *out_index = key - (u32)'1';
        }
        return DS_WM_HOTKEY_LAUNCH;
    }
    return DS_WM_HOTKEY_NONE;
}

int ds_wm_hotkey_consume(struct ds_wm_hotkey *h, u32 key, u32 flags,
                         u32 *out_index)
{
    int action;

    if (out_index != NULL) {
        *out_index = DS_WM_LAUNCH_INDEX_NONE;
    }
    action = ds_wm_hotkey_map(key, flags, out_index);
    if (h != NULL) {
        h->last_key = key;
        h->last_action = (u32)action;
        if (action == DS_WM_HOTKEY_NONE) {
            h->ignored++;
        }
    }
    return action;
}

int ds_wm_input_is_global(u32 flags)
{
    return ((flags & DS_INPUT_F_GLOBAL) != 0u) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 面板绘制                                                                   */
/* ------------------------------------------------------------------------- */

#define WM_OPAQUE(rgb) (0xFF000000u | (rgb))

void ds_wm_draw_panel(struct ds_buffer *b, const struct ds_wm_layout *l,
                      const struct ds_wm_tasks *tasks, u32 self_id,
                      const struct ds_wm_launcher *apps, u32 launcher_open)
{
    u32 i;
    u32 slot = 0;

    if (b == NULL || l == NULL) {
        return;
    }
    (void)apps; /* 启动器条目数已由 WM 的证据行打印；面板只画按钮 */
    ds_draw_fill(b, 0, 0, (i64)l->panel_w, (i64)l->panel_h,
                 WM_OPAQUE(DS_WM_PANEL_BG));
    if (l->panel_h > 2u) {
        ds_draw_fill(b, 0, (i64)l->panel_h - 2, (i64)l->panel_w, 2,
                     WM_OPAQUE(DS_WM_ACCENT));
    }
    ds_draw_text(b, (i64)l->title.x + 8, (i64)l->title.y + 12,
                 DS_WM_TITLE_TEXT, WM_OPAQUE(DS_WM_PANEL_FG),
                 WM_OPAQUE(DS_WM_PANEL_BG));

    ds_draw_fill(b, (i64)l->launcher.x, (i64)l->launcher.y,
                 (i64)l->launcher.w, (i64)l->launcher.h,
                 WM_OPAQUE((launcher_open != 0u) ? DS_WM_BTN_FOCUS
                                                  : DS_WM_BTN_BG));
    ds_draw_outline(b, (i64)l->launcher.x, (i64)l->launcher.y, l->launcher.w,
                    l->launcher.h, 1, WM_OPAQUE(DS_WM_ACCENT));
    ds_draw_text(b, (i64)l->launcher.x + 8, (i64)l->launcher.y + 8,
                 DS_WM_LAUNCHER_TEXT, WM_OPAQUE(DS_WM_BTN_FG),
                 WM_OPAQUE((launcher_open != 0u) ? DS_WM_BTN_FOCUS
                                                  : DS_WM_BTN_BG));

    if (tasks != NULL) {
        for (i = 0; i < tasks->count; i++) {
            const struct ds_wm_task *t = &tasks->task[i];
            struct ds_wm_rect r;
            char text[DS_TITLE_MAX];
            u32 bg;

            if (t->id == 0u || t->id == self_id) {
                continue;
            }
            if (!ds_wm_task_rect(l, slot, &r)) {
                break;
            }
            bg = (t->focused != 0u) ? DS_WM_BTN_FOCUS : DS_WM_BTN_BG;
            ds_draw_fill(b, (i64)r.x, (i64)r.y, (i64)r.w, (i64)r.h,
                         WM_OPAQUE(bg));
            ds_draw_outline(b, (i64)r.x, (i64)r.y, r.w, r.h, 1,
                            WM_OPAQUE(DS_WM_ACCENT));
            ds_wm_text_fit(text, (u32)DS_TITLE_MAX,
                           (t->title[0] != '\0') ? t->title : "app");
            if (wm_strlen(text) > (u32)DS_WM_TASK_CHARS) {
                text[DS_WM_TASK_CHARS] = '\0';
            }
            ds_draw_text(b, (i64)r.x + 4, (i64)r.y + 8, text,
                         WM_OPAQUE(DS_WM_BTN_FG), WM_OPAQUE(bg));
            slot++;
        }
    }

    ds_draw_text(b, (i64)l->clock.x + 8, (i64)l->clock.y + 12,
                 DS_WM_CLOCK_TEXT, WM_OPAQUE(DS_WM_CLOCK_FG),
                 WM_OPAQUE(DS_WM_PANEL_BG));

    ds_draw_fill(b, (i64)l->notif.x, (i64)l->notif.y, (i64)l->notif.w,
                 (i64)l->notif.h, WM_OPAQUE(DS_WM_NOTIF_BG));
    ds_draw_outline(b, (i64)l->notif.x, (i64)l->notif.y, l->notif.w,
                    l->notif.h, 1, WM_OPAQUE(DS_WM_ACCENT));
    ds_draw_fill(b, (i64)l->notif.x + 8, (i64)l->notif.y + 9, 6, 6,
                 WM_OPAQUE(DS_WM_NOTIF_DOT));
}

u32 ds_wm_panel_pixel(const u32 *px, u32 stride_px, u32 x, u32 y)
{
    if (px == NULL || x >= stride_px) {
        return 0;
    }
    return px[(u64)y * (u64)stride_px + (u64)x];
}
