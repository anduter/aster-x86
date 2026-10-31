/*
 * Aster SDK alpha —— session manager 通道（ABI 1.0 未冻结）
 *
 * 对应 ring3 session manager（/bin/sessiond，L2c）的会话协议，逐字节镜像
 * src/desktop/include/session_protocol.h：
 *
 *   init ──spawn──> sessiond ──spawn──> displayd
 *                      │      └─spawn──> wm（WM role_token）
 *   wm ──LAUNCH(token,path)──> sessiond ──spawn──> app（APP role_token）
 *   app ──REGISTER(token,port,surface,title)──> sessiond（窗口注册表）
 *   app/wm ──HEARTBEAT(token)──> sessiond（崩溃检测）
 *   app ──EXIT(token,code)──> sessiond（正常退出）
 *
 * **会话通道只经 argv 传递**：sessiond 拉起进程时给
 *
 *   /bin/<app> [window] session=<sessiond ticket> token=0x<role_token>
 *
 * 所以 SDK 应用在 main 里第一件事就是 `aster_session_connect(&s, argc, argv)`：
 *   - 解析 `session=` / `token=`（十进制与 0x 十六进制都接受）；
 *   - 建**独立的会话回复端口**（与 display 客户端的端口分开，避免 INPUT 与
 *     ENTRY/LIST_REPLY 互相干扰）；
 *   - 没有 `session=`（例如 shell 里直接 spawn）返回 -ENOENT，应用可以按
 *     "无会话" 模式继续跑（CREATE/FOCUS 的 token 传 0，displayd 走 v1/v2
 *     兼容路径）。
 *
 * 典型应用流程（见 sdk/examples/hello_gui）：
 *
 *   rc = aster_session_connect(&sess, argc, argv);   // -ENOENT = 无会话
 *   cfg.token = (rc == 0) ? sess.token : 0;
 *   aster_app_open(&app, &cfg);                      // v3 CREATE 自带 token
 *   aster_session_register(&sess, app.surface.id, "My App");
 *   while (...) {
 *       aster_app_poll(...);
 *       if ((round % 10) == 0) aster_session_heartbeat(&sess, round);
 *   }
 *   aster_session_exit(&sess, 0);
 *
 * 角色令牌（32 位）：bit0..7 seq | bit8..15 role | bit16..31 nonce；0 永远
 * 不是合法令牌。应用**不需要自己造令牌**——它信任 sessiond 经 argv 发来的
 * 那个值，并在 CREATE/FOCUS 里原样使用。`aster_session_token_make()` 主要给
 * 模拟器/host 测试用，不要拿它伪造 WM 令牌（sessiond 会按签发记录校验）。
 *
 * 错误码（所有 API 返回 0 成功 / 负 errno）：
 *   -ENOENT  没有会话通道，或 LOOKUP 没找到记录
 *   -EINVAL  argv/消息格式错误（magic/version/len/类型）
 *   -EPERM   令牌角色不够（例如 APP 发 LAUNCH/KILL，sessiond 会拒绝）
 *   -ETIMEDOUT 等 ENTRY/LIST_REPLY 超时
 *   -ECANCELED 会话端口被关闭（sessiond 退出/被 kill）
 *   -ENOMEM  端口/共享资源不足
 *   其它负值来自 port_create/port_send/port_recv 原样透传。
 *
 * 超时单位是 TSC tick（QEMU TCG 下约 1 tick ≈ 1 ns，20e6 ≈ 20ms）。
 */
#ifndef ASTER_SESSION_H
#define ASTER_SESSION_H

#include <aster/types.h>
#include <aster/syscall.h>
#include <aster/ipc.h>

/* ---- 协议版本 / 尺寸 ------------------------------------------------------ */
#define ASTER_SESSION_MAGIC 0x534E314Du /* "SN1M" */
#define ASTER_SESSION_VERSION 1u
#define ASTER_SESSION_HDR_SIZE 20u
#define ASTER_SESSION_MSG_MAX 512u
#define ASTER_SESSION_MSG_WORDS_MAX 123u /* (512 - 20) / 4 */
#define ASTER_SESSION_PORT_DEPTH 16u
#define ASTER_SESSION_LIST_MAX 8u
#define ASTER_SESSION_PATH_MAX 64u
#define ASTER_SESSION_ARG_MAX 16u
#define ASTER_SESSION_TITLE_MAX 16u /* 15 字符 + NUL */
#define ASTER_SESSION_TITLE_WORDS 4u

