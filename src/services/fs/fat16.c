/*
 * ng WP2-2: FAT16 实现（VFS 后端，L1 服务域）。
 *
 * 分层（与 ramfs.c 相同的约束）：
 *   - 只 include fat16.h -> blk.h / vfs.h / types.h；
 *   - 所有块 IO 走 struct blk_env（内核绑 RAM 盘，host 绑 fake 镜像）；
 *   - 所有失败返回**具体负 errno**：ENOENT/EINVAL/EIO/ENOSPC/EEXIST/EISDIR/
 *     ENOTDIR/ENAMETOOLONG/EBUSY/ENOMEM。
 *
 * 磁盘结构（全部按小端读）：
 *   [0]            BPB + 0x55AA
 *   [reserved]     第 1 份 FAT（fat_sectors 个扇区），之后是其余 FAT 副本
 *   [root_start]   根目录固定区域（root_entries * 32 字节）
 *   [data_start]   簇 2 开始的数据区；簇 n 的扇区 = data_start+(n-2)*spc
 *
 * 簇链：FAT 表项 0 = 空闲，2..cluster_count+1 = 下一簇，0xFFF7 = 坏簇，
 * 0xFFF8..0xFFFF = EOC。遍历用 chain_at()/chain_length()/chain_last()，
 * 三者都做“越界/坏簇/空闲/循环”检测；链比要求短或循环 -> EIO。
 *
 * 目录项：名字 11 字节（8+3，空格填充，大写），attr 0x0F = LFN 项（跳过），
 * attr & 0x08 = 卷标（跳过），首字节 0x00 = 目录结束，0xE5 = 已删除。
 * 节点身份 = (父目录标识, 目录项在该目录流里的字节偏移)，改名不改变身份；
 * 打开中的节点 refs>0，不会被回收，unlink 打开中的文件返回 EBUSY。
 *
 * 限制（与 WP2-2 出口一致）：只做 8.3；不做 rmdir / 跨目录改名 / LFN /
 * 时间戳 / 权限模型；不支持 FAT12/FAT32（BPB 必须是 FAT16 几何）。
 */
#include "fat16.h"

/* ------------------------------------------------------------------------- */
/* 极简内存/字符串（freestanding，不依赖 libc）                              */
/* ------------------------------------------------------------------------- */

