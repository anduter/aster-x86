/* =============================================================================
 * fs/fat16.c - FAT16 只根目录实现（读 + 写 + 删除）
 *
 * 目标是“和宿主交换文件”，所以刻意保持简单：
 *   - 只支持根目录（FAT16 根目录是固定区域，不用簇链，最容易写对）
 *   - 8.3 短名，不做 LFN；名字自动转大写
 *   - 簇大小按 BPB 读，512 字节簇的 8MB 盘就是标准 FAT16
 *   - 两份 FAT 都回写；写完清空块缓存（块设备层写穿透会自动失效）
 * ============================================================================= */
#include "fat16.h"
#include "include/dev.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/kernel.h"

#define FAT16_MAX_HANDLES 8

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wtime;
    uint16_t wdate;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed)) fat_dirent_t;

typedef struct {
    bool     used;
    bool     write;
    uint8_t  name[11];
    uint32_t dir_lba;      /* 目录项所在扇区 */
    uint32_t dir_idx;      /* 扇区内第几项 */
    uint32_t first_cluster;
    uint32_t cur_cluster;
    uint32_t offset;
    uint32_t size;
} fat_handle_t;

static struct {
    block_device_t *dev;
    uint32_t sector_size;
    uint32_t sec_per_cluster;
    uint32_t reserved;
    uint32_t fats;
    uint32_t root_entries;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    char     drive[FS_DRIVE_MAX];
    bool     mounted;
} fat;

static fat_handle_t handles[FAT16_MAX_HANDLES];
static uint8_t sec_buf[512];

bool fat16_mounted(void) { return fat.mounted; }
const char *fat16_drive(void) { return fat.drive; }

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int read_sector(uint32_t lba, void *buf)
{
    if (fat.dev == 0) return -1;
    return (blk_read(fat.dev, lba, buf) == E_OK) ? 0 : -1;
}

static int write_sector(uint32_t lba, const void *buf)
{
    if (fat.dev == 0) return -1;
    return (blk_write(fat.dev, lba, buf) == E_OK) ? 0 : -1;
}

/* ---- FAT 表项 ---- */
static uint32_t cluster_lba(uint32_t cluster)
{
    return fat.data_start + (cluster - 2U) * fat.sec_per_cluster;
}

static uint16_t fat_get(uint32_t cluster)
{
    uint32_t off = cluster * 2U;
    uint32_t lba = fat.reserved + off / fat.sector_size;
    uint32_t idx = (off % fat.sector_size) / 2U;

    if (read_sector(lba, sec_buf) != 0) return 0xFFFFU;
    return (uint16_t)(sec_buf[idx * 2U] | ((uint16_t)sec_buf[idx * 2U + 1U] << 8));
}

static int fat_set(uint32_t cluster, uint16_t value)
{
    uint32_t off = cluster * 2U;
    uint32_t fat_lba = fat.reserved + off / fat.sector_size;
    uint32_t idx = (off % fat.sector_size) / 2U;

    if (read_sector(fat_lba, sec_buf) != 0) return -1;
    sec_buf[idx * 2U] = (uint8_t)(value & 0xFF);
    sec_buf[idx * 2U + 1U] = (uint8_t)(value >> 8);

    for (uint32_t f = 0; f < fat.fats; f++) {
        if (write_sector(fat_lba + f * fat.fat_sectors, sec_buf) != 0) return -1;
    }
    return 0;
}

/* 分配一个空闲簇并清零；任何一步失败都把刚占用的 FAT 项还回去，
 * 否则会留下"FAT 里已占用、目录里没人引用"的永久簇泄漏。 */
static uint32_t cluster_alloc(void)
{
    for (uint32_t c = 2; c < fat.cluster_count + 2U; c++) {
        if (fat_get(c) == 0U) {
            if (fat_set(c, 0xFFFFU) != 0) return 0;
            /* 清零 */
            for (uint32_t s = 0; s < fat.sec_per_cluster; s++) {
                memset(sec_buf, 0, sizeof(sec_buf));
                if (write_sector(cluster_lba(c) + s, sec_buf) != 0) {
                    (void)fat_set(c, 0);        /* 回滚：别泄漏这个簇 */
                    return 0;
                }
            }
            return c;
        }
    }
    return 0;
}

static void cluster_free_chain(uint32_t first)
{
    uint32_t c = first;
    uint32_t guard = 0;

    while (c >= 2U && c < fat.cluster_count + 2U && guard++ < fat.cluster_count) {
        uint16_t next = fat_get(c);
        fat_set(c, 0);
        c = next;
    }
}