/* ---- 角色 / 状态 ---------------------------------------------------------- */
#define ASTER_SESSION_ROLE_NONE 0u
#define ASTER_SESSION_ROLE_WM 1u      /* 可以 LAUNCH/KILL/切焦点 */
#define ASTER_SESSION_ROLE_APP 2u     /* L3 应用：只上报自己的窗口 */
#define ASTER_SESSION_ROLE_SERVICE 3u /* 服务（保留） */
#define ASTER_SESSION_ROLE_SESSION 4u /* session manager 自己（保留） */

#define ASTER_SESSION_STATE_FREE 0u
#define ASTER_SESSION_STATE_RUNNING 1u
#define ASTER_SESSION_STATE_EXITED 2u
#define ASTER_SESSION_STATE_DEAD 3u

#define ASTER_SESSION_TOKEN_ROLE(t) (((t) >> 8) & 0xFFu)
#define ASTER_SESSION_TOKEN_SEQ(t) ((t) & 0xFFu)
#define ASTER_SESSION_TOKEN_NONCE(t) ((t) >> 16)

/* ---- 消息类型 ------------------------------------------------------------- */
#define ASTER_SESSION_MSG_REGISTER 1u
#define ASTER_SESSION_MSG_HEARTBEAT 2u
#define ASTER_SESSION_MSG_LAUNCH 3u
#define ASTER_SESSION_MSG_EXIT 4u
#define ASTER_SESSION_MSG_KILL 5u
#define ASTER_SESSION_MSG_LOOKUP 6u
#define ASTER_SESSION_MSG_ENTRY 7u
#define ASTER_SESSION_MSG_LIST 8u
#define ASTER_SESSION_MSG_LIST_REPLY 9u
#define ASTER_SESSION_MSG_SESSION_STATUS 10u

/* ---- 错误码（负值；名字镜像 ASTER_E*，便于错误日志里区分来源） ---------- */
#define ASTER_SESSION_OK 0
#define ASTER_SESSION_ENOENT ASTER_ENOENT
#define ASTER_SESSION_EINVAL ASTER_EINVAL
#define ASTER_SESSION_EPERM ASTER_EPERM
#define ASTER_SESSION_ETIMEDOUT ASTER_ETIMEDOUT
#define ASTER_SESSION_ECANCELED ASTER_ECANCELED
#define ASTER_SESSION_ENOMEM ASTER_ENOMEM
#define ASTER_SESSION_EIO ASTER_EIO

/* ---- 时序（TSC tick） ----------------------------------------------------- */
#define ASTER_SESSION_REPLY_WAIT_TICKS 400000000ULL
#define ASTER_SESSION_REPLY_WAIT_TRIES 20u
#define ASTER_SESSION_HEARTBEAT_ROUNDS 10u

/* ---- body（全部 u32；长度与 session_protocol.h 一致） -------------------- */
struct aster_session_register_body {
    u32 token;
    u32 port;       /* 客户端自己的回复端口 ticket */
    u32 surface_id; /* 0 = 没有窗口 */
    u32 title[ASTER_SESSION_TITLE_WORDS];
};

struct aster_session_heartbeat_body {
    u32 token;
    u32 rounds;
};

struct aster_session_launch_body {
    u32 token;
    u32 path[ASTER_SESSION_PATH_MAX / 4u];
    u32 arg[ASTER_SESSION_ARG_MAX / 4u];
};

struct aster_session_exit_body {
    u32 token;
    u32 code;
};

struct aster_session_kill_body {
    u32 token;
    u32 pid;
    u32 surface_id;
};

struct aster_session_lookup_body {
    u32 surface_id;
    u32 pid;
};

/* 一条注册表记录（40 字节）。 */
struct aster_session_entry_info {
    u32 pid;
    u32 token;
    u32 surface_id;
    u32 state; /* ASTER_SESSION_STATE_* */
    u32 role;  /* ASTER_SESSION_ROLE_* */
    u32 reserved;
    u32 title[ASTER_SESSION_TITLE_WORDS];
};

struct aster_session_entry_body {
    u32 found;
    u32 reserved;
    struct aster_session_entry_info info;
};

struct aster_session_list_body {
    u32 reserved;
};

struct aster_session_status_body {
    u32 reserved;
};

