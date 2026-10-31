/*
 * Aster SDK alpha —— display 协议 v3 封装（ABI 1.0 未冻结）
 *
 * 对应 ring3 display server（/bin/displayd）的控制面协议：
 *   控制面：IPC 16 字节头 + 20 字节协议头 + 固定长度 body（≤ 512 字节）
 *   数据面：surface 像素在跨进程共享内存里，客户端用 SYS_SHM_MAP 映射
 *
 * 一次窗口的生命周期（顺序不能乱）：
 *   connect()  : 读 /run/display.port 拿 displayd ticket + 建自己的回复端口
 *   CREATE     : 请求 {w,h,flags,x,y,owner_pid,title,token} -> CREATE_REPLY
 *                {surface_id, shm_handle, pages, x,y,w,h}
 *   MAP        : 客户端先 SYS_SHM_MAP(shm_handle) 拿到像素 VA，再发 MAP
 *   COMMIT     : 把 surface 局部矩形合成到屏幕（可重复）
 *   FOCUS      : 置焦点（v3 带 role_token），之后按键以 INPUT 推给本客户端
 *   LIST       : 要一份 surface 快照（WM/任务列表用；v3 每条带 token）
 *   DESTROY    : 销毁 surface
 *
 * 协议版本策略（alpha）：
 *   - 本 SDK **发送 version 3**：CREATE body 44（末尾 token）、FOCUS 的第二个
 *     u32 是 token、LIST_REPLY 按 v3 48 字节 entry 编码；
 *   - 解码器接受 version 1..3：v1 CREATE 16 字节、v2 CREATE 40 字节（token=0），
 *     v2 LIST_REPLY 仍按 44 字节 entry 解析（token=0）。SDK 按消息自带的
 *     version 选 body/stride，所以 v3 displayd 的回复能直接解析。
 *   - token 来自 session manager（见 <aster/session.h>）：应用把 sessiond 经
 *     argv 发来的 role_token 填进 CREATE/FOCUS；没有会话通道时传 0（v1/v2
 *     的旧行为，displayd 不做归属校验）。
 *   - 协议版本/结构以 src/desktop/include/ds_protocol.h 为准；WP4-1 冻结 ABI
 *     前不要依赖跨版本二进制兼容。
 *
 * 像素格式：ARGB8888（u32 = 0xAARRGGBB），surface 内 pitch = width 像素。
 */
#ifndef ASTER_DISPLAY_H
#define ASTER_DISPLAY_H

#include <aster/types.h>
#include <aster/syscall.h>
#include <aster/ipc.h>
#include <aster/fs.h>

/* ---- 发现 / 状态文件 ----------------------------------------------------- */
#define ASTER_DISPLAY_PORT_PATH "/run/display.port"
#define ASTER_DISPLAY_STATUS_PATH "/run/display.status"

/* ---- 协议版本 ------------------------------------------------------------ */
#define ASTER_DISPLAY_MAGIC 0x4453314Du /* "DS1M" */
#define ASTER_DISPLAY_VERSION_SEND 3u   /* 本 SDK 发出的版本（v3: token） */
#define ASTER_DISPLAY_VERSION_MIN 1u    /* 解码器接受的最小版本 */
#define ASTER_DISPLAY_VERSION_MAX 3u    /* 解码器接受的最大版本（v3 只多 token） */

/* ---- 尺寸 / 资源上限 ----------------------------------------------------- */
#define ASTER_DISPLAY_HDR_SIZE 20u
#define ASTER_DISPLAY_MSG_MAX 512u
#define ASTER_DISPLAY_MSG_WORDS_MAX 123u /* (512 - 20) / 4 */
#define ASTER_DISPLAY_TITLE_MAX 16u      /* 15 字符 + NUL */
#define ASTER_DISPLAY_TITLE_WORDS 4u
#define ASTER_DISPLAY_SURFACES_MAX 8u
#define ASTER_DISPLAY_LIST_MAX 8u
#define ASTER_DISPLAY_MAX_W 1280u
#define ASTER_DISPLAY_MAX_H 800u
#define ASTER_DISPLAY_PAGE_SIZE 4096u
#define ASTER_DISPLAY_PAGES_MAX 1024u /* 单 surface 数据面上限 4 MiB */
#define ASTER_DISPLAY_PORT_DEPTH 16u

/* ---- 放置策略（displayd 默认；与 ds_protocol.h 一致） -------------------- */
#define ASTER_DISPLAY_FIRST_X 160u
#define ASTER_DISPLAY_FIRST_Y 120u
#define ASTER_DISPLAY_CASCADE 32u
#define ASTER_DISPLAY_CREATE_F_POS 0x1u /* x/y 有效 */

/* ---- 消息类型 ------------------------------------------------------------ */
#define ASTER_DISPLAY_MSG_CREATE 1u
#define ASTER_DISPLAY_MSG_CREATE_REPLY 2u
#define ASTER_DISPLAY_MSG_MAP 3u
#define ASTER_DISPLAY_MSG_COMMIT 4u
#define ASTER_DISPLAY_MSG_DESTROY 5u
#define ASTER_DISPLAY_MSG_FOCUS 6u
#define ASTER_DISPLAY_MSG_INPUT 7u
#define ASTER_DISPLAY_MSG_BYE 8u
#define ASTER_DISPLAY_MSG_LIST 9u
#define ASTER_DISPLAY_MSG_LIST_REPLY 10u
#define ASTER_DISPLAY_MSG_REGISTER_HOTKEY 11u
#define ASTER_DISPLAY_MSG_WM_STATUS 12u
#define ASTER_DISPLAY_MSG_SESSION_STATUS 13u /* v3: sessiond -> displayd 汇总 */

