/*
 * ng WP3-3: session manager 协议编解码（纯函数，host 单测直接驱动）。
 *
 * 与 ds_protocol.c 同一套路：只做字节搬运与格式校验，不依赖 libc / 内核 /
 * 串口。sessiond（ring3）、wm/demo（客户端）与 host 单测共用这一份代码。
 */
#include "session_protocol.h"

_Static_assert(sizeof(struct session_register_body) == 28,
               "register body must be 28");
_Static_assert(sizeof(struct session_heartbeat_body) == 8,
               "heartbeat body must be 8");
_Static_assert(sizeof(struct session_launch_body) == 84,
               "launch body must be 84");
_Static_assert(sizeof(struct session_exit_body) == 8, "exit body must be 8");
_Static_assert(sizeof(struct session_kill_body) == 12, "kill body must be 12");
_Static_assert(sizeof(struct session_lookup_body) == 8,
               "lookup body must be 8");
_Static_assert(sizeof(struct session_entry_body) == 48,
               "entry body must be 48");
_Static_assert(sizeof(struct session_entry_info) == 40,
               "entry info must be 40");
_Static_assert(sizeof(struct session_list_reply_body) == 336,
               "list reply body must be 336");
_Static_assert(sizeof(struct session_list_body) == 4, "list body must be 4");
_Static_assert(sizeof(struct session_status_body) == 4,
               "status body must be 4");
_Static_assert(sizeof(struct session_msg) == SESSION_MSG_MAX,
               "session_msg must be header + body window");
_Static_assert(SESSION_HDR_SIZE + 336u <= SESSION_MSG_MAX,
               "LIST_REPLY must fit in SESSION_MSG_MAX");

/* ------------------------------------------------------------------------- */
/* 尺寸表                                                                     */
/* ------------------------------------------------------------------------- */

u32 session_msg_body_size(u32 type)
{
    switch (type) {
    case SESSION_MSG_REGISTER:
        return (u32)sizeof(struct session_register_body);
    case SESSION_MSG_HEARTBEAT:
        return (u32)sizeof(struct session_heartbeat_body);
    case SESSION_MSG_LAUNCH:
        return (u32)sizeof(struct session_launch_body);
    case SESSION_MSG_EXIT:
        return (u32)sizeof(struct session_exit_body);
    case SESSION_MSG_KILL:
        return (u32)sizeof(struct session_kill_body);
    case SESSION_MSG_LOOKUP:
        return (u32)sizeof(struct session_lookup_body);
    case SESSION_MSG_ENTRY:
        return (u32)sizeof(struct session_entry_body);
    case SESSION_MSG_LIST:
        return (u32)sizeof(struct session_list_body);
    case SESSION_MSG_LIST_REPLY:
        return (u32)sizeof(struct session_list_reply_body);
    case SESSION_MSG_SESSION_STATUS:
        return (u32)sizeof(struct session_status_body);
    default:
        return 0;
    }
}

u32 session_msg_size(u32 type)
{
    u32 body = session_msg_body_size(type);

    return (body == 0) ? 0 : (SESSION_HDR_SIZE + body);
}

/* ------------------------------------------------------------------------- */
/* 编码 / 解码                                                                */
/* ------------------------------------------------------------------------- */

int session_msg_encode(const struct session_msg *m, void *buf, u32 cap)
{
    u8 *out = (u8 *)buf;
    u32 body;
    u32 total;
    u32 magic = SESSION_MAGIC;
    u16 version = (u16)SESSION_VERSION;
    u16 type;
    i32 status;
    u32 seq;

    if (m == NULL || buf == NULL) {
        return DS_EINVAL;
    }
    body = session_msg_body_size(m->type);
    if (body == 0) {
        return DS_EINVAL;
    }
    total = SESSION_HDR_SIZE + body;
    if (total > SESSION_MSG_MAX || total > cap) {
        return DS_EINVAL;
    }
    type = m->type;
    status = m->status;
    seq = m->seq;
    ds_memzero(out, (u64)total);
    ds_memcopy(out + 0, &magic, 4);
    ds_memcopy(out + 4, &version, 2);
    ds_memcopy(out + 6, &type, 2);
    ds_memcopy(out + 8, &total, 4);
    ds_memcopy(out + 12, &status, 4);
    ds_memcopy(out + 16, &seq, 4);
    ds_memcopy(out + SESSION_HDR_SIZE, &m->u, (u64)body);
    return (int)total;
}

int session_msg_decode(const void *buf, u32 len, struct session_msg *out)
{
    const u8 *p = (const u8 *)buf;
    u32 magic = 0;
    u16 version = 0;
    u16 type = 0;
    u32 mlen = 0;
    u32 body;
    u32 total;

    if (buf == NULL || out == NULL || len < SESSION_HDR_SIZE) {
        return DS_EINVAL;
    }
    ds_memcopy(&magic, p + 0, 4);
    ds_memcopy(&version, p + 4, 2);
    ds_memcopy(&type, p + 6, 2);
    ds_memcopy(&mlen, p + 8, 4);
    if (magic != SESSION_MAGIC) {
        return DS_EINVAL;
    }
    if (version != (u16)SESSION_VERSION) {
        return DS_EINVAL;
    }
    body = session_msg_body_size((u32)type);
    if (body == 0) {
        return DS_EINVAL;
    }
    total = SESSION_HDR_SIZE + body;
    if (len != total || mlen != total) {
        return DS_EINVAL;
    }
    ds_memzero(out, (u64)sizeof(*out));
    ds_memcopy(out, p, (u64)total);
    return DS_OK;
}