static void mem_copy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void mem_zero(void *dst, u64 n)
{
    u8 *d = (u8 *)dst;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

static int mem_eq(const void *a, const void *b, u64 n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    u64 i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

static u32 str_len(const char *s)
{
    u32 n = 0;

    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* 小端读写                                                                  */
/* ------------------------------------------------------------------------- */

static u32 rd16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void wr16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* 块设备 IO                                                                 */
/* ------------------------------------------------------------------------- */

static int io_read(struct fat16 *fs, u32 sector, void *buf)
{
    if (fs->blk.read == NULL || (u64)sector >= fs->blk.sectors) {
        fs->st.eio++;
        return VFS_EIO;
    }
    if (fs->blk.read(fs->blk.ctx, (u64)sector, buf, 1) != 0) {
        fs->st.eio++;
        return VFS_EIO;
    }
    return VFS_OK;
}

static int io_write(struct fat16 *fs, u32 sector, const void *buf)
{
    if (fs->blk.write == NULL || (u64)sector >= fs->blk.sectors) {
        fs->st.eio++;
        return VFS_EIO;
    }
    if (fs->blk.write(fs->blk.ctx, (u64)sector, buf, 1) != 0) {
        fs->st.eio++;
        return VFS_EIO;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* FAT 表单扇区缓存                                                          */
/* ------------------------------------------------------------------------- */

/* 把 FAT 内第 index 个扇区读进缓存（命中直接返回）。 */
static int fat_cache_load(struct fat16 *fs, u32 index)
{
    if (fs->fat_cache_valid != 0 && fs->fat_cache_index == index) {
        return VFS_OK;
    }
    if (index >= fs->fat_sectors) {
        fs->st.eio++;
        return VFS_EIO;
    }
    if (io_read(fs, fs->fat_start + index, fs->fat_cache) != VFS_OK) {
        fs->fat_cache_valid = 0;
        return VFS_EIO;
    }
    fs->fat_cache_index = index;
    fs->fat_cache_valid = 1;
    fs->fat_cache_dirty = 0;
    return VFS_OK;
}

/* 把缓存扇区写回所有 FAT 副本（FAT16 的冗余表必须保持一致）。 */
static int fat_cache_flush(struct fat16 *fs)
{
    u32 f;

    if (fs->fat_cache_valid == 0 || fs->fat_cache_dirty == 0) {
        return VFS_OK;
    }
    for (f = 0; f < fs->num_fats; f++) {
        u32 sector = fs->fat_start + f * fs->fat_sectors + fs->fat_cache_index;

        if (io_write(fs, sector, fs->fat_cache) != VFS_OK) {
            return VFS_EIO;
        }
    }
    fs->fat_cache_dirty = 0;
    return VFS_OK;
}

static int fat_get(struct fat16 *fs, u32 cluster, u32 *value)
{
    u32 off;
    u32 index;

    if (cluster > fs->cluster_count + 1u) {
        fs->st.eio++;
        return VFS_EIO;
    }
    off = cluster * 2u;
    index = off / FAT16_SECTOR_SIZE;
    if (fat_cache_load(fs, index) != VFS_OK) {
        return VFS_EIO;
    }
    *value = rd16(fs->fat_cache + (off % FAT16_SECTOR_SIZE));
    return VFS_OK;
}

static int fat_set(struct fat16 *fs, u32 cluster, u32 value)
{
    u32 off;
    u32 index;

    if (cluster > fs->cluster_count + 1u) {
        fs->st.eio++;
        return VFS_EIO;
    }
    off = cluster * 2u;
    index = off / FAT16_SECTOR_SIZE;
    if (fat_cache_load(fs, index) != VFS_OK) {
        return VFS_EIO;
    }
    wr16(fs->fat_cache + (off % FAT16_SECTOR_SIZE), value & 0xFFFFu);
    fs->fat_cache_dirty = 1;
    return fat_cache_flush(fs);
}

/* ------------------------------------------------------------------------- */
/* 几何 / 簇                                                                 */
/* ------------------------------------------------------------------------- */

static u32 max_cluster(const struct fat16 *fs)
{
    return fs->cluster_count + 1u;
}

/* 数据簇的起始扇区；调用方保证 2 <= cluster <= max_cluster。 */
static u32 cluster_lba(const struct fat16 *fs, u32 cluster)
{
    return fs->data_start + (cluster - 2u) * fs->sec_per_cluster;
}

static int cluster_zero(struct fat16 *fs, u32 cluster)
{
    u8 zero[FAT16_SECTOR_SIZE];
    u32 s;

    mem_zero(zero, (u64)sizeof(zero));
    for (s = 0; s < fs->sec_per_cluster; s++) {
        if (io_write(fs, cluster_lba(fs, cluster) + s, zero) != VFS_OK) {
            return VFS_EIO;
        }
    }
    return VFS_OK;
}

/*
 * 分配一个空闲簇：FAT 标记 EOC + 数据清零。失败时把刚占用的 FAT 项还回去
 * （未清零成功也一样），不留“FAT 已占用、没人引用”的泄漏簇。
 */
static int cluster_alloc(struct fat16 *fs, u32 *out)
{
    u32 total = fs->cluster_count;
    u32 i;

    if (total == 0) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    for (i = 0; i < total; i++) {
        u32 cluster = 2u + ((fs->last_alloc + i) % total);
        u32 value = 0;

        if (fat_get(fs, cluster, &value) != VFS_OK) {
            return VFS_EIO;
        }
        if (value != FAT16_CLUSTER_FREE) {
            continue;
        }
        if (fat_set(fs, cluster, FAT16_CLUSTER_EOC_MARK) != VFS_OK) {
            return VFS_EIO;
        }
        if (cluster_zero(fs, cluster) != VFS_OK) {
            (void)fat_set(fs, cluster, FAT16_CLUSTER_FREE);
            return VFS_EIO;
        }
        fs->last_alloc = cluster;
        fs->st.allocs++;
        *out = cluster;
        return VFS_OK;
    }
    fs->st.enospc++;
    return VFS_ENOSPC;
}

/* 释放整条链；遇到坏项/越界/循环就停下（不追进 FAT 外）。 */
static int chain_free(struct fat16 *fs, u32 first)
{
    u32 cluster = first;
    u32 guard = 0;
    u32 max = max_cluster(fs);

    if (first == 0) {
        return VFS_OK;
    }
    if (first < 2 || first > max) {
        fs->st.eio++;
        return VFS_EIO;
    }
    while (cluster >= 2 && cluster <= max) {
        u32 next = 0;

        if (fat_get(fs, cluster, &next) != VFS_OK) {
            return VFS_EIO;
        }
        if (fat_set(fs, cluster, FAT16_CLUSTER_FREE) != VFS_OK) {
            return VFS_EIO;
        }
        fs->st.frees++;
        if (next >= FAT16_CLUSTER_EOC || next == FAT16_CLUSTER_BAD ||
            next == FAT16_CLUSTER_FREE) {
            break;
        }
        if (next < 2 || next > max) {
            break; /* 损坏的链：当前簇已释放，停下来 */
        }
        if (++guard > fs->cluster_count) {
            fs->st.chain_loops++;
            return VFS_EIO;
        }
        cluster = next;
    }
    return VFS_OK;
}

/*
 * 第 index 个簇（0 = 首簇）。返回 1 = 找到，0 = 链比 index 短（正常结束），
 * 负数 = 链损坏 / 循环 / 越界（EIO）。
 */
static int chain_at(struct fat16 *fs, u32 first, u32 index, u32 *out)
{
    u32 cluster = first;
    u32 i;
    u32 max = max_cluster(fs);

    if (first < 2 || first > max) {
        fs->st.chain_short++;
        return 0;
    }
    if (index >= fs->cluster_count) {
        fs->st.chain_loops++;
        return VFS_EIO;
    }
    for (i = 0; i < index; i++) {
        u32 next = 0;

        if (fat_get(fs, cluster, &next) != VFS_OK) {
            return VFS_EIO;
        }
        if (next >= FAT16_CLUSTER_EOC) {
            fs->st.chain_short++;
            return 0;
        }
        if (next == FAT16_CLUSTER_BAD || next == FAT16_CLUSTER_FREE ||
            next < 2 || next > max || next == first) {
            if (next == first) {
                fs->st.chain_loops++;
            } else {
                fs->st.eio++;
            }
            return VFS_EIO;
        }
        cluster = next;
    }
    *out = cluster;
    return 1;
}

/* 链长（簇数）。返回负 errno（坏项/循环/越界）。 */
static int chain_length(struct fat16 *fs, u32 first, u32 *len)
{
    u32 cluster = first;
    u32 count = 0;
    u32 max = max_cluster(fs);

    if (first < 2 || first > max) {
        fs->st.eio++;
        return VFS_EIO;
    }
    for (;;) {
        u32 next = 0;

        count++;
        if (count > fs->cluster_count) {
            fs->st.chain_loops++;
            return VFS_EIO;
        }
        if (fat_get(fs, cluster, &next) != VFS_OK) {
            return VFS_EIO;
        }
        if (next >= FAT16_CLUSTER_EOC) {
            break;
        }
        if (next == FAT16_CLUSTER_BAD || next == FAT16_CLUSTER_FREE ||
            next < 2 || next > max) {
            fs->st.eio++;
            return VFS_EIO;
        }
        cluster = next;
    }
    *len = count;
    return VFS_OK;
}

/* 链尾簇；链损坏 -> EIO。 */
static int chain_last(struct fat16 *fs, u32 first, u32 *out)
{
    u32 cluster = first;
    u32 guard = 0;
    u32 max = max_cluster(fs);

    if (first < 2 || first > max) {
        fs->st.eio++;
        return VFS_EIO;
    }
    for (;;) {
        u32 next = 0;

        if (fat_get(fs, cluster, &next) != VFS_OK) {
            return VFS_EIO;
        }
        if (next >= FAT16_CLUSTER_EOC) {
            *out = cluster;
            return VFS_OK;
        }
        if (next == FAT16_CLUSTER_BAD || next == FAT16_CLUSTER_FREE ||
            next < 2 || next > max) {
            fs->st.eio++;
            return VFS_EIO;
        }
        if (++guard > fs->cluster_count) {
            fs->st.chain_loops++;
            return VFS_EIO;
        }
        cluster = next;
    }
}

/* 把 last 之后的链释放，并把 last 标成 EOC。 */
static int chain_free_after(struct fat16 *fs, u32 last)
{
    u32 next = 0;

    if (fat_get(fs, last, &next) != VFS_OK) {
        return VFS_EIO;
    }
    if (next >= 2 && next <= max_cluster(fs)) {
        if (chain_free(fs, next) != VFS_OK) {
            return VFS_EIO;
        }
    }
    return fat_set(fs, last, FAT16_CLUSTER_EOC_MARK);
}

/* ------------------------------------------------------------------------- */
/* 8.3 名字                                                                  */
/* ------------------------------------------------------------------------- */

static char upper_ascii(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static int name_char_ok(char c)
{
    if (c <= 0x20 || c >= 0x7F) {
        return 0;
    }
    switch (c) {
    case '"':
    case '*':
    case '+':
    case ',':
    case '/':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '[':
    case '\\':
    case ']':
    case '|':
        return 0;
    default:
        return 1;
    }
}

/*
 * 名字 -> 11 字节 8.3（大写、空格填充）。
 *   0            = OK
 *   ENAMETOOLONG = base > 8 或 ext > 3
 *   EINVAL       = 空名字 / 空 base / 非法字符 / 多个点
 */
static int name_to_83(const char *name, u32 len, u8 out[FAT16_NAME83_LEN])
{
    u8 tmp[FAT16_NAME83_LEN];
    u32 i = 0;
    u32 base = 0;
    u32 ext = 0;

    mem_zero(tmp, (u64)sizeof(tmp));
    for (i = 0; i < FAT16_NAME83_LEN; i++) {
        tmp[i] = (u8)' ';
    }
    if (name == NULL || len == 0) {
        return VFS_EINVAL;
    }
    i = 0;
    while (i < len && name[i] != '.') {
        char c = name[i];

        if (base >= 8u) {
            return VFS_ENAMETOOLONG;
        }
        if (name_char_ok(c) == 0) {
            return VFS_EINVAL;
        }
        tmp[base++] = (u8)upper_ascii(c);
        i++;
    }
    if (base == 0) {
        return VFS_EINVAL;
    }
    if (i < len && name[i] == '.') {
        i++;
        while (i < len) {
            char c = name[i];

            if (ext >= 3u) {
                return VFS_ENAMETOOLONG;
            }
            if (name_char_ok(c) == 0) {
                return VFS_EINVAL;
            }
            tmp[8u + ext] = (u8)upper_ascii(c);
            ext++;
            i++;
        }
    }
    mem_copy(out, tmp, (u64)sizeof(tmp));
    return VFS_OK;
}

/* 11 字节 8.3 -> 可显示名字（去掉空格填充，最多 12 + NUL）。 */
static void name83_to_display(const u8 raw[FAT16_NAME83_LEN], char *out)
{
    u32 n = 0;
    u32 i;

    for (i = 0; i < 8u; i++) {
        if (raw[i] != (u8)' ') {
            out[n++] = (char)raw[i];
        }
    }
    if (raw[8] != (u8)' ') {
        out[n++] = '.';
        for (i = 8u; i < 11u; i++) {
            if (raw[i] != (u8)' ') {
                out[n++] = (char)raw[i];
            }
        }
    }
    out[n] = '\0';
}

/* 目录项 32 字节序列化（FAT16：cluster_hi 必须 0）。 */
static void entry_make(u8 out[FAT16_ENTRY_SIZE],
                       const u8 name[FAT16_NAME83_LEN], u8 attr, u32 first,
                       u32 size)
{
    mem_zero(out, FAT16_ENTRY_SIZE);
    mem_copy(out, name, FAT16_NAME83_LEN);
    out[11] = attr;
    out[12] = 0; /* ntres */
    wr16(out + 20, 0); /* cluster_hi */
    wr16(out + 26, first & 0xFFFFu);
    wr32(out + 28, size);
}

static int entry_is_dot(const u8 raw[FAT16_ENTRY_SIZE])
{
    static const u8 dot[FAT16_NAME83_LEN] = {'.', ' ', ' ', ' ', ' ', ' ',
                                             ' ', ' ', ' ', ' ', ' '};
    static const u8 dotdot[FAT16_NAME83_LEN] = {'.', '.', ' ', ' ', ' ', ' ',
                                                ' ', ' ', ' ', ' ', ' '};

    return mem_eq(raw, dot, FAT16_NAME83_LEN) ||
           mem_eq(raw, dotdot, FAT16_NAME83_LEN);
}

/* ------------------------------------------------------------------------- */
/* 目录项访问（根目录固定区 / 子目录簇链）                                   */
/* ------------------------------------------------------------------------- */

/* 读目录流 byte_off 处的 32 字节目录项：1=读到，0=目录结束，负 errno。 */
static int dir_read_entry(struct fat16 *fs, u32 dir_cluster, u32 byte_off,
                          u8 out[FAT16_ENTRY_SIZE])
{
    u8 sector[FAT16_SECTOR_SIZE];
    u32 sector_no;
    u32 within;
    u32 cluster;
    int rc;

    if (dir_cluster == FAT16_ROOT_DIR) {
        u64 total = (u64)fs->root_dir_sectors * FAT16_SECTOR_SIZE;

        if ((u64)byte_off + FAT16_ENTRY_SIZE > total) {
            return 0;
        }
        sector_no = fs->root_start + byte_off / FAT16_SECTOR_SIZE;
        within = byte_off % FAT16_SECTOR_SIZE;
    } else {
        u32 cindex = byte_off / fs->bytes_per_cluster;

        rc = chain_at(fs, dir_cluster, cindex, &cluster);
        if (rc == 0) {
            return 0; /* 子目录簇链结束 */
        }
        if (rc < 0) {
            return VFS_EIO;
        }
        within = byte_off % fs->bytes_per_cluster;
        sector_no = cluster_lba(fs, cluster) + within / FAT16_SECTOR_SIZE;
        within = within % FAT16_SECTOR_SIZE;
    }
    if (io_read(fs, sector_no, sector) != VFS_OK) {
        return VFS_EIO;
    }
    mem_copy(out, sector + within, FAT16_ENTRY_SIZE);
    return 1;
}

/* 写目录项（不能越过目录末尾；追加前先 dir_extend）。 */
static int dir_write_entry(struct fat16 *fs, u32 dir_cluster, u32 byte_off,
                           const u8 entry[FAT16_ENTRY_SIZE])
{
    u8 sector[FAT16_SECTOR_SIZE];
    u32 sector_no;
    u32 within;
    u32 cluster;
    int rc;

    if (dir_cluster == FAT16_ROOT_DIR) {
        u64 total = (u64)fs->root_dir_sectors * FAT16_SECTOR_SIZE;

        if ((u64)byte_off + FAT16_ENTRY_SIZE > total) {
            return VFS_EIO;
        }
        sector_no = fs->root_start + byte_off / FAT16_SECTOR_SIZE;
        within = byte_off % FAT16_SECTOR_SIZE;
    } else {
        u32 cindex = byte_off / fs->bytes_per_cluster;

        rc = chain_at(fs, dir_cluster, cindex, &cluster);
        if (rc != 1) {
            fs->st.eio++;
            return VFS_EIO;
        }
        within = byte_off % fs->bytes_per_cluster;
        sector_no = cluster_lba(fs, cluster) + within / FAT16_SECTOR_SIZE;
        within = within % FAT16_SECTOR_SIZE;
    }
    if (io_read(fs, sector_no, sector) != VFS_OK) {
        return VFS_EIO;
    }
    mem_copy(sector + within, entry, FAT16_ENTRY_SIZE);
    return io_write(fs, sector_no, sector);
}

/* 子目录追加一个簇，返回新簇首字节在目录流里的偏移（out_off）。 */
static int dir_extend(struct fat16 *fs, u32 dir_cluster, u32 *out_off)
{
    u32 len = 0;
    u32 last = 0;
    u32 fresh = 0;
    int rc;

    rc = chain_length(fs, dir_cluster, &len);
    if (rc != VFS_OK) {
        return rc;
    }
    rc = chain_last(fs, dir_cluster, &last);
    if (rc != VFS_OK) {
        return rc;
    }
    rc = cluster_alloc(fs, &fresh);
    if (rc != VFS_OK) {
        return rc;
    }
    if (fat_set(fs, last, fresh) != VFS_OK) {
        (void)fat_set(fs, fresh, FAT16_CLUSTER_FREE);
        return VFS_EIO;
    }
    *out_off = len * fs->bytes_per_cluster;
    return VFS_OK;
}

struct dir_scan {
    u32 found;
    u32 found_off;
    u8 found_entry[FAT16_ENTRY_SIZE];
    u32 have_free;
    u32 free_off;
    u32 hit_end;
    u32 end_off;
};

/*
 * 在 dir_cluster 里找 8.3 名字 want。
 *   返回 OK       = 找到（found_off/found_entry）；
 *   返回 ENOENT   = 没找到（have_free/free_off/hit_end/end_off 可用于创建）；
 *   返回负 errno = 读盘失败 / 簇链损坏。
 */
static int dir_find(struct fat16 *fs, u32 dir_cluster,
                    const u8 want[FAT16_NAME83_LEN], struct dir_scan *scan)
{
    u32 off = 0;

    mem_zero(scan, (u64)sizeof(*scan));
    for (;;) {
        u8 raw[FAT16_ENTRY_SIZE];
        u8 attr;
        int rc = dir_read_entry(fs, dir_cluster, off, raw);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            scan->hit_end = 1;
            scan->end_off = off;
            break;
        }
        if (raw[0] == 0x00u) {
            if (scan->have_free == 0) {
                scan->have_free = 1;
                scan->free_off = off;
            }
            scan->hit_end = 1;
            scan->end_off = off;
            break;
        }
        if (raw[0] == 0xE5u) {
            if (scan->have_free == 0) {
                scan->have_free = 1;
                scan->free_off = off;
            }
            off += FAT16_ENTRY_SIZE;
            continue;
        }
        attr = (u8)(raw[11] & FAT16_ATTR_MASK);
        if (attr == FAT16_ATTR_LFN) {
            off += FAT16_ENTRY_SIZE; /* LFN：跳过（不还原长名） */
            continue;
        }
        if ((attr & FAT16_ATTR_VOLUME) != 0) {
            off += FAT16_ENTRY_SIZE; /* 卷标：跳过 */
            continue;
        }
        if (mem_eq(raw, want, FAT16_NAME83_LEN)) {
            scan->found = 1;
            scan->found_off = off;
            mem_copy(scan->found_entry, raw, FAT16_ENTRY_SIZE);
            return VFS_OK;
        }
        off += FAT16_ENTRY_SIZE;
    }
    return VFS_ENOENT;
}

/* ------------------------------------------------------------------------- */
/* 节点缓存                                                                  */
/* ------------------------------------------------------------------------- */

static u32 ifmt_of(u32 type)
{
    return (type == VFS_NODE_DIR) ? VFS_S_IFDIR : VFS_S_IFREG;
}

static void fill_vnode(struct fat16 *fs, struct fat16_node *n)
{
    u32 type = ((n->attr & FAT16_ATTR_DIR) != 0) ? VFS_NODE_DIR : VFS_NODE_FILE;

    n->vnode.type = type;
    if (type == VFS_NODE_DIR) {
        n->vnode.mode = 0755u;
    } else if ((n->attr & FAT16_ATTR_READONLY) != 0) {
        n->vnode.mode = 0444u;
    } else {
        n->vnode.mode = 0644u;
    }
    n->vnode.size = (type == VFS_NODE_DIR) ? 0u : n->size;
    n->vnode.ino = n->ino;
    n->vnode.ops = fat16_ops();
    n->vnode.fs_ctx = fs;
    n->vnode.priv = n;
}

static struct fat16_node *node_of(struct fat16 *fs, const struct vfs_node *vn)
{
    if (fs == NULL || vn == NULL || vn->fs_ctx != fs || vn->priv == NULL) {
        return NULL;
    }
    return (struct fat16_node *)vn->priv;
}

static u32 dir_id_of(const struct fat16_node *n)
{
    return n->is_root ? FAT16_ROOT_DIR : n->first_cluster;
}

static struct fat16_node *node_find(struct fat16 *fs, u32 parent, u32 entry_off)
{
    u32 i;

    for (i = 1; i < FAT16_MAX_NODES; i++) {
        struct fat16_node *n = &fs->nodes[i];

        if (n->used != 0 && n->parent == parent &&
            n->entry_off == entry_off) {
            return n;
        }
    }
    return NULL;
}

static void node_refresh(struct fat16_node *n, const u8 raw[FAT16_ENTRY_SIZE])
{
    n->attr = (u8)(raw[11] & FAT16_ATTR_MASK);
    n->first_cluster = rd16(raw + 26) | ((u32)rd16(raw + 20) << 16);
    n->size = rd32(raw + 28);
    n->deleted = 0;
    name83_to_display(raw, n->name);
}

/*
 * 取得节点：同 (parent, entry_off) 复用；否则挑空闲槽，槽满时回收
 * refs==0 且不是调用方正在使用的 keep 节点的槽。返回 NULL = 节点池满。
 */
static struct fat16_node *node_get(struct fat16 *fs, u32 parent, u32 entry_off,
                                   const u8 raw[FAT16_ENTRY_SIZE],
                                   const struct vfs_node *keep)
{
    struct fat16_node *n = node_find(fs, parent, entry_off);
    u32 slot = FAT16_MAX_NODES;
    u32 i;

    if (n != NULL) {
        node_refresh(n, raw);
        fill_vnode(fs, n);
        return n;
    }
    for (i = 1; i < FAT16_MAX_NODES; i++) {
        if (fs->nodes[i].used == 0) {
            slot = i;
            break;
        }
    }
    if (slot == FAT16_MAX_NODES) {
        for (i = 1; i < FAT16_MAX_NODES; i++) {
            struct fat16_node *cand = &fs->nodes[i];

            if (cand->used == 0 || cand->is_root != 0 || cand->refs != 0) {
                continue;
            }
            if (keep != NULL && &cand->vnode == keep) {
                continue;
            }
            slot = i;
            break;
        }
    }
    if (slot == FAT16_MAX_NODES) {
        fs->st.rejects++;
        return NULL;
    }
    n = &fs->nodes[slot];
    if (n->used == 0) {
        fs->st.nodes_used++;
        if (fs->st.nodes_used > fs->st.node_peak) {
            fs->st.node_peak = fs->st.nodes_used;
        }
    }
    mem_zero(n, (u64)sizeof(*n));
    n->used = 1;
    n->refs = 0;
    n->parent = parent;
    n->entry_off = entry_off;
    n->ino = ++fs->ino_seq;
    node_refresh(n, raw);
    fill_vnode(fs, n);
    return n;
}

/* 目录项被删除后回收对应节点（refs>0 时保持，等 release 再回收）。 */
static void node_drop(struct fat16 *fs, u32 parent, u32 entry_off)
{
    struct fat16_node *n = node_find(fs, parent, entry_off);

    if (n == NULL) {
        return;
    }
    n->deleted = 1;
    if (n->refs == 0) {
        n->used = 0;
        n->deleted = 0;
        if (fs->st.nodes_used > 0) {
            fs->st.nodes_used--;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 数据读写（按扇区 RMW）                                                    */
/* ------------------------------------------------------------------------- */

static int node_write_entry(struct fat16 *fs, struct fat16_node *n)
{
    u8 raw[FAT16_ENTRY_SIZE];
    int rc;

    if (n->is_root != 0) {
        return VFS_OK;
    }
    rc = dir_read_entry(fs, n->parent, n->entry_off, raw);
    if (rc != 1) {
        fs->st.eio++;
        return VFS_EIO;
    }
    wr16(raw + 20, 0);
    wr16(raw + 26, n->first_cluster & 0xFFFFu);
    wr32(raw + 28, n->size);
    return dir_write_entry(fs, n->parent, n->entry_off, raw);
}

/* 回滚 node_ensure 追加的簇，恢复 base_first/base_len。 */
static void chain_rollback(struct fat16 *fs, struct fat16_node *n,
                           u32 base_first, u32 base_len)
{
    if (base_first == 0) {
        if (n->first_cluster != 0) {
            (void)chain_free(fs, n->first_cluster);
        }
        n->first_cluster = 0;
        return;
    }
    if (base_len > 0) {
        u32 last = 0;

        if (chain_at(fs, base_first, base_len - 1u, &last) == 1) {
            (void)chain_free_after(fs, last);
        }
    }
    n->first_cluster = base_first;
}

/* 保证节点的簇链覆盖 end_off 字节（只追加）；失败回滚到调用前链长。 */
static int node_ensure(struct fat16 *fs, struct fat16_node *n, u32 end_off)
{
    u32 need;
    u32 base_first = n->first_cluster;
    u32 base_len = 0;
    u32 len = 0;
    int rc;

    if (end_off == 0) {
        return VFS_OK;
    }
    need = (end_off + fs->bytes_per_cluster - 1u) / fs->bytes_per_cluster;
    if (need > fs->cluster_count) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    if (base_first != 0) {
        rc = chain_length(fs, base_first, &base_len);
        if (rc != VFS_OK) {
            return rc;
        }
    }
    if (base_len >= need) {
        return VFS_OK;
    }
    len = base_len;
    if (len == 0) {
        u32 first = 0;

        rc = cluster_alloc(fs, &first);
        if (rc != VFS_OK) {
            return rc;
        }
        n->first_cluster = first;
        len = 1;
    }
    while (len < need) {
        u32 last = 0;
        u32 fresh = 0;

        rc = chain_last(fs, n->first_cluster, &last);
        if (rc != VFS_OK) {
            chain_rollback(fs, n, base_first, base_len);
            return rc;
        }
        rc = cluster_alloc(fs, &fresh);
        if (rc != VFS_OK) {
            chain_rollback(fs, n, base_first, base_len);
            return rc;
        }
        if (fat_set(fs, last, fresh) != VFS_OK) {
            (void)fat_set(fs, fresh, FAT16_CLUSTER_FREE);
            chain_rollback(fs, n, base_first, base_len);
            return VFS_EIO;
        }
        len++;
    }
    return VFS_OK;
}

static int zero_range(struct fat16 *fs, struct fat16_node *n, u32 from, u32 to)
{
    u8 sector[FAT16_SECTOR_SIZE];

    while (from < to) {
        u32 cindex = from / fs->bytes_per_cluster;
        u32 within = from % fs->bytes_per_cluster;
        u32 cluster = 0;
        u32 sector_no;
        u32 in_sector;
        u32 chunk;
        int rc = chain_at(fs, n->first_cluster, cindex, &cluster);

        if (rc != 1) {
            fs->st.eio++;
            return VFS_EIO;
        }
        sector_no = cluster_lba(fs, cluster) + within / FAT16_SECTOR_SIZE;
        in_sector = within % FAT16_SECTOR_SIZE;
        chunk = to - from;
        if (chunk > FAT16_SECTOR_SIZE - in_sector) {
            chunk = FAT16_SECTOR_SIZE - in_sector;
        }
        if (io_read(fs, sector_no, sector) != VFS_OK) {
            return VFS_EIO;
        }
        mem_zero(sector + in_sector, chunk);
        if (io_write(fs, sector_no, sector) != VFS_OK) {
            return VFS_EIO;
        }
        from += chunk;
    }
    return VFS_OK;
}

static int copy_into_file(struct fat16 *fs, struct fat16_node *n, u32 off,
                          const u8 *src, u32 len)
{
    u8 sector[FAT16_SECTOR_SIZE];
    u32 done = 0;

    while (done < len) {
        u32 pos = off + done;
        u32 cindex = pos / fs->bytes_per_cluster;
        u32 within = pos % fs->bytes_per_cluster;
        u32 cluster = 0;
        u32 sector_no;
        u32 in_sector;
        u32 chunk;
        int rc = chain_at(fs, n->first_cluster, cindex, &cluster);

        if (rc != 1) {
            fs->st.eio++;
            return VFS_EIO;
        }
        sector_no = cluster_lba(fs, cluster) + within / FAT16_SECTOR_SIZE;
        in_sector = within % FAT16_SECTOR_SIZE;
        chunk = len - done;
        if (chunk > FAT16_SECTOR_SIZE - in_sector) {
            chunk = FAT16_SECTOR_SIZE - in_sector;
        }
        if (in_sector == 0 && chunk == FAT16_SECTOR_SIZE) {
            if (io_write(fs, sector_no, src + done) != VFS_OK) {
                return VFS_EIO;
            }
        } else {
            if (io_read(fs, sector_no, sector) != VFS_OK) {
                return VFS_EIO;
            }
            mem_copy(sector + in_sector, src + done, chunk);
            if (io_write(fs, sector_no, sector) != VFS_OK) {
                return VFS_EIO;
            }
        }
        done += chunk;
    }
    return VFS_OK;
}

/* ------------------------------------------------------------------------- */
/* ops：目录                                                                  */
/* ------------------------------------------------------------------------- */

static int fat16_lookup(void *ctx, struct vfs_node *dirvn, const char *name,
                        u32 len, struct vfs_node **out)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *dn = node_of(fs, dirvn);
    struct dir_scan scan;
    u8 want[FAT16_NAME83_LEN];
    struct fat16_node *n;
    u32 parent;
    int rc;

    if (fs == NULL || dn == NULL || name == NULL || out == NULL) {
        return VFS_EINVAL;
    }
    if ((dn->attr & FAT16_ATTR_DIR) == 0) {
        return VFS_ENOTDIR;
    }
    if (name_to_83(name, len, want) != VFS_OK) {
        return VFS_ENOENT; /* 8.3 里不存在这个名字 */
    }
    parent = dir_id_of(dn);
    rc = dir_find(fs, parent, want, &scan);
    if (rc == VFS_ENOENT) {
        return VFS_ENOENT;
    }
    if (rc != VFS_OK) {
        return rc;
    }
    n = node_get(fs, parent, scan.found_off, scan.found_entry, dirvn);
    if (n == NULL) {
        return VFS_ENOSPC;
    }
    *out = &n->vnode;
    return VFS_OK;
}

static int create_common(struct fat16 *fs, struct vfs_node *dirvn,
                         const char *name, u32 len, u32 type,
                         struct vfs_node **out)
{
    struct fat16_node *dn = node_of(fs, dirvn);
    struct dir_scan scan;
    u8 want[FAT16_NAME83_LEN];
    u8 raw[FAT16_ENTRY_SIZE];
    u32 parent;
    u32 entry_off;
    u32 first = 0;
    struct fat16_node *n;
    int rc;

    if (fs == NULL || dn == NULL || name == NULL || out == NULL) {
        return VFS_EINVAL;
    }
    if ((dn->attr & FAT16_ATTR_DIR) == 0) {
        return VFS_ENOTDIR;
    }
    if (type != VFS_NODE_FILE && type != VFS_NODE_DIR) {
        return VFS_EINVAL; /* FAT16 不支持符号链接 */
    }
    rc = name_to_83(name, len, want);
    if (rc != VFS_OK) {
        fs->st.rejects++;
        return rc;
    }
    parent = dir_id_of(dn);
    rc = dir_find(fs, parent, want, &scan);
    if (rc == VFS_OK) {
        return VFS_EEXIST;
    }
    if (rc != VFS_ENOENT) {
        return rc;
    }
    if (scan.have_free != 0) {
        entry_off = scan.free_off;
    } else {
        if (parent == FAT16_ROOT_DIR) {
            fs->st.enospc++;
            return VFS_ENOSPC;
        }
        rc = dir_extend(fs, parent, &entry_off);
        if (rc != VFS_OK) {
            return rc;
        }
    }
    if (type == VFS_NODE_DIR) {
        static const u8 dot_name[FAT16_NAME83_LEN] = {'.', ' ', ' ', ' ', ' ',
                                                      ' ', ' ', ' ', ' ', ' ',
                                                      ' '};
        static const u8 dotdot_name[FAT16_NAME83_LEN] = {
            '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        u32 dot_dot = (parent == FAT16_ROOT_DIR) ? 0u : parent;

        rc = cluster_alloc(fs, &first);
        if (rc != VFS_OK) {
            return rc;
        }
        entry_make(raw, dot_name, FAT16_ATTR_DIR, first, 0);
        if (dir_write_entry(fs, first, 0, raw) != VFS_OK) {
            (void)chain_free(fs, first);
            return VFS_EIO;
        }
        entry_make(raw, dotdot_name, FAT16_ATTR_DIR, dot_dot, 0);
        if (dir_write_entry(fs, first, FAT16_ENTRY_SIZE, raw) != VFS_OK) {
            (void)chain_free(fs, first);
            return VFS_EIO;
        }
    }
    entry_make(raw, want,
               (type == VFS_NODE_DIR) ? (u8)FAT16_ATTR_DIR
                                      : (u8)FAT16_ATTR_ARCHIVE,
               first, 0);
    if (dir_write_entry(fs, parent, entry_off, raw) != VFS_OK) {
        if (first != 0) {
            (void)chain_free(fs, first);
        }
        return VFS_EIO;
    }
    n = node_get(fs, parent, entry_off, raw, dirvn);
    if (n == NULL) {
        u8 del[FAT16_ENTRY_SIZE];

        mem_copy(del, raw, FAT16_ENTRY_SIZE);
        del[0] = 0xE5u;
        (void)dir_write_entry(fs, parent, entry_off, del);
        if (first != 0) {
            (void)chain_free(fs, first);
        }
        return VFS_ENOSPC;
    }
    fs->st.creates++;
    *out = &n->vnode;
    return VFS_OK;
}

static int fat16_create(void *ctx, struct vfs_node *dir, const char *name,
                        u32 len, u32 type, u32 mode, struct vfs_node **out)
{
    (void)mode; /* FAT16 没有 Unix 权限位；read-only 属性由别处设置 */
    return create_common((struct fat16 *)ctx, dir, name, len, type, out);
}

static int fat16_unlink(void *ctx, struct vfs_node *dirvn, const char *name,
                        u32 len)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *dn = node_of(fs, dirvn);
    struct dir_scan scan;
    struct fat16_node *n;
    u8 want[FAT16_NAME83_LEN];
    u8 raw[FAT16_ENTRY_SIZE];
    u32 parent;
    u32 first;
    int rc;

    if (fs == NULL || dn == NULL || name == NULL) {
        return VFS_EINVAL;
    }
    if ((dn->attr & FAT16_ATTR_DIR) == 0) {
        return VFS_ENOTDIR;
    }
    rc = name_to_83(name, len, want);
    if (rc != VFS_OK) {
        return VFS_ENOENT;
    }
    parent = dir_id_of(dn);
    rc = dir_find(fs, parent, want, &scan);
    if (rc == VFS_ENOENT) {
        return VFS_ENOENT;
    }
    if (rc != VFS_OK) {
        return rc;
    }
    mem_copy(raw, scan.found_entry, FAT16_ENTRY_SIZE);
    if ((raw[11] & FAT16_ATTR_DIR) != 0) {
        return VFS_EISDIR;
    }
    n = node_find(fs, parent, scan.found_off);
    if (n != NULL && n->refs != 0) {
        fs->st.busy++;
        return VFS_EBUSY;
    }
    first = rd16(raw + 26) | ((u32)rd16(raw + 20) << 16);
    raw[0] = 0xE5u;
    if (dir_write_entry(fs, parent, scan.found_off, raw) != VFS_OK) {
        return VFS_EIO;
    }
    node_drop(fs, parent, scan.found_off);
    if (chain_free(fs, first) != VFS_OK) {
        return VFS_EIO;
    }
    fs->st.deletes++;
    return VFS_OK;
}

static int fat16_rename(void *ctx, struct vfs_node *olddir,
                        const char *oldname, u32 oldlen,
                        struct vfs_node *newdir, const char *newname,
                        u32 newlen)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *odn = node_of(fs, olddir);
    struct fat16_node *ndn = node_of(fs, newdir);
    struct dir_scan scan;
    struct fat16_node *n;
    u8 old83[FAT16_NAME83_LEN];
    u8 new83[FAT16_NAME83_LEN];
    u8 raw[FAT16_ENTRY_SIZE];
    u32 parent;
    int rc;

    if (fs == NULL || odn == NULL || ndn == NULL || oldname == NULL ||
        newname == NULL) {
        return VFS_EINVAL;
    }
    if ((odn->attr & FAT16_ATTR_DIR) == 0 ||
        (ndn->attr & FAT16_ATTR_DIR) == 0) {
        return VFS_ENOTDIR;
    }
    if (dir_id_of(odn) != dir_id_of(ndn)) {
        return VFS_EINVAL; /* 只支持同一目录内改名 */
    }
    rc = name_to_83(oldname, oldlen, old83);
    if (rc != VFS_OK) {
        return VFS_ENOENT;
    }
    rc = name_to_83(newname, newlen, new83);
    if (rc != VFS_OK) {
        fs->st.rejects++;
        return rc;
    }
    parent = dir_id_of(odn);
    rc = dir_find(fs, parent, old83, &scan);
    if (rc == VFS_ENOENT) {
        return VFS_ENOENT;
    }
    if (rc != VFS_OK) {
        return rc;
    }
    mem_copy(raw, scan.found_entry, FAT16_ENTRY_SIZE);
    if ((raw[11] & FAT16_ATTR_DIR) != 0) {
        return VFS_EISDIR;
    }
    if (mem_eq(old83, new83, FAT16_NAME83_LEN)) {
        fs->st.renames++;
        return VFS_OK; /* 大小写不同但 8.3 相同：视为同一项 */
    }
    {
        struct dir_scan other;

        rc = dir_find(fs, parent, new83, &other);
        if (rc == VFS_OK) {
            return VFS_EEXIST;
        }
        if (rc != VFS_ENOENT) {
            return rc;
        }
    }
    mem_copy(raw, new83, FAT16_NAME83_LEN);
    if (dir_write_entry(fs, parent, scan.found_off, raw) != VFS_OK) {
        return VFS_EIO;
    }
    n = node_find(fs, parent, scan.found_off);
    if (n != NULL) {
        name83_to_display(new83, n->name);
        fill_vnode(fs, n);
    }
    fs->st.renames++;
    return VFS_OK;
}

static int fat16_readdir(void *ctx, struct vfs_node *dirvn, u32 index,
                         char *name, u32 name_max, struct vfs_stat *st)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *dn = node_of(fs, dirvn);
    u32 parent;
    u32 off = 0;
    u32 seen = 0;

    if (fs == NULL || dn == NULL || name == NULL || st == NULL) {
        return VFS_EINVAL;
    }
    if ((dn->attr & FAT16_ATTR_DIR) == 0) {
        return VFS_ENOTDIR;
    }
    parent = dir_id_of(dn);
    for (;;) {
        u8 raw[FAT16_ENTRY_SIZE];
        u8 attr;
        char display[FAT16_DISPLAY_MAX];
        u32 nlen;
        int rc = dir_read_entry(fs, parent, off, raw);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return 0;
        }
        if (raw[0] == 0x00u) {
            return 0;
        }
        if (raw[0] == 0xE5u) {
            off += FAT16_ENTRY_SIZE;
            continue;
        }
        attr = (u8)(raw[11] & FAT16_ATTR_MASK);
        if (attr == FAT16_ATTR_LFN || (attr & FAT16_ATTR_VOLUME) != 0 ||
            entry_is_dot(raw) != 0) {
            off += FAT16_ENTRY_SIZE;
            continue;
        }
        if (seen == index) {
            name83_to_display(raw, display);
            nlen = str_len(display);
            if (nlen + 1u > name_max) {
                return VFS_ENAMETOOLONG;
            }
            mem_copy(name, display, (u64)nlen);
            name[nlen] = '\0';
            st->type = ((attr & FAT16_ATTR_DIR) != 0) ? VFS_NODE_DIR
                                                      : VFS_NODE_FILE;
            st->mode = ifmt_of(st->type) |
                       (((attr & FAT16_ATTR_DIR) != 0)
                            ? 0755u
                            : (((attr & FAT16_ATTR_READONLY) != 0) ? 0444u
                                                                  : 0644u));
            st->size = ((attr & FAT16_ATTR_DIR) != 0) ? 0u : rd32(raw + 28);
            st->ino = ((u64)parent << 32) | (u64)(off / FAT16_ENTRY_SIZE + 1u);
            fs->st.readdirs++;
            return 1;
        }
        seen++;
        off += FAT16_ENTRY_SIZE;
    }
}

/* ------------------------------------------------------------------------- */
/* ops：文件数据                                                              */
/* ------------------------------------------------------------------------- */

static int fat16_read(void *ctx, struct vfs_node *vn, u64 off, void *buf,
                      u64 len)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *n = node_of(fs, vn);
    u8 sector[FAT16_SECTOR_SIZE];
    u8 *dst = (u8 *)buf;
    u64 done = 0;
    u64 want;

    if (fs == NULL || n == NULL || buf == NULL) {
        return VFS_EINVAL;
    }
    if ((n->attr & FAT16_ATTR_DIR) != 0) {
        return VFS_EISDIR;
    }
    if (off >= n->size) {
        return 0;
    }
    want = n->size - off;
    if (len < want) {
        want = len;
    }
    while (done < want) {
        u64 pos = off + done;
        u32 cindex;
        u32 within;
        u32 cluster = 0;
        u32 sector_no;
        u32 in_sector;
        u32 chunk;
        int rc;

        if (pos > 0xFFFFFFFFULL) {
            fs->st.eio++;
            return VFS_EIO;
        }
        cindex = (u32)(pos / fs->bytes_per_cluster);
        within = (u32)(pos % fs->bytes_per_cluster);
        rc = chain_at(fs, n->first_cluster, cindex, &cluster);
        if (rc == 0) {
            fs->st.chain_short++;
            return VFS_EIO; /* 目录项 size 大于实际链长 */
        }
        if (rc < 0) {
            return VFS_EIO;
        }
        sector_no = cluster_lba(fs, cluster) + within / FAT16_SECTOR_SIZE;
        in_sector = within % FAT16_SECTOR_SIZE;
        chunk = (u32)(want - done);
        if (chunk > FAT16_SECTOR_SIZE - in_sector) {
            chunk = FAT16_SECTOR_SIZE - in_sector;
        }
        if (io_read(fs, sector_no, sector) != VFS_OK) {
            return VFS_EIO;
        }
        mem_copy(dst + done, sector + in_sector, chunk);
        done += chunk;
    }
    fs->st.reads++;
    return (int)done;
}

static int fat16_write(void *ctx, struct vfs_node *vn, u64 off, const void *buf,
                       u64 len)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *n = node_of(fs, vn);
    u32 end;
    int rc;

    if (fs == NULL || n == NULL || buf == NULL) {
        return VFS_EINVAL;
    }
    if ((n->attr & FAT16_ATTR_DIR) != 0) {
        return VFS_EISDIR;
    }
    if (len == 0) {
        return 0;
    }
    if (off > 0xFFFFFFFFULL || len > 0xFFFFFFFFULL - off ||
        len > 0x7FFFFFFFULL) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    end = (u32)(off + len);
    if (off > n->size) {
        rc = node_ensure(fs, n, (u32)off);
        if (rc != VFS_OK) {
            return rc;
        }
        rc = zero_range(fs, n, n->size, (u32)off);
        if (rc != VFS_OK) {
            return rc;
        }
    }
    rc = node_ensure(fs, n, end);
    if (rc != VFS_OK) {
        return rc;
    }
    rc = copy_into_file(fs, n, (u32)off, (const u8 *)buf, (u32)len);
    if (rc != VFS_OK) {
        return rc;
    }
    if (end > n->size) {
        n->size = end;
    }
    fill_vnode(fs, n);
    rc = node_write_entry(fs, n);
    if (rc != VFS_OK) {
        return VFS_EIO;
    }
    fs->st.writes++;
    return (int)len;
}

static int fat16_truncate(void *ctx, struct vfs_node *vn, u64 size)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *n = node_of(fs, vn);
    u32 old_size;
    u32 new_size;
    int rc;

    if (fs == NULL || n == NULL) {
        return VFS_EINVAL;
    }
    if ((n->attr & FAT16_ATTR_DIR) != 0) {
        return VFS_EISDIR;
    }
    if (size > 0xFFFFFFFFULL) {
        fs->st.enospc++;
        return VFS_ENOSPC;
    }
    new_size = (u32)size;
    old_size = n->size;
    if (new_size == old_size) {
        return VFS_OK;
    }
    if (new_size == 0) {
        rc = chain_free(fs, n->first_cluster);
        if (rc != VFS_OK) {
            return rc;
        }
        n->first_cluster = 0;
        n->size = 0;
    } else if (new_size < old_size) {
        u32 need = (new_size + fs->bytes_per_cluster - 1u) /
                   fs->bytes_per_cluster;
        u32 last = 0;
        u32 keep_end;
        u32 zero_to;

        rc = chain_at(fs, n->first_cluster, need - 1u, &last);
        if (rc != 1) {
            fs->st.chain_short++;
            return VFS_EIO;
        }
        keep_end = need * fs->bytes_per_cluster;
        zero_to = (old_size < keep_end) ? old_size : keep_end;
        if (new_size < zero_to) {
            rc = zero_range(fs, n, new_size, zero_to);
            if (rc != VFS_OK) {
                return rc;
            }
        }
        rc = chain_free_after(fs, last);
        if (rc != VFS_OK) {
            return rc;
        }
        n->size = new_size;
    } else {
        u32 base_first = n->first_cluster;
        u32 base_len = 0;

        if (base_first != 0) {
            rc = chain_length(fs, base_first, &base_len);
            if (rc != VFS_OK) {
                return rc;
            }
        }
        rc = node_ensure(fs, n, new_size);
        if (rc != VFS_OK) {
            return rc;
        }
        rc = zero_range(fs, n, old_size, new_size);
        if (rc != VFS_OK) {
            chain_rollback(fs, n, base_first, base_len);
            return rc;
        }
        n->size = new_size;
    }
    fill_vnode(fs, n);
    rc = node_write_entry(fs, n);
    if (rc != VFS_OK) {
        return VFS_EIO;
    }
    fs->st.truncates++;
    return VFS_OK;
}

static int fat16_retain(void *ctx, struct vfs_node *vn)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *n = node_of(fs, vn);

    if (fs == NULL || n == NULL) {
        return VFS_EINVAL;
    }
    n->refs++;
    return VFS_OK;
}