/* ---- INPUT --------------------------------------------------------------- */
#define ASTER_INPUT_KEY_DOWN 1u
#define ASTER_INPUT_KEY_UP 2u
#define ASTER_INPUT_F_GLOBAL 0x100u /* 全局热键副本（普通焦点转发没有） */
#define ASTER_HOTKEY_ANY 0u

/* ---- 时序（TSC tick；QEMU TCG 下约 1 GHz） ------------------------------- */
#define ASTER_DISPLAY_POLL_TICKS 20000000ULL
#define ASTER_DISPLAY_INPUT_WAIT_TICKS 20000000ULL
#define ASTER_DISPLAY_INPUT_WAIT_TRIES 100u
#define ASTER_DISPLAY_REPLY_WAIT_TICKS 400000000ULL
#define ASTER_DISPLAY_REPLY_WAIT_TRIES 20u

/* ---- v2/v3 body（全部 u32；v3 只追加 token） ----------------------------- */
struct aster_display_create_body {
    u32 w;
    u32 h;
    u32 flags; /* ASTER_DISPLAY_CREATE_F_* */
    u32 x;     /* flags & POS 时有效 */
    u32 y;
    u32 owner_pid;             /* 客户端自报 getpid() */
    u32 title[ASTER_DISPLAY_TITLE_WORDS];
    u32 token;                 /* v3；v2 不发送（0） */
};