struct aster_session_list_reply_body {
    u32 count;
    u32 procs;
    u32 apps;
    u32 windows;
    struct aster_session_entry_info entries[ASTER_SESSION_LIST_MAX];
};

/* ---- 解码后的完整消息（与 session_msg 同布局；大小 = 512） ---------------- */
struct aster_session_msg {
    u32 magic;
    u16 version;
    u16 type;
    u32 len;
    i32 status;
    u32 seq;
    union {
        struct aster_session_register_body reg;
        struct aster_session_heartbeat_body hb;
        struct aster_session_launch_body launch;
        struct aster_session_exit_body exit;
        struct aster_session_kill_body kill;
        struct aster_session_lookup_body lookup;
        struct aster_session_entry_body entry;
        struct aster_session_list_body list;
        struct aster_session_list_reply_body list_reply;
        struct aster_session_status_body status;
        u32 words[ASTER_SESSION_MSG_WORDS_MAX];
    } u;
};

_Static_assert(sizeof(struct aster_session_register_body) == 28,
               "session register body must be 28 bytes");
_Static_assert(sizeof(struct aster_session_heartbeat_body) == 8,
               "session heartbeat body must be 8 bytes");
_Static_assert(sizeof(struct aster_session_launch_body) == 84,
               "session launch body must be 84 bytes");
_Static_assert(sizeof(struct aster_session_exit_body) == 8,
               "session exit body must be 8 bytes");
_Static_assert(sizeof(struct aster_session_kill_body) == 12,
               "session kill body must be 12 bytes");
_Static_assert(sizeof(struct aster_session_lookup_body) == 8,
               "session lookup body must be 8 bytes");
_Static_assert(sizeof(struct aster_session_entry_info) == 40,
               "session entry info must be 40 bytes");
_Static_assert(sizeof(struct aster_session_entry_body) == 48,
               "session entry body must be 48 bytes");
_Static_assert(sizeof(struct aster_session_list_body) == 4,
               "session list body must be 4 bytes");
_Static_assert(sizeof(struct aster_session_status_body) == 4,
               "session status body must be 4 bytes");
_Static_assert(sizeof(struct aster_session_list_reply_body) == 336,
               "session list reply body must be 336 bytes");
_Static_assert(sizeof(struct aster_session_msg) == ASTER_SESSION_MSG_MAX,
               "aster_session_msg must be 512 bytes");

/* ---- 线格式工具 ---------------------------------------------------------- */
static inline u32 aster_session__get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static inline void aster_session__put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

/* 某类型消息的 body 字节数；未知类型返回 0。 */
static inline u32 aster_session__body_size(u32 type)
{
    switch (type) {
    case ASTER_SESSION_MSG_REGISTER:
        return (u32)sizeof(struct aster_session_register_body);
    case ASTER_SESSION_MSG_HEARTBEAT:
        return (u32)sizeof(struct aster_session_heartbeat_body);
    case ASTER_SESSION_MSG_LAUNCH:
        return (u32)sizeof(struct aster_session_launch_body);
    case ASTER_SESSION_MSG_EXIT:
        return (u32)sizeof(struct aster_session_exit_body);
    case ASTER_SESSION_MSG_KILL:
        return (u32)sizeof(struct aster_session_kill_body);
    case ASTER_SESSION_MSG_LOOKUP:
        return (u32)sizeof(struct aster_session_lookup_body);
    case ASTER_SESSION_MSG_ENTRY:
        return (u32)sizeof(struct aster_session_entry_body);
    case ASTER_SESSION_MSG_LIST:
        return (u32)sizeof(struct aster_session_list_body);
    case ASTER_SESSION_MSG_LIST_REPLY:
        return (u32)sizeof(struct aster_session_list_reply_body);
    case ASTER_SESSION_MSG_SESSION_STATUS:
        return (u32)sizeof(struct aster_session_status_body);
    default:
        return 0;
    }
}

/* 整条消息（20 字节头 + body）；未知类型返回 0。 */
static inline u32 aster_session_msg_size(u32 type)
{
    u32 body = aster_session__body_size(type);

    return (body == 0u) ? 0u : (ASTER_SESSION_HDR_SIZE + body);
}

/* ---- 标题 / 路径 / 参数打包（与 session_protocol.c 相同） ---------------- */
static inline void aster_session_title_set(
    u32 out[ASTER_SESSION_TITLE_WORDS], const char *s)
{
    u32 i;

    for (i = 0; i < ASTER_SESSION_TITLE_WORDS; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i + 1u < ASTER_SESSION_TITLE_MAX && s[i] != '\0'; i++) {
        out[i / 4u] |= ((u32)(u8)s[i]) << (8u * (i % 4u));
    }
}

