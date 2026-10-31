/*
 * ng WP2-2 host 单测：FAT16（VFS 后端）。
 *
 * 覆盖（同一份 fat16.c 在内核里也跑）：
 *   1. 挂载几何：合法镜像；坏 BPB（magic/bytes-per-sector/每簇扇区/reserved/
 *      FAT 数/root entries/total sectors/FAT size/每簇字节/簇数上限）逐个
 *      返回 EINVAL；空实例/空设备/EIO；
 *   2. 目录：8.3 名字（大小写归一/长度上限/非法字符）、0x00 结束、0xE5 跳过、
 *      卷标与 LFN 跳过、子目录 "."/".."、跨目录 lookup/readdir；
 *   3. 读：整文件/部分/跨簇/EOF；链循环、链截断、坏簇、越界簇号、链中空闲
 *      项 -> EIO；目录当文件 EISDIR、文件当目录 ENOTDIR；
 *   4. 写：创建、跨扇区/跨簇、稀疏写补零、stat 大小、目录项回写、释放；
 *   5. 截断：缩小（释放尾部簇 + 清零尾）、扩大（分配 + 补零）、0；
 *   6. 删除/改名：EEXIST、ENAMETOOLONG、跨目录 EINVAL、目录 EISDIR、
 *      打开中 EBUSY；
 *   7. 满盘：ENOSPC；删除后空间回收可继续写；
 *   8. VFS 集成：mount 到 "/"、open/read/stat/listdir/mkdir/unlink/rename/
 *      truncate 全走 vfs_*，fd retain/release 与 EBUSY。
 *
 * 所有负向用例断言**具体错误码**，不按“负值即失败”放过。
 */
#include <string.h>

#include "fat16.h"
#include "test_util.h"
#include "vfs.h"

#define IMG_MAX_SECTORS 512u
#define IMG_BYTES (IMG_MAX_SECTORS * FAT16_SECTOR_SIZE)

static u8 g_img[IMG_BYTES];
static u8 g_big[16 * 1024];
static u8 g_rbuf[16 * 1024];

struct image {
    u8 *data;
    u32 total;
    u32 spc;
    u32 reserved;
    u32 fats;
    u32 fat_sectors;
    u32 root_entries;
    u32 root_start;
    u32 root_dir_sectors;
    u32 data_start;
    u32 clusters;
};

struct fake_disk {
    u8 *data;
    u64 sectors;
};

/* ------------------------------------------------------------------------- */
/* 小端 / 名字 / 镜像构造                                                    */
/* ------------------------------------------------------------------------- */

static void w16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void w32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static u32 r16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