/* ---- 名字转换 ---- */
static char upper_ch(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int to_83(const char *name, uint8_t out[11])
{
    uint32_t i = 0, j = 0;

    while (*name == '/') name++;
    if (*name == '\0') return -1;

    for (i = 0; i < 11; i++) out[i] = ' ';

    i = 0;
    while (*name != '\0' && *name != '.') {
        char c = upper_ch(*name);
        if (i >= 8U) return -1;
        if (c == ' ' || c == '/' || c == '\\' || c == ':') return -1;
        out[i++] = (uint8_t)c;
        name++;
    }
    if (*name == '.') {
        name++;
        j = 0;
        while (*name != '\0') {
            char c = upper_ch(*name);
            if (j >= 3U) return -1;
            out[8 + j++] = (uint8_t)c;
            name++;
        }
    }
    return 0;
}

/* 目录项 -> 可显示名字 */
static void from_83(const uint8_t *in, char *out, uint32_t max)
{
    uint32_t n = 0;

    while (n < 8U && in[n] != ' ' && n + 1U < max) { out[n] = (char)in[n]; n++; }
    if (in[8] != ' ') {
        if (n + 1U < max) out[n++] = '.';
        for (uint32_t i = 8; i < 11U && in[i] != ' ' && n + 1U < max; i++) out[n++] = (char)in[i];
    }
    out[n] = '\0';
}

/* 找根目录里的一项；返回目录项位置，item 可空 */
static int find_root_entry(const char *rel, fat_dirent_t *item,
                           uint32_t *out_lba, uint32_t *out_idx)
{
    uint8_t want[11];
    int found = 0;

    if (to_83(rel, want) != 0) return -1;

    for (uint32_t s = 0; s < fat.root_sectors; s++) {
        if (read_sector(fat.root_start + s, sec_buf) != 0) return -1;
        for (uint32_t i = 0; i < fat.sector_size / 32U; i++) {
            fat_dirent_t *de = (fat_dirent_t *)(void *)(sec_buf + i * 32U);
            if ((uint8_t)de->name[0] == 0x00U) return found ? 0 : E_NOENT;
            if ((uint8_t)de->name[0] == 0xE5U) continue;
            if ((de->attr & 0x0FU) == 0x0FU) continue;      /* LFN */
            if ((de->attr & 0x08U) != 0U) continue;         /* 卷标 */

            if (memcmp(de->name, want, 11) == 0) {
                found = 1;
                if (item != 0) *item = *de;
                if (out_lba != 0) *out_lba = fat.root_start + s;
                if (out_idx != 0) *out_idx = i;
                return 0;
            }
        }
    }
    return found ? 0 : E_NOENT;
}

/* 找一个空目录项（0x00 或 0xE5） */
static int find_free_entry(uint32_t *out_lba, uint32_t *out_idx)
{
    for (uint32_t s = 0; s < fat.root_sectors; s++) {
        if (read_sector(fat.root_start + s, sec_buf) != 0) return -1;
        for (uint32_t i = 0; i < fat.sector_size / 32U; i++) {
            uint8_t first = sec_buf[i * 32U];
            if (first == 0x00U || first == 0xE5U) {
                *out_lba = fat.root_start + s;
                *out_idx = i;
                return 0;
            }
        }
    }
    return -1;
}

/* ==================== 挂载 ==================== */

int fat16_mount(const char *blk_name, const char *drive)
{
    block_device_t *dev = blk_lookup(blk_name);
    uint8_t sb[512];
    uint32_t sector_size, sec_per_cluster, reserved, fats, root_entries;
    uint32_t total_sectors, fat_sectors, root_secs, data_start, clusters;

    if (dev == 0) return E_NOENT;
    if (dev->sector_size != 512U) return E_INVAL;

    if (blk_read(dev, 0, sb) != E_OK) return E_INVAL;
    if (sb[510] != 0x55U || sb[511] != 0xAAU) return E_INVAL;

    /* 先解析到局部变量，全部校验通过后再提交。
     * 原实现先把 fat.dev 等字段写成新盘的值再去校验，任一条失败都会留下
     * "mounted 还是旧值、几何字段却是新盘/半解析"的状态；重新挂载失败时
     * 甚至会把原本可用的挂载点弄坏。 */
    sector_size = rd16(sb + 11);
    sec_per_cluster = sb[13];
    reserved = rd16(sb + 14);
    fats = sb[16];
    root_entries = rd16(sb + 17);
    total_sectors = rd16(sb + 19);
    fat_sectors = rd16(sb + 22);
    if (total_sectors == 0U) total_sectors = rd32(sb + 32);

    if (sector_size != 512U || sec_per_cluster == 0U ||
        fats == 0U || fat_sectors == 0U || root_entries == 0U) {
        return E_INVAL;
    }

    root_secs = (root_entries * 32U + sector_size - 1U) / sector_size;
    data_start = reserved + fats * fat_sectors + root_secs;
    if (total_sectors <= data_start || total_sectors > dev->sector_count) {
        return E_INVAL;
    }
    clusters = (total_sectors - data_start) / sec_per_cluster;

    /* 4085..65524 才是 FAT16；否则是 FAT12/FAT32 */
    if (clusters < 4085U || clusters > 65524U) {
        kprintf("fat16: not FAT16 (clusters=%u)\n", (unsigned int)clusters);
        return E_INVAL;
    }

    /* 全部校验通过：这里才提交状态 */
    fat.dev = dev;
    fat.sector_size = sector_size;
    fat.sec_per_cluster = sec_per_cluster;
    fat.reserved = reserved;
    fat.fats = fats;
    fat.root_entries = root_entries;
    fat.total_sectors = total_sectors;
    fat.fat_sectors = fat_sectors;
    fat.root_start = reserved + fats * fat_sectors;
    fat.root_sectors = root_secs;
    fat.data_start = data_start;
    fat.cluster_count = clusters;
    memset(handles, 0, sizeof(handles));
    strncpy(fat.drive, drive, FS_DRIVE_MAX - 1U);
    fat.drive[FS_DRIVE_MAX - 1U] = '\0';
    fat.mounted = true;

    kprintf("fat16: %s mounted as %s:/ (%u clusters, %u KB)\n",
            blk_name, fat.drive, (unsigned int)clusters,
            (unsigned int)(clusters * fat.sec_per_cluster / 2U));
    return E_OK;
}

/* ==================== 目录 / 元数据 ==================== */

static const char *rel_of(const char *rel)
{
    while (*rel == '/') rel++;
    return rel;
}

int fat16_listdir(const char *rel, uint32_t index, char *name, uint32_t max,
                  fs_stat_t *out)
{
    uint32_t seen = 0;

    if (!fat.mounted) return E_NOENT;
    if (rel != 0 && *rel_of(rel) != '\0') return E_NOENT;   /* 只有根目录 */

    for (uint32_t s = 0; s < fat.root_sectors; s++) {
        if (read_sector(fat.root_start + s, sec_buf) != 0) return E_INVAL;
        for (uint32_t i = 0; i < fat.sector_size / 32U; i++) {
            fat_dirent_t *de = (fat_dirent_t *)(void *)(sec_buf + i * 32U);
            if ((uint8_t)de->name[0] == 0x00U) return E_NOENT;
            if ((uint8_t)de->name[0] == 0xE5U) continue;
            if ((de->attr & 0x0FU) == 0x0FU) continue;
            if ((de->attr & 0x08U) != 0U) continue;

            if (seen == index) {
                if (name != 0 && max > 0U) from_83(de->name, name, max);
                if (out != 0) {
                    out->type = ((de->attr & 0x10U) != 0U) ? FS_TYPE_DIR : FS_TYPE_FILE;
                    out->size = de->size;
                    out->mtime = 0;
                }
                return E_OK;
            }
            seen++;
        }
    }
    return E_NOENT;
}

int fat16_stat(const char *rel, fs_stat_t *out)
{
    fat_dirent_t de;
    int r = find_root_entry(rel, &de, 0, 0);

    if (r != 0) return r;
    if (out != 0) {
        out->type = ((de.attr & 0x10U) != 0U) ? FS_TYPE_DIR : FS_TYPE_FILE;
        out->size = de.size;
        out->mtime = 0;
    }
    return E_OK;
}

int fat16_unlink(const char *rel)
{
    fat_dirent_t de;
    uint32_t lba = 0, idx = 0;
    int r;

    if (!fat.mounted) return E_NOENT;
    r = find_root_entry(rel, &de, &lba, &idx);
    if (r == E_NOENT) return E_NOENT;
    if (r != 0) return E_INVAL;             /* 读盘失败 != 文件不存在 */

    /* 打开中的目录项不能删：删掉后簇链被回收，读句柄仍按旧的 first_cluster
     * 继续读，读到的是已被别的文件复用的簇（数据串台）。 */
    for (int i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (handles[i].used && handles[i].dir_lba == lba && handles[i].dir_idx == idx) {
            return E_BUSY;
        }
    }

    /* 先让目录项失效，再回收簇链：反过来的话，写完目录项失败会留下
     * "目录项仍有效、簇却已释放"的半删除状态，原文件从此读出来是垃圾。 */
    if (read_sector(lba, sec_buf) != 0) return E_INVAL;
    sec_buf[idx * 32U] = 0xE5U;
    if (write_sector(lba, sec_buf) != 0) return E_INVAL;

    cluster_free_chain(de.cluster_lo);
    return E_OK;
}

int fat16_mkdir(const char *rel)
{
    (void)rel;
    return E_NOSPC;             /* 只支持根目录里的文件 */
}

/* 根目录内 8.3 改名：找到目录项 -> 改 11 字节短名 -> 回写扇区。
 *
 * 限制（和本后端其余部分一致，刻意保持简单）：
 *   - 只支持根目录：old/new 里出现 '/' 一律 E_INVAL（不支持跨目录移动）；
 *   - 只支持 8.3 短名：长名返回 E_INVAL（不做 LFN）；
 *   - 不替换已存在的目标：目标被占用返回 E_EXIST（避免"释放目标簇链"这种
 *     需要回滚的多步操作；调用方可以先 unlink 目标再改名）；
 *   - 大小写不敏感：8.3 名相同（如 a.txt -> A.TXT）视为同一项，返回 E_OK。
 *
 * 目录项位置 (dir_lba, dir_idx) 不变，所以打开中的文件改名是安全的：
 * 句柄用 dir_lba/dir_idx 回写簇号和大小，不依赖名字。 */
int fat16_rename(const char *oldrel, const char *newrel)
{
    fat_dirent_t de;
    fat_dirent_t other;
    uint8_t newname[11];
    uint32_t lba = 0, idx = 0;
    uint32_t olba = 0, oidx = 0;
    const char *o;
    const char *n;
    int r;

    if (!fat.mounted) return E_NOENT;

    o = rel_of(oldrel);
    n = rel_of(newrel);
    if (*o == '\0' || *n == '\0') return E_INVAL;
    if (strchr(o, '/') != 0 || strchr(n, '/') != 0) return E_INVAL;   /* 只有根目录 */

    r = find_root_entry(o, &de, &lba, &idx);
    if (r == E_NOENT) return E_NOENT;
    if (r != 0) return E_INVAL;                 /* 读盘失败 != 不存在 */
    if ((de.attr & 0x10U) != 0U) return E_INVAL;    /* 目录项改名不支持 */
    if ((de.attr & 0x08U) != 0U) return E_INVAL;    /* 卷标 */

    if (to_83(n, newname) != 0) return E_INVAL;     /* 长名/非法名 */

    /* 目标名被别的目录项占用：不覆盖 */
    r = find_root_entry(n, &other, &olba, &oidx);
    if (r == 0) {
        if (olba == lba && oidx == idx) return E_OK;    /* 同一个 8.3 名 */
        return E_EXIST;
    }
    if (r != E_NOENT) return E_INVAL;

    if (read_sector(lba, sec_buf) != 0) return E_INVAL;
    memcpy(sec_buf + idx * 32U, newname, 11U);
    if (write_sector(lba, sec_buf) != 0) return E_INVAL;
    return E_OK;
}

/* ==================== 文件句柄 ==================== */

int fat16_open(const char *rel, uint32_t flags)
{
    fat_dirent_t de;
    int h = -1;
    int r;

    if (!fat.mounted) return -1;

    for (int i = 0; i < FAT16_MAX_HANDLES; i++) {
        if (!handles[i].used) { h = i; break; }
    }
    if (h < 0) return -1;

    memset(&handles[h], 0, sizeof(handles[h]));
    handles[h].used = true;

    if ((flags & FS_OPEN_CREAT) != 0U || (flags & FS_OPEN_WRITE) != 0U) {
        handles[h].write = true;

        r = find_root_entry(rel, &de, &handles[h].dir_lba, &handles[h].dir_idx);
        if (r == 0) {
            /* 已存在：截断。先清目录项再释放旧簇链，避免写目录项失败后
             * 留下"目录项仍指向已释放簇"的半截文件。 */
            uint32_t old_chain = de.cluster_lo;

            if ((de.attr & 0x10U) != 0U) { handles[h].used = false; return -1; }
            if (read_sector(handles[h].dir_lba, sec_buf) != 0) { handles[h].used = false; return -1; }
            wr16(sec_buf + handles[h].dir_idx * 32U + 26U, 0);
            wr32(sec_buf + handles[h].dir_idx * 32U + 28U, 0);
            if (write_sector(handles[h].dir_lba, sec_buf) != 0) { handles[h].used = false; return -1; }
            cluster_free_chain(old_chain);
        } else if (r == E_NOENT) {
            /* 新建目录项 */
            if (to_83(rel, handles[h].name) != 0) { handles[h].used = false; return -1; }
            if (find_free_entry(&handles[h].dir_lba, &handles[h].dir_idx) != 0) {
                handles[h].used = false;
                return -1;
            }
            if (read_sector(handles[h].dir_lba, sec_buf) != 0) { handles[h].used = false; return -1; }
            uint8_t *slot = sec_buf + handles[h].dir_idx * 32U;
            memset(slot, 0, 32);
            memcpy(slot, handles[h].name, 11);
            slot[11] = 0x20U;                   /* archive */
            if (write_sector(handles[h].dir_lba, sec_buf) != 0) { handles[h].used = false; return -1; }
        } else {
            /* 读盘失败：绝不能当成"文件不存在"去新建目录项，否则会重复建项 */
            handles[h].used = false;
            return -1;
        }
        return h;
    }

    /* 只读打开 */
    r = find_root_entry(rel, &de, &handles[h].dir_lba, &handles[h].dir_idx);
    if (r != 0) { handles[h].used = false; return -1; }
    if ((de.attr & 0x10U) != 0U) { handles[h].used = false; return -1; }

    memcpy(handles[h].name, de.name, 11);
    handles[h].first_cluster = de.cluster_lo;
    handles[h].cur_cluster = de.cluster_lo;
    handles[h].size = de.size;
    handles[h].offset = 0;
    return h;
}

int fat16_read(int h, void *buf, uint32_t len)
{
    fat_handle_t *fh;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;

    if (h < 0 || h >= FAT16_MAX_HANDLES || !handles[h].used) return -1;
    fh = &handles[h];

    while (done < len && fh->offset < fh->size) {
        uint32_t cluster_off = fh->offset % (fat.sec_per_cluster * 512U);
        uint32_t lba = cluster_lba(fh->cur_cluster) + cluster_off / 512U;
        uint32_t within = fh->offset % 512U;
        uint32_t n = 512U - within;

        if (n > len - done) n = len - done;
        if (n > fh->size - fh->offset) n = fh->size - fh->offset;

        if (read_sector(lba, sec_buf) != 0) return -1;
        memcpy(p + done, sec_buf + within, n);

        done += n;
        fh->offset += n;

        if (fh->offset % (fat.sec_per_cluster * 512U) == 0U && fh->offset < fh->size) {
            uint16_t next = fat_get(fh->cur_cluster);
            if (next < 2U) break;
            fh->cur_cluster = next;
        }
    }
    return (int)done;
}

int fat16_write(int h, const void *buf, uint32_t len)
{
    fat_handle_t *fh;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;

    if (h < 0 || h >= FAT16_MAX_HANDLES || !handles[h].used) return -1;
    fh = &handles[h];
    if (!fh->write) return -1;
    if (buf == 0 && len != 0U) return -1;
    if (len == 0U) return 0;

    while (done < len) {
        uint32_t cluster_bytes = fat.sec_per_cluster * 512U;
        uint32_t cluster_off = fh->offset % cluster_bytes;

        if (fh->cur_cluster < 2U) {
            uint32_t c = cluster_alloc();
            /* 盘满且一个字节都没写出去：返回错误而不是 0（0 会被当成"成功写了 0 字节"） */
            if (c == 0U) return (done > 0U) ? (int)done : -1;
            if (fh->first_cluster < 2U) {
                fh->first_cluster = c;
            } else if (fat_set(fh->cur_cluster, (uint16_t)c) != 0) {
                (void)fat_set(c, 0);            /* 回滚：这个簇还没挂上链 */
                return -1;
            }
            fh->cur_cluster = c;
        }

        uint32_t lba = cluster_lba(fh->cur_cluster) + cluster_off / 512U;
        uint32_t within = fh->offset % 512U;
        uint32_t n = 512U - within;

        if (n > len - done) n = len - done;

        if (read_sector(lba, sec_buf) != 0) return -1;
        memcpy(sec_buf + within, p + done, n);
        if (write_sector(lba, sec_buf) != 0) return -1;

        done += n;
        fh->offset += n;
        if (fh->offset > fh->size) fh->size = fh->offset;

        if (fh->offset % cluster_bytes == 0U) {
            uint32_t c = cluster_alloc();
            if (c == 0U) break;                 /* 盘满：已写部分保留，不预分配 */
            if (fat_set(fh->cur_cluster, (uint16_t)c) != 0) {
                (void)fat_set(c, 0);
                return -1;
            }
            fh->cur_cluster = c;
        }
    }
    return (int)done;
}

int fat16_close(int h)
{
    fat_handle_t *fh;
    int r = 0;

    if (h < 0 || h >= FAT16_MAX_HANDLES || !handles[h].used) return -1;
    fh = &handles[h];

    if (fh->write) {
        if (read_sector(fh->dir_lba, sec_buf) != 0) {
            r = -1;
        } else {
            uint8_t *slot = sec_buf + fh->dir_idx * 32U;
            wr16(slot + 26U, (uint32_t)(uint16_t)fh->first_cluster);
            wr32(slot + 28U, fh->size);
            if (write_sector(fh->dir_lba, sec_buf) != 0) r = -1;
        }
    }

    /* 无论持久化成败都必须还槽位：原实现失败时直接 return -1，
     * 这个 fd 槽就永久泄漏，反复失败会把 FAT16 8 个句柄耗尽。 */
    memset(fh, 0, sizeof(*fh));
    return r;
}

/* =============================================================================
 * 后端适配器：把 fat16_* 包装成 fs_backend_t
 * ============================================================================= */

/* 只接管 "fat:" / "FAT:" 前缀，其余交给后面的后端（ramfs） */
static int fat16_probe(const char *path, const char **rel)
{
    if (path == 0) return 0;
    if ((path[0] != 'f' && path[0] != 'F') ||
        (path[1] != 'a' && path[1] != 'A') ||
        (path[2] != 't' && path[2] != 'T') ||
        path[3] != ':') {
        return 0;
    }
    if (rel != 0) *rel = path + 4;
    return 1;
}

static int fat16_be_open(const char *rel, uint32_t flags, uint32_t uid, uint32_t gid)
{
    (void)uid;
    (void)gid;
    if (!fat16_mounted()) return -1;
    return fat16_open(rel, flags & (FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT));
}

static int fat16_be_read(int h, void *buf, uint32_t len)
{
    return fat16_read(h, buf, len);
}

static int fat16_be_write(int h, const void *buf, uint32_t len)
{
    return fat16_write(h, buf, len);
}

static int fat16_be_close(int h)
{
    return fat16_close(h);
}

/* FAT16 句柄没有引用计数：拒绝 dup/fork 共享，避免双重关闭 */
static int fat16_be_ref(int h)
{
    (void)h;
    return -1;
}

static int fat16_be_stat(const char *rel, fs_stat_t *out)
{
    if (!fat16_mounted() || fat16_stat(rel, out) != E_OK) return -1;
    out->mode = 0;
    out->uid = 0;
    out->gid = 0;
    return 0;
}

static int fat16_be_listdir(const char *rel, uint32_t index, char *name,
                            uint32_t name_max, fs_stat_t *out)
{
    if (!fat16_mounted()) return -1;
    return (fat16_listdir(rel, index, name, name_max, out) == E_OK) ? 0 : -1;
}

static int fat16_be_mkdir(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)uid;
    (void)gid;
    if (!fat16_mounted()) return -1;
    return (fat16_mkdir(rel) == E_OK) ? 0 : -1;
}

static int fat16_be_unlink(const char *rel, uint32_t uid, uint32_t gid)
{
    (void)uid;
    (void)gid;
    if (!fat16_mounted()) return -1;
    return (fat16_unlink(rel) == E_OK) ? 0 : -1;
}

/* FAT16 没有 Unix 权限模型：uid/gid 不参与判断（VFS 层也跳过判权） */
static int fat16_be_rename(const char *oldrel, const char *newrel,
                           uint32_t uid, uint32_t gid)
{
    (void)uid;
    (void)gid;
    return fat16_rename(oldrel, newrel);
}

const fs_backend_t fat16_backend = {
    "fat16",
    fat16_probe,
    fat16_be_open,
    fat16_be_read,
    fat16_be_write,
    fat16_be_close,
    fat16_be_ref,
    fat16_be_stat,
    fat16_be_listdir,
    fat16_be_mkdir,
    fat16_be_unlink,
    fat16_be_rename
};