struct aster_display_create_reply_body {
    u32 surface_id;
    u32 shm_handle;
    u32 pages;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct aster_display_map_body {
    u32 surface_id;
    u32 reserved;
};

struct aster_display_commit_body {
    u32 surface_id;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
};

struct aster_display_destroy_body {
    u32 surface_id;
    u32 reserved;
};

struct aster_display_focus_body {
    u32 surface_id;
    u32 token; /* v2 叫 reserved，必须 0；v3 是请求者 role_token */
};

struct aster_display_input_body {
    u32 surface_id;
    u32 kind; /* ASTER_INPUT_KEY_* */
    u32 key;  /* ASCII（不可打印为 0） */
    u32 scancode;
    u32 flags;
};

struct aster_display_bye_body {
    u32 reason;
    u32 reserved;
};

struct aster_display_list_body {
    u32 reserved;
};

struct aster_display_list_entry {
    u32 id;
    u32 owner_pid;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 focused;
    u32 token; /* v3；v2 为 0 */
    u32 title[ASTER_DISPLAY_TITLE_WORDS];
};

struct aster_display_list_reply_body {
    u32 count;
    u32 reserved;
    struct aster_display_list_entry entries[ASTER_DISPLAY_LIST_MAX];
};

struct aster_display_register_hotkey_body {
    u32 surface_id;
    u32 key;
    u32 flags;
    u32 reserved;
};

struct aster_display_wm_status_body {
    u32 surface_id;
    u32 tasks;
    u32 focus;
    u32 switches;
    u32 lastswitch;
    u32 launches;
    u32 lastpid;
};

/*
 * v3（只读汇总）：sessiond 把会话计数交给 displayd，displayd 追加进已有的
 * /run/display.status 的 `s*` 字段（不新建 ramfs 节点）。普通应用不需要发它；
 * SDK 暴露结构/编码是为了让模拟器/测试能构造 v3 全消息。
 */
struct aster_display_session_status_body {
    u32 pid;
    u32 dpid;
    u32 procs;
    u32 apps;
    u32 windows;
    u32 tokens;
    u32 launches;
    u32 kills;
    u32 exits;
    u32 reaps;
    u32 restarts;
    u32 wmpid;
    u32 wmstate;
    u32 apppid;
    u32 tokenfail;
    u32 registry;
    u32 reserved;
};

/* v2 的 LIST_REPLY body 360 字节（entry 44）；v3 392（entry 48）。 */
#define ASTER_DISPLAY_CREATE_BODY_V1 16u
#define ASTER_DISPLAY_CREATE_BODY_V2 40u
#define ASTER_DISPLAY_LIST_ENTRY_V2 44u
#define ASTER_DISPLAY_LIST_ENTRY_V3 48u
#define ASTER_DISPLAY_LIST_REPLY_BODY_V2 360u
#define ASTER_DISPLAY_LIST_REPLY_BODY_V3 392u

_Static_assert(sizeof(struct aster_display_create_body) == 44,
               "create body v3 layout must be 44 bytes");
_Static_assert(sizeof(struct aster_display_create_reply_body) == 28,
               "create_reply body must be 28 bytes");
_Static_assert(sizeof(struct aster_display_commit_body) == 20,
               "commit body must be 20 bytes");
_Static_assert(sizeof(struct aster_display_input_body) == 20,
               "input body must be 20 bytes");
_Static_assert(sizeof(struct aster_display_list_entry) == 48,
               "list entry v3 layout must be 48 bytes");
_Static_assert(sizeof(struct aster_display_list_reply_body) == 392,
               "list_reply body v3 layout must be 392 bytes");
_Static_assert(sizeof(struct aster_display_wm_status_body) == 28,
               "wm_status body must be 28 bytes");
_Static_assert(sizeof(struct aster_display_session_status_body) == 68,
               "session_status body must be 68 bytes");

/* ---- 解码后的完整消息（与 ds_msg 同布局；大小 = 512） -------------------- */
struct aster_display_msg {
    u32 magic;
    u16 version;
    u16 type;
    u32 len; /* 含 20 字节头 */
    i32 status;
    u32 seq;
    union {
        struct aster_display_create_body create;
        struct aster_display_create_reply_body create_reply;
        struct aster_display_map_body map;
        struct aster_display_commit_body commit;
        struct aster_display_destroy_body destroy;
        struct aster_display_focus_body focus;
        struct aster_display_input_body input;
        struct aster_display_bye_body bye;
        struct aster_display_list_body list;
        struct aster_display_list_reply_body list_reply;
        struct aster_display_register_hotkey_body register_hotkey;
        struct aster_display_wm_status_body wm_status;
        struct aster_display_session_status_body session_status;
        u32 words[ASTER_DISPLAY_MSG_WORDS_MAX];
    } u;
};
_Static_assert(sizeof(struct aster_display_msg) == ASTER_DISPLAY_MSG_MAX,
               "aster_display_msg must be 512 bytes");

/* ---- 客户端句柄 / surface ------------------------------------------------ */
struct aster_display {
    u32 server_ticket; /* displayd 的端口 ticket（/run/display.port） */
    u32 port_ticket;   /* 本客户端的回复端口 ticket */
    u32 seq;
    u32 connected;
};

struct aster_surface {
    u32 id;         /* displayd 分配的 surface id（>0） */
    u32 shm_handle; /* 跨进程共享内存 handle */
    u32 pages;
    u32 width;
    u32 height;
    u32 x; /* 屏幕左上角（由 displayd 决定） */
    u32 y;
    u32 pitch_px;  /* 行跨度（像素），由 SDK 填 = width */
    u32 *pixels;   /* SYS_SHM_MAP 后的 ARGB8888 基址 */
};

struct aster_display_entry {
    u32 id;
    u32 owner_pid;
    u32 x;
    u32 y;
    u32 w;
    u32 h;
    u32 focused;
    u32 token; /* v3 回复里有效；v2 为 0 */
    char title[ASTER_DISPLAY_TITLE_MAX];
};

struct aster_display_list {
    u32 count;
    struct aster_display_entry entries[ASTER_DISPLAY_LIST_MAX];
};

struct aster_display_input {
    u32 surface_id;
    u32 kind;
    u32 key;
    u32 scancode;
    u32 flags;
};

struct aster_display_create_req {
    u32 width;
    u32 height;
    u32 flags; /* ASTER_DISPLAY_CREATE_F_POS 或 0 */
    i32 x;     /* flags & POS 时有效 */
    i32 y;
    const char *title; /* NULL = 空标题 */
    u32 owner_pid;     /* 0 = 用 aster_getpid() */
    u32 token;         /* v3：session manager 签发的 role_token（0 = 无会话） */
};

struct aster_display_wm_status_info {
    u32 surface_id;
    u32 tasks;
    u32 focus;
    u32 switches;
    u32 lastswitch;
    u32 launches;
    u32 lastpid;
};

/* ---- 线格式工具 ---------------------------------------------------------- */
static inline u32 aster_display__get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static inline void aster_display__put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

/* 某类型在指定协议版本下的 body 字节数；未知类型/版本返回 0。 */
static inline u32 aster_display__body_size(u32 type, u16 version)
{
    if (version < (u16)ASTER_DISPLAY_VERSION_MIN ||
        version > (u16)ASTER_DISPLAY_VERSION_MAX) {
        return 0;
    }
    switch (type) {
    case ASTER_DISPLAY_MSG_CREATE:
        if (version == 1u) {
            return ASTER_DISPLAY_CREATE_BODY_V1;
        }
        if (version == 2u) {
            return ASTER_DISPLAY_CREATE_BODY_V2;
        }
        return (u32)sizeof(struct aster_display_create_body);
    case ASTER_DISPLAY_MSG_CREATE_REPLY:
        return (u32)sizeof(struct aster_display_create_reply_body);
    case ASTER_DISPLAY_MSG_MAP:
        return (u32)sizeof(struct aster_display_map_body);
    case ASTER_DISPLAY_MSG_COMMIT:
        return (u32)sizeof(struct aster_display_commit_body);
    case ASTER_DISPLAY_MSG_DESTROY:
        return (u32)sizeof(struct aster_display_destroy_body);
    case ASTER_DISPLAY_MSG_FOCUS:
        return (u32)sizeof(struct aster_display_focus_body);
    case ASTER_DISPLAY_MSG_INPUT:
        return (u32)sizeof(struct aster_display_input_body);
    case ASTER_DISPLAY_MSG_BYE:
        return (u32)sizeof(struct aster_display_bye_body);
    case ASTER_DISPLAY_MSG_LIST:
        return (version == 1u) ? 0u : (u32)sizeof(struct aster_display_list_body);
    case ASTER_DISPLAY_MSG_LIST_REPLY:
        if (version == 1u) {
            return 0;
        }
        return (version == 2u) ? ASTER_DISPLAY_LIST_REPLY_BODY_V2
                               : ASTER_DISPLAY_LIST_REPLY_BODY_V3;
    case ASTER_DISPLAY_MSG_REGISTER_HOTKEY:
        return (version == 1u)
                   ? 0u
                   : (u32)sizeof(struct aster_display_register_hotkey_body);
    case ASTER_DISPLAY_MSG_WM_STATUS:
        return (version == 1u)
                   ? 0u
                   : (u32)sizeof(struct aster_display_wm_status_body);
    case ASTER_DISPLAY_MSG_SESSION_STATUS:
        /* v3 新消息：v1/v2 没有。 */
        return (version < 3u)
                   ? 0u
                   : (u32)sizeof(struct aster_display_session_status_body);
    default:
        return 0;
    }
}

/* 标题打包：16 字节，NUL padding（与 ds_title_set 相同）。 */
static inline void aster_display_title_set(u32 out[ASTER_DISPLAY_TITLE_WORDS],
                                           const char *s)
{
    u32 i;

    for (i = 0; i < ASTER_DISPLAY_TITLE_WORDS; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i + 1u < (u32)ASTER_DISPLAY_TITLE_MAX && s[i] != '\0'; i++) {
        u32 word = i / 4u;
        u32 shift = (i % 4u) * 8u;

        out[word] |= ((u32)(u8)s[i]) << shift;
    }
}

static inline void aster_display_title_get(const u32 in[ASTER_DISPLAY_TITLE_WORDS],
                                           char *out, u32 cap)
{
    u32 i;

    if (out == NULL || cap == 0 || in == NULL) {
        return;
    }
    for (i = 0; i + 1u < cap && i + 1u < (u32)ASTER_DISPLAY_TITLE_MAX; i++) {
        u32 word = i / 4u;
        u32 shift = (i % 4u) * 8u;
        char ch = (char)((in[word] >> shift) & 0xFFu);

        out[i] = ch;
        if (ch == '\0') {
            return;
        }
    }
    out[(cap < ASTER_DISPLAY_TITLE_MAX) ? (cap - 1u)
                                        : (ASTER_DISPLAY_TITLE_MAX - 1u)] = '\0';
}

/* ---- 编码 / 解码 --------------------------------------------------------- */
static inline i32 aster_display_encode(const struct aster_display_msg *m,
                                       u8 *buf, u32 cap)
{
    u8 *b;
    u32 body;
    u32 total;
    u32 i;
    u16 version = (u16)ASTER_DISPLAY_VERSION_SEND;

    if (m == NULL || buf == NULL) {
        return ASTER_EINVAL;
    }
    body = aster_display__body_size((u32)m->type, version);
    if (body == 0) {
        return ASTER_EINVAL;
    }
    total = ASTER_DISPLAY_HDR_SIZE + body;
    if (total > ASTER_DISPLAY_MSG_MAX || total > cap) {
        return ASTER_EINVAL;
    }
    aster_memzero(buf, (u64)total);
    aster_display__put32(buf + 0, ASTER_DISPLAY_MAGIC);
    buf[4] = (u8)(version & 0xFFu);
    buf[5] = (u8)((version >> 8) & 0xFFu);
    buf[6] = (u8)(m->type & 0xFFu);
    buf[7] = (u8)(((u32)m->type >> 8) & 0xFFu);
    aster_display__put32(buf + 8, total);
    aster_display__put32(buf + 12, (u32)m->status);
    aster_display__put32(buf + 16, m->seq);

    b = buf + ASTER_DISPLAY_HDR_SIZE;
    switch (m->type) {
    case ASTER_DISPLAY_MSG_CREATE:
        aster_display__put32(b + 0, m->u.create.w);
        aster_display__put32(b + 4, m->u.create.h);
        aster_display__put32(b + 8, m->u.create.flags);
        aster_display__put32(b + 12, m->u.create.x);
        aster_display__put32(b + 16, m->u.create.y);
        aster_display__put32(b + 20, m->u.create.owner_pid);
        for (i = 0; i < ASTER_DISPLAY_TITLE_WORDS; i++) {
            aster_display__put32(b + 24 + i * 4u, m->u.create.title[i]);
        }
        /* v3：body 44，末尾是 session manager 签发的 role_token（0 = 无会话）。 */
        aster_display__put32(b + 40, m->u.create.token);
        break;
    case ASTER_DISPLAY_MSG_CREATE_REPLY:
        aster_display__put32(b + 0, m->u.create_reply.surface_id);
        aster_display__put32(b + 4, m->u.create_reply.shm_handle);
        aster_display__put32(b + 8, m->u.create_reply.pages);
        aster_display__put32(b + 12, m->u.create_reply.x);
        aster_display__put32(b + 16, m->u.create_reply.y);
        aster_display__put32(b + 20, m->u.create_reply.w);
        aster_display__put32(b + 24, m->u.create_reply.h);
        break;
    case ASTER_DISPLAY_MSG_MAP:
        aster_display__put32(b + 0, m->u.map.surface_id);
        aster_display__put32(b + 4, 0);
        break;
    case ASTER_DISPLAY_MSG_COMMIT:
        aster_display__put32(b + 0, m->u.commit.surface_id);
        aster_display__put32(b + 4, m->u.commit.x);
        aster_display__put32(b + 8, m->u.commit.y);
        aster_display__put32(b + 12, m->u.commit.w);
        aster_display__put32(b + 16, m->u.commit.h);
        break;
    case ASTER_DISPLAY_MSG_DESTROY:
        aster_display__put32(b + 0, m->u.destroy.surface_id);
        aster_display__put32(b + 4, 0);
        break;
    case ASTER_DISPLAY_MSG_FOCUS:
        aster_display__put32(b + 0, m->u.focus.surface_id);
        /* v3：第二个 u32 是请求者 role_token（0 = 旧的无校验行为）。 */
        aster_display__put32(b + 4, m->u.focus.token);
        break;
    case ASTER_DISPLAY_MSG_INPUT:
        aster_display__put32(b + 0, m->u.input.surface_id);
        aster_display__put32(b + 4, m->u.input.kind);
        aster_display__put32(b + 8, m->u.input.key);
        aster_display__put32(b + 12, m->u.input.scancode);
        aster_display__put32(b + 16, m->u.input.flags);
        break;
    case ASTER_DISPLAY_MSG_BYE:
        aster_display__put32(b + 0, m->u.bye.reason);
        aster_display__put32(b + 4, 0);
        break;
    case ASTER_DISPLAY_MSG_LIST:
        aster_display__put32(b + 0, 0);
        break;
    case ASTER_DISPLAY_MSG_LIST_REPLY:
        /* 客户端一般不主动发 LIST_REPLY；保留编码能力给测试/模拟器。
         * v3 编码：entry 48 字节，token 在 +28，title 在 +32。 */
        aster_display__put32(b + 0, m->u.list_reply.count);
        aster_display__put32(b + 4, 0);
        for (i = 0; i < ASTER_DISPLAY_LIST_MAX; i++) {
            const struct aster_display_list_entry *e =
                &m->u.list_reply.entries[i];
            u32 off = 8u + i * ASTER_DISPLAY_LIST_ENTRY_V3;

            aster_display__put32(b + off + 0, e->id);
            aster_display__put32(b + off + 4, e->owner_pid);
            aster_display__put32(b + off + 8, e->x);
            aster_display__put32(b + off + 12, e->y);
            aster_display__put32(b + off + 16, e->w);
            aster_display__put32(b + off + 20, e->h);
            aster_display__put32(b + off + 24, e->focused);
            aster_display__put32(b + off + 28, e->token);
            aster_display__put32(b + off + 32, e->title[0]);
            aster_display__put32(b + off + 36, e->title[1]);
            aster_display__put32(b + off + 40, e->title[2]);
            aster_display__put32(b + off + 44, e->title[3]);
        }
        break;
    case ASTER_DISPLAY_MSG_REGISTER_HOTKEY:
        aster_display__put32(b + 0, m->u.register_hotkey.surface_id);
        aster_display__put32(b + 4, m->u.register_hotkey.key);
        aster_display__put32(b + 8, 0);
        aster_display__put32(b + 12, 0);
        break;
    case ASTER_DISPLAY_MSG_WM_STATUS:
        aster_display__put32(b + 0, m->u.wm_status.surface_id);
        aster_display__put32(b + 4, m->u.wm_status.tasks);
        aster_display__put32(b + 8, m->u.wm_status.focus);
        aster_display__put32(b + 12, m->u.wm_status.switches);
        aster_display__put32(b + 16, m->u.wm_status.lastswitch);
        aster_display__put32(b + 20, m->u.wm_status.launches);
        aster_display__put32(b + 24, m->u.wm_status.lastpid);
        break;
    case ASTER_DISPLAY_MSG_SESSION_STATUS:
        aster_display__put32(b + 0, m->u.session_status.pid);
        aster_display__put32(b + 4, m->u.session_status.dpid);
        aster_display__put32(b + 8, m->u.session_status.procs);
        aster_display__put32(b + 12, m->u.session_status.apps);
        aster_display__put32(b + 16, m->u.session_status.windows);
        aster_display__put32(b + 20, m->u.session_status.tokens);
        aster_display__put32(b + 24, m->u.session_status.launches);
        aster_display__put32(b + 28, m->u.session_status.kills);
        aster_display__put32(b + 32, m->u.session_status.exits);
        aster_display__put32(b + 36, m->u.session_status.reaps);
        aster_display__put32(b + 40, m->u.session_status.restarts);
        aster_display__put32(b + 44, m->u.session_status.wmpid);
        aster_display__put32(b + 48, m->u.session_status.wmstate);
        aster_display__put32(b + 52, m->u.session_status.apppid);
        aster_display__put32(b + 56, m->u.session_status.tokenfail);
        aster_display__put32(b + 60, m->u.session_status.registry);
        aster_display__put32(b + 64, m->u.session_status.reserved);
        break;
    default:
        return ASTER_EINVAL;
    }
    return (i32)total;
}

static inline i32 aster_display_decode(const u8 *buf, u32 len,
                                       struct aster_display_msg *out)
{
    u32 magic;
    u16 version;
    u16 type;
    u32 mlen;
    u32 body;
    u32 total;