/* BPB 只写 0 号扇区（用于坏几何用例：mount 只读 BPB）。 */
static void make_bpb(u8 *s, u32 total, u32 spc, u32 reserved, u32 fats,
                     u32 fat_sec, u32 root_entries)
{
    memset(s, 0, FAT16_SECTOR_SIZE);
    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    memcpy(s + 3, "ASTER   ", 8);
    w16(s + 11, 512);
    s[13] = (u8)spc;
    w16(s + 14, reserved);
    s[16] = (u8)fats;
    w16(s + 17, root_entries);
    w16(s + 19, (total <= 0xFFFFu) ? total : 0u);
    s[21] = 0xF8;
    w16(s + 22, fat_sec);
    w16(s + 24, 18);
    w16(s + 26, 2);
    w32(s + 28, 0);
    w32(s + 32, (total > 0xFFFFu) ? total : 0u);
    s[36] = 0x80;
    s[38] = 0x29;
    w32(s + 39, 0x12345678u);
    memcpy(s + 43, "ASTERFAT16 ", 11);
    memcpy(s + 54, "FAT16   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void img_format(struct image *im, u8 *data, u32 total, u32 spc,
                       u32 reserved, u32 fats, u32 fat_sectors,
                       u32 root_entries)
{
    im->data = data;
    im->total = total;
    im->spc = spc;
    im->reserved = reserved;
    im->fats = fats;
    im->fat_sectors = fat_sectors;
    im->root_entries = root_entries;
    memset(data, 0, (size_t)total * FAT16_SECTOR_SIZE);
    im->root_start = reserved + fats * fat_sectors;
    im->root_dir_sectors =
        (root_entries * FAT16_ENTRY_SIZE + FAT16_SECTOR_SIZE - 1u) /
        FAT16_SECTOR_SIZE;
    im->data_start = im->root_start + im->root_dir_sectors;
    im->clusters = (total - im->data_start) / spc;
    make_bpb(data, total, spc, reserved, fats, fat_sectors, root_entries);
    {
        u32 f;

        for (f = 0; f < fats; f++) {
            u8 *fat = data + (reserved + f * fat_sectors) * FAT16_SECTOR_SIZE;

            w16(fat + 0, 0xFFF8);
            w16(fat + 2, 0xFFFF);
        }
    }
}

static void img_set_fat(struct image *im, u32 cluster, u32 value)
{
    u32 f;

    for (f = 0; f < im->fats; f++) {
        u8 *fat = im->data +
                  (im->reserved + f * im->fat_sectors) * FAT16_SECTOR_SIZE;

        w16(fat + cluster * 2u, value);
    }
}

static u32 img_get_fat(struct image *im, u32 cluster)
{
    u8 *fat = im->data + im->reserved * FAT16_SECTOR_SIZE;

    return r16(fat + cluster * 2u);
}

/* 把 "HELLO.TXT" 之类转成 11 字节 8.3（空格填充，大写）。 */
static void n83(const char *name, u8 out[11])
{
    u32 i = 0;
    u32 base = 0;
    u32 ext = 0;

    memset(out, ' ', 11);
    while (name[i] != '\0' && name[i] != '.') {
        char c = name[i];

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[base++] = (u8)c;
        i++;
    }
    if (name[i] == '.' && base <= 8) {
        i++;
        while (name[i] != '\0' && base + ext < 11) {
            char c = name[i];

            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if (ext < 3) {
                out[8 + ext] = (u8)c;
            }
            ext++;
            i++;
        }
    }
}

static void name83_any(const char *name, u8 out[11])
{
    if (strcmp(name, ".") == 0) {
        memset(out, ' ', 11);
        out[0] = '.';
        return;
    }
    if (strcmp(name, "..") == 0) {
        memset(out, ' ', 11);
        out[0] = '.';
        out[1] = '.';
        return;
    }
    n83(name, out);
}

static void img_root_entry(struct image *im, u32 index, const char *name,
                           u8 attr, u32 cluster, u32 size)
{
    u8 *slot = im->data + im->root_start * FAT16_SECTOR_SIZE +
               index * FAT16_ENTRY_SIZE;

    memset(slot, 0, FAT16_ENTRY_SIZE);
    if (name != NULL) {
        name83_any(name, slot);
    }
    slot[11] = attr;
    w16(slot + 26, cluster & 0xFFFFu);
    w32(slot + 28, size);
}

/* 子目录里的第 index 项（"." / ".." 也用它写）。 */
static void img_dir_entry(struct image *im, u32 dir_cluster, u32 index,
                          const char *name, u8 attr, u32 cluster, u32 size)
{
    u8 *slot = im->data +
               (im->data_start + (dir_cluster - 2u) * im->spc) *
                   FAT16_SECTOR_SIZE +
               index * FAT16_ENTRY_SIZE;

    memset(slot, 0, FAT16_ENTRY_SIZE);
    if (name != NULL) {
        name83_any(name, slot);
    }
    slot[11] = attr;
    w16(slot + 26, cluster & 0xFFFFu);
    w32(slot + 28, size);
}

static void img_cluster_data(struct image *im, u32 cluster, u32 offset,
                             const void *data, u32 len)
{
    u8 *p = im->data +
            (im->data_start + (cluster - 2u) * im->spc) * FAT16_SECTOR_SIZE +
            offset;

    memcpy(p, data, len);
}

static int fake_read(void *ctx, u64 sector, void *buf, u32 count)
{
    struct fake_disk *d = (struct fake_disk *)ctx;

    if (d == NULL || d->data == NULL || count == 0 ||
        sector + count > d->sectors) {
        return -1;
    }
    memcpy(buf, d->data + sector * FAT16_SECTOR_SIZE,
           (size_t)count * FAT16_SECTOR_SIZE);
    return 0;
}

static int fake_write(void *ctx, u64 sector, const void *buf, u32 count)
{
    struct fake_disk *d = (struct fake_disk *)ctx;

    if (d == NULL || d->data == NULL || count == 0 ||
        sector + count > d->sectors) {
        return -1;
    }
    memcpy(d->data + sector * FAT16_SECTOR_SIZE, buf,
           (size_t)count * FAT16_SECTOR_SIZE);
    return 0;
}

static int fake_read_fail(void *ctx, u64 sector, void *buf, u32 count)
{
    (void)ctx;
    (void)sector;
    (void)buf;
    (void)count;
    return -1;
}

static void disk_bind(struct fake_disk *d, struct image *im)
{
    d->data = im->data;
    d->sectors = im->total;
}

static void blk_bind(struct blk_env *blk, struct fake_disk *d)
{
    blk->read = fake_read;
    blk->write = fake_write;
    blk->sectors = d->sectors;
    blk->ctx = d;
}

static void blk_bind2(struct blk_env *blk, struct fake_disk *d, u64 sectors)
{
    blk->read = fake_read;
    blk->write = fake_write;
    blk->sectors = sectors;
    blk->ctx = d;
    d->sectors = sectors;
}

/*
 * 标准镜像（512 扇区，每簇 2 扇区 = 1024 字节，2 份 FAT）：
 *   根目录：卷标 + LFN + 10 个可见项 + 0x00 结束；
 *   DIR 子目录：".", "..", A.TXT；
 *   特殊链：LOOP 自环 / SHORT 截断 / BAD 坏簇 / OOR 越界 / FREE 链中空闲。
 */
static const char *k_hello = "hello from fat16\r\n"; /* 18 字节 */
static const char *k_a = "dir a\n";                  /* 6 字节 */
static const char *k_big = "0123456789abcdef";       /* 重复成 3000 字节 */

static void std_image(struct image *im)
{
    u32 i;

    img_format(im, g_img, 512, 2, 1, 2, 1, 64);
    /* 0：卷标（跳过）；1：LFN 项（跳过）；2 起是可见项。 */
    memset(im->data + im->root_start * FAT16_SECTOR_SIZE, 0,
           im->root_dir_sectors * FAT16_SECTOR_SIZE);
    {
        u8 *vol = im->data + im->root_start * FAT16_SECTOR_SIZE;
        u8 *lfn = vol + FAT16_ENTRY_SIZE;

        memset(vol, ' ', 11);
        memcpy(vol, "TESTVOL", 7);
        vol[11] = FAT16_ATTR_VOLUME;
        lfn[0] = 0x41;
        lfn[11] = 0x0F;
    }
    img_root_entry(im, 2, "HELLO.TXT", FAT16_ATTR_ARCHIVE, 2, 18);
    img_root_entry(im, 3, "EMPTY.TXT", FAT16_ATTR_ARCHIVE, 0, 0);
    img_root_entry(im, 4, "DIR", FAT16_ATTR_DIR, 3, 0);
    img_root_entry(im, 5, "LOOP.TXT", FAT16_ATTR_ARCHIVE, 4, 2048);
    img_root_entry(im, 6, "SHORT.TXT", FAT16_ATTR_ARCHIVE, 5, 2048);
    img_root_entry(im, 7, "BAD.TXT", FAT16_ATTR_ARCHIVE, 6, 2048);
    img_root_entry(im, 8, "OOR.TXT", FAT16_ATTR_ARCHIVE, 7, 2048);
    img_root_entry(im, 9, "RDONLY.TXT", FAT16_ATTR_READONLY | FAT16_ATTR_ARCHIVE,
                   0, 0);
    img_root_entry(im, 10, "BIG.BIN", FAT16_ATTR_ARCHIVE, 10, 3000);
    img_root_entry(im, 11, "FREE.TXT", FAT16_ATTR_ARCHIVE, 13, 2048);
    /* 12 = 0x00 结束（格式化时已清零）。 */

    img_cluster_data(im, 2, 0, k_hello, 18);
    img_dir_entry(im, 3, 0, ".", FAT16_ATTR_DIR, 3, 0);
    img_dir_entry(im, 3, 1, "..", FAT16_ATTR_DIR, 0, 0);
    img_dir_entry(im, 3, 2, "A.TXT", FAT16_ATTR_ARCHIVE, 9, 6);
    img_cluster_data(im, 9, 0, k_a, 6);

    for (i = 0; i < 3000u; i++) {
        g_big[i] = (u8)k_big[i % 16u];
    }
    img_cluster_data(im, 10, 0, g_big, 1024);
    img_cluster_data(im, 11, 0, g_big + 1024, 1024);
    img_cluster_data(im, 12, 0, g_big + 2048, 952);

    img_set_fat(im, 2, 0xFFFF);
    img_set_fat(im, 3, 0xFFFF);
    img_set_fat(im, 4, 4); /* 自环 */
    img_set_fat(im, 5, 0xFFFF); /* 链截断（size 要求 2 簇） */
    img_set_fat(im, 6, FAT16_CLUSTER_BAD);
    img_set_fat(im, 7, 0x7FFF); /* 越界簇号 */
    img_set_fat(im, 9, 0xFFFF);
    img_set_fat(im, 10, 11);
    img_set_fat(im, 11, 12);
    img_set_fat(im, 12, 0xFFFF);
    img_set_fat(im, 13, 0); /* 链中空闲项：size 要求 2 簇 */
}

static void std_mount(struct fat16 *fs, struct fake_disk *disk,
                      struct blk_env *blk, struct image *im)
{
    std_image(im);
    disk_bind(disk, im);
    blk_bind(blk, disk);
    fat16_init(fs);
    CHECK(fat16_mount(fs, blk) == VFS_OK);
}

static struct vfs_node *lookup_raw(struct fat16 *fs, struct vfs_node *dir,
                                   const char *name, int *rc_out)
{
    struct vfs_node *out = NULL;
    int rc = fat16_ops()->lookup(fs, dir, name, (u32)strlen(name), &out);

    if (rc_out != NULL) {
        *rc_out = rc;
    }
    return (rc == VFS_OK) ? out : NULL;
}

static int create_raw(struct fat16 *fs, struct vfs_node *dir, const char *name,
                      u32 type, struct vfs_node **out)
{
    return fat16_ops()->create(fs, dir, name, (u32)strlen(name), type, 0644,
                               out);
}

/* ------------------------------------------------------------------------- */
/* 1. 挂载几何                                                               */
/* ------------------------------------------------------------------------- */

static void t_mount_bad_bpb(void)
{
    static u8 bpb[FAT16_SECTOR_SIZE];
    struct fake_disk disk;
    struct blk_env blk;
    struct fat16 fs;
    u32 n = 0;

    disk.data = bpb;
    disk.sectors = 512;
    blk_bind2(&blk, &disk, 512);
    fat16_init(&fs);

    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_OK);
    /* 合法几何：512 扇区、1 扇区/簇、2 份 2 扇区 FAT、16 个 root entries。 */
    CHECK(fat16_cluster_count(&fs) ==
          (512u - (1u + 2u * 2u + 1u)) / 1u);

    /* 坏 magic */
    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    bpb[510] = 0;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* bytes-per-sector != 512 */
    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    w16(bpb + 11, 1024);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* sectors-per-cluster = 0 / 非 2 的幂 / > 128 */
    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    bpb[13] = 0;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    bpb[13] = 3;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    make_bpb(bpb, 512, 1, 1, 2, 2, 16);
    bpb[13] = 129;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* reserved = 0 */
    make_bpb(bpb, 512, 1, 0, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* FAT 数 = 0 / 5 */
    make_bpb(bpb, 512, 1, 1, 0, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    make_bpb(bpb, 512, 1, 1, 5, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* root entries = 0 / 513（FAT16 根目录最大 512 项） */
    make_bpb(bpb, 512, 1, 1, 2, 2, 0);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    make_bpb(bpb, 512, 1, 1, 2, 2, 513);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* FAT size = 0 / 太小（覆盖不了 clusters+2 个 16 位表项） */
    make_bpb(bpb, 512, 1, 1, 2, 0, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    make_bpb(bpb, 512, 1, 1, 2, 1, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* total sectors = 0 */
    make_bpb(bpb, 0, 1, 1, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* total 超过设备扇区数 */
    make_bpb(bpb, 600, 1, 1, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* data_start >= total（FAT 区域就吃掉整盘） */
    make_bpb(bpb, 8, 1, 1, 2, 4, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* 每簇字节 > 32 KiB（spc=128 -> 64 KiB） */
    make_bpb(bpb, 4096, 128, 1, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* 簇数超过 FAT16 上限 65524 */
    disk.sectors = 300000;
    blk.sectors = 300000;
    make_bpb(bpb, 300000, 1, 1, 2, 2, 16);
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;

    /* 缺 read/write / sectors=0 / 空实例 */
    CHECK(fat16_mount(&fs, NULL) == VFS_EINVAL);
    n++;
    CHECK(fat16_mount(NULL, &blk) == VFS_EINVAL);
    n++;
    disk.sectors = 0;
    blk.sectors = 0;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    disk.sectors = 512;
    disk.data = bpb;
    blk.sectors = 512;
    blk.read = NULL;
    CHECK(fat16_mount(&fs, &blk) == VFS_EINVAL);
    n++;
    CHECK(n == 21);
    CHECK(fat16_stat(&fs)->mounts == 1);
    /* 参数级拒绝（空实例/空设备/sectors=0/缺 read）不计入 mount_fails。 */
    CHECK(fat16_stat(&fs)->mount_fails == 17);
}

static void t_mount_geometry(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;

    std_mount(&fs, &disk, &blk, &im);
    CHECK(fs.mounted == 1);
    CHECK(fat16_cluster_count(&fs) == 252);
    CHECK(fat16_bytes_per_cluster(&fs) == 1024);
    CHECK(fat16_total_sectors(&fs) == 512);
    CHECK(fs.sec_per_cluster == 2);
    CHECK(fs.reserved == 1);
    CHECK(fs.num_fats == 2);
    CHECK(fs.root_entries == 64);
    CHECK(fs.root_dir_sectors == 4);
    CHECK(fs.root_start == 3);
    CHECK(fs.data_start == 7);
    CHECK(fat16_root(&fs) != NULL);
    CHECK(fat16_root(&fs)->type == VFS_NODE_DIR);
    CHECK(fat16_root(&fs)->ino == 1);
    /* 10 个簇被链占用（FREE.TXT 的首簇在 FAT 里是 0，属于损坏链用例）。 */
    CHECK(fat16_free_clusters(&fs) == 242);
    CHECK(img_get_fat(&im, 2) == 0xFFFF);
    CHECK(img_get_fat(&im, 14) == 0);
    CHECK(fat16_stat(&fs)->mounts == 1);
    CHECK(fat16_stat(&fs)->nodes_used == 1);
    /* FAT 表项：HELLO 首簇 EOC、空闲簇 0、坏簇/越界原样可读。 */
    {
        u32 v = 0xDEAD;

        CHECK(fat16_fat_entry(&fs, 2, &v) == VFS_OK && v == 0xFFFF);
        CHECK(fat16_fat_entry(&fs, 14, &v) == VFS_OK && v == 0);
        CHECK(fat16_fat_entry(&fs, 6, &v) == VFS_OK &&
              v == FAT16_CLUSTER_BAD);
        CHECK(fat16_fat_entry(&fs, 7, &v) == VFS_OK && v == 0x7FFF);
        CHECK(fat16_fat_entry(&fs, 254, &v) == VFS_EINVAL);
    }
    /* 失败挂载不破坏已挂载实例。 */
    {
        struct fat16 bad;
        struct blk_env nope;

        fat16_init(&bad);
        nope.read = fake_read_fail;
        nope.write = fake_write;
        nope.sectors = 512;
        nope.ctx = &disk;
        CHECK(fat16_mount(&bad, &nope) == VFS_EIO);
        CHECK(fat16_cluster_count(&fs) == 252);
        CHECK(fat16_root(&fs)->type == VFS_NODE_DIR);
    }
}

/* ------------------------------------------------------------------------- */
/* 2. 目录 / 3. 读                                                            */
/* ------------------------------------------------------------------------- */

static void t_lookup_read(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *hello;
    struct vfs_node *dir;
    struct vfs_node *a;
    struct vfs_node *n;
    int rc = 0;
    int fd;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);

    /* 大小写不敏感；8.3 长度/非法字符的 lookup 直接 ENOENT。 */
    hello = lookup_raw(&fs, root, "HELLO.TXT", &rc);
    CHECK(rc == VFS_OK && hello != NULL);
    CHECK(hello->type == VFS_NODE_FILE);
    CHECK(hello->size == 18);
    CHECK(strcmp(((struct fat16_node *)hello->priv)->name, "HELLO.TXT") == 0);
    n = lookup_raw(&fs, root, "hello.txt", &rc);
    CHECK(rc == VFS_OK && n == hello); /* 同一目录项命中同一节点 */
    CHECK(lookup_raw(&fs, root, "HELLO.TXTX", &rc) == NULL &&
          rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, "TOOLONGNAME.TXT", &rc) == NULL &&
          rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, "BAD*NAME.TXT", &rc) == NULL &&
          rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, "MISSING.TXT", &rc) == NULL &&
          rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, ".", &rc) == NULL && rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, "TESTVOL", &rc) == NULL && rc == VFS_ENOENT);
    CHECK(lookup_raw(&fs, root, "LFNLONG.TXT", &rc) == NULL &&
          rc == VFS_ENOENT);

    /* 读：整文件 / 部分 / EOF。 */
    memset(g_rbuf, 0, 32);
    CHECK(fat16_ops()->read(&fs, hello, 0, g_rbuf, 64) == 18);
    CHECK(memcmp(g_rbuf, k_hello, 18) == 0);
    memset(g_rbuf, 0, 32);
    CHECK(fat16_ops()->read(&fs, hello, 6, g_rbuf, 5) == 5);
    CHECK(memcmp(g_rbuf, "from ", 5) == 0);
    CHECK(fat16_ops()->read(&fs, hello, 18, g_rbuf, 4) == 0);
    CHECK(fat16_ops()->read(&fs, hello, 100, g_rbuf, 4) == 0);
    CHECK(fat16_ops()->read(&fs, hello, 0, g_rbuf, 0) == 0);

    /* 跨簇：BIG.BIN 3000 字节（3 簇）。 */
    n = lookup_raw(&fs, root, "BIG.BIN", &rc);
    CHECK(rc == VFS_OK && n != NULL && n->size == 3000);
    memset(g_rbuf, 0, 3000);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 3000) == 3000);
    CHECK(memcmp(g_rbuf, g_big, 3000) == 0);
    memset(g_rbuf, 0, 2000);
    CHECK(fat16_ops()->read(&fs, n, 1000, g_rbuf, 2000) == 2000);
    CHECK(memcmp(g_rbuf, g_big + 1000, 2000) == 0);
    CHECK(fat16_ops()->read(&fs, n, 2990, g_rbuf, 100) == 10);
    CHECK(memcmp(g_rbuf, g_big + 2990, 10) == 0);

    /* 空文件。 */
    n = lookup_raw(&fs, root, "EMPTY.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL && n->size == 0);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 10) == 0);

    /* 子目录：DIR / A.TXT；"." 与 ".." 不参与 lookup。 */
    dir = lookup_raw(&fs, root, "DIR", &rc);
    CHECK(rc == VFS_OK && dir != NULL && dir->type == VFS_NODE_DIR);
    CHECK(dir->size == 0);
    a = lookup_raw(&fs, dir, "A.TXT", &rc);
    CHECK(rc == VFS_OK && a != NULL && a->size == 6);
    CHECK(fat16_ops()->read(&fs, a, 0, g_rbuf, 16) == 6);
    CHECK(memcmp(g_rbuf, k_a, 6) == 0);

    /* 文件当目录 / 目录当文件。 */
    CHECK(lookup_raw(&fs, hello, "A.TXT", &rc) == NULL && rc == VFS_ENOTDIR);
    CHECK(fat16_ops()->read(&fs, dir, 0, g_rbuf, 1) == VFS_EISDIR);
    CHECK(fat16_ops()->write(&fs, dir, 0, "x", 1) == VFS_EISDIR);

    /* 只读属性映射成 0444；普通文件 0644。 */
    n = lookup_raw(&fs, root, "RDONLY.TXT", &rc);
    CHECK(rc == VFS_OK && (n->mode & 0777) == 0444);
    CHECK((hello->mode & 0777) == 0644);
    CHECK((dir->mode & 0777) == 0755);

    /* VFS open 目录 + fd 生命周期。 */
    vfs_reset();
    {
        struct vfs_ctx ctx;

        vfs_ctx_init(&ctx);
        CHECK(vfs_mount("/", fat16_ops(), &fs, fat16_root(&fs)) == VFS_OK);
        fd = vfs_open(&ctx, "/DIR", VFS_O_RDONLY | VFS_O_DIRECTORY, 0);
        CHECK(fd > 0);
        if (fd > 0) {
            CHECK(vfs_write(&ctx, fd, "x", 1) == VFS_EISDIR);
            CHECK(vfs_close(&ctx, fd) == VFS_OK);
        }
        CHECK(vfs_open(&ctx, "/DIR/A.TXT", VFS_O_RDONLY, 0) > 0);
        CHECK(vfs_open(&ctx, "/NOPE", VFS_O_RDONLY, 0) == VFS_ENOENT);
        CHECK(vfs_stat(&ctx, "/BIG.BIN", NULL) == VFS_EFAULT);
    }
}

static void t_readdir(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *dir;
    static const char *names[] = {"HELLO.TXT", "EMPTY.TXT", "DIR",
                                  "LOOP.TXT",  "SHORT.TXT", "BAD.TXT",
                                  "OOR.TXT",   "RDONLY.TXT", "BIG.BIN",
                                  "FREE.TXT"};
    char name[VFS_NAME_MAX];
    struct vfs_stat st;
    u32 i;
    int rc;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);
    for (i = 0; i < 10u; i++) {
        memset(name, 0, sizeof(name));
        memset(&st, 0, sizeof(st));
        rc = fat16_ops()->readdir(&fs, root, i, name, (u32)sizeof(name), &st);
        CHECK(rc == 1);
        CHECK(strcmp(name, names[i]) == 0);
        CHECK(st.type == ((i == 2u) ? VFS_NODE_DIR : VFS_NODE_FILE));
        CHECK(st.ino != 0);
    }
    CHECK(fat16_ops()->readdir(&fs, root, 10, name, (u32)sizeof(name), &st) ==
          0);
    /* 卷标与 LFN 不计入、".", ".." 不计入。 */
    dir = lookup_raw(&fs, root, "DIR", NULL);
    CHECK(dir != NULL);
    rc = fat16_ops()->readdir(&fs, dir, 0, name, (u32)sizeof(name), &st);
    CHECK(rc == 1 && strcmp(name, "A.TXT") == 0);
    CHECK(fat16_ops()->readdir(&fs, dir, 1, name, (u32)sizeof(name), &st) == 0);
    /* 名字缓冲不足 -> ENAMETOOLONG。 */
    CHECK(fat16_ops()->readdir(&fs, root, 0, name, 4, &st) ==
          VFS_ENAMETOOLONG);
    CHECK(fat16_ops()->readdir(&fs, root, 0, NULL, 64, &st) == VFS_EINVAL);
    CHECK(fat16_ops()->readdir(&fs, root, 0, name, 64, NULL) == VFS_EINVAL);
    {
        struct vfs_ctx ctx;

        vfs_reset();
        vfs_ctx_init(&ctx);
        CHECK(vfs_mount("/", fat16_ops(), &fs, fat16_root(&fs)) == VFS_OK);
        {
            struct vfs_dirent ents[16];
            int n = vfs_listdir(&ctx, "/", ents, 16);

            CHECK(n == 10);
            CHECK(ents[0].type == VFS_NODE_FILE);
            CHECK(strcmp(ents[0].name, "HELLO.TXT") == 0);
            CHECK(ents[2].type == VFS_NODE_DIR);
            CHECK(strcmp(ents[2].name, "DIR") == 0);
        }
        {
            struct vfs_dirent ents[4];
            int n = vfs_listdir(&ctx, "/DIR", ents, 4);

            CHECK(n == 1 && strcmp(ents[0].name, "A.TXT") == 0);
            CHECK(vfs_listdir(&ctx, "/HELLO.TXT", ents, 4) == VFS_ENOTDIR);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 4. 写 / 创建                                                              */
/* ------------------------------------------------------------------------- */

static void t_create_write(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *n = NULL;
    struct vfs_node *sub = NULL;
    struct vfs_node *in_sub = NULL;
    u32 before;
    u32 i;
    int rc = 0;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);
    before = fat16_free_clusters(&fs);

    /* 8.3 名字：小写归一、>8.3 ENAMETOOLONG、非法字符 EINVAL。 */
    CHECK(create_raw(&fs, root, "NEW.TXT", VFS_NODE_FILE, &n) == VFS_OK);
    CHECK(n != NULL && n->size == 0);
    CHECK(lookup_raw(&fs, root, "new.txt", &rc) == n && rc == VFS_OK);
    CHECK(create_raw(&fs, root, "NEW.TXT", VFS_NODE_FILE, &n) == VFS_EEXIST);
    CHECK(create_raw(&fs, root, "TOOLONGNAME.TXT", VFS_NODE_FILE, &n) ==
          VFS_ENAMETOOLONG);
    CHECK(create_raw(&fs, root, "A.TOOLONG", VFS_NODE_FILE, &n) ==
          VFS_ENAMETOOLONG);
    CHECK(create_raw(&fs, root, "BAD*NAME.TXT", VFS_NODE_FILE, &n) ==
          VFS_EINVAL);
    CHECK(create_raw(&fs, root, "", VFS_NODE_FILE, &n) == VFS_EINVAL);
    CHECK(create_raw(&fs, root, ".TXT", VFS_NODE_FILE, &n) == VFS_EINVAL);
    CHECK(create_raw(&fs, root, "SYM", VFS_NODE_SYMLINK, &n) == VFS_EINVAL);

    /* 写 600 字节（跨扇区、跨簇边界 1024 不跨；再写到 1500 跨簇）。 */
    n = lookup_raw(&fs, root, "NEW.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    for (i = 0; i < 600u; i++) {
        g_big[i] = (u8)(i * 7u + 3u);
    }
    CHECK(fat16_ops()->write(&fs, n, 0, g_big, 600) == 600);
    CHECK(n->size == 600);
    CHECK(fat16_free_clusters(&fs) == before - 1u);
    memset(g_rbuf, 0, 700);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 700) == 600);
    CHECK(memcmp(g_rbuf, g_big, 600) == 0);

    /* 稀疏写：offset 1500 起写 200，[600,1500) 必须补零。 */
    for (i = 0; i < 200u; i++) {
        g_big[i] = (u8)(0xA0u + (u8)i);
    }
    CHECK(fat16_ops()->write(&fs, n, 1500, g_big, 200) == 200);
    CHECK(n->size == 1700);
    memset(g_rbuf, 0xEE, 1700);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 1700) == 1700);
    CHECK(memcmp(g_rbuf, g_big, 600) != 0); /* 先确认缓冲不是旧内容 */
    {
        int zero = 1;

        for (i = 600; i < 1500u; i++) {
            if (g_rbuf[i] != 0) {
                zero = 0;
                break;
            }
        }
        CHECK(zero == 1);
    }
    CHECK(memcmp(g_rbuf + 1500, g_big, 200) == 0);

    /* 跨多簇写 + 读回。 */
    for (i = 0; i < 5000u; i++) {
        g_big[i] = (u8)(i * 13u + 1u);
    }
    CHECK(fat16_ops()->write(&fs, n, 2000, g_big, 5000) == 5000);
    CHECK(n->size == 7000);
    memset(g_rbuf, 0, 7000);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 7000) == 7000);
    CHECK(memcmp(g_rbuf + 2000, g_big, 5000) == 0);

    /* 关闭/重开路径（直接 lookup 同一节点）。 */
    CHECK(lookup_raw(&fs, root, "NEW.TXT", &rc) == n);

    /* 子目录：mkdir + 跨目录创建/读。 */
    CHECK(create_raw(&fs, root, "SUB", VFS_NODE_DIR, &sub) == VFS_OK);
    CHECK(sub != NULL && sub->type == VFS_NODE_DIR);
    CHECK(create_raw(&fs, root, "SUB", VFS_NODE_DIR, &sub) == VFS_EEXIST);
    sub = lookup_raw(&fs, root, "SUB", &rc);
    CHECK(rc == VFS_OK && sub != NULL);
    CHECK(create_raw(&fs, sub, "IN.TXT", VFS_NODE_FILE, &in_sub) == VFS_OK);
    CHECK(fat16_ops()->write(&fs, in_sub, 0, "inside", 6) == 6);
    CHECK(in_sub->size == 6);
    in_sub = lookup_raw(&fs, sub, "IN.TXT", &rc);
    CHECK(rc == VFS_OK && in_sub != NULL && in_sub->size == 6);
    memset(g_rbuf, 0, 8);
    CHECK(fat16_ops()->read(&fs, in_sub, 0, g_rbuf, 8) == 6);
    CHECK(memcmp(g_rbuf, "inside", 6) == 0);
    /* 子目录的 "." / ".." 已被 readdir 过滤；listdir 只有 IN.TXT。 */
    {
        char name[VFS_NAME_MAX];
        struct vfs_stat st;

        CHECK(fat16_ops()->readdir(&fs, sub, 0, name, (u32)sizeof(name),
                                   &st) == 1);
        CHECK(strcmp(name, "IN.TXT") == 0);
        CHECK(fat16_ops()->readdir(&fs, sub, 1, name, (u32)sizeof(name),
                                   &st) == 0);
    }
    /* 子目录写满后追加目录项（分配新簇）。 */
    for (i = 0; i < 40u; i++) {
        char nm[16];

        nm[0] = 'F';
        nm[1] = (char)('A' + (int)(i / 10u));
        nm[2] = (char)('0' + (int)(i % 10u));
        nm[3] = '.';
        nm[4] = 'T';
        nm[5] = 'X';
        nm[6] = 'T';
        nm[7] = '\0';
        CHECK(create_raw(&fs, sub, nm, VFS_NODE_FILE, &n) == VFS_OK);
    }
    {
        char name[VFS_NAME_MAX];
        struct vfs_stat st;
        u32 count = 0;

        while (fat16_ops()->readdir(&fs, sub, count, name,
                                    (u32)sizeof(name), &st) == 1) {
            count++;
        }
        CHECK(count == 41); /* IN.TXT + 40 */
    }
}