static void fat16_release(void *ctx, struct vfs_node *vn)
{
    struct fat16 *fs = (struct fat16 *)ctx;
    struct fat16_node *n = node_of(fs, vn);

    if (fs == NULL || n == NULL) {
        return;
    }
    if (n->refs > 0) {
        n->refs--;
    }
    if (n->refs == 0 && n->deleted != 0 && n->is_root == 0) {
        n->used = 0;
        n->deleted = 0;
        if (fs->st.nodes_used > 0) {
            fs->st.nodes_used--;
        }
    }
}

static const struct vfs_fs_ops g_fat16_ops = {
    .lookup = fat16_lookup,
    .create = fat16_create,
    .unlink = fat16_unlink,
    .rename = fat16_rename,
    .readdir = fat16_readdir,
    .read = fat16_read,
    .write = fat16_write,
    .truncate = fat16_truncate,
    .readlink = NULL,
    .symlink = NULL,
    .retain = fat16_retain,
    .release = fat16_release,
};

/* ------------------------------------------------------------------------- */
/* 对外：初始化 / 挂载 / 诊断                                                */
/* ------------------------------------------------------------------------- */

void fat16_init(struct fat16 *fs)
{
    u8 *raw = (u8 *)fs;
    u32 i;

    if (fs == NULL) {
        return;
    }
    for (i = 0; i < (u32)sizeof(*fs); i++) {
        raw[i] = 0;
    }
    fs->fat_cache_valid = 0;
    fs->fat_cache_index = 0;
}

