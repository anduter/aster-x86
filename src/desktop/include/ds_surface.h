/*
 * ng WP3-1: display server 的 surface 表（L2a，纯状态机）。
 *
 * 表的所有者是 displayd（`src/desktop/display/displayd.c`）。这里不碰 IPC /
 * 页表 / 帧缓冲：输入是"一块 surface 的像素 VA + 几何"，输出是 surface 的
 * z-order 顺序与裁剪后的脏矩形。所以 host 单测可以直接构造表驱动它
 * （tests/unit-ng/test_desktop.c）。
 *
 * 规则（都有 host 单测）：
 *   - z-order：order[] 从**后到前**排列；CREATE 追加到末尾（最上层），
 *     FOCUS 把该 surface 移到末尾并成为焦点。
 *   - id 从 1 单调递增、永不复用（槽位复用不影响 id）。
 *   - 放置：第一个 surface 在 (DS_FIRST_X, DS_FIRST_Y)，之后每个级联
 *     +DS_CASCADE；超出屏幕时贴边（w/h 可以大于屏幕，合成时裁剪）。
 *   - COMMIT 的矩形先裁剪到 surface 内部；完全不可见 -> EINVAL（记账 rejects）。
 *     COMMIT 要求 surface 已经 MAP（协议顺序 CREATE -> MAP -> COMMIT），否则
 *     EBUSY —— 避免客户端在共享内存还没映射时就合成到脏数据。
 *   - DESTROY 清槽位/顺序；焦点被销毁时焦点落到当前最上层（没有则 0）。
 */
#ifndef NG_DS_SURFACE_H
#define NG_DS_SURFACE_H

#include "ds_protocol.h"
#include "types.h"

struct ds_rect {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct ds_surface {
    u32 used;
    u32 id;
    u32 w;
    u32 h;
    u32 x; /* 屏幕坐标（左上角） */
    u32 y;
    char title[DS_TITLE_MAX]; /* v2 CREATE title（v1 客户端为空串） */
    u32 owner_pid;     /* v2：客户端自报 getpid()；v1 客户端为 0 */
    u32 token;         /* v3：CREATE 带的 role_token（旧客户端/旧版本为 0） */
    u32 client_ticket; /* 客户端回复端口 ticket（INPUT 推送目标） */
    u32 shm_handle;    /* SYS_SHM_ALLOC 返回的共享内存 handle */
    u32 shm_pages;
    u64 pixels; /* 本进程里该 surface 的 ARGB 像素 VA（0 = 未映射） */
    u32 mapped; /* 客户端发过 MAP */
    u32 committed;
    u32 commits; /* COMMIT 次数 */
    struct ds_rect last_commit;
};

struct ds_table {
    struct ds_surface slot[DS_SURFACES_MAX];
    u32 order[DS_SURFACES_MAX]; /* 槽位下标，后 -> 前 */
    u32 count;
    u32 next_id;
    u32 focus_id;
    u32 screen_w;
    u32 screen_h;
    u64 created;
    u64 destroyed;
    u64 commit_total;
    u32 rejects;
};

/* 初始化空表；screen_w/h 决定放置的贴边策略（0 视为 720p 兜底）。 */
void ds_table_init(struct ds_table *t, u32 screen_w, u32 screen_h);
const char *ds_table_version(void);

/*
 * 建一个 surface（v1 语义：空标题、级联放置）。w/h 或 shm_pages 不合法 ->
 * EINVAL，表满 -> ENOSPC，像素缓冲不够 -> EINVAL。成功写 *out_id（>0）。
 */
int ds_table_create(struct ds_table *t, u32 w, u32 h, u32 owner_pid,
                    u32 client_ticket, u32 shm_handle, u32 shm_pages,
                    u64 pixels, u32 *out_id);

/*
 * WP3-2：带标题/指定位置的 CREATE（display 协议 v2）。
 * WP3-3：追加 token（v3 的 role_token；0 = 旧客户端）。
 * title 可为 NULL；x/y >= 0 表示客户端指定左上角（DS_CREATE_F_POS）。
 */
int ds_table_create_ex(struct ds_table *t, u32 w, u32 h, u32 owner_pid,
                       u32 client_ticket, u32 shm_handle, u32 shm_pages,
                       u64 pixels, const char *title, i32 x, i32 y, u32 token,
                       u32 *out_id);

struct ds_surface *ds_table_find(struct ds_table *t, u32 id);
/* 第 index 个（0 = 最下层）；越界返回 NULL。 */
struct ds_surface *ds_table_at(struct ds_table *t, u32 index);
u32 ds_table_count(const struct ds_table *t);
u32 ds_table_focus_id(const struct ds_table *t);

/*
 * WP3-2：把表按 z-order（后 -> 前）填进 LIST_REPLY 的 entries[]，返回条数
 * （<= min(max, DS_LIST_MAX)）。focused 位来自 t->focus_id，标题来自 surface。
 */
u32 ds_table_fill_list(const struct ds_table *t, struct ds_list_entry *out,
                       u32 max);

/* 客户端映射完成（幂等：重复 MAP 返回 DS_OK）。 */
int ds_table_mark_mapped(struct ds_table *t, u32 id);

/*
 * 记录一次 COMMIT：裁到 surface 内部，*out_clip 是实际可见的 surface 局部
 * 矩形（w/h 可能为 0 的场景直接返回 EINVAL，不记账）。要求已 mapped。
 */
int ds_table_commit(struct ds_table *t, u32 id, const struct ds_rect *r,
                    struct ds_rect *out_clip);

int ds_table_focus(struct ds_table *t, u32 id);
int ds_table_destroy(struct ds_table *t, u32 id);

/*
 * v3：带归属校验的 DESTROY（display 协议 v3 的 ds_destroy_body.token）。
 * 允许的请求者（任一即可）：
 *   - 令牌等于 surface 自己的 role_token（窗口应用销毁自己的窗口）；
 *   - 令牌等于 wm_token（WM 清理任意窗口）；
 *   - token == 0 且 surface 没有令牌、且 from == client_ticket（v1/v2 旧客户端，
 *     或 demo 的 scratch surface）。
 * 其余（含 token=0 去销毁有令牌的窗口）返回 DS_EPERM 且不改变表；
 * id 不存在返回 DS_ENOENT。失败也计入 t->rejects。
 */
int ds_surface_can_destroy(const struct ds_surface *s, u32 token, u32 wm_token,
                           u32 from);
int ds_table_destroy_checked(struct ds_table *t, u32 id, u32 token,
                             u32 wm_token, u32 from);

/* ---- 纯几何 helper -------------------------------------------------------- */
int ds_rect_empty(const struct ds_rect *r);
u32 ds_rect_area(const struct ds_rect *r);
/* 把 r 裁到 [0,bw)x[0,bh)；完全不可见返回 0，*out 为 0 面积矩形。 */
int ds_rect_clip(const struct ds_rect *r, u32 bw, u32 bh, struct ds_rect *out);
/* 两个矩形求交；不相交返回 0。 */
int ds_rect_intersect(const struct ds_rect *a, const struct ds_rect *b,
                      struct ds_rect *out);

#endif /* NG_DS_SURFACE_H */