/* ------------------------------------------------------------------------- */
/* 5. 截断                                                                   */
/* ------------------------------------------------------------------------- */

static void t_truncate(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *n;
    u32 i;
    int rc = 0;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);
    CHECK(create_raw(&fs, root, "TRUNC.TXT", VFS_NODE_FILE, &n) == VFS_OK);
    n = lookup_raw(&fs, root, "TRUNC.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    for (i = 0; i < 5000u; i++) {
        g_big[i] = (u8)(i + 7u);
    }
    CHECK(fat16_ops()->write(&fs, n, 0, g_big, 5000) == 5000);
    CHECK(n->size == 5000);

    /* 缩小到 100：释放尾部簇，[100, ...) 不可见。 */
    CHECK(fat16_ops()->truncate(&fs, n, 100) == VFS_OK);
    CHECK(n->size == 100);
    memset(g_rbuf, 0, 1024);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 1024) == 100);
    CHECK(memcmp(g_rbuf, g_big, 100) == 0);

    /* 扩大到 3000：新区域补零。 */
    CHECK(fat16_ops()->truncate(&fs, n, 3000) == VFS_OK);
    CHECK(n->size == 3000);
    memset(g_rbuf, 0xEE, 3100);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 3100) == 3000);
    CHECK(memcmp(g_rbuf, g_big, 100) == 0);
    {
        int zero = 1;

        for (i = 100; i < 3000u; i++) {
            if (g_rbuf[i] != 0) {
                zero = 0;
                break;
            }
        }
        CHECK(zero == 1);
    }

    /* 截断到 0：链全部释放，读 0。 */
    CHECK(fat16_ops()->truncate(&fs, n, 0) == VFS_OK);
    CHECK(n->size == 0);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 10) == 0);
    CHECK(fat16_ops()->truncate(&fs, n, 0) == VFS_OK); /* 幂等 */
    CHECK(fat16_ops()->truncate(&fs, n, 0x100000000ULL) == VFS_ENOSPC);
    CHECK(fat16_ops()->truncate(&fs, fat16_root(&fs), 0) == VFS_EISDIR);
}

