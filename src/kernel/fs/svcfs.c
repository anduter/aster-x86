/* =============================================================================
 * fs/svcfs.c - "svc:" 用户态文件服务后端
 *
 * VFS 把 svc:/... 的 open 转成一次 service_connect("fsd") + channel read，
 * 内容缓存在内核句柄里；文件系统语义由 ring3 fsd 提供。
 * ============================================================================= */
#include "fs.h"
#include "include/svc.h"
#include "include/svc.h"
#include "include/proc.h"
#include "include/kernel.h"

#define SVC_MAX_HANDLES 8U
#define SVC_FILE_MAX    128U

typedef struct {
    bool     used;
    bool     dirty;
    uint32_t len;
    uint32_t off;
    uint8_t  data[SVC_FILE_MAX];
} svc_handle_t;

static svc_handle_t handles[SVC_MAX_HANDLES];

static int svc_probe(const char *path, const char **rel)
{
    if (path == 0 || strncmp(path, "svc:", 4) != 0) return 0;
    const char *p = path + 4;
    while (*p == '/') p++;
    *rel = p;
    return 1;
}

static int svc_open(const char *rel, uint32_t flags, uint32_t uid, uint32_t gid)
{
    uint32_t client = 0;
    int slot = -1;
    int n;

    (void)rel; (void)flags; (void)uid; (void)gid;

    for (uint32_t i = 0; i < SVC_MAX_HANDLES; i++) {
        if (!handles[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    for (int retry = 0; retry < 100; retry++) {
        if (service_connect("fsd", &client) == 0) break;
        client = 0;
        proc_sleep_ticks(1U);
    }
    if (client == 0U) return -1;

    if (chan_send(client, "read", 4) != 4) {
        chan_release(client);
        return -1;
    }
    n = chan_recv_block(client, handles[slot].data, SVC_FILE_MAX - 1U);
    chan_release(client);
    if (n < 0) return -1;

    handles[slot].len = (uint32_t)n;
    handles[slot].off = 0;
    handles[slot].data[n] = '\0';
    handles[slot].used = true;
    handles[slot].dirty = false;
    return slot;
}

static int svc_read(int h, void *buf, uint32_t len)
{
    uint32_t n;

    if (h < 0 || h >= (int)SVC_MAX_HANDLES || !handles[h].used) return -1;
    n = handles[h].len - handles[h].off;
    if (n > len) n = len;
    if (n != 0U) memcpy(buf, handles[h].data + handles[h].off, n);
    handles[h].off += n;
    return (int)n;
}

static int svc_write(int h, const void *buf, uint32_t len)
{
    svc_handle_t *s;

    if (h < 0 || h >= (int)SVC_MAX_HANDLES || !handles[h].used) return -1;
    s = &handles[h];
    if (buf == 0 || len == 0U) return 0;
    if (s->off + len > SVC_FILE_MAX) {
        if (s->off >= SVC_FILE_MAX) return -1;
        len = SVC_FILE_MAX - s->off;
    }
    memcpy(s->data + s->off, buf, len);
    s->off += len;
    if (s->off > s->len) s->len = s->off;
    s->dirty = true;
    return (int)len;
}

static int svc_close(int h)
{
    uint8_t req[64];
    uint32_t client = 0;
    svc_handle_t *s;
    int r = 0;

    if (h < 0 || h >= (int)SVC_MAX_HANDLES || !handles[h].used) return -1;
    s = &handles[h];

    if (s->dirty) {
        uint32_t n = s->len;
        if (n > sizeof(req) - 1U) n = sizeof(req) - 1U;
        for (int retry = 0; retry < 100; retry++) {
            if (service_connect("fsd", &client) == 0) break;
            client = 0;
            proc_sleep_ticks(1U);
        }
        if (client == 0U) {
            r = -1;                     /* 服务不可用：数据没写出去，但槽位必须还 */
        } else {
            req[0] = 'w';
            memcpy(req + 1, s->data, n);
            /* 原实现忽略 send/recv 的返回值：命令只发了一半、或服务端没确认，
             * close 仍返回 0，脏数据被静默丢弃。这里向上报错。 */
            if (chan_send(client, req, n + 1U) != (int)(n + 1U)) {
                r = -1;
            } else if (chan_recv_block(client, req, sizeof(req)) < 0) {
                r = -1;
            }
            chan_release(client);
        }
    }

    /* 无论成功与否都释放槽位，否则失败一次就永久少一个 svc: 句柄 */
    s->used = false;
    s->dirty = false;
    s->len = s->off = 0;
    return r;
}

static int svc_ref(int h)
{
    return (h >= 0 && h < (int)SVC_MAX_HANDLES && handles[h].used) ? 0 : -1;
}

static int svc_stat(const char *rel, fs_stat_t *out)
{
    int h;

    (void)rel;
    if (out == 0) return -1;
    h = svc_open(rel, FS_OPEN_READ, 0, 0);
    if (h < 0) return -1;
    out->type = FS_TYPE_FILE;
    out->size = handles[h].len;
    out->mode = 0444;
    out->uid = 0;
    out->gid = 0;
    svc_close(h);
    return 0;
}

static int svc_listdir(const char *rel, uint32_t index, char *name,
                       uint32_t name_max, fs_stat_t *out)
{
    (void)rel; (void)index; (void)name; (void)name_max; (void)out;
    return -1;
}

static int svc_mkdir(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)rel; (void)uid; (void)gid;
    return -1;
}

static int svc_unlink(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)rel; (void)uid; (void)gid;
    return -1;
}

/* svcfs 是只读的服务文件系统：改名/移动不支持 */
static int svc_rename(const char *oldrel, const char *newrel, uint32_t uid, uint32_t gid)
{
    (void)oldrel; (void)newrel; (void)uid; (void)gid;
    return E_INVAL;
}

const fs_backend_t svc_backend = {
    "svcfs",
    svc_probe,
    svc_open,
    svc_read,
    svc_write,
    svc_close,
    svc_ref,
    svc_stat,
    svc_listdir,
    svc_mkdir,
    svc_unlink,
    svc_rename
};