static inline void aster_session_title_get(
    const u32 in[ASTER_SESSION_TITLE_WORDS], char *out, u32 cap)
{
    u32 i;

    if (out == NULL || cap == 0 || in == NULL) {
        return;
    }
    out[0] = '\0';
    for (i = 0; i + 1u < cap && i + 1u < ASTER_SESSION_TITLE_MAX; i++) {
        char ch = (char)((in[i / 4u] >> (8u * (i % 4u))) & 0xFFu);

        out[i] = ch;
        if (ch == '\0') {
            return;
        }
    }
    out[(cap < ASTER_SESSION_TITLE_MAX) ? (cap - 1u)
                                        : (ASTER_SESSION_TITLE_MAX - 1u)] = '\0';
}

static inline void aster_session_path_set(u32 out[ASTER_SESSION_PATH_MAX / 4u],
                                          const char *s)
{
    u32 i;

    for (i = 0; i < ASTER_SESSION_PATH_MAX / 4u; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i + 1u < ASTER_SESSION_PATH_MAX && s[i] != '\0'; i++) {
        out[i / 4u] |= ((u32)(u8)s[i]) << (8u * (i % 4u));
    }
}

static inline void aster_session_path_get(
    const u32 in[ASTER_SESSION_PATH_MAX / 4u], char *out, u32 cap)
{
    u32 max;
    u32 i;

    if (out == NULL || cap == 0 || in == NULL) {
        return;
    }
    out[0] = '\0';
    max = (cap - 1u < ASTER_SESSION_PATH_MAX) ? (cap - 1u)
                                              : ASTER_SESSION_PATH_MAX;
    for (i = 0; i < max; i++) {
        char ch = (char)((in[i / 4u] >> (8u * (i % 4u))) & 0xFFu);

        out[i] = ch;
        if (ch == '\0') {
            break;
        }
    }
    out[(i < max) ? i : max] = '\0';
}

static inline void aster_session_arg_set(u32 out[ASTER_SESSION_ARG_MAX / 4u],
                                         const char *s)
{
    u32 i;

    for (i = 0; i < ASTER_SESSION_ARG_MAX / 4u; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i + 1u < ASTER_SESSION_ARG_MAX && s[i] != '\0'; i++) {
        out[i / 4u] |= ((u32)(u8)s[i]) << (8u * (i % 4u));
    }
}

static inline void aster_session_arg_get(
    const u32 in[ASTER_SESSION_ARG_MAX / 4u], char *out, u32 cap)
{
    u32 max;
    u32 i;

    if (out == NULL || cap == 0 || in == NULL) {
        return;
    }
    out[0] = '\0';
    max = (cap - 1u < ASTER_SESSION_ARG_MAX) ? (cap - 1u) : ASTER_SESSION_ARG_MAX;
    for (i = 0; i < max; i++) {
        char ch = (char)((in[i / 4u] >> (8u * (i % 4u))) & 0xFFu);

        out[i] = ch;
        if (ch == '\0') {
            break;
        }
    }
    out[(i < max) ? i : max] = '\0';
}

/* ---- 角色令牌 ------------------------------------------------------------ */
static inline u32 aster_session_token_make(u32 role, u32 seq, u32 nonce)
{
    u32 r = role & 0xFFu;
    u32 q = seq & 0xFFu;
    u32 n = nonce & 0xFFFFu;

    /* 归一化：0 永远不是合法令牌（角色默认 APP、seq/nonce 至少 1）。 */
    if (r == ASTER_SESSION_ROLE_NONE) {
        r = ASTER_SESSION_ROLE_APP;
    }
    if (q == 0u) {
        q = 1u;
    }
    if (n == 0u) {
        n = 1u;
    }
    return (n << 16) | (r << 8) | q;
}

static inline i32 aster_session_token_valid(u32 token)
{
    u32 role = ASTER_SESSION_TOKEN_ROLE(token);

    if (token == 0u || ASTER_SESSION_TOKEN_NONCE(token) == 0u ||
        ASTER_SESSION_TOKEN_SEQ(token) == 0u) {
        return 0;
    }
    return (role >= ASTER_SESSION_ROLE_WM && role <= ASTER_SESSION_ROLE_SESSION)
               ? 1
               : 0;
}