/* ------------------------------------------------------------------------- */
/* 角色令牌                                                                   */
/* ------------------------------------------------------------------------- */

u32 session_token_make(u32 role, u32 seq, u32 nonce)
{
    u32 r = role & 0xFFu;
    u32 q = seq & 0xFFu;
    u32 n = nonce & 0xFFFFu;

    /*
     * 归一化非法输入（0 = "没有令牌"，不能被签发）：角色至少是 APP、
     * 序号至少 1、nonce 非 0。host 单测覆盖这三条。
     */
    if (r == SESSION_ROLE_NONE) {
        r = SESSION_ROLE_APP;
    }
    if (q == 0u) {
        q = 1u;
    }
    if (n == 0u) {
        n = 1u;
    }
    return (n << 16) | (r << 8) | q;
}

int session_token_valid(u32 token)
{
    u32 role = SESSION_TOKEN_ROLE(token);

    if (token == 0) {
        return 0;
    }
    if (SESSION_TOKEN_NONCE(token) == 0) {
        return 0;
    }
    if (SESSION_TOKEN_SEQ(token) == 0) {
        return 0;
    }
    return (role >= SESSION_ROLE_WM && role <= SESSION_ROLE_SESSION) ? 1 : 0;
}

int session_token_is(u32 token, u32 role)
{
    return (token != 0 && SESSION_TOKEN_ROLE(token) == role) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 路径/参数/argv 项                                                          */
/* ------------------------------------------------------------------------- */

void session_path_set(u32 out[SESSION_PATH_MAX / 4u], const char *s)
{
    u8 raw[SESSION_PATH_MAX];
    u32 i;

    for (i = 0; i < SESSION_PATH_MAX / 4u; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i < (u32)SESSION_PATH_MAX; i++) {
        raw[i] = 0;
    }
    for (i = 0; i + 1u < (u32)SESSION_PATH_MAX && s[i] != '\0'; i++) {
        raw[i] = (u8)s[i];
    }
    for (i = 0; i < SESSION_PATH_MAX / 4u; i++) {
        u32 w = 0;
        u32 b;

        for (b = 0; b < 4u; b++) {
            w |= ((u32)raw[i * 4u + b]) << (8u * b);
        }
        out[i] = w;
    }
}

void session_path_get(const u32 in[SESSION_PATH_MAX / 4u], char *out, u32 cap)
{
    u32 max = (cap - 1u < (u32)SESSION_PATH_MAX) ? (cap - 1u)
                                                 : (u32)SESSION_PATH_MAX;
    u32 i;

    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    for (i = 0; i < max; i++) {
        u32 w = in[i / 4u];
        char c = (char)((w >> (8u * (i % 4u))) & 0xFFu);

        if (c == '\0') {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

void session_arg_set(u32 out[SESSION_ARG_MAX / 4u], const char *s)
{
    u8 raw[SESSION_ARG_MAX];
    u32 i;

    for (i = 0; i < SESSION_ARG_MAX / 4u; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i < (u32)SESSION_ARG_MAX; i++) {
        raw[i] = 0;
    }
    for (i = 0; i + 1u < (u32)SESSION_ARG_MAX && s[i] != '\0'; i++) {
        raw[i] = (u8)s[i];
    }
    for (i = 0; i < SESSION_ARG_MAX / 4u; i++) {
        u32 w = 0;
        u32 b;

        for (b = 0; b < 4u; b++) {
            w |= ((u32)raw[i * 4u + b]) << (8u * b);
        }
        out[i] = w;
    }
}

void session_arg_get(const u32 in[SESSION_ARG_MAX / 4u], char *out, u32 cap)
{
    u32 max = (cap - 1u < (u32)SESSION_ARG_MAX) ? (cap - 1u)
                                                : (u32)SESSION_ARG_MAX;
    u32 i;

    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    for (i = 0; i < max; i++) {
        u32 w = in[i / 4u];
        char c = (char)((w >> (8u * (i % 4u))) & 0xFFu);

        if (c == '\0') {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

int session_arg_parse_u32(const char *s, const char *key, u32 *out)
{
    u32 i = 0;
    u32 v = 0;
    u32 digits = 0;
    int hex = 0;

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

        if (hex) {
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
                return 0; /* 溢出 */
            }
            v = (v << 4) | d;
        } else {
            if (c < '0' || c > '9') {
                return 0;
            }
            if (v > 429496729u) {
                return 0;
            }
            v = v * 10u + (c - '0');
        }
        digits++;
        i++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* 注册表记录                                                                 */
/* ------------------------------------------------------------------------- */

void session_entry_fill(struct session_entry_info *e, u32 pid, u32 token,
                        u32 surface_id, u32 state, u32 role, const char *title)
{
    if (e == NULL) {
        return;
    }
    ds_memzero(e, (u64)sizeof(*e));
    e->pid = pid;
    e->token = token;
    e->surface_id = surface_id;
    e->state = state;
    e->role = role;
    ds_title_set(e->title, title);
}
