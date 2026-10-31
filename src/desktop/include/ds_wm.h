/*
 * ng WP3-2: shell/WM（L2b）的纯逻辑 —— 面板布局、启动器解析、任务列表、
 * 热键状态机、面板绘制。
 *
 * 这些函数只依赖 types.h / ds_protocol.h / ds_draw.h，不碰 IPC、syscall、
 * 共享内存或 LFB：
 *   - `src/desktop/shell/wm.c`（ring3 `/bin/wm`）把它们接到 display 协议上；
 *   - `tests/unit-ng/test_wm.c` 直接驱动它们做 host 单测。
 *
 * WM 自身是 displayd 的**普通客户端**：面板是一块 1280x32 的 surface（放在
 * (0,0)），像素走共享内存，控制面走 v2 的 LIST / FOCUS / REGISTER_HOTKEY。
 * WM 不直接碰 LFB，也不拥有应用进程（那是 WP3-3 session manager）。
 */
#ifndef NG_DS_WM_H
#define NG_DS_WM_H

#include "ds_draw.h"
#include "ds_protocol.h"
#include "types.h"

/* ---- 面板几何 / 颜色（QMP 像素断言与 host 单测共用） --------------------- */
#define DS_WM_PANEL_W 1280u
#define DS_WM_PANEL_H 32u
#define DS_WM_PANEL_BG 0x00202838u
#define DS_WM_PANEL_FG 0x00E8F0FFu
#define DS_WM_ACCENT 0x0030507Au
#define DS_WM_BTN_BG 0x00304460u
#define DS_WM_BTN_FG 0x00C8D8FFu
#define DS_WM_BTN_FOCUS 0x004A7AC0u
#define DS_WM_CLOCK_FG 0x0090A8C8u
#define DS_WM_NOTIF_BG 0x00384A68u
#define DS_WM_NOTIF_DOT 0x00FFC040u

/* 面板内部像素探针：落在纯背景上（标题从 x=8 开始画）。 */
#define DS_WM_PROBE_X 4u
#define DS_WM_PROBE_Y 4u

#define DS_WM_TITLE_TEXT "Aster"
#define DS_WM_LAUNCHER_TEXT "Apps"
#define DS_WM_CLOCK_TEXT "--:--"
#define DS_WM_TASK_W 96u
#define DS_WM_TASK_H 24u

/* 标题文本在面板按钮里的截断长度（8px 字宽、左右各 4px 内边距）。 */
#define DS_WM_TASK_CHARS ((DS_WM_TASK_W - 8u) / 8u)

