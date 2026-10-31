/*
 * ng WP3-2: display server 协议编解码（纯函数，host 单测直接驱动）。
 *
 * 只做字节搬运和格式校验，不依赖 libc / 内核 / 串口：内核、ring3 客户端、
 * host 单测共用同一份代码。消息布局见 ds_protocol.h。
 *
 * v1 兼容：解码器接受 version 1 与 2。version 1 的 CREATE body 只有 16 字节
 * （w/h/flags/reserved），解码后 title 为空串、owner_pid/x/y 为 0；其余 v1
 * 消息与 v2 同构。LIST / LIST_REPLY / REGISTER_HOTKEY 只存在于 v2。
 */
#include "ds_protocol.h"

_Static_assert(sizeof(struct ds_create_body) == 44, "create body v3 must be 44");
_Static_assert(DS_CREATE_BODY_V2 == 40u, "create body v2 must be 40");
_Static_assert(sizeof(struct ds_create_reply_body) == 28,
               "create_reply body must be 28");
_Static_assert(sizeof(struct ds_map_body) == 8, "map body must be 8");
_Static_assert(sizeof(struct ds_commit_body) == 20, "commit body must be 20");
_Static_assert(sizeof(struct ds_destroy_body) == 8, "destroy body must be 8");
_Static_assert(sizeof(struct ds_focus_body) == 8, "focus body must be 8");
_Static_assert(sizeof(struct ds_input_body) == 20, "input body must be 20");
_Static_assert(sizeof(struct ds_bye_body) == 8, "bye body must be 8");
_Static_assert(sizeof(struct ds_list_body) == 4, "list body must be 4");
_Static_assert(sizeof(struct ds_list_entry) == 48, "list entry v3 must be 48");
_Static_assert(sizeof(struct ds_list_reply_body) == 392,
               "list reply body v3 must be 392");
_Static_assert(DS_MSG_HDR_SIZE + 392u <= DS_MSG_MAX,
               "LIST_REPLY v3 must fit in DS_MSG_MAX");
_Static_assert(sizeof(struct ds_register_hotkey_body) == 16,
               "register_hotkey body must be 16");
_Static_assert(sizeof(struct ds_wm_status_body) == 28,
               "wm_status body must be 28");
_Static_assert(sizeof(struct ds_session_status_body) == 68,
               "session_status body must be 68");
_Static_assert(sizeof(struct ds_msg) == DS_MSG_MAX,
               "ds_msg must be header + (DS_MSG_MAX - header) bytes of body");

/* ------------------------------------------------------------------------- */
/* 极简 mem/str                                                              */
/* ------------------------------------------------------------------------- */