static void root_init(struct fat16 *fs)
{
    struct fat16_node *n = &fs->nodes[0];

    n->used = 1;
    n->refs = 1; /* 根节点常驻，不被回收 */
    n->is_root = 1;
    n->parent = FAT16_ROOT_DIR;
    n->entry_off = 0;
    n->first_cluster = 0;
    n->size = 0;
    n->ino = 1;
    n->attr = FAT16_ATTR_DIR;
    n->deleted = 0;
    n->name[0] = '/';
    n->name[1] = '\0';
    fs->ino_seq = 1;
    fs->st.nodes_used = 1;
    fs->st.node_peak = 1;
    fill_vnode(fs, n);
}

int fat16_mount(struct fat16 *fs, const struct blk_env *blk)
{
    u8 bs[FAT16_SECTOR_SIZE];
    u32 bytes_per_sector;
    u32 sec_per_cluster;
    u32 reserved;
    u32 fats;
    u32 root_entries;
    u32 total_sectors;
    u32 fat_sectors;
    u32 root_dir_sectors;
    u32 data_start;
    u32 clusters;
    u64 need_fat_bytes;
    u64 fat_bytes;

    if (fs == NULL || blk == NULL || blk->read == NULL || blk->write == NULL) {
        return VFS_EINVAL;
    }
    if (blk->sectors == 0) {
        return VFS_EINVAL;
    }
    if (blk->read(blk->ctx, 0, bs, 1) != 0) {
        return VFS_EIO;
    }
    if (bs[510] != 0x55u || bs[511] != 0xAAu) {
        fs->st.mount_fails++;
        return VFS_EINVAL;
    }
    bytes_per_sector = rd16(bs + 11);
    sec_per_cluster = bs[13];
    reserved = rd16(bs + 14);
    fats = bs[16];
    root_entries = rd16(bs + 17);
    total_sectors = rd16(bs + 19);
    if (total_sectors == 0) {
        total_sectors = rd32(bs + 32);
    }
    fat_sectors = rd16(bs + 22);

    if (bytes_per_sector != FAT16_SECTOR_SIZE ||
        sec_per_cluster == 0 ||
        sec_per_cluster > FAT16_MAX_SECTORS_PER_CLUSTER ||
        (sec_per_cluster & (sec_per_cluster - 1u)) != 0 ||
        sec_per_cluster * FAT16_SECTOR_SIZE > FAT16_MAX_CLUSTER_BYTES ||
        reserved == 0 || fats == 0 || fats > 4 || root_entries == 0 ||
        root_entries > FAT16_MAX_ROOT_ENTRIES || fat_sectors == 0 ||
        total_sectors == 0) {
        fs->st.mount_fails++;
        return VFS_EINVAL;
    }
    if ((u64)total_sectors > blk->sectors) {
        fs->st.mount_fails++;
        return VFS_EINVAL; /* BPB 声明的盘比设备大 */
    }
    root_dir_sectors =
        (root_entries * FAT16_ENTRY_SIZE + FAT16_SECTOR_SIZE - 1u) /
        FAT16_SECTOR_SIZE;
    data_start = reserved + fats * fat_sectors + root_dir_sectors;
    if (data_start >= total_sectors) {
        fs->st.mount_fails++;
        return VFS_EINVAL;
    }
    clusters = (total_sectors - data_start) / sec_per_cluster;
    if (clusters == 0 || clusters > FAT16_MAX_CLUSTERS) {
        fs->st.mount_fails++;
        return VFS_EINVAL;
    }
    need_fat_bytes = ((u64)clusters + 2u) * 2u;
    fat_bytes = (u64)fat_sectors * FAT16_SECTOR_SIZE;
    if (fat_bytes < need_fat_bytes) {
        fs->st.mount_fails++;
        return VFS_EINVAL; /* FAT 表覆盖不了所有簇 */
    }

    /* 全部校验通过才提交（失败时旧实例内容不被半改写）。 */
    fat16_init(fs);
    fs->blk = *blk;
    fs->sector_size = bytes_per_sector;
    fs->sec_per_cluster = sec_per_cluster;
    fs->bytes_per_cluster = sec_per_cluster * FAT16_SECTOR_SIZE;
    fs->reserved = reserved;
    fs->num_fats = fats;
    fs->root_entries = root_entries;
    fs->root_dir_sectors = root_dir_sectors;
    fs->fat_sectors = fat_sectors;
    fs->total_sectors = total_sectors;
    fs->fat_start = reserved;
    fs->root_start = reserved + fats * fat_sectors;
    fs->data_start = data_start;
    fs->cluster_count = clusters;
    fs->last_alloc = 0;
    fs->mounted = 1;
    root_init(fs);
    fs->st.mounts++;
    return VFS_OK;
}

