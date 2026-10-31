/*
 * ng WP3-1: display server 协议编解码（纯函数，host 单测直接驱动）。
 *
 * 只做字节搬运和格式校验，不依赖 libc / 内核 / 串口：内核、ring3 客户端、
 * host 单测共用同一份代码。消息布局见 ds_protocol.h。
 */
#include "ds_protocol.h"

_Static_assert(sizeof(struct ds_create_body) == 16, "create body must be 16");
_Static_assert(sizeof(struct ds_create_reply_body) == 28,
               "create_reply body must be 28");
_Static_assert(sizeof(struct ds_map_body) == 8, "map body must be 8");
_Static_assert(sizeof(struct ds_commit_body) == 20, "commit body must be 20");
_Static_assert(sizeof(struct ds_destroy_body) == 8, "destroy body must be 8");
_Static_assert(sizeof(struct ds_focus_body) == 8, "focus body must be 8");
_Static_assert(sizeof(struct ds_input_body) == 20, "input body must be 20");
_Static_assert(sizeof(struct ds_bye_body) == 8, "bye body must be 8");
_Static_assert(sizeof(struct ds_msg) == DS_MSG_HDR_SIZE + 32,
               "ds_msg must be header + 32-byte body union");

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
/* 尺寸表                                                                     */
/* ------------------------------------------------------------------------- */

u32 ds_msg_body_size(u32 type)
{
    switch (type) {
    case DS_MSG_CREATE:
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
    default:
        return 0;
    }
}

u32 ds_msg_size(u32 type)
{
    u32 body = ds_msg_body_size(type);

    return (body == 0) ? 0 : (DS_MSG_HDR_SIZE + body);
}

/* ------------------------------------------------------------------------- */
/* 编码 / 解码                                                                */
/* ------------------------------------------------------------------------- */

int ds_msg_encode(const struct ds_msg *m, void *buf, u32 cap)
{
    struct ds_msg local;
    u8 *out = (u8 *)buf;
    u32 body;
    u32 total;

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
    /* 编码器不修改调用方的消息：在本地副本上补 magic/version/len。 */
    local = *m;
    local.magic = DS_PROTO_MAGIC;
    local.version = DS_PROTO_VERSION;
    local.len = total;
    ds_memcopy(out, &local, (u64)total);
    return (int)total;
}

int ds_msg_decode(const void *buf, u32 len, struct ds_msg *out)
{
    const u8 *p = (const u8 *)buf;
    struct ds_msg tmp;
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
    if (version != DS_PROTO_VERSION) {
        return DS_EINVAL;
    }
    body = ds_msg_body_size((u32)type);
    if (body == 0) {
        return DS_EINVAL;
    }
    total = DS_MSG_HDR_SIZE + body;
    if (len != total || mlen != total) {
        return DS_EINVAL;
    }
    ds_memzero(&tmp, (u64)sizeof(tmp));
    ds_memcopy(&tmp, p, (u64)total);
    *out = tmp;
    return DS_OK;
}

int ds_msg_status_ok(const struct ds_msg *m)
{
    return (m != NULL && m->status == 0) ? 1 : 0;
}
