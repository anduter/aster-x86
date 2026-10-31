/*
 * ng WP3-3: session manager 纯状态机实现（无 libc / 无内核依赖）。
 */
#include "session_table.h"

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void st_zero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void st_copy(char *dst, const char *src, u32 cap)
{
    u32 i;

    if (dst == NULL || cap == 0) {
        return;
    }
    for (i = 0; i + 1u < cap && src != NULL && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------------- */
/* 表 / 令牌                                                                  */
/* ------------------------------------------------------------------------- */

void session_table_init(struct session_table *t)
{
    if (t == NULL) {
        return;
    }
    st_zero(t, (u64)sizeof(*t));
    t->wm_slot = SESSION_SLOT_NONE;
    t->next_seq = 1;
    t->next_nonce = 0x4D31u; /* 种子；每次签发 +1，保证非 0 */
}

static int st_alloc_slot(struct session_table *t, u32 pid, u32 role, u64 now,
                         u32 token, u32 *out_token)
{
    u32 slot;
    struct session_entry *e;

    if (t == NULL || pid == 0) {
        return DS_EINVAL;
    }
    if (role != SESSION_ROLE_WM && role != SESSION_ROLE_APP &&
        role != SESSION_ROLE_SERVICE) {
        return DS_EINVAL;
    }
    if (session_by_pid(t, pid) != NULL) {
        return DS_EBUSY;
    }
    if (session_by_token(t, token) != NULL) {
        return DS_EBUSY;
    }
    for (slot = 0; slot < SESSION_MAX_ENTRIES; slot++) {
        if (!t->e[slot].used) {
            break;
        }
    }
    if (slot == SESSION_MAX_ENTRIES) {
        return DS_ENOSPC;
    }
    e = &t->e[slot];
    st_zero(e, (u64)sizeof(*e));
    e->used = 1;
    e->pid = pid;
    e->token = token;
    e->role = role;
    e->state = SESSION_STATE_RUNNING;
    e->last_seen = now;
    t->count++;
    if (role == SESSION_ROLE_WM) {
        t->wm_slot = slot;
    }
    if (out_token != NULL) {
        *out_token = token;
    }
    return (int)slot;
}

int session_alloc(struct session_table *t, u32 pid, u32 role, u64 now,
                  u32 *out_token)
{
    u32 token;

    if (out_token != NULL) {
        *out_token = 0;
    }
    if (t == NULL || pid == 0) {
        return DS_EINVAL;
    }
    if (role != SESSION_ROLE_WM && role != SESSION_ROLE_APP &&
        role != SESSION_ROLE_SERVICE) {
        return DS_EINVAL;
    }
    t->next_seq++;
    if ((t->next_seq & 0xFFu) == 0u) {
        t->next_seq++; /* 序号 0 不合法 */
    }
    t->next_nonce++;
    token = session_token_make(role, t->next_seq, t->next_nonce);
    if (token == 0) {
        return DS_EINVAL;
    }
    return st_alloc_slot(t, pid, role, now, token, out_token);
}

int session_alloc_with_token(struct session_table *t, u32 pid, u32 role,
                             u64 now, u32 token)
{
    if (t == NULL || !session_token_valid(token)) {
        return DS_EINVAL;
    }
    if (SESSION_TOKEN_ROLE(token) != role) {
        return DS_EINVAL;
    }
    /* 保持令牌序号的单调性：显式令牌也推进 seq/nonce。 */
    if (SESSION_TOKEN_SEQ(token) > (t->next_seq & 0xFFu)) {
        t->next_seq = SESSION_TOKEN_SEQ(token);
    }
    t->next_nonce++;
    return st_alloc_slot(t, pid, role, now, token, NULL);
}

struct session_entry *session_at(struct session_table *t, u32 slot)
{
    if (t == NULL || slot >= SESSION_MAX_ENTRIES) {
        return NULL;
    }
    return (t->e[slot].used != 0) ? &t->e[slot] : NULL;
}

struct session_entry *session_by_pid(struct session_table *t, u32 pid)
{
    u32 i;

    if (t == NULL || pid == 0) {
        return NULL;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].pid == pid) {
            return &t->e[i];
        }
    }
    return NULL;
}

struct session_entry *session_by_token(struct session_table *t, u32 token)
{
    u32 i;