/* ------------------------------------------------------------------------- */
/* 6. 删除 / 改名                                                            */
/* ------------------------------------------------------------------------- */

static void t_rename_unlink(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *dir;
    struct vfs_node *n = NULL;
    int rc = 0;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);
    CHECK(create_raw(&fs, root, "R1.TXT", VFS_NODE_FILE, &n) == VFS_OK);
    n = lookup_raw(&fs, root, "R1.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->write(&fs, n, 0, "rename me", 9) == 9);

    /* 同目录改名：数据/大小不变，旧名 ENOENT，新名可读。 */
    CHECK(fat16_ops()->rename(&fs, root, "R1.TXT", 6, root, "R2.TXT", 6) ==
          VFS_OK);
    CHECK(lookup_raw(&fs, root, "R1.TXT", &rc) == NULL && rc == VFS_ENOENT);
    n = lookup_raw(&fs, root, "R2.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL && n->size == 9);
    CHECK(memcmp(((struct fat16_node *)n->priv)->name, "R2.TXT", 7) == 0);

    /* 大小写不同但 8.3 相同 -> OK（同一个目录项）。 */
    CHECK(fat16_ops()->rename(&fs, root, "R2.TXT", 6, root, "r2.txt", 6) ==
          VFS_OK);
    /* 目标存在 -> EEXIST；新名 >8.3 -> ENAMETOOLONG。 */
    CHECK(create_raw(&fs, root, "R3.TXT", VFS_NODE_FILE, &n) == VFS_OK);
    CHECK(fat16_ops()->rename(&fs, root, "R2.TXT", 6, root, "R3.TXT", 6) ==
          VFS_EEXIST);
    CHECK(fat16_ops()->rename(&fs, root, "R2.TXT", 6, root, "TOOLONGNAME.TXT",
                              14) == VFS_ENAMETOOLONG);
    CHECK(fat16_ops()->rename(&fs, root, "MISSING.TXT", 11, root, "X.TXT", 5) ==
          VFS_ENOENT);
    /* 目录改名 -> EISDIR（VFS 层也会先拒）。 */
    dir = lookup_raw(&fs, root, "DIR", &rc);
    CHECK(rc == VFS_OK && dir != NULL);
    CHECK(fat16_ops()->rename(&fs, root, "DIR", 3, root, "DIR2", 4) ==
          VFS_EISDIR);
    /* 跨目录改名 -> EINVAL。 */
    CHECK(create_raw(&fs, root, "SUB2", VFS_NODE_DIR, &n) == VFS_OK);
    {
        struct vfs_node *sub2 = lookup_raw(&fs, root, "SUB2", &rc);

        CHECK(rc == VFS_OK && sub2 != NULL);
        CHECK(fat16_ops()->rename(&fs, root, "R2.TXT", 6, sub2, "R4.TXT", 6) ==
              VFS_EINVAL);
    }
    /* 删目录 -> EISDIR；删不存在 -> ENOENT。 */
    CHECK(fat16_ops()->unlink(&fs, root, "DIR", 3) == VFS_EISDIR);
    CHECK(fat16_ops()->unlink(&fs, root, "NOPE.TXT", 8) == VFS_ENOENT);
    /* 打开中 unlink -> EBUSY；release 后可删，空间回收。 */
    n = lookup_raw(&fs, root, "R3.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->retain(&fs, n) == VFS_OK);
    CHECK(fat16_ops()->unlink(&fs, root, "R3.TXT", 6) == VFS_EBUSY);
    fat16_ops()->release(&fs, n);
    CHECK(fat16_ops()->unlink(&fs, root, "R3.TXT", 6) == VFS_OK);
    CHECK(lookup_raw(&fs, root, "R3.TXT", &rc) == NULL && rc == VFS_ENOENT);
    /* 删除 R2.TXT 后簇回收。 */
    {
        u32 mid = fat16_free_clusters(&fs);

        CHECK(fat16_ops()->unlink(&fs, root, "R2.TXT", 6) == VFS_OK);
        CHECK(fat16_free_clusters(&fs) == mid + 1u);
    }
    CHECK(fat16_stat(&fs)->deletes >= 2);
    CHECK(fat16_stat(&fs)->renames >= 2);
}

/* ------------------------------------------------------------------------- */
/* 7. 簇链边界                                                               */
/* ------------------------------------------------------------------------- */

static void t_chain_edges(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *n;
    int rc = 0;

    std_mount(&fs, &disk, &blk, &im);
    root = fat16_root(&fs);

    /* 自环：第 0 簇可读，第 1 簇（offset 1024）要遍历 FAT 时检测循环。 */
    n = lookup_raw(&fs, root, "LOOP.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 512) == 512);
    CHECK(fat16_ops()->read(&fs, n, 1024, g_rbuf, 512) == VFS_EIO);
    CHECK(fat16_stat(&fs)->chain_loops >= 1);

    /* 链截断：size 2048 但 FAT[5] = EOC，第二簇 EIO。 */
    n = lookup_raw(&fs, root, "SHORT.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->read(&fs, n, 0, g_rbuf, 512) == 512);
    CHECK(fat16_ops()->read(&fs, n, 1024, g_rbuf, 512) == VFS_EIO);
    CHECK(fat16_stat(&fs)->chain_short >= 1);

    /* 坏簇 / 越界簇号。 */
    n = lookup_raw(&fs, root, "BAD.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->read(&fs, n, 1024, g_rbuf, 512) == VFS_EIO);
    n = lookup_raw(&fs, root, "OOR.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->read(&fs, n, 1024, g_rbuf, 512) == VFS_EIO);

    /* 链中空闲项（FAT=0）同样 EIO。 */
    n = lookup_raw(&fs, root, "FREE.TXT", &rc);
    CHECK(rc == VFS_OK && n != NULL);
    CHECK(fat16_ops()->read(&fs, n, 1024, g_rbuf, 512) == VFS_EIO);

    /* 坏链上的 truncate 只走到需要的簇，可以成功；数据区不越界。 */
    n = lookup_raw(&fs, root, "LOOP.TXT", &rc);
    CHECK(fat16_ops()->truncate(&fs, n, 100) == VFS_OK);
    CHECK(lookup_raw(&fs, root, "LOOP.TXT", &rc) == n);

    /* 目录链循环：子目录簇全填 0xE5（删除项），读完一簇后要跟随
     * FAT[14]=14 的自环 -> readdir EIO。 */
    {
        struct image im2;
        struct fake_disk d2;
        struct blk_env b2;
        struct fat16 fs2;
        struct vfs_node *r2;
        struct vfs_node *sub;
        u8 deleted[1024];

        std_image(&im2);
        img_root_entry(&im2, 4, "DIR", FAT16_ATTR_DIR, 14, 0);
        memset(deleted, 0xE5, sizeof(deleted));
        img_cluster_data(&im2, 14, 0, deleted, (u32)sizeof(deleted));
        img_set_fat(&im2, 14, 14);
        disk_bind(&d2, &im2);
        blk_bind(&b2, &d2);
        fat16_init(&fs2);
        CHECK(fat16_mount(&fs2, &b2) == VFS_OK);
        r2 = fat16_root(&fs2);
        sub = lookup_raw(&fs2, r2, "DIR", &rc);
        CHECK(rc == VFS_OK && sub != NULL);
        {
            char name[VFS_NAME_MAX];
            struct vfs_stat st;

            CHECK(fat16_ops()->readdir(&fs2, sub, 0, name,
                                       (u32)sizeof(name), &st) == VFS_EIO);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 8. 满盘 ENOSPC                                                            */
/* ------------------------------------------------------------------------- */

static void t_enospc(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_node *root;
    struct vfs_node *a = NULL;
    struct vfs_node *b = NULL;
    u32 i;
    int rc = 0;

    /* 32 扇区、1 扇区/簇、FAT=1：28 个数据簇，写满 28*512 = 14336 字节。 */
    img_format(&im, g_img, 32, 1, 1, 2, 1, 16);
    CHECK(im.clusters == 28);
    disk_bind(&disk, &im);
    blk_bind(&blk, &disk);
    fat16_init(&fs);
    CHECK(fat16_mount(&fs, &blk) == VFS_OK);
    root = fat16_root(&fs);
    CHECK(fat16_free_clusters(&fs) == 28);

    for (i = 0; i < 14336u; i++) {
        g_big[i] = (u8)(i * 3u);
    }
    CHECK(create_raw(&fs, root, "FILL.BIN", VFS_NODE_FILE, &a) == VFS_OK);
    a = lookup_raw(&fs, root, "FILL.BIN", &rc);
    CHECK(rc == VFS_OK && a != NULL);
    CHECK(fat16_ops()->write(&fs, a, 0, g_big, 14336) == 14336);
    CHECK(a->size == 14336);
    CHECK(fat16_free_clusters(&fs) == 0);

    /* 满盘：新文件写 1 字节 -> ENOSPC（一个簇也分配不到）。 */
    CHECK(create_raw(&fs, root, "MORE.BIN", VFS_NODE_FILE, &b) == VFS_OK);
    b = lookup_raw(&fs, root, "MORE.BIN", &rc);
    CHECK(rc == VFS_OK && b != NULL);
    CHECK(fat16_ops()->write(&fs, b, 0, "x", 1) == VFS_ENOSPC);
    CHECK(b->size == 0);
    CHECK(fat16_stat(&fs)->enospc >= 1);

    /* 满盘扩大截断 -> ENOSPC。 */
    CHECK(fat16_ops()->truncate(&fs, b, 600) == VFS_ENOSPC);
    CHECK(b->size == 0);

    /* 删除大文件后空间回收，同一个文件可以再写。 */
    CHECK(fat16_ops()->unlink(&fs, root, "FILL.BIN", 8) == VFS_OK);
    CHECK(fat16_free_clusters(&fs) == 28);
    CHECK(fat16_ops()->write(&fs, b, 0, "ok", 2) == 2);
    CHECK(b->size == 2);
    CHECK(fat16_free_clusters(&fs) == 27);

    /* 再写满剩余空间，验证最后一个簇用尽后 ENOSPC。 */
    CHECK(fat16_ops()->write(&fs, b, 2, g_big, 27 * 512 - 2) ==
          27 * 512 - 2);
    CHECK(fat16_free_clusters(&fs) == 1);
    CHECK(fat16_ops()->write(&fs, b, 27 * 512, g_big, 512) == 512);
    CHECK(fat16_free_clusters(&fs) == 0);
    CHECK(b->size == 28 * 512);
    CHECK(fat16_ops()->write(&fs, b, 28 * 512, "z", 1) == VFS_ENOSPC);
}

/* ------------------------------------------------------------------------- */
/* 9. VFS 集成                                                               */
/* ------------------------------------------------------------------------- */

static void t_vfs_integration(void)
{
    struct fat16 fs;
    struct fake_disk disk;
    struct blk_env blk;
    struct image im;
    struct vfs_ctx ctx;
    struct vfs_stat st;
    int fd;
    int fd2;
    int n;
    u32 i;

    std_mount(&fs, &disk, &blk, &im);
    vfs_reset();
    vfs_ctx_init(&ctx);
    CHECK(vfs_mount("/", fat16_ops(), &fs, fat16_root(&fs)) == VFS_OK);
    CHECK(vfs_mount_count() == 1);

    /* stat / open / read 走完整 VFS 路径。 */
    CHECK(vfs_stat(&ctx, "/HELLO.TXT", &st) == VFS_OK);
    CHECK(st.type == VFS_NODE_FILE);
    CHECK(st.size == 18);
    CHECK((st.mode & VFS_S_IFMT) == VFS_S_IFREG);
    CHECK(st.ino != 0);
    fd = vfs_open(&ctx, "/HELLO.TXT", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    memset(g_rbuf, 0, 32);
    CHECK(vfs_read(&ctx, fd, g_rbuf, 32) == 18);
    CHECK(memcmp(g_rbuf, k_hello, 18) == 0);
    CHECK(vfs_read(&ctx, fd, g_rbuf, 32) == 0);
    CHECK(vfs_close(&ctx, fd) == VFS_OK);

    /* 子目录 + listdir。 */
    CHECK(vfs_stat(&ctx, "/DIR", &st) == VFS_OK && st.type == VFS_NODE_DIR);
    fd = vfs_open(&ctx, "/DIR/A.TXT", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&ctx, fd, g_rbuf, 16) == 6);
    CHECK(memcmp(g_rbuf, k_a, 6) == 0);
    CHECK(vfs_close(&ctx, fd) == VFS_OK);
    {
        struct vfs_dirent ents[8];

        n = vfs_listdir(&ctx, "/DIR", ents, 8);
        CHECK(n == 1 && strcmp(ents[0].name, "A.TXT") == 0);
    }

    /* mkdir + create + write + read + truncate + rename + unlink。 */
    CHECK(vfs_mkdir(&ctx, "/VDIR", 0755) == VFS_OK);
    CHECK(vfs_mkdir(&ctx, "/VDIR", 0755) == VFS_EEXIST);
    CHECK(vfs_mkdir(&ctx, "/TOOLONGNAME.TXT", 0755) == VFS_ENAMETOOLONG);
    CHECK(vfs_stat(&ctx, "/VDIR", &st) == VFS_OK && st.type == VFS_NODE_DIR);
    fd = vfs_open(&ctx, "/VDIR/X.TXT", VFS_O_CREAT | VFS_O_RDWR, 0644);
    CHECK(fd > 0);
    for (i = 0; i < 2000u; i++) {
        g_big[i] = (u8)(i * 11u + 5u);
    }
    CHECK(vfs_write(&ctx, fd, g_big, 2000) == 2000);
    CHECK(vfs_close(&ctx, fd) == VFS_OK);
    CHECK(vfs_stat(&ctx, "/VDIR/X.TXT", &st) == VFS_OK && st.size == 2000);
    fd = vfs_open(&ctx, "/VDIR/X.TXT", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(vfs_read(&ctx, fd, g_rbuf, 2000) == 2000);
    CHECK(memcmp(g_rbuf, g_big, 2000) == 0);
    CHECK(vfs_close(&ctx, fd) == VFS_OK);
    CHECK(vfs_truncate(&ctx, "/VDIR/X.TXT", 100) == VFS_OK);
    CHECK(vfs_stat(&ctx, "/VDIR/X.TXT", &st) == VFS_OK && st.size == 100);
    CHECK(vfs_rename(&ctx, "/VDIR/X.TXT", "/VDIR/Y.TXT") == VFS_OK);
    CHECK(vfs_stat(&ctx, "/VDIR/X.TXT", &st) == VFS_ENOENT);
    CHECK(vfs_stat(&ctx, "/VDIR/Y.TXT", &st) == VFS_OK && st.size == 100);
    CHECK(vfs_rename(&ctx, "/VDIR/Y.TXT", "/HELLO.TXT") == VFS_EEXIST);
    CHECK(vfs_rename(&ctx, "/VDIR/Y.TXT", "/DIR/Y.TXT") == VFS_EINVAL);
    CHECK(vfs_unlink(&ctx, "/VDIR/Y.TXT") == VFS_OK);
    CHECK(vfs_stat(&ctx, "/VDIR/Y.TXT", &st) == VFS_ENOENT);

    /* 目录 unlink -> EISDIR；符号链接 -> EROFS（FAT16 没有 symlink）。 */
    CHECK(vfs_unlink(&ctx, "/DIR") == VFS_EISDIR);
    CHECK(vfs_symlink(&ctx, "/LINK", "/HELLO.TXT") == VFS_EROFS);
    CHECK(vfs_readlink(&ctx, "/HELLO.TXT", (char *)g_rbuf, 16) == VFS_EINVAL);

    /* 打开中的文件 unlink -> EBUSY；close 后可删。 */
    fd = vfs_open(&ctx, "/VDIR", VFS_O_RDONLY, 0);
    CHECK(fd > 0);
    /* 打开目录后仍可读文件（节点缓存不互相踩）。 */
    fd2 = vfs_open(&ctx, "/HELLO.TXT", VFS_O_RDONLY, 0);
    CHECK(fd2 > 0);
    CHECK(vfs_unlink(&ctx, "/HELLO.TXT") == VFS_EBUSY);
    CHECK(vfs_close(&ctx, fd2) == VFS_OK);
    CHECK(vfs_unlink(&ctx, "/HELLO.TXT") == VFS_OK);
    CHECK(vfs_stat(&ctx, "/HELLO.TXT", &st) == VFS_ENOENT);
    CHECK(vfs_close(&ctx, fd) == VFS_OK);

    /* 干净卸载（根挂载不允许，换一个子挂载验证 ERROR 语义）。 */
    CHECK(vfs_umount("/") == VFS_EBUSY);
    vfs_reset();
    CHECK(vfs_mount_count() == 0);
}

/* ------------------------------------------------------------------------- */

int test_fat16(void)
{
    t_mount_bad_bpb();
    t_mount_geometry();
    t_lookup_read();
    t_readdir();
    t_create_write();
    t_truncate();
    t_rename_unlink();
    t_chain_edges();
    t_enospc();
    t_vfs_integration();
    return 0;
}