void ds_memzero(void *p, u64 n)
{
    u8 *b = (u8 *)p;
    u64 i;

    if (b == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

void ds_memcopy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    if (d == NULL || s == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

int ds_strlen(const char *s)
{
    int n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int ds_streq(const char *a, const char *b)
{
    int i = 0;

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
/* 标题打包                                                                   */
/* ------------------------------------------------------------------------- */

void ds_title_set(u32 out[DS_TITLE_WORDS], const char *s)
{
    u8 raw[DS_TITLE_MAX];
    u32 i;

    for (i = 0; i < DS_TITLE_WORDS; i++) {
        out[i] = 0;
    }
    if (s == NULL) {
        return;
    }
    for (i = 0; i < (u32)DS_TITLE_MAX; i++) {
        raw[i] = 0;
    }
    for (i = 0; i + 1u < (u32)DS_TITLE_MAX && s[i] != '\0'; i++) {
        raw[i] = (u8)s[i];
    }
    for (i = 0; i < DS_TITLE_WORDS; i++) {
        u32 w = 0;
        u32 b;

        for (b = 0; b < 4u; b++) {
            w |= ((u32)raw[i * 4u + b]) << (8u * b);
        }
        out[i] = w;
    }
}

void ds_title_get(const u32 in[DS_TITLE_WORDS], char *out, u32 cap)
{
    u32 max = (cap - 1u < (u32)DS_TITLE_MAX) ? (cap - 1u) : (u32)DS_TITLE_MAX;
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

void ds_list_entry_set(struct ds_list_entry *e, u32 id, u32 owner_pid, u32 x,
                       u32 y, u32 w, u32 h, u32 focused, u32 token,
                       const char *title)
{
    if (e == NULL) {
        return;
    }
    ds_memzero(e, (u64)sizeof(*e));
    e->id = id;
    e->owner_pid = owner_pid;
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    e->focused = (focused != 0) ? 1u : 0u;
    e->token = token;
    ds_title_set(e->title, title);
}

/* ------------------------------------------------------------------------- */
/* 尺寸表                                                                     */
/* ------------------------------------------------------------------------- */

u32 ds_msg_body_size_v(u32 type, u16 version)
{
    if (version < (u16)DS_PROTO_VERSION_MIN ||
        version > (u16)DS_PROTO_VERSION) {
        return 0;
    }
    switch (type) {
    case DS_MSG_CREATE:
        if (version == 1u) {
            return DS_CREATE_BODY_V1;
        }
        if (version == 2u) {
            return DS_CREATE_BODY_V2;
        }
        return (u32)sizeof(struct ds_create_body);
    case DS_MSG_CREATE_REPLY:
        return (u32)sizeof(struct ds_create_reply_body);
    case DS_MSG_MAP:
        return (u32)sizeof(struct ds_map_body);
    case DS_MSG_COMMIT:
        return (u32)sizeof(struct ds_commit_body);
    case DS_MSG_DESTROY:
        return (u32)sizeof(struct ds_destroy_body);
    case DS_MSG_FOCUS:
        return (u32)sizeof(struct ds_focus_body);
    case DS_MSG_INPUT:
        return (u32)sizeof(struct ds_input_body);
    case DS_MSG_BYE:
        return (u32)sizeof(struct ds_bye_body);
    case DS_MSG_LIST:
        return (version == 1u) ? 0u : (u32)sizeof(struct ds_list_body);
    case DS_MSG_LIST_REPLY:
        if (version == 1u) {
            return 0u;
        }
        if (version == 2u) {
            return DS_LIST_REPLY_BODY_V2;
        }
        return (u32)sizeof(struct ds_list_reply_body);
    case DS_MSG_REGISTER_HOTKEY:
        return (version == 1u) ? 0u
                               : (u32)sizeof(struct ds_register_hotkey_body);
    case DS_MSG_WM_STATUS:
        return (version == 1u) ? 0u : (u32)sizeof(struct ds_wm_status_body);
    case DS_MSG_SESSION_STATUS:
        return (version == 1u || version == 2u)
                   ? 0u
                   : (u32)sizeof(struct ds_session_status_body);
    default:
        return 0;
    }
}

u32 ds_msg_body_size(u32 type)
{
    return ds_msg_body_size_v(type, (u16)DS_PROTO_VERSION);
}

u32 ds_msg_size_v(u32 type, u16 version)
{
    u32 body = ds_msg_body_size_v(type, version);

    return (body == 0) ? 0 : (DS_MSG_HDR_SIZE + body);
}

u32 ds_msg_size(u32 type)
{
    return ds_msg_size_v(type, (u16)DS_PROTO_VERSION);
}

/* ------------------------------------------------------------------------- */
/* 编码 / 解码                                                                */
/* ------------------------------------------------------------------------- */

int ds_msg_encode(const struct ds_msg *m, void *buf, u32 cap)
{
    u8 *out = (u8 *)buf;
    u32 body;
    u32 total;
    u32 magic = DS_PROTO_MAGIC;
    u16 version = (u16)DS_PROTO_VERSION;
    u16 type;
    i32 status;
    u32 seq;

    if (m == NULL || buf == NULL) {
        return DS_EINVAL;
    }
    body = ds_msg_body_size(m->type);
    if (body == 0) {
        return DS_EINVAL;
    }
    total = DS_MSG_HDR_SIZE + body;
    if (total > DS_MSG_MAX || total > cap) {
        return DS_EINVAL;
    }
    /* 逐字段写头（不用结构体赋值：freestanding 下 clang 会调 memcpy）。 */
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
    ds_memcopy(out + DS_MSG_HDR_SIZE, &m->u, (u64)body);
    return (int)total;
}

int ds_msg_decode(const void *buf, u32 len, struct ds_msg *out)
{
    const u8 *p = (const u8 *)buf;
    u32 magic = 0;
    u16 version = 0;
    u16 type = 0;
    u32 mlen = 0;
    u32 body;
    u32 total;

    if (buf == NULL || out == NULL || len < DS_MSG_HDR_SIZE) {
        return DS_EINVAL;
    }
    ds_memcopy(&magic, p + 0, 4);
    ds_memcopy(&version, p + 4, 2);
    ds_memcopy(&type, p + 6, 2);
    ds_memcopy(&mlen, p + 8, 4);
    if (magic != DS_PROTO_MAGIC) {
        return DS_EINVAL;
    }
    if (version < (u16)DS_PROTO_VERSION_MIN ||
        version > (u16)DS_PROTO_VERSION) {
        return DS_EINVAL;
    }
    body = ds_msg_body_size_v((u32)type, version);
    if (body == 0) {
        return DS_EINVAL;
    }
    total = DS_MSG_HDR_SIZE + body;
    if (len != total || mlen != total) {
        return DS_EINVAL;
    }
    ds_memzero(out, (u64)sizeof(*out));
    ds_memcopy(out, p, (u64)total);
    return DS_OK;
}

int ds_msg_status_ok(const struct ds_msg *m)
{
    return (m != NULL && m->status == 0) ? 1 : 0;
}