struct ds_wm_rect {
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct ds_wm_layout {
    u32 panel_w;
    u32 panel_h;
    struct ds_wm_rect title;
    struct ds_wm_rect launcher;
    struct ds_wm_rect clock;
    struct ds_wm_rect notif;
    u32 task_x;
    u32 task_y;
    u32 task_w;
    u32 task_h;
    u32 task_gap;
    u32 task_max; /* 能放进任务区的最多按钮数（<= DS_LIST_MAX） */
};

/* 按面板宽度算一次布局；task_max 由时钟/通知区的位置反推。 */
void ds_wm_layout_init(struct ds_wm_layout *l, u32 panel_w);

/* 第 index 个任务按钮（index < task_max）；越界返回 0。 */
int ds_wm_task_rect(const struct ds_wm_layout *l, u32 index,
                    struct ds_wm_rect *out);

/* 把 src 截断到 cap-1 字节（面板按钮标题最多 DS_WM_TASK_CHARS 字符）。 */
void ds_wm_text_fit(char *dst, u32 cap, const char *src);

/* ---- 启动器 --------------------------------------------------------------- */
#define DS_WM_APPS_MAX 16u
#define DS_WM_NAME_MAX 16u
#define DS_WM_PATH_MAX 48u

#define DS_WM_APP_BIN 1u /* /bin/<name>             可执行文件 */
#define DS_WM_APP_PKG 2u /* /System/Packages/<name> 包目录（本轮不可 spawn） */

struct ds_wm_app {
    char path[DS_WM_PATH_MAX];
    char name[DS_WM_NAME_MAX];
    u32 kind;      /* DS_WM_APP_* */
    u32 launchable; /* 1 = 键盘 1..9 可以启动 */
};

struct ds_wm_launcher {
    struct ds_wm_app app[DS_WM_APPS_MAX];
    u32 count;     /* 总条目（bins + pkgs） */
    u32 bins;      /* /bin 条目 */
    u32 pkgs;      /* /System/Packages 条目 */
    u32 launchable;
};

void ds_wm_launcher_init(struct ds_wm_launcher *l);

/*
 * 把一个目录项加进启动器。`is_dir` 是 VFS_NODE_DIR 判定。
 *   - /bin 里的普通文件 -> BIN；`shell` / `displayd` / `wm` 标记为不可启动
 *     （第二个 shell 会抢串口、第二个 displayd 会覆盖发现文件）；
 *   - /System/Packages 里的目录 -> PKG（本轮只列出，不 spawn）。
 * 返回 1 = 加入；0 = 跳过/重复/表满。
 */
int ds_wm_launcher_add(struct ds_wm_launcher *l, const char *dir,
                       const char *name, u32 name_len, u32 is_dir);

/* 排序（kind 后 name，插入排序；稳定、确定性，键盘 1..9 依赖它）。 */
void ds_wm_launcher_finalize(struct ds_wm_launcher *l);

/* 第 index 个"可启动"条目（0 基；键盘 1..9 用这个下标）；越界返回 NULL。 */
const struct ds_wm_app *ds_wm_launcher_at(const struct ds_wm_launcher *l,
                                           u32 index);

/* ---- 任务列表（LIST_REPLY 的快照） --------------------------------------- */
#define DS_WM_TASKS_MAX DS_LIST_MAX

struct ds_wm_task {
    u32 id;
    u32 owner_pid;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 focused;
    char title[DS_TITLE_MAX];
};

struct ds_wm_tasks {
    struct ds_wm_task task[DS_WM_TASKS_MAX];
    u32 count;
    u32 focus_id;
};

/* 解析 LIST_REPLY；返回条数（0 = 不是合法 LIST_REPLY / 空表）。 */
u32 ds_wm_tasks_from_msg(const struct ds_msg *m, struct ds_wm_tasks *out);
int ds_wm_tasks_find(const struct ds_wm_tasks *t, u32 id);
/* 可切换的窗口数（排除 WM 自己的面板）。 */
u32 ds_wm_tasks_apps(const struct ds_wm_tasks *t, u32 self_id);
/*
 * 下一个焦点窗口：在"排除 self_id"的列表里取当前焦点的下一个（回绕）；
 * 当前焦点不在列表里就取第一个。没有可切换窗口返回 DS_ENOENT。
 */
int ds_wm_tasks_next(const struct ds_wm_tasks *t, u32 self_id, u32 *out_id);

/* ---- 热键状态机 ----------------------------------------------------------- */
#define DS_WM_HOTKEY_NONE 0u
#define DS_WM_HOTKEY_TASK_SWITCH 1u /* Tab（Alt+Tab 也落到 Tab：PS/2 无 Alt 位） */
#define DS_WM_HOTKEY_LAUNCH 2u      /* '1'..'9' */

#define DS_WM_LAUNCH_INDEX_NONE 0xFFFFFFFFu

struct ds_wm_hotkey {
    u32 registered;
    u32 switches;
    u32 launches;
    u32 ignored; /* 不是热键的按键（照常转发给焦点客户端） */
    u32 last_key;
    u32 last_action;
    u32 last_index;
};

void ds_wm_hotkey_init(struct ds_wm_hotkey *h);
/* '1'..'9' -> 0..8；其它返回 DS_WM_LAUNCH_INDEX_NONE。 */
u32 ds_wm_hotkey_index(u32 key);
/* 纯映射：返回 DS_WM_HOTKEY_*；*out_index 只在 LAUNCH 时有意义。 */
int ds_wm_hotkey_map(u32 key, u32 flags, u32 *out_index);
/* 消费一个按键：更新计数，返回动作（NONE 时计数 ignored）。 */
int ds_wm_hotkey_consume(struct ds_wm_hotkey *h, u32 key, u32 flags,
                         u32 *out_index);
/* INPUT.flags 里是否带全局副本标记。 */
int ds_wm_input_is_global(u32 flags);

/* ---- 面板绘制 ------------------------------------------------------------- */
/*
 * 把面板画进 buffer（必须正好 1280x32；WM 的共享内存 surface）。
 * tasks 里 id == self_id 的条目就是 WM 自己的面板，不作为任务按钮画。
 */
void ds_wm_draw_panel(struct ds_buffer *b, const struct ds_wm_layout *l,
                      const struct ds_wm_tasks *tasks, u32 self_id,
                      const struct ds_wm_launcher *apps, u32 launcher_open);

/* 读面板内部像素（绘制后取证据；越界返回 0）。 */
u32 ds_wm_panel_pixel(const u32 *px, u32 stride_px, u32 x, u32 y);

#endif /* NG_DS_WM_H */
