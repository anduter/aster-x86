/* =============================================================================
 * fs/blkfs.c - "blk:" ring3 块设备服务后端
 *
 * open("blk:/sector0") 通过 service_connect("blkd") 读取一个扇区，
 * 再把扇区内容作为一个只读文件暴露给 VFS。
 * ============================================================================= */
#include "fs.h"
#include "include/svc.h"
#include "include/svc.h"
#include "include/proc.h"
#include "include/kernel.h"

#define BLK_MAX_HANDLES 8U
#define BLK_FILE_MAX    128U

typedef struct {
    bool     used;
    bool     dirty;
    uint32_t len;
    uint32_t off;
    uint8_t  data[BLK_FILE_MAX];
} blk_handle_t;

static blk_handle_t handles[BLK_MAX_HANDLES];

static int blk_probe(const char *path, const char **rel)
{
    if (path == 0 || strncmp(path, "blk:", 4) != 0) return 0;
    const char *p = path + 4;
    while (*p == '/') p++;
    *rel = p;
    return 1;
}

static int blk_open(const char *rel, uint32_t flags, uint32_t uid, uint32_t gid)
{
    uint32_t client = 0;
    int slot = -1;
    int n;

    (void)rel; (void)flags; (void)uid; (void)gid;

    for (uint32_t i = 0; i < BLK_MAX_HANDLES; i++) {
        if (!handles[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    for (int retry = 0; retry < 100; retry++) {
        if (service_connect("blkd", &client) == 0) break;
        client = 0;
        proc_sleep_ticks(1U);
    }
    if (client == 0U) return -1;

    if (chan_send(client, "read", 4) != 4) {
        chan_release(client);
        return -1;
    }
    n = chan_recv_block(client, handles[slot].data, BLK_FILE_MAX - 1U);
    chan_release(client);
    if (n < 0) return -1;

    handles[slot].len = (uint32_t)n;
    handles[slot].off = 0;
    handles[slot].data[n] = '\0';
    handles[slot].used = true;
    handles[slot].dirty = false;
    return slot;
}

static int blk_read(int h, void *buf, uint32_t len)
{
    uint32_t n;

    if (h < 0 || h >= (int)BLK_MAX_HANDLES || !handles[h].used) return -1;
    n = handles[h].len - handles[h].off;
    if (n > len) n = len;
    if (n != 0U) memcpy(buf, handles[h].data + handles[h].off, n);
    handles[h].off += n;
    return (int)n;
}

static int blk_write(int h, const void *buf, uint32_t len)
{
    blk_handle_t *s;

    if (h < 0 || h >= (int)BLK_MAX_HANDLES || !handles[h].used) return -1;
    s = &handles[h];
    if (buf == 0 || len == 0U) return 0;
    if (s->off + len > BLK_FILE_MAX) {
        if (s->off >= BLK_FILE_MAX) return -1;
        len = BLK_FILE_MAX - s->off;
    }
    memcpy(s->data + s->off, buf, len);
    s->off += len;
    if (s->off > s->len) s->len = s->off;
    s->dirty = true;
    return (int)len;
}

static int blk_close(int h)
{
    uint8_t req[64];
    uint32_t client = 0;
    blk_handle_t *s;

    if (h < 0 || h >= (int)BLK_MAX_HANDLES || !handles[h].used) return -1;
    s = &handles[h];

    if (s->dirty) {
        uint32_t n = s->len;
        if (n > sizeof(req) - 1U) n = sizeof(req) - 1U;
        for (int retry = 0; retry < 100; retry++) {
            if (service_connect("blkd", &client) == 0) break;
            client = 0;
            proc_sleep_ticks(1U);
        }
        if (client == 0U) return -1;
        req[0] = 'w';
        memcpy(req + 1, s->data, n);
        (void)chan_send(client, req, n + 1U);
        (void)chan_recv_block(client, req, sizeof(req));
        chan_release(client);
    }

    s->used = false;
    s->dirty = false;
    s->len = s->off = 0;
    return 0;
}

static int blk_ref(int h)
{
    return (h >= 0 && h < (int)BLK_MAX_HANDLES && handles[h].used) ? 0 : -1;
}

static int blk_stat(const char *rel, fs_stat_t *out)
{
    (void)rel;
    if (out == 0) return -1;
    out->type = FS_TYPE_FILE;
    out->size = 32;
    out->mode = 0444;
    out->uid = 0;
    out->gid = 0;
    return 0;
}

static int blk_listdir(const char *rel, uint32_t index, char *name,
                       uint32_t name_max, fs_stat_t *out)
{
    (void)rel; (void)index; (void)name; (void)name_max; (void)out;
    return -1;
}

static int blk_mkdir(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)rel; (void)uid; (void)gid;
    return -1;
}

static int blk_unlink(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)rel; (void)uid; (void)gid;
    return -1;
}

const fs_backend_t blk_backend = {
    "blkfs",
    blk_probe,
    blk_open,
    blk_read,
    blk_write,
    blk_close,
    blk_ref,
    blk_stat,
    blk_listdir,
    blk_mkdir,
    blk_unlink
};
