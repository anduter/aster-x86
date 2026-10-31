/*
 * ng WP3-3: session manager 的纯状态机（L2c，host 单测直接驱动）。
 *
 * 这里只有"会话表 + 令牌 + 策略 + 重启退避 + 状态文件文本"，不碰 IPC / 串口 /
 * 页表 / 线程：sessiond（ring3）负责把这些结果搬到 syscall 上。
 *
 * 规则（都有 host 单测，tests/unit-ng/test_session.c）：
 *   - 会话表 8 槽；每个会话项 = {pid, role_token, role, state, surface_id,
 *     port, title, last_seen}。pid 单调（内核分配），令牌由本模块签发。
 *   - 角色令牌 32 位：seq(8) | role(8) | nonce(16)；签发时 nonce/seq 归一化，
 *     0 永远不是合法令牌。
 *   - 只有 WM 角色可以 LAUNCH / KILL；APP 只能 REGISTER/HEARTBEAT/EXIT 自己。
 *   - 焦点策略：新窗口 REGISTER 之后由 sessiond 发 FOCUS（令牌校验在
 *     displayd 侧：surface.token == token，或令牌角色是 WM）。
 *   - 崩溃检测：last_seen 超过 stale_ticks 视为死；WM 死 -> 指数退避重启
 *     （restart_limit 次）；APP 死 -> 回收（kill/wait + DESTROY 窗口）。
 *   - 注册表与 displayd LIST 同步：每条 running 且有窗口的项必须在 LIST 里
 *     找到同 id / 同 owner_pid / 同 token 的记录，否则计 mismatch。
 */
#ifndef NG_SESSION_TABLE_H
#define NG_SESSION_TABLE_H

#include "ds_protocol.h"
#include "session_protocol.h"
#include "types.h"

#define SESSION_SLOT_NONE 0xFFFFFFFFu

struct session_entry {
    u32 used;
    u32 pid;
    u32 token;
    u32 role;
    u32 state; /* SESSION_STATE_* */
    u32 surface_id;
    u32 port; /* 客户端端口 ticket（0 = 还没上报） */
    char title[DS_TITLE_MAX];
    u32 heartbeats;
    u32 restarts; /* 这个槽位被重启的次数（WM） */
    u32 exit_code;
    u64 last_seen; /* TSC tick（最后一次 REGISTER/HEARTBEAT） */
};

struct session_table {
    struct session_entry e[SESSION_MAX_ENTRIES];
    u32 count;
    u32 next_seq;   /* 令牌序号（全局单调，1..255 回绕） */
    u32 next_nonce; /* 令牌 nonce（会话内单调） */
    u32 wm_slot;    /* 当前 WM 的槽位（SESSION_SLOT_NONE = 没有） */
    /*
     * 已作废的令牌（WM 重启/应用回收）：displayd 的 LIST 里可能还有一条
     * 旧窗口的滞后快照，看到这些令牌不算"外来令牌"，避免瞬时 mismatch。
     */
    u32 retired[4];
    u32 retired_n;
    u64 launches;   /* LAUNCH 成功次数 */
    u64 kills;      /* KILL 成功次数 */
    u64 exits;      /* EXIT 次数 */
    u64 reaps;      /* 崩溃回收次数（APP） */
    u64 rejects;    /* 令牌/角色拒绝次数 */
    u64 restarts;   /* WM 重启次数 */
    u64 token_checks; /* 注册表同步里校验过的令牌数 */
    u64 token_fail;   /* 令牌/pid 绑定不一致次数 */
    u32 registry_syncs;
    u32 registry_ok; /* 最近一次同步是否通过（1 = 通过） */
};

#define SESSION_BACKOFF_BASE 20000000ULL  /* 20ms @ ~1GHz TSC */
#define SESSION_BACKOFF_CAP 160000000ULL  /* 最多 160ms */
#define SESSION_RESTART_LIMIT 3u

void session_table_init(struct session_table *t);