    if (buf == NULL || out == NULL || len < ASTER_DISPLAY_HDR_SIZE) {
        return ASTER_EINVAL;
    }
    magic = aster_display__get32(buf + 0);
    version = (u16)((u32)buf[4] | ((u32)buf[5] << 8));
    type = (u16)((u32)buf[6] | ((u32)buf[7] << 8));
    mlen = aster_display__get32(buf + 8);
    if (magic != ASTER_DISPLAY_MAGIC) {
        return ASTER_EINVAL;
    }
    body = aster_display__body_size((u32)type, version);
    if (body == 0) {
        return ASTER_EINVAL;
    }
    total = ASTER_DISPLAY_HDR_SIZE + body;
    if (len != total || mlen != total || total > ASTER_DISPLAY_MSG_MAX) {
        return ASTER_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    out->magic = magic;
    out->version = version;
    out->type = type;
    out->len = total;
    out->status = (i32)aster_display__get32(buf + 12);
    out->seq = aster_display__get32(buf + 16);
    aster_memcopy(&out->u, buf + ASTER_DISPLAY_HDR_SIZE, (u64)body);
    return 0;
}

/* ---- IPC 发送 / 接收 ----------------------------------------------------- */
static inline i32 aster_display__send(const struct aster_display *d,
                                      const struct aster_display_msg *m)
{
    u8 frame[ASTER_IPC_HDR_SIZE + ASTER_DISPLAY_MSG_MAX];
    i32 n;
    i64 rc;

    if (d == NULL || m == NULL || d->connected == 0 ||
        d->server_ticket == 0) {
        return ASTER_EINVAL;
    }
    n = aster_display_encode(m, frame + ASTER_IPC_HDR_SIZE,
                             ASTER_DISPLAY_MSG_MAX);
    if (n < 0) {
        return n;
    }
    aster_ipc__put32(frame + 0, (u32)m->type);
    aster_ipc__put32(frame + 4, (u32)n);
    aster_ipc__put32(frame + 8, d->port_ticket);
    aster_ipc__put32(frame + 12, 0);
    rc = aster_port_send(d->server_ticket, frame,
                         (u32)ASTER_IPC_HDR_SIZE + (u32)n,
                         ASTER_DISPLAY_REPLY_WAIT_TICKS);
    return (rc < 0) ? (i32)rc : 0;
}

/* 返回消息类型（>0）或负 errno。 */
static inline i32 aster_display__recv(struct aster_display *d,
                                      struct aster_display_msg *m,
                                      u64 timeout)
{
    u8 frame[ASTER_IPC_HDR_SIZE + ASTER_DISPLAY_MSG_MAX];
    u32 hlen;
    i64 rc;

    if (d == NULL || m == NULL || d->connected == 0) {
        return ASTER_EINVAL;
    }
    rc = aster_port_recv(d->port_ticket, frame, (u32)sizeof(frame), timeout);
    if (rc < 0) {
        return (i32)rc;
    }
    if (rc < (i64)ASTER_IPC_HDR_SIZE || rc > (i64)sizeof(frame)) {
        return ASTER_EINVAL;
    }
    hlen = aster_ipc__get32(frame + 4);
    if ((u64)hlen + (u64)ASTER_IPC_HDR_SIZE != (u64)rc ||
        hlen > ASTER_DISPLAY_MSG_MAX) {
        return ASTER_EINVAL;
    }
    if (aster_display_decode(frame + ASTER_IPC_HDR_SIZE, hlen, m) != 0) {
        return ASTER_EINVAL;
    }
    return (i32)m->type;
}

/* 等一个指定类型的回复；期间收到其它消息（例如 INPUT）按 alpha 语义丢弃。 */
static inline i32 aster_display__wait_reply(struct aster_display *d, u32 want,
                                            struct aster_display_msg *out)
{
    u32 tries;