    if (t == NULL || token == 0) {
        return NULL;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].token == token) {
            return &t->e[i];
        }
    }
    return NULL;
}

struct session_entry *session_by_surface(struct session_table *t,
                                         u32 surface_id)
{
    u32 i;

    if (t == NULL || surface_id == 0) {
        return NULL;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].surface_id == surface_id) {
            return &t->e[i];
        }
    }
    return NULL;
}

struct session_entry *session_wm(struct session_table *t)
{
    if (t == NULL || t->wm_slot == SESSION_SLOT_NONE) {
        return NULL;
    }
    return session_at(t, t->wm_slot);
}

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

int session_register_window(struct session_table *t, u32 token, u32 port,
                            u32 surface_id, const char *title, u64 now)
{
    struct session_entry *e = session_by_token(t, token);

    if (e == NULL) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_ENOENT;
    }
    if (e->state != SESSION_STATE_RUNNING) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_EPERM;
    }
    if (surface_id != 0) {
        struct session_entry *owner = session_by_surface(t, surface_id);

        if (owner != NULL && owner != e) {
            /* 同一个 surface 只能属于一个会话项。 */
            t->rejects++;
            return DS_EBUSY;
        }
        /*
         * v3 审计加固（第二轮）：owner == e 表示同一条会话项**重复 REGISTER**
         * （客户端丢了回复之后重试，或者重新上报标题）。以前这里同样返回
         * EBUSY：重试的客户端永远注册不上，rejects 被无辜抬高。现在幂等：
         * 端口/窗口/标题/心跳时间照常更新，返回 DS_OK。
         */
    }
    e->port = port;
    e->surface_id = surface_id;
    e->last_seen = now;
    st_copy(e->title, title, (u32)sizeof(e->title));
    return DS_OK;
}

int session_heartbeat(struct session_table *t, u32 token, u64 now)
{
    struct session_entry *e = session_by_token(t, token);

    if (e == NULL) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_ENOENT;
    }
    if (e->state != SESSION_STATE_RUNNING) {
        return DS_EPERM;
    }
    e->heartbeats++;
    e->last_seen = now;
    return DS_OK;
}

int session_mark_exit(struct session_table *t, u32 token, u32 code)
{
    struct session_entry *e = session_by_token(t, token);

    if (e == NULL) {
        if (t != NULL) {
            t->rejects++;
        }
        return DS_ENOENT;
    }
    if (e->state == SESSION_STATE_RUNNING) {
        t->exits++;
    }
    e->state = SESSION_STATE_EXITED;
    e->exit_code = code;
    return DS_OK;
}

int session_mark_dead(struct session_table *t, u32 pid)
{
    struct session_entry *e = session_by_pid(t, pid);

    if (e == NULL) {
        return DS_ENOENT;
    }
    if (e->state == SESSION_STATE_RUNNING) {
        e->state = SESSION_STATE_DEAD;
        if (e->role == SESSION_ROLE_APP) {
            t->reaps++;
        }
    }
    return DS_OK;
}

void session_retire_token(struct session_table *t, u32 token)
{
    if (t == NULL || token == 0) {
        return;
    }
    t->retired[t->retired_n % 4u] = token;
    t->retired_n++;
}

int session_token_retired(const struct session_table *t, u32 token)
{
    u32 i;

    if (t == NULL || token == 0) {
        return 0;
    }
    for (i = 0; i < 4u; i++) {
        if (t->retired[i] == token) {
            return 1;
        }
    }
    return 0;
}

int session_launch_allowed(struct session_table *t, u32 token)
{
    struct session_entry *e = session_by_token(t, token);

    if (t == NULL || token == 0) {
        return DS_ENOENT;
    }
    if (e == NULL) {
        t->rejects++;
        return DS_ENOENT;
    }
    if (e->role != SESSION_ROLE_WM || e->state != SESSION_STATE_RUNNING) {
        t->rejects++;
        return DS_EPERM;
    }
    return DS_OK;
}

int session_kill_allowed(struct session_table *t, u32 token)
{
    return session_launch_allowed(t, token);
}

/* ------------------------------------------------------------------------- */
/* 计数                                                                       */
/* ------------------------------------------------------------------------- */