static inline i32 aster_session_token_is(u32 token, u32 role)
{
    return (token != 0u && ASTER_SESSION_TOKEN_ROLE(token) == role) ? 1 : 0;
}

/* ---- 编码 / 解码（version 1，严格长度） --------------------------------- */
static inline i32 aster_session_encode(const struct aster_session_msg *m,
                                       u8 *buf, u32 cap)
{
    u8 *b;
    u32 body;
    u32 total;

    if (m == NULL || buf == NULL) {
        return ASTER_SESSION_EINVAL;
    }
    body = aster_session__body_size((u32)m->type);
    if (body == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    total = ASTER_SESSION_HDR_SIZE + body;
    if (total > ASTER_SESSION_MSG_MAX || total > cap) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(buf, (u64)total);
    aster_session__put32(buf + 0, ASTER_SESSION_MAGIC);
    buf[4] = (u8)(ASTER_SESSION_VERSION & 0xFFu);
    buf[5] = (u8)((ASTER_SESSION_VERSION >> 8) & 0xFFu);
    buf[6] = (u8)(m->type & 0xFFu);
    buf[7] = (u8)(((u32)m->type >> 8) & 0xFFu);
    aster_session__put32(buf + 8, total);
    aster_session__put32(buf + 12, (u32)m->status);
    aster_session__put32(buf + 16, m->seq);

    b = buf + ASTER_SESSION_HDR_SIZE;
    aster_memcopy(b, &m->u, (u64)body);
    return (i32)total;
}

static inline i32 aster_session_decode(const u8 *buf, u32 len,
                                       struct aster_session_msg *out)
{
    u32 magic;
    u16 version;
    u16 type;
    u32 mlen;
    u32 body;
    u32 total;

    if (buf == NULL || out == NULL || len < ASTER_SESSION_HDR_SIZE) {
        return ASTER_SESSION_EINVAL;
    }
    magic = aster_session__get32(buf + 0);
    version = (u16)((u32)buf[4] | ((u32)buf[5] << 8));
    type = (u16)((u32)buf[6] | ((u32)buf[7] << 8));
    mlen = aster_session__get32(buf + 8);
    if (magic != ASTER_SESSION_MAGIC ||
        version != (u16)ASTER_SESSION_VERSION) {
        return ASTER_SESSION_EINVAL;
    }
    body = aster_session__body_size((u32)type);
    if (body == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    total = ASTER_SESSION_HDR_SIZE + body;
    if (len != total || mlen != total) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    aster_memcopy(out, buf, (u64)total);
    return 0;
}

/* ---- argv 解析 ----------------------------------------------------------- */
/*
 * 解析 "key<number>" 形式的单个 argv 项（key 例如 "session=" / "token="）。
 * 支持十进制与 0x/0X 十六进制；成功返回 1 并写 *out，失败返回 0。
 * 与内核 session_arg_parse_u32() 同一套路，但十进制/十六进制溢出都严格
 * 拒绝（内核版本对 4294967296 会回绕成 0；SDK 不接受这种输入）。
 */
static inline i32 aster_session_parse_u32(const char *s, const char *key,
                                          u32 *out)
{
    u32 i = 0;
    u32 v = 0;
    u32 digits = 0;
    i32 hex = 0;

    if (s == NULL || key == NULL || out == NULL) {
        return 0;
    }
    while (key[i] != '\0') {
        if (s[i] != key[i]) {
            return 0;
        }
        i++;
    }
    if (s[i] == '0' && (s[i + 1u] == 'x' || s[i + 1u] == 'X')) {
        hex = 1;
        i += 2u;
    }
    while (s[i] != '\0') {
        u32 c = (u32)(u8)s[i];
        u32 d;

        if (hex != 0) {
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10u;
            } else {
                return 0;
            }
            if (v > 0x0FFFFFFFu) {
                return 0; /* 超过 8 位 / 32 位溢出 */
            }
            v = (v << 4) | d;
        } else {
            if (c < '0' || c > '9') {
                return 0;
            }
            d = c - '0';
            if (v > (0xFFFFFFFFu - d) / 10u) {
                return 0; /* 十进制溢出 */
            }
            v = v * 10u + d;
        }
        digits++;
        i++;
    }
    if (digits == 0u) {
        return 0;
    }
    *out = v;
    return 1;
}

/* ---- 客户端句柄 ---------------------------------------------------------- */
struct aster_session {
    u32 server_ticket; /* sessiond 的端口 ticket（来自 argv `session=`） */
    u32 port_ticket;   /* 本进程的会话回复端口（REGISTER 把它告诉 sessiond） */
    u32 token;         /* sessiond 签发的 role_token（argv `token=`） */
    u32 seq;
    u32 connected;
};

/* 用户友好的注册表条目/快照（协议 struct 的解析结果）。 */
struct aster_session_entry {
    u32 pid;
    u32 token;
    u32 surface_id;
    u32 state;
    u32 role;
    char title[ASTER_SESSION_TITLE_MAX];
};

struct aster_session_list {
    u32 count;
    u32 procs;
    u32 apps;
    u32 windows;
    struct aster_session_entry entries[ASTER_SESSION_LIST_MAX];
};

/*
 * 只解析 argv（不建端口）：成功返回 1（找到 session=），没找到返回 0，
 * session=/token= 格式错误返回 -EINVAL（保持 *s 里已解析出的值）。
 * 主要给 host 单测/需要在建端口前做决策的调用方。
 */
static inline i32 aster_session_parse_args(struct aster_session *s, long argc,
                                           char **argv)
{
    long i;
    u32 found = 0;

    if (s == NULL) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(s, (u64)sizeof(*s));
    for (i = 1; i < argc && argv != NULL; i++) {
        u32 v = 0;

        if (argv[i] == NULL) {
            continue;
        }
        if (aster_session_parse_u32(argv[i], "session=", &v) != 0) {
            s->server_ticket = v;
            found = 1;
        } else if (aster_session_parse_u32(argv[i], "token=", &v) != 0) {
            s->token = v;
        } else if (aster_streq(argv[i], "session=") ||
                   aster_streq(argv[i], "token=")) {
            return ASTER_SESSION_EINVAL; /* 有 key 没有数字 */
        }
    }
    return (found != 0u) ? 1 : 0;
}

/*
 * 会话连接入口（应用 main 的第一件事）：
 *   1. 解析 argv 里的 `session=<ticket>` / `token=<hex|dec>`；
 *   2. 没有 session= -> 返回 -ENOENT（*s 清零；无会话模式）；
 *   3. 有 -> 建独立回复端口，成功返回 0。
 * 失败返回负 errno（-EINVAL 参数、-ENOMEM 端口、其它透传）。
 */
static inline i32 aster_session_connect(struct aster_session *s, long argc,
                                        char **argv)
{
    i32 rc;
    i64 port;

    if (s == NULL) {
        return ASTER_SESSION_EINVAL;
    }
    rc = aster_session_parse_args(s, argc, argv);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0 || s->server_ticket == 0u) {
        aster_memzero(s, (u64)sizeof(*s));
        return ASTER_SESSION_ENOENT;
    }
    port = aster_port_create(ASTER_SESSION_PORT_DEPTH);
    if (port <= 0) {
        aster_memzero(s, (u64)sizeof(*s));
        return (port == 0) ? ASTER_SESSION_EIO : (i32)port;
    }
    s->port_ticket = (u32)port;
    s->seq = 0;
    s->connected = 1;
    return 0;
}