    for (tries = 0; tries < ASTER_DISPLAY_REPLY_WAIT_TRIES; tries++) {
        i32 type = aster_display__recv(d, out,
                                       ASTER_DISPLAY_REPLY_WAIT_TICKS);

        if (type == (i32)want) {
            return 0;
        }
        if (type == ASTER_ETIMEDOUT) {
            continue;
        }
        if (type < 0) {
            return type;
        }
    }
    return ASTER_ETIMEDOUT;
}

/* ---- 连接 / 发现 --------------------------------------------------------- */
/* 读 /run/display.port；成功返回 ticket（>0），失败返回 0。 */
static inline u32 aster_display_discover(void)
{
    char buf[24];
    i64 fd;
    i64 n;
    u32 v = 0;
    u32 i;
    u32 seen = 0;

    fd = aster_open(ASTER_DISPLAY_PORT_PATH, ASTER_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    n = aster_read((int)fd, buf, (u64)(sizeof(buf) - 1u));
    (void)aster_close((int)fd);
    if (n <= 0) {
        return 0;
    }
    for (i = 0; i < (u32)n; i++) {
        char c = buf[i];

        if (c >= '0' && c <= '9') {
            v = v * 10u + (u32)(c - '0');
            seen = 1;
        } else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            break;
        } else {
            return 0;
        }
    }
    return (seen != 0) ? v : 0;
}

static inline i32 aster_display_connect(struct aster_display *d)
{
    u32 server;
    i64 port;

    if (d == NULL) {
        return ASTER_EINVAL;
    }
    aster_memzero(d, (u64)sizeof(*d));
    server = aster_display_discover();
    if (server == 0) {
        return ASTER_ENOENT;
    }
    port = aster_port_create(ASTER_DISPLAY_PORT_DEPTH);
    if (port <= 0) {
        return (port == 0) ? ASTER_EIO : (i32)port;
    }
    d->server_ticket = server;
    d->port_ticket = (u32)port;
    d->seq = 0;
    d->connected = 1;
    return 0;
}

static inline void aster_display_close(struct aster_display *d)
{
    struct aster_display_msg m;

    if (d == NULL) {
        return;
    }
    if (d->connected != 0 && d->server_ticket != 0) {
        aster_memzero(&m, (u64)sizeof(m));
        m.type = ASTER_DISPLAY_MSG_BYE;
        m.seq = ++d->seq;
        (void)aster_display__send(d, &m);
    }
    if (d->port_ticket != 0) {
        (void)aster_port_close(d->port_ticket);
    }
    aster_memzero(d, (u64)sizeof(*d));
}

/* ---- CREATE / MAP -------------------------------------------------------- */
/*
 * 只发 CREATE 并解析 CREATE_REPLY；不映射共享内存（便于测试/多 surface）。
 * 成功后 out 的 pixels 仍为 NULL，需再调 aster_display_map()。
 */
static inline i32 aster_display_create(struct aster_display *d,
                                       const struct aster_display_create_req *req,
                                       struct aster_surface *out)
{
    struct aster_display_msg m;
    i32 rc;

