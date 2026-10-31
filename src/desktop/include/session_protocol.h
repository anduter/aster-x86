/*
 * ng WP3-3: session manager（L2c）协议 —— 应用生命周期/窗口注册表/角色令牌。
 *
 * sessiond 是 ring3 进程，应用只能经它启动：
 *
 *   init ──spawn──> sessiond ──spawn──> displayd（L2a）
 *                     │      └─spawn──> wm（L2b，带 WM 角色令牌）
 *                     │
 *   wm ──LAUNCH(token,path)──> sessiond ──spawn──> app（带 APP 角色令牌）
 *   app ──REGISTER(token,surface,title)──> sessiond ──FOCUS(token)──> displayd
 *   app/wm ──HEARTBEAT(token)──> sessiond（崩溃检测；过期 -> 回收/重启）
 *   app ──EXIT(token,code)──> sessiond（正常退出回收）
 *   wm  ──KILL(token,pid)──> sessiond（可选：会话内终止应用）
 *   wm  ──LIST/LOOKUP──> sessiond（窗口注册表查询）
 *
 * 通道发现：sessiond 把自己的端口 ticket 经 **argv** 交给它启动的进程
 *   （`argv[2]="session=<ticket>"`、`argv[3]="token=<hex>"`），不新建 ramfs
 *   发现文件（ramfs 节点池只有 32 个）。应用只会从 sessiond 拿到令牌，令牌
 *   是不透明能力（32 位，含角色/序号/nonce）。
 *
 * 角色令牌布局（32 位）：
 *   bit  0..7   seq   同一角色的序号（1..255）
 *   bit  8..15  role  SESSION_ROLE_*
 *   bit 16..31  nonce 签发时的 TSC/pid 混合（非 0，让令牌不可猜）
 *
 * 控制面消息 = 20 字节头（magic/version/type/len/status/seq）+ 固定长度 body，
 * body 只用 u32；编解码是纯函数（host 单测 tests/unit-ng/test_session.c）。
 */
#ifndef NG_SESSION_PROTOCOL_H
#define NG_SESSION_PROTOCOL_H

#include "ds_protocol.h" /* 复用 DS_TITLE_WORDS / ds_title_set/get / ds_mem* */
#include "types.h"

/* ---- 协议版本 / 尺寸 ------------------------------------------------------ */
#define SESSION_MAGIC 0x534E314Du /* "SN1M"（内存序 "M1NS"） */
#define SESSION_VERSION 1u
#define SESSION_HDR_SIZE 20u
#define SESSION_MSG_MAX 512u
#define SESSION_MSG_WORDS_MAX 123u /* (512 - 20) / 4 */
#define SESSION_IPC_HDR_SIZE 16u   /* 与 struct ipc_msg_hdr 逐字节一致 */

/* ---- 资源上限 ------------------------------------------------------------- */
#define SESSION_MAX_ENTRIES 8u   /* 会话表项（WM + 最多 7 个应用） */
#define SESSION_PATH_MAX 64u     /* LAUNCH path（= PROC_PATH_MAX） */
#define SESSION_ARG_MAX 16u      /* LAUNCH arg（"window" 等短参数） */
#define SESSION_TITLE_WORDS DS_TITLE_WORDS
#define SESSION_ENTRY_WORDS 10u  /* LIST_REPLY 的每条 entry：40 字节 */
#define SESSION_PORT_DEPTH 16u

/* ---- 角色 ---------------------------------------------------------------- */
#define SESSION_ROLE_NONE 0u
#define SESSION_ROLE_WM 1u      /* shell/WM：可以 LAUNCH/KILL/切换焦点 */
#define SESSION_ROLE_APP 2u     /* L3 应用：只能上报自己的窗口 */
#define SESSION_ROLE_SERVICE 3u /* 服务（displayd 等，保留） */
#define SESSION_ROLE_SESSION 4u /* session manager 自己（保留） */

#define SESSION_TOKEN_ROLE(t) (((t) >> 8) & 0xFFu)
#define SESSION_TOKEN_SEQ(t) ((t) & 0xFFu)
#define SESSION_TOKEN_NONCE(t) ((t) >> 16)

/* 表项状态 */
#define SESSION_STATE_FREE 0u
#define SESSION_STATE_RUNNING 1u
#define SESSION_STATE_EXITED 2u /* 正常退出（EXIT 或已经被回收） */
#define SESSION_STATE_DEAD 3u   /* 崩溃，等重启/清理 */