static inline void aster_session_close(struct aster_session *s)
{
    if (s == NULL) {
        return;
    }
    if (s->port_ticket != 0u) {
        (void)aster_port_close(s->port_ticket);
    }
    aster_memzero(s, (u64)sizeof(*s));
}

/* ---- IPC 发送 / 接收 ----------------------------------------------------- */
static inline i32 aster_session__send_to(struct aster_session *s,
                                         const struct aster_session_msg *m,
                                         u64 timeout)
{
    u8 frame[ASTER_IPC_HDR_SIZE + ASTER_SESSION_MSG_MAX];
    i32 n;
    i64 rc;

    if (s == NULL || m == NULL || s->connected == 0u ||
        s->server_ticket == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    n = aster_session_encode(m, frame + ASTER_IPC_HDR_SIZE,
                             ASTER_SESSION_MSG_MAX);
    if (n < 0) {
        return n;
    }
    aster_ipc__put32(frame + 0, (u32)m->type);
    aster_ipc__put32(frame + 4, (u32)n);
    aster_ipc__put32(frame + 8, s->port_ticket);
    aster_ipc__put32(frame + 12, 0);
    rc = aster_port_send(s->server_ticket, frame,
                         (u32)ASTER_IPC_HDR_SIZE + (u32)n, timeout);
    return (rc < 0) ? (i32)rc : 0;
}

/* 内部一次投递（非阻塞；sessiond 的队列深度只有 16）。 */
static inline i32 aster_session__send(struct aster_session *s,
                                      const struct aster_session_msg *m)
{
    return aster_session__send_to(s, m, ASTER_TIMEOUT_NOW);
}

/* 收一条会话消息；返回消息类型（>0）或负 errno。 */
static inline i32 aster_session__recv(struct aster_session *s,
                                      struct aster_session_msg *m,
                                      u64 timeout)
{
    u8 frame[ASTER_IPC_HDR_SIZE + ASTER_SESSION_MSG_MAX];
    u32 hlen;
    i64 rc;

    if (s == NULL || m == NULL || s->connected == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    rc = aster_port_recv(s->port_ticket, frame, (u32)sizeof(frame), timeout);
    if (rc < 0) {
        return (i32)rc;
    }
    if (rc < (i64)ASTER_IPC_HDR_SIZE || rc > (i64)sizeof(frame)) {
        return ASTER_SESSION_EINVAL;
    }
    hlen = aster_ipc__get32(frame + 4);
    if ((u64)hlen + (u64)ASTER_IPC_HDR_SIZE != (u64)rc) {
        return ASTER_SESSION_EINVAL;
    }
    if (aster_session_decode(frame + ASTER_IPC_HDR_SIZE, hlen, m) != 0) {
        return ASTER_SESSION_EINVAL;
    }
    return (i32)m->type;
}

/* 等指定类型的回复；超时重试，其它消息丢弃（alpha 语义）。 */
static inline i32 aster_session__wait_reply(struct aster_session *s, u32 want,
                                            struct aster_session_msg *out)
{
    u32 tries;

    for (tries = 0; tries < ASTER_SESSION_REPLY_WAIT_TRIES; tries++) {
        i32 type = aster_session__recv(s, out,
                                       ASTER_SESSION_REPLY_WAIT_TICKS);

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
    return ASTER_SESSION_ETIMEDOUT;
}

/* ---- 客户端动作 ---------------------------------------------------------- */
/* 上报窗口（surface_id=0 表示无窗口）。 */
static inline i32 aster_session_register(struct aster_session *s,
                                         u32 surface_id, const char *title)
{
    struct aster_session_msg m;

    if (s == NULL || s->connected == 0u) {
        return ASTER_SESSION_ENOENT;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_REGISTER;
    m.seq = ++s->seq;
    m.u.reg.token = s->token;
    m.u.reg.port = s->port_ticket;
    m.u.reg.surface_id = surface_id;
    aster_session_title_set(m.u.reg.title, title);
    return aster_session__send(s, &m);
}

static inline i32 aster_session_heartbeat(struct aster_session *s, u32 rounds)
{
    struct aster_session_msg m;

    if (s == NULL || s->connected == 0u) {
        return ASTER_SESSION_ENOENT;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_HEARTBEAT;
    m.seq = ++s->seq;
    m.u.hb.token = s->token;
    m.u.hb.rounds = rounds;
    return aster_session__send(s, &m);
}

static inline i32 aster_session_exit(struct aster_session *s, u32 code)
{
    struct aster_session_msg m;

    if (s == NULL || s->connected == 0u) {
        return ASTER_SESSION_ENOENT;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_EXIT;
    m.seq = ++s->seq;
    m.u.exit.token = s->token;
    m.u.exit.code = code;
    return aster_session__send(s, &m);
}

/* WM 专用：请求 sessiond 启动 path（arg 可为 NULL）。无回复。 */
static inline i32 aster_session_launch(struct aster_session *s,
                                       const char *path, const char *arg)
{
    struct aster_session_msg m;

    if (s == NULL || s->connected == 0u || path == NULL) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LAUNCH;
    m.seq = ++s->seq;
    m.u.launch.token = s->token;
    aster_session_path_set(m.u.launch.path, path);
    aster_session_arg_set(m.u.launch.arg, arg);
    return aster_session__send(s, &m);
}

/* WM 专用：会话内终止 pid/surface_id（0 = 忽略该字段）。无回复。 */
static inline i32 aster_session_kill(struct aster_session *s, u32 pid,
                                     u32 surface_id)
{
    struct aster_session_msg m;

    if (s == NULL || s->connected == 0u) {
        return ASTER_SESSION_ENOENT;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_KILL;
    m.seq = ++s->seq;
    m.u.kill.token = s->token;
    m.u.kill.pid = pid;
    m.u.kill.surface_id = surface_id;
    return aster_session__send(s, &m);
}

/* 纯函数：把 ENTRY 回复解析成 SDK 条目；found=0 返回 -ENOENT。 */
static inline i32 aster_session_parse_entry(
    const struct aster_session_msg *m, struct aster_session_entry *out)
{
    if (m == NULL || out == NULL ||
        m->type != (u16)ASTER_SESSION_MSG_ENTRY) {
        return ASTER_SESSION_EINVAL;
    }
    if (m->status != 0) {
        return m->status;
    }
    if (m->u.entry.found == 0u) {
        return ASTER_SESSION_ENOENT;
    }
    aster_memzero(out, (u64)sizeof(*out));
    out->pid = m->u.entry.info.pid;
    out->token = m->u.entry.info.token;
    out->surface_id = m->u.entry.info.surface_id;
    out->state = m->u.entry.info.state;
    out->role = m->u.entry.info.role;
    aster_session_title_get(m->u.entry.info.title, out->title,
                            (u32)sizeof(out->title));
    return 0;
}

/* 纯函数：把 LIST_REPLY 解析成快照。 */
static inline i32 aster_session_parse_list_reply(
    const struct aster_session_msg *m, struct aster_session_list *out)
{
    u32 count;
    u32 i;

    if (m == NULL || out == NULL ||
        m->type != (u16)ASTER_SESSION_MSG_LIST_REPLY) {
        return ASTER_SESSION_EINVAL;
    }
    if (m->status != 0) {
        return m->status;
    }
    aster_memzero(out, (u64)sizeof(*out));
    count = m->u.list_reply.count;
    if (count > ASTER_SESSION_LIST_MAX) {
        count = ASTER_SESSION_LIST_MAX;
    }
    out->count = count;
    out->procs = m->u.list_reply.procs;
    out->apps = m->u.list_reply.apps;
    out->windows = m->u.list_reply.windows;
    for (i = 0; i < count; i++) {
        const struct aster_session_entry_info *e =
            &m->u.list_reply.entries[i];

        out->entries[i].pid = e->pid;
        out->entries[i].token = e->token;
        out->entries[i].surface_id = e->surface_id;
        out->entries[i].state = e->state;
        out->entries[i].role = e->role;
        aster_session_title_get(e->title, out->entries[i].title,
                                (u32)sizeof(out->entries[i].title));
    }
    return 0;
}

/* LOOKUP：按 surface_id / pid 查注册表（0 = 忽略该条件）。 */
static inline i32 aster_session_lookup(struct aster_session *s, u32 surface_id,
                                       u32 pid,
                                       struct aster_session_entry *out)
{
    struct aster_session_msg m;
    i32 rc;

    if (s == NULL || out == NULL || s->connected == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LOOKUP;
    m.seq = ++s->seq;
    m.u.lookup.surface_id = surface_id;
    m.u.lookup.pid = pid;
    rc = aster_session__send(s, &m);
    if (rc < 0) {
        return rc;
    }
    rc = aster_session__wait_reply(s, ASTER_SESSION_MSG_ENTRY, &m);
    if (rc < 0) {
        return rc;
    }
    return aster_session_parse_entry(&m, out);
}

/* LIST：要一份注册表快照。 */
static inline i32 aster_session_list(struct aster_session *s,
                                     struct aster_session_list *out)
{
    struct aster_session_msg m;
    i32 rc;

    if (s == NULL || out == NULL || s->connected == 0u) {
        return ASTER_SESSION_EINVAL;
    }
    aster_memzero(out, (u64)sizeof(*out));
    aster_memzero(&m, (u64)sizeof(m));
    m.type = ASTER_SESSION_MSG_LIST;
    m.seq = ++s->seq;
    rc = aster_session__send(s, &m);
    if (rc < 0) {
        return rc;
    }
    rc = aster_session__wait_reply(s, ASTER_SESSION_MSG_LIST_REPLY, &m);
    if (rc < 0) {
        return rc;
    }
    return aster_session_parse_list_reply(&m, out);
}

#endif /* ASTER_SESSION_H */