    if (d == NULL || req == NULL || out == NULL || d->connected == 0) {
        return ASTER_EINVAL;
    }
    if (req->width == 0 || req->height == 0 ||
        req->width > ASTER_DISPLAY_MAX_W || req->height > ASTER_DISPLAY_MAX_H) {
        return ASTER_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_CREATE;
    m.seq = ++d->seq;
    m.u.create.w = req->width;
    m.u.create.h = req->height;
    m.u.create.flags = req->flags;
    if ((req->flags & ASTER_DISPLAY_CREATE_F_POS) != 0u) {
        m.u.create.x = (u32)req->x;
        m.u.create.y = (u32)req->y;
    }
    m.u.create.owner_pid =
        (req->owner_pid != 0u) ? req->owner_pid : aster_getpid();
    m.u.create.token = req->token;
    aster_display_title_set(m.u.create.title, req->title);

    rc = aster_display__send(d, &m);
    if (rc < 0) {
        return rc;
    }
    rc = aster_display__wait_reply(d, ASTER_DISPLAY_MSG_CREATE_REPLY, &m);
    if (rc < 0) {
        return rc;
    }
    if (m.status != 0) {
        return m.status;
    }
    if (m.u.create_reply.surface_id == 0 ||
        m.u.create_reply.shm_handle == 0 || m.u.create_reply.pages == 0 ||
        m.u.create_reply.w == 0 || m.u.create_reply.h == 0) {
        return ASTER_EIO;
    }
    out->id = m.u.create_reply.surface_id;
    out->shm_handle = m.u.create_reply.shm_handle;
    out->pages = m.u.create_reply.pages;
    out->x = m.u.create_reply.x;
    out->y = m.u.create_reply.y;
    out->width = m.u.create_reply.w;
    out->height = m.u.create_reply.h;
    out->pitch_px = out->width;
    out->pixels = NULL;
    return 0;
}

/*
 * 把 surface 的共享内存映射进本进程，再发 MAP 告诉 displayd "可以合成了"。
 * 幂等：pixels 非空时直接返回 0。
 */
static inline i32 aster_display_map(struct aster_display *d,
                                    struct aster_surface *s)
{
    struct aster_display_msg m;
    u64 va = 0;
    i64 rc;