/* 消息类型 */
#define SESSION_MSG_REGISTER 1u  /* client -> sessiond：token,port,surface,title */
#define SESSION_MSG_HEARTBEAT 2u /* client -> sessiond：token,rounds */
#define SESSION_MSG_LAUNCH 3u    /* wm -> sessiond：token,path,arg */
#define SESSION_MSG_EXIT 4u      /* client -> sessiond：token,code */
#define SESSION_MSG_KILL 5u      /* wm -> sessiond：token,pid,surface */
#define SESSION_MSG_LOOKUP 6u    /* client -> sessiond：surface_id / pid */
#define SESSION_MSG_ENTRY 7u     /* sessiond -> client：LOOKUP 的回复 */
#define SESSION_MSG_LIST 8u      /* client -> sessiond：要注册表快照 */
#define SESSION_MSG_LIST_REPLY 9u /* sessiond -> client：count + entries[] */
#define SESSION_MSG_SESSION_STATUS 10u /* client -> sessiond：状态查询（保留） */

/* ---- body（全部 u32，长度固定） ------------------------------------------ */
struct session_register_body {
    u32 token;
    u32 port;       /* 客户端自己的端口 ticket（回复用） */
    u32 surface_id; /* 窗口（0 = 没有窗口，如无 GUI 应用） */
    u32 title[SESSION_TITLE_WORDS];
};

struct session_heartbeat_body {
    u32 token;
    u32 rounds; /* 客户端自增的轮次（诊断用） */
};

struct session_launch_body {
    u32 token; /* 请求者令牌（必须是 WM 角色） */
    u32 path[SESSION_PATH_MAX / 4u];
    u32 arg[SESSION_ARG_MAX / 4u];
};

struct session_exit_body {
    u32 token;
    u32 code;
};

struct session_kill_body {
    u32 token; /* 请求者令牌（必须是 WM 角色） */
    u32 pid;
    u32 surface_id;
};

struct session_lookup_body {
    u32 surface_id;
    u32 pid;
};

/* 一条注册表记录（40 字节）。 */
struct session_entry_info {
    u32 pid;
    u32 token;
    u32 surface_id;
    u32 state;
    u32 role;
    u32 reserved;
    u32 title[SESSION_TITLE_WORDS];
};

/* LOOKUP 的回复（48 字节）；found=0 时 info 无意义。 */
struct session_entry_body {
    u32 found;
    u32 reserved;
    struct session_entry_info info;
};

struct session_list_body {
    u32 reserved;
};

struct session_status_body {
    u32 reserved;
};

struct session_list_reply_body {
    u32 count;
    u32 procs;   /* 表里的进程数（含 WM） */
    u32 apps;    /* L3 应用数 */
    u32 windows; /* 有窗口的项数 */
    struct session_entry_info entries[SESSION_MAX_ENTRIES];
};

/* ---- 解码后的完整消息 ----------------------------------------------------- */
struct session_msg {
    u32 magic;
    u16 version;
    u16 type;
    u32 len;
    i32 status;
    u32 seq;
    union {
        struct session_register_body reg;
        struct session_heartbeat_body hb;
        struct session_launch_body launch;
        struct session_exit_body exit;
        struct session_kill_body kill;
        struct session_lookup_body lookup;
        struct session_entry_body entry;
        struct session_list_body list;
        struct session_list_reply_body list_reply;
        struct session_status_body status;
        u32 words[SESSION_MSG_WORDS_MAX];
    } u;
};

/* IPC 16 字节头（与 struct ipc_msg_hdr 逐字节一致）。 */
struct session_ipc_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};
_Static_assert(sizeof(struct session_ipc_hdr) == SESSION_IPC_HDR_SIZE,
               "session_ipc_hdr must be 16 bytes");

/* ---- 纯函数 --------------------------------------------------------------- */
u32 session_msg_body_size(u32 type);
u32 session_msg_size(u32 type);
int session_msg_encode(const struct session_msg *m, void *buf, u32 cap);
int session_msg_decode(const void *buf, u32 len, struct session_msg *out);

/* 角色令牌 */
u32 session_token_make(u32 role, u32 seq, u32 nonce);
int session_token_valid(u32 token);
/* 令牌的角色是否是 role（token=0 时返回 0）。 */
int session_token_is(u32 token, u32 role);

/* 路径/参数打包（NUL padding；超长截断） */
void session_path_set(u32 out[SESSION_PATH_MAX / 4u], const char *s);
void session_path_get(const u32 in[SESSION_PATH_MAX / 4u], char *out, u32 cap);
void session_arg_set(u32 out[SESSION_ARG_MAX / 4u], const char *s);
void session_arg_get(const u32 in[SESSION_ARG_MAX / 4u], char *out, u32 cap);

/* 解析 "session=<dec>" / "token=<hex|dec>" 形式的 argv 项；失败返回 0。 */
int session_arg_parse_u32(const char *s, const char *key, u32 *out);

/* 填一条注册表记录（列表回复/LOOKUP 共用）。 */
void session_entry_fill(struct session_entry_info *e, u32 pid, u32 token,
                        u32 surface_id, u32 state, u32 role,
                        const char *title);

#endif /* NG_SESSION_PROTOCOL_H */