const struct vfs_fs_ops *fat16_ops(void)
{
    return &g_fat16_ops;
}

struct vfs_node *fat16_root(struct fat16 *fs)
{
    if (fs == NULL || fs->mounted == 0) {
        return NULL;
    }
    fill_vnode(fs, &fs->nodes[0]);
    return &fs->nodes[0].vnode;
}

u32 fat16_cluster_count(const struct fat16 *fs)
{
    return (fs != NULL) ? fs->cluster_count : 0;
}

u32 fat16_bytes_per_cluster(const struct fat16 *fs)
{
    return (fs != NULL) ? fs->bytes_per_cluster : 0;
}

u32 fat16_total_sectors(const struct fat16 *fs)
{
    return (fs != NULL) ? fs->total_sectors : 0;
}

int fat16_fat_entry(struct fat16 *fs, u32 cluster, u32 *value)
{
    if (fs == NULL || fs->mounted == 0 || value == NULL ||
        cluster > fs->cluster_count + 1u) {
        return VFS_EINVAL;
    }
    return fat_get(fs, cluster, value);
}

u32 fat16_free_clusters(struct fat16 *fs)
{
    u32 free_count = 0;
    u32 cluster;

    if (fs == NULL || fs->mounted == 0) {
        return 0;
    }
    for (cluster = 2; cluster <= max_cluster(fs); cluster++) {
        u32 value = 0;

        if (fat_get(fs, cluster, &value) != VFS_OK) {
            return free_count;
        }
        if (value == FAT16_CLUSTER_FREE) {
            free_count++;
        }
    }
    return free_count;
}

const struct fat16_stats *fat16_stat(const struct fat16 *fs)
{
    return (fs != NULL) ? &fs->st : NULL;
}