    if (d == NULL || s == NULL || d->connected == 0 || s->id == 0) {
        return ASTER_EINVAL;
    }
    if (s->pixels != NULL) {
        return 0;
    }
    rc = aster_shm_map(s->shm_handle, &va);
    if (rc < 0) {
        return (i32)rc;
    }
    if (va == 0) {
        return ASTER_EFAULT;
    }
    s->pixels = (u32 *)(usize)va;
    s->pitch_px = s->width;

    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_MAP;
    m.seq = ++d->seq;
    m.u.map.surface_id = s->id;
    {
        i32 rc = aster_display__send(d, &m);

        if (rc < 0) {
            /* 服务端没收到 MAP：把本地像素指针清掉，允许调用方重试。 */
            s->pixels = NULL;
        }
        return rc;
    }
}

/* CREATE + MAP 的常用组合。 */
static inline i32 aster_display_create_mapped(
    struct aster_display *d, const struct aster_display_create_req *req,
    struct aster_surface *out)
{
    struct aster_display_msg m;
    i32 rc = aster_display_create(d, req, out);

    if (rc < 0) {
        return rc;
    }
    rc = aster_display_map(d, out);
    if (rc < 0) {
        /* 映射失败：尽力销毁刚创建的 surface，避免 displayd 留垃圾。 */
        if (out->id != 0) {
            aster_memzero(&m, (u64)sizeof(m));
            m.type = ASTER_DISPLAY_MSG_DESTROY;
            m.seq = ++d->seq;
            m.u.destroy.surface_id = out->id;
            (void)aster_display__send(d, &m);
        }
        aster_memzero(out, (u64)sizeof(*out));
        return rc;
    }
    return 0;
}

/* ---- COMMIT / FOCUS / LIST / DESTROY ------------------------------------- */
static inline i32 aster_display_commit(struct aster_display *d,
                                       const struct aster_surface *s, u32 x,
                                       u32 y, u32 w, u32 h)
{
    struct aster_display_msg m;

    if (d == NULL || s == NULL || s->id == 0 || w == 0 || h == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_COMMIT;
    m.seq = ++d->seq;
    m.u.commit.surface_id = s->id;
    m.u.commit.x = x;
    m.u.commit.y = y;
    m.u.commit.w = w;
    m.u.commit.h = h;
    return aster_display__send(d, &m);
}

static inline i32 aster_surface_commit_all(struct aster_display *d,
                                           const struct aster_surface *s)
{
    if (s == NULL) {
        return ASTER_EINVAL;
    }
    return aster_display_commit(d, s, 0, 0, s->width, s->height);
}

/*
 * v3 FOCUS：token 是本进程的 role_token（0 = 旧的无校验行为）。
 * displayd 只接受「surface 自己的令牌」或「WM 令牌」，其余计入 focus reject。
 */
static inline i32 aster_display_focus_ex(struct aster_display *d,
                                         u32 surface_id, u32 token)
{
    struct aster_display_msg m;

    if (d == NULL || surface_id == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_FOCUS;
    m.seq = ++d->seq;
    m.u.focus.surface_id = surface_id;
    m.u.focus.token = token;
    return aster_display__send(d, &m);
}

/* 无令牌兼容形式（等价于 focus_ex(..., 0)）。 */
static inline i32 aster_display_focus(struct aster_display *d, u32 surface_id)
{
    return aster_display_focus_ex(d, surface_id, 0);
}

/*
 * 纯函数：把已解码的 LIST_REPLY 解析成 SDK 的条目数组。按消息自带的 version
 * 选 entry stride（v2 44 / v3 48），不碰 IPC，方便 host 单测/模拟器复用。
 */
static inline i32 aster_display_parse_list_reply(
    const struct aster_display_msg *m, struct aster_display_list *out)
{
    const u8 *body;
    u32 count;
    u32 stride;
    u32 title_off;
    u32 i;

    if (m == NULL || out == NULL ||
        m->type != (u16)ASTER_DISPLAY_MSG_LIST_REPLY) {
        return ASTER_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    body = (const u8 *)&m->u;
    count = aster_display__get32(body + 0);
    if (count > ASTER_DISPLAY_LIST_MAX) {
        count = ASTER_DISPLAY_LIST_MAX;
    }
    out->count = count;
    stride = (m->version >= 3u) ? ASTER_DISPLAY_LIST_ENTRY_V3
                                : ASTER_DISPLAY_LIST_ENTRY_V2;
    title_off = (m->version >= 3u) ? 32u : 28u;
    for (i = 0; i < count; i++) {
        const u8 *e = body + 8u + i * stride;
        struct aster_display_entry *dst = &out->entries[i];
        u32 words[ASTER_DISPLAY_TITLE_WORDS];

        dst->id = aster_display__get32(e + 0);
        dst->owner_pid = aster_display__get32(e + 4);
        dst->x = aster_display__get32(e + 8);
        dst->y = aster_display__get32(e + 12);
        dst->w = aster_display__get32(e + 16);
        dst->h = aster_display__get32(e + 20);
        dst->focused = aster_display__get32(e + 24);
        dst->token = (m->version >= 3u) ? aster_display__get32(e + 28) : 0u;
        aster_memcopy(words, e + title_off, (u64)sizeof(words));
        aster_display_title_get(words, dst->title,
                                (u32)sizeof(dst->title));
    }
    return 0;
}

static inline i32 aster_display_list(struct aster_display *d,
                                     struct aster_display_list *out)
{
    struct aster_display_msg m;
    i32 rc;

    if (d == NULL || out == NULL || d->connected == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_LIST;
    m.seq = ++d->seq;
    rc = aster_display__send(d, &m);
    if (rc < 0) {
        return rc;
    }
    rc = aster_display__wait_reply(d, ASTER_DISPLAY_MSG_LIST_REPLY, &m);
    if (rc < 0) {
        return rc;
    }
    if (m.status != 0) {
        return m.status;
    }
    return aster_display_parse_list_reply(&m, out);
}

static inline i32 aster_display_destroy(struct aster_display *d,
                                        struct aster_surface *s)
{
    struct aster_display_msg m;

    if (d == NULL || s == NULL || s->id == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_DESTROY;
    m.seq = ++d->seq;
    m.u.destroy.surface_id = s->id;
    (void)aster_display__send(d, &m);
    /* 没有 SYS_SHM_FREE：映射要等进程退出才释放。 */
    s->pixels = NULL;
    s->id = 0;
    return 0;
}

/* ---- 输入 / 高级（v2 WM 接口） ------------------------------------------ */
/*
 * 收一个事件：
 *   1  = 收到 INPUT，*out 填好
 *   0  = timeout 内没有 INPUT（其它协议消息被忽略）
 *   <0 = 错误
 */
static inline i32 aster_display_poll_input(struct aster_display *d,
                                           struct aster_display_input *out,
                                           u64 timeout)
{
    struct aster_display_msg m;
    i32 type;

    if (d == NULL || out == NULL || d->connected == 0) {
        return ASTER_EINVAL;
    }
    type = aster_display__recv(d, &m, timeout);
    if (type == ASTER_ETIMEDOUT) {
        return 0;
    }
    if (type < 0) {
        return type;
    }
    if (type != (i32)ASTER_DISPLAY_MSG_INPUT) {
        return 0;
    }
    out->surface_id = m.u.input.surface_id;
    out->kind = m.u.input.kind;
    out->key = m.u.input.key;
    out->scancode = m.u.input.scancode;
    out->flags = m.u.input.flags;
    return 1;
}

/* 反复 poll，直到收到 INPUT 或用完 tries；1 收到 / 0 超时 / <0 错误。 */
static inline i32 aster_display_wait_input(struct aster_display *d,
                                           struct aster_display_input *out)
{
    u32 tries;

    for (tries = 0; tries < ASTER_DISPLAY_INPUT_WAIT_TRIES; tries++) {
        i32 rc = aster_display_poll_input(d, out,
                                          ASTER_DISPLAY_INPUT_WAIT_TICKS);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* WM 注册全局热键；displayd 会回一条同类型 status 消息。 */
static inline i32 aster_display_register_hotkey(struct aster_display *d,
                                                u32 surface_id, u32 key)
{
    struct aster_display_msg m;
    i32 rc;

    if (d == NULL || d->connected == 0 || surface_id == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_REGISTER_HOTKEY;
    m.seq = ++d->seq;
    m.u.register_hotkey.surface_id = surface_id;
    m.u.register_hotkey.key = key;
    rc = aster_display__send(d, &m);
    if (rc < 0) {
        return rc;
    }
    rc = aster_display__wait_reply(d, ASTER_DISPLAY_MSG_REGISTER_HOTKEY, &m);
    if (rc < 0) {
        return rc;
    }
    return m.status;
}

/* WM 把任务切换/启动计数汇总给 displayd（无回复）。 */
static inline i32 aster_display_wm_status(
    struct aster_display *d, const struct aster_display_wm_status_info *st)
{
    struct aster_display_msg m;

    if (d == NULL || st == NULL || d->connected == 0) {
        return ASTER_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_DISPLAY_MSG_WM_STATUS;
    m.seq = ++d->seq;
    m.u.wm_status.surface_id = st->surface_id;
    m.u.wm_status.tasks = st->tasks;
    m.u.wm_status.focus = st->focus;
    m.u.wm_status.switches = st->switches;
    m.u.wm_status.lastswitch = st->lastswitch;
    m.u.wm_status.launches = st->launches;
    m.u.wm_status.lastpid = st->lastpid;
    return aster_display__send(d, &m);
}

#endif /* ASTER_DISPLAY_H */