u32 session_apps(const struct session_table *t)
{
    u32 i;
    u32 n = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].state == SESSION_STATE_RUNNING &&
            t->e[i].role == SESSION_ROLE_APP) {
            n++;
        }
    }
    return n;
}

u32 session_windows(const struct session_table *t)
{
    u32 i;
    u32 n = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].state == SESSION_STATE_RUNNING &&
            t->e[i].surface_id != 0) {
            n++;
        }
    }
    return n;
}

u32 session_procs(const struct session_table *t)
{
    u32 i;
    u32 n = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].state == SESSION_STATE_RUNNING) {
            n++;
        }
    }
    return n;
}

u32 session_last_app_pid(const struct session_table *t)
{
    u32 i;
    u32 pid = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && t->e[i].state == SESSION_STATE_RUNNING &&
            t->e[i].role == SESSION_ROLE_APP && t->e[i].pid > pid) {
            pid = t->e[i].pid;
        }
    }
    return pid;
}

u32 session_tokens(const struct session_table *t)
{
    u32 i;
    u32 n = 0;

    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        if (t->e[i].used && session_token_valid(t->e[i].token)) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 注册表 <-> displayd LIST 同步                                              */
/* ------------------------------------------------------------------------- */

static const struct ds_list_entry *st_list_find(const struct ds_list_entry *l,
                                                u32 n, u32 id)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (l[i].id == id) {
            return &l[i];
        }
    }
    return NULL;
}

u32 session_sync_list(struct session_table *t,
                      const struct ds_list_entry *list, u32 n, u32 *out_bound)
{
    u32 i;
    u32 bound = 0;
    u32 mismatch = 0;

    if (out_bound != NULL) {
        *out_bound = 0;
    }
    if (t == NULL) {
        return 0;
    }
    if (list == NULL) {
        n = 0;
    }
    /*
     * 防御：n 通常是 displayd LIST_REPLY 的 count（IPC body 里的 u32）。
     * 即使调用方忘了校验，也不能越过 entries[] 的容量（审计发现：
     * sessiond 以前把消息里的 count 原样传进来 -> 越界读）。
     */
    if (n > DS_LIST_MAX) {
        n = DS_LIST_MAX;
    }
    /* 1. 会话表里每个有窗口的 running 项都要在 LIST 里。 */
    for (i = 0; i < SESSION_MAX_ENTRIES; i++) {
        struct session_entry *e = &t->e[i];
        const struct ds_list_entry *le;

        if (!e->used || e->state != SESSION_STATE_RUNNING) {
            continue;
        }
        if (e->surface_id == 0) {
            continue;
        }
        le = st_list_find(list, n, e->surface_id);
        if (le == NULL) {
            /*
             * 时序：REGISTER 之后收到的第一份 LIST 可能是在窗口 CREATE 之前
             * 拍下的快照（displayd 与 sessiond 是两个进程）。不能据此清掉
             * 窗口引用，也不算 mismatch —— 窗口回收由 EXIT/心跳路径负责。
             */
            continue;
        }
        t->token_checks++;
        if (le->owner_pid != e->pid || le->token != e->token) {
            t->token_fail++;
            mismatch++;
            continue;
        }
        bound++;
    }
    /* 2. LIST 里带令牌的窗口必须能反查到同一个会话项。 */
    for (i = 0; i < n; i++) {
        struct session_entry *e;

        if (list[i].token == 0) {
            continue; /* v1/v2 客户端 / 非会话窗口不算违规 */
        }
        e = session_by_token(t, list[i].token);
        t->token_checks++;
        if (e == NULL) {
            if (session_token_retired(t, list[i].token)) {
                continue; /* 旧窗口的滞后快照（WM 重启/应用回收） */
            }
            t->token_fail++;
            mismatch++;
            continue;
        }
        if (e->pid != list[i].owner_pid || e->surface_id != list[i].id) {
            t->token_fail++;
            mismatch++;
        }
    }
    t->registry_syncs++;
    t->registry_ok = (mismatch == 0) ? 1u : 0u;
    if (out_bound != NULL) {
        *out_bound = bound;
    }
    return mismatch;
}

/* ------------------------------------------------------------------------- */
/* 重启退避 / 状态文本                                                        */
/* ------------------------------------------------------------------------- */