/*
 * 分配一个会话项并签发令牌。pid 必须 > 0；role 必须是 WM/APP/SERVICE。
 * 成功返回槽位（>=0），表满/参数错返回负数（DS_ENOSPC/DS_EINVAL）。
 */
int session_alloc(struct session_table *t, u32 pid, u32 role, u64 now,
                  u32 *out_token);
/*
 * 用**指定**令牌登记会话项（sessiond 先签发令牌、把令牌经 argv 交给子进程，
 * spawn 成功后才知道 pid，所以需要这一步）。token 必须合法且未被占用。
 */
int session_alloc_with_token(struct session_table *t, u32 pid, u32 role,
                             u64 now, u32 token);
struct session_entry *session_by_pid(struct session_table *t, u32 pid);
struct session_entry *session_by_token(struct session_table *t, u32 token);
struct session_entry *session_by_surface(struct session_table *t,
                                         u32 surface_id);
struct session_entry *session_at(struct session_table *t, u32 slot);
struct session_entry *session_wm(struct session_table *t);

/* 窗口注册（token 必须属于一个 running 项）。port/title 可空。 */
int session_register_window(struct session_table *t, u32 token, u32 port,
                            u32 surface_id, const char *title, u64 now);
/* 心跳（只更新 last_seen/计数）。 */
int session_heartbeat(struct session_table *t, u32 token, u64 now);
/* 正常退出：state -> EXITED，记 exit_code。 */
int session_mark_exit(struct session_table *t, u32 token, u32 code);
/* 崩溃：state -> DEAD（按 pid 找）。 */
int session_mark_dead(struct session_table *t, u32 pid);
/* 作废一个令牌（进程退出/重启后调用；保留最近 4 个用于 LIST 滞后快照）。 */
void session_retire_token(struct session_table *t, u32 token);
/* 令牌是否已经作废（最近 4 个之内）。 */
int session_token_retired(const struct session_table *t, u32 token);
/*
 * LAUNCH/KILL 的权限检查：令牌必须是 WM 角色且 running。
 * 返回 DS_OK / DS_EPERM（角色不对）/ DS_ENOENT（令牌不存在）。
 */
int session_launch_allowed(struct session_table *t, u32 token);
int session_kill_allowed(struct session_table *t, u32 token);

/* 计数：running 的应用 / 有窗口的 running 项 / running 的全部项。 */
u32 session_apps(const struct session_table *t);
u32 session_windows(const struct session_table *t);
u32 session_procs(const struct session_table *t);
/* 最近一个 running 应用 pid（没有返回 0）。 */
u32 session_last_app_pid(const struct session_table *t);
/* 有效令牌数（token != 0 且 running）。 */
u32 session_tokens(const struct session_table *t);

/*
 * 与 displayd 的 LIST_REPLY 同步：
 *   - running 且 surface_id != 0 的项：LIST 里必须存在同 id 的记录，且
 *     owner_pid == pid、token == token；否则 mismatch++。
 *   - LIST 记录不存在 -> 窗口已经没了：把 surface_id 清 0（window_gone++）。
 *   - LIST 里带 token 但会话表找不到对应令牌 -> mismatch++（外来令牌）。
 * 返回 mismatch 数；*out_bound 写成功校验的窗口数。
 */
u32 session_sync_list(struct session_table *t,
                      const struct ds_list_entry *list, u32 n,
                      u32 *out_bound);

/* 指数退避：restart_index 从 1 开始（20ms, 40ms, 80ms, 160ms 封顶）。 */
u64 session_backoff_ticks(u32 restart_index);
/* restarts < limit 时返回 1 并写 *out_delay（退避 tick）。 */
int session_should_restart(u32 restarts, u32 limit, u64 *out_delay);

/*
 * 状态文件文本（`/run/session.status`，k=v 单行 + '\n'）。返回写出的字节数；
 * cap 不够时尽量截断并返回 cap-1。
 */
u32 session_status_format(const struct session_table *t, u32 self_pid,
                          u32 displayd_pid, u32 wm_pid, u32 wm_state,
                          u32 wm_restarts, char *buf, u32 cap);

#endif /* NG_SESSION_TABLE_H */