u64 session_backoff_ticks(u32 restart_index)
{
    u64 d = SESSION_BACKOFF_BASE;
    u32 i;

    if (restart_index == 0) {
        restart_index = 1;
    }
    for (i = 1; i < restart_index; i++) {
        d <<= 1;
        if (d >= SESSION_BACKOFF_CAP) {
            return SESSION_BACKOFF_CAP;
        }
    }
    return (d > SESSION_BACKOFF_CAP) ? SESSION_BACKOFF_CAP : d;
}

int session_should_restart(u32 restarts, u32 limit, u64 *out_delay)
{
    if (out_delay != NULL) {
        *out_delay = 0;
    }
    if (restarts >= limit) {
        return 0;
    }
    if (out_delay != NULL) {
        *out_delay = session_backoff_ticks(restarts + 1u);
    }
    return 1;
}

static u32 st_put(char *buf, u32 cap, u32 len, const char *s)
{
    while (s != NULL && *s != '\0') {
        if (len + 1u >= cap) {
            break;
        }
        buf[len++] = *s++;
    }
    if (cap > 0) {
        buf[len] = '\0';
    }
    return len;
}

static u32 st_put_u32(char *buf, u32 cap, u32 len, u32 v)
{
    char tmp[11];
    u32 n = 0;
    u32 i;

    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    if (cap > 0 && len + n + 1u >= cap) {
        return len;
    }
    for (i = 0; i < n; i++) {
        buf[len++] = tmp[n - 1u - i];
    }
    if (cap > 0) {
        buf[len] = '\0';
    }
    return len;
}

static u32 st_put_u64(char *buf, u32 cap, u32 len, u64 v)
{
    return (v <= 0xFFFFFFFFULL) ? st_put_u32(buf, cap, len, (u32)v)
                                : st_put_u32(buf, cap, len, 0xFFFFFFFFu);
}

u32 session_status_format(const struct session_table *t, u32 self_pid,
                          u32 displayd_pid, u32 wm_pid, u32 wm_state,
                          u32 wm_restarts, char *buf, u32 cap)
{
    u32 len = 0;

    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    len = st_put(buf, cap, len, "up=1 pid=");
    len = st_put_u32(buf, cap, len, self_pid);
    len = st_put(buf, cap, len, " dpid=");
    len = st_put_u32(buf, cap, len, displayd_pid);
    len = st_put(buf, cap, len, " procs=");
    len = st_put_u32(buf, cap, len, session_procs(t));
    len = st_put(buf, cap, len, " apps=");
    len = st_put_u32(buf, cap, len, session_apps(t));
    len = st_put(buf, cap, len, " windows=");
    len = st_put_u32(buf, cap, len, session_windows(t));
    len = st_put(buf, cap, len, " tokens=");
    len = st_put_u32(buf, cap, len, session_tokens(t));
    len = st_put(buf, cap, len, " launches=");
    len = st_put_u64(buf, cap, len, (t != NULL) ? t->launches : 0);
    len = st_put(buf, cap, len, " kills=");
    len = st_put_u64(buf, cap, len, (t != NULL) ? t->kills : 0);
    len = st_put(buf, cap, len, " exits=");
    len = st_put_u64(buf, cap, len, (t != NULL) ? t->exits : 0);
    len = st_put(buf, cap, len, " reaps=");
    len = st_put_u64(buf, cap, len, (t != NULL) ? t->reaps : 0);
    len = st_put(buf, cap, len, " restarts=");
    len = st_put_u32(buf, cap, len, wm_restarts);
    len = st_put(buf, cap, len, " wmpid=");
    len = st_put_u32(buf, cap, len, wm_pid);
    len = st_put(buf, cap, len, " wmstate=");
    len = st_put_u32(buf, cap, len, wm_state);
    len = st_put(buf, cap, len, " apppid=");
    len = st_put_u32(buf, cap, len, session_last_app_pid(t));
    len = st_put(buf, cap, len, " tokenfail=");
    len = st_put_u64(buf, cap, len, (t != NULL) ? t->token_fail : 0);
    len = st_put(buf, cap, len, " registry=");
    len = st_put_u32(buf, cap, len, (t != NULL) ? t->registry_ok : 0);
    len = st_put(buf, cap, len, "\n");
    return len;
}
