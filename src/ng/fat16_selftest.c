/*
 * ng WP2-2: FAT16 内核自检（run-ng 的证据行）。
 *
 *   ng: fat16 mount ok (clusters=2841)   嵌入镜像 -> PMM RAM 盘 -> /mnt/fat
 *   ng: fat16 read ok                    HELLO.TXT / DIR/A.TXT / listdir
 *   ng: fat16 write ok                   NEW.TXT 创建/写/stat/截断/改名/删除
 *   ng: fat16 chain edge ok (n=4)        合成镜像的循环/截断/坏簇/越界检测
 *   ng: fat16 bad bpb rejected (n=11)    坏几何逐个 EINVAL
 *   ng: WP2-2 selftest PASS              （main.c）
 *
 * 真实镜像来自 tools/mkfat-ng.ps1（1.44MB，HELLO.TXT/18B、DIR/A.TXT/6B），
 * 由 build-ng.ps1 以 .incbin 嵌进 .rodata，fat_ramdisk 复制到 PMM 页。
 * 合成镜像在 g_syn 里现场构造（32 KiB），只用于负向/边界用例，不占 PMM。
 */
#include "fat16_selftest.h"

#include "blk.h"
#include "fat16.h"
#include "fat_ramdisk.h"
#include "fs_syscall.h"
#include "serial.h"
#include "types.h"
#include "vfs.h"

static struct fat16 g_fat;
static struct fat16 g_syn_fs;
static u8 g_wbuf[1024];
static u8 g_rbuf[1024];
static int g_fails;

#define SYN_SECTORS 64u
#define SYN_BYTES (SYN_SECTORS * FAT16_SECTOR_SIZE)
static u8 g_syn[SYN_BYTES];

static void fail_item(const char *what)
{
    g_fails++;
    serial_puts("ng: ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

static void kv_zero(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

static void kv_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int kv_eq(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    u32 i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* 合成镜像（32 KiB：64 扇区、2 扇区/簇 = 1024B、30 个数据簇）               */
/* ------------------------------------------------------------------------- */

static void syn_u16(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

static void syn_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void syn_set_fat(u32 cluster, u32 value)
{
    u32 f;

    for (f = 0; f < 2u; f++) {
        u8 *fat = g_syn + (1u + f * 1u) * FAT16_SECTOR_SIZE;

        syn_u16(fat + cluster * 2u, value);
    }
}

/* 几何：reserved=1、FAT=2x1、root=16（1 扇区）、data.start=4。 */
static void syn_bpb(u32 total)
{
    u8 *s = g_syn;

    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    kv_copy(s + 3, "ASTER   ", 8);
    syn_u16(s + 11, 512);
    s[13] = 2; /* 2 扇区/簇 */
    syn_u16(s + 14, 1);
    s[16] = 2;
    syn_u16(s + 17, 16);
    syn_u16(s + 19, total);
    s[21] = 0xF8;
    syn_u16(s + 22, 1);
    syn_u16(s + 24, 18);
    syn_u16(s + 26, 2);
    s[38] = 0x29;
    syn_u32(s + 39, 0x4E473032u);
    kv_copy(s + 43, "SYNTHETIC  ", 11);
    kv_copy(s + 54, "FAT16   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;
}

/* 建一个 SYN.TXT（首簇 2、size 字节、FAT[2]=fat_value）的小镜像。 */
static void syn_build(u32 fat_value, u32 size)
{
    u8 entry[32];
    u32 i;

    kv_zero(g_syn, SYN_BYTES);
    syn_bpb(SYN_SECTORS);
    syn_set_fat(0, 0xFFF8);
    syn_set_fat(1, 0xFFFF);
    syn_set_fat(2, fat_value);
    /* 根目录第 1 项："SYN     TXT"。 */
    kv_zero(entry, 32);
    kv_copy(entry, "SYN     TXT", 11);
    entry[11] = 0x20;
    syn_u16(entry + 26, 2);
    syn_u32(entry + 28, size);
    kv_copy(g_syn + 3u * FAT16_SECTOR_SIZE, entry, 32);
    /* 数据区首扇区写模式（第二簇保持 0）。 */
    for (i = 0; i < 512u; i++) {
        g_syn[4u * FAT16_SECTOR_SIZE + i] = (u8)(i * 5u + 1u);
    }
}

static int syn_read(void *ctx, u64 sector, void *buf, u32 count)
{
    (void)ctx;
    if (buf == NULL || count == 0 || sector + (u64)count > SYN_SECTORS) {
        return -1;
    }
    kv_copy(buf, g_syn + sector * FAT16_SECTOR_SIZE,
            count * FAT16_SECTOR_SIZE);
    return 0;
}

static int syn_write(void *ctx, u64 sector, const void *buf, u32 count)
{
    (void)ctx;
    if (buf == NULL || count == 0 || sector + (u64)count > SYN_SECTORS) {
        return -1;
    }
    kv_copy(g_syn + sector * FAT16_SECTOR_SIZE, buf,
            count * FAT16_SECTOR_SIZE);
    return 0;
}

/* 读 SYN.TXT：前 1024 字节（首簇）应成功；offset 1024 开始要遍历 FAT。 */
static int syn_edge_detected(u32 fat_value, u32 size)
{
    struct blk_env blk;
    struct vfs_node *root;
    struct vfs_node *node = NULL;
    int rc;

    syn_build(fat_value, size);
    blk.read = syn_read;
    blk.write = syn_write;
    blk.sectors = SYN_SECTORS;
    blk.ctx = NULL;
    fat16_init(&g_syn_fs);
    if (fat16_mount(&g_syn_fs, &blk) != VFS_OK) {
        return 0;
    }
    root = fat16_root(&g_syn_fs);
    rc = fat16_ops()->lookup(&g_syn_fs, root, "SYN.TXT", 7, &node);
    if (rc != VFS_OK || node == NULL) {
        return 0;
    }
    if (fat16_ops()->read(&g_syn_fs, node, 0, g_rbuf, 1024) != 1024) {
        return 0;
    }
    return (fat16_ops()->read(&g_syn_fs, node, 1024, g_rbuf, 512) == VFS_EIO);
}

/* ------------------------------------------------------------------------- */
/* 1. 真实镜像：挂载 / 读 / 写                                               */
/* ------------------------------------------------------------------------- */

static int check_mount(struct vfs_ctx *c)
{
    const struct blk_env *blk;
    struct vfs_stat st;
    int rc;

    if (fat_ramdisk_init() != 0) {
        fail_item("fat16 mount");
        return 1;
    }
    blk = fat_ramdisk_env();
    if (blk == NULL || blk->sectors != 2880u ||
        fat_ramdisk_sectors() != 2880u) {
        fail_item("fat16 mount");
        return 1;
    }
    rc = vfs_mkdir(c, "/mnt", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        fail_item("fat16 mount");
        return 1;
    }
    rc = vfs_mkdir(c, "/mnt/fat", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        fail_item("fat16 mount");
        return 1;
    }
    fat16_init(&g_fat);
    rc = fat16_mount(&g_fat, blk);
    if (rc != VFS_OK) {
        serial_puts("ng: fat16 mount rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("fat16 mount");
        return 1;
    }
    rc = vfs_mount("/mnt/fat", fat16_ops(), &g_fat, fat16_root(&g_fat));
    if (rc != VFS_OK) {
        fail_item("fat16 mount");
        return 1;
    }
    if (vfs_stat(c, "/mnt/fat", &st) != VFS_OK || st.type != VFS_NODE_DIR) {
        fail_item("fat16 mount");
        return 1;
    }
    if (fat16_cluster_count(&g_fat) != 2841u ||
        fat16_bytes_per_cluster(&g_fat) != 512u) {
        fail_item("fat16 mount");
        return 1;
    }
    serial_puts("ng: fat16 mount ok (clusters=");
    serial_putdec((u64)fat16_cluster_count(&g_fat));
    serial_puts(")\r\n");
    return 0;
}

static int check_read(struct vfs_ctx *c)
{
    static const char hello[] = "hello from fat16\r\n";
    static const char a[] = "dir a\n";
    struct vfs_stat st;
    struct vfs_dirent ents[8];
    int fd;
    int n;

    fd = vfs_open(c, "/mnt/fat/HELLO.TXT", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("fat16 read");
        return 1;
    }
    kv_zero(g_rbuf, (u32)sizeof(g_rbuf));
    n = vfs_read(c, fd, g_rbuf, 32);
    if (vfs_close(c, fd) != VFS_OK || n != 18 ||
        !kv_eq(g_rbuf, hello, 18)) {
        fail_item("fat16 read");
        return 1;
    }
    if (vfs_stat(c, "/mnt/fat/HELLO.TXT", &st) != VFS_OK || st.size != 18) {
        fail_item("fat16 read");
        return 1;
    }
    fd = vfs_open(c, "/mnt/fat/DIR/A.TXT", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("fat16 read");
        return 1;
    }
    kv_zero(g_rbuf, 16);
    n = vfs_read(c, fd, g_rbuf, 16);
    if (vfs_close(c, fd) != VFS_OK || n != 6 || !kv_eq(g_rbuf, a, 6)) {
        fail_item("fat16 read");
        return 1;
    }
    n = vfs_listdir(c, "/mnt/fat", ents, 8);
    if (n != 2 || ents[0].type != VFS_NODE_FILE ||
        ents[1].type != VFS_NODE_DIR ||
        ents[0].name[0] != 'H' || ents[1].name[0] != 'D') {
        fail_item("fat16 read");
        return 1;
    }
    n = vfs_listdir(c, "/mnt/fat/DIR", ents, 8);
    if (n != 1 || ents[0].name[0] != 'A' || ents[0].size != 6) {
        fail_item("fat16 read");
        return 1;
    }
    serial_puts("ng: fat16 read ok\r\n");
    return 0;
}

static int check_write(struct vfs_ctx *c)
{
    struct vfs_stat st;
    int fd;
    u32 i;

    for (i = 0; i < 600u; i++) {
        g_wbuf[i] = (u8)(i * 7u + 0x21u);
    }
    fd = vfs_open(c, "/mnt/fat/NEW.TXT", VFS_O_CREAT | VFS_O_RDWR, 0644);
    if (fd <= 0) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_write(c, fd, g_wbuf, 600) != 600 ||
        vfs_close(c, fd) != VFS_OK) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_stat(c, "/mnt/fat/NEW.TXT", &st) != VFS_OK || st.size != 600) {
        fail_item("fat16 write");
        return 1;
    }
    fd = vfs_open(c, "/mnt/fat/NEW.TXT", VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("fat16 write");
        return 1;
    }
    kv_zero(g_rbuf, 700);
    if (vfs_read(c, fd, g_rbuf, 700) != 600 ||
        !kv_eq(g_rbuf, g_wbuf, 600) || vfs_close(c, fd) != VFS_OK) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_stat(c, "/mnt/fat/NEW.TXT", &st) != VFS_OK || st.size != 600) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_truncate(c, "/mnt/fat/NEW.TXT", 100) != VFS_OK ||
        vfs_stat(c, "/mnt/fat/NEW.TXT", &st) != VFS_OK || st.size != 100) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_rename(c, "/mnt/fat/NEW.TXT", "/mnt/fat/NEW2.TXT") != VFS_OK ||
        vfs_stat(c, "/mnt/fat/NEW.TXT", &st) != VFS_ENOENT ||
        vfs_stat(c, "/mnt/fat/NEW2.TXT", &st) != VFS_OK || st.size != 100) {
        fail_item("fat16 write");
        return 1;
    }
    if (vfs_unlink(c, "/mnt/fat/NEW2.TXT") != VFS_OK ||
        vfs_stat(c, "/mnt/fat/NEW2.TXT", &st) != VFS_ENOENT) {
        fail_item("fat16 write");
        return 1;
    }
    serial_puts("ng: fat16 write ok\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 2. 坏 BPB / 簇链边界                                                      */
/* ------------------------------------------------------------------------- */

static void check_bad_bpb(void)
{
    struct blk_env blk;
    u32 n = 0;

    blk.read = syn_read;
    blk.write = syn_write;
    blk.sectors = SYN_SECTORS;
    blk.ctx = NULL;

#define SYN_MOUNT_AFTER(patch)                                                 \
    do {                                                                       \
        syn_build(0xFFFF, 0);                                                  \
        patch;                                                                 \
        fat16_init(&g_syn_fs);                                                 \
        if (fat16_mount(&g_syn_fs, &blk) == VFS_EINVAL) {                      \
            n++;                                                               \
        }                                                                      \
    } while (0)

    SYN_MOUNT_AFTER(g_syn[510] = 0);
    SYN_MOUNT_AFTER(syn_u16(g_syn + 11, 1024));
    SYN_MOUNT_AFTER(g_syn[13] = 0);
    SYN_MOUNT_AFTER(g_syn[13] = 3);
    SYN_MOUNT_AFTER(syn_u16(g_syn + 14, 0));
    SYN_MOUNT_AFTER(g_syn[16] = 0);
    SYN_MOUNT_AFTER(syn_u16(g_syn + 17, 0));
    SYN_MOUNT_AFTER(syn_u16(g_syn + 22, 0));
    SYN_MOUNT_AFTER(syn_u16(g_syn + 19, 0));
    SYN_MOUNT_AFTER(syn_u16(g_syn + 19, 128));
    SYN_MOUNT_AFTER(syn_u16(g_syn + 19, 4); syn_u16(g_syn + 14, 4);
                    syn_u16(g_syn + 17, 16); syn_u16(g_syn + 22, 10));
#undef SYN_MOUNT_AFTER

    if (n != 11) {
        serial_puts("ng: fat16 bad bpb count=");
        serial_putdec((u64)n);
        serial_puts("\r\n");
        fail_item("fat16 bad bpb rejected");
    } else {
        serial_puts("ng: fat16 bad bpb rejected (n=");
        serial_putdec((u64)n);
        serial_puts(")\r\n");
    }
}

static void check_chain_edges(void)
{
    u32 n = 0;

    if (syn_edge_detected(2, 2048)) { /* 自环 */
        n++;
    }
    if (syn_edge_detected(0xFFF8, 2048)) { /* 链截断 */
        n++;
    }
    if (syn_edge_detected(0xFFF7, 2048)) { /* 坏簇 */
        n++;
    }
    if (syn_edge_detected(0x7FFF, 2048)) { /* 越界簇号 */
        n++;
    }
    if (n != 4) {
        serial_puts("ng: fat16 chain edge count=");
        serial_putdec((u64)n);
        serial_puts("\r\n");
        fail_item("fat16 chain edge");
    } else {
        serial_puts("ng: fat16 chain edge ok (n=");
        serial_putdec((u64)n);
        serial_puts(")\r\n");
    }
}

/* ------------------------------------------------------------------------- */

int fat16_selftest(void)
{
    struct vfs_ctx *c;

    g_fails = 0;
    c = fs_sys_ctx();
    g_fails += check_mount(c);
    if (g_fails == 0) {
        g_fails += check_read(c);
        g_fails += check_write(c);
    }
    check_bad_bpb(); /* 计数不匹配时自己 fail_item，不再重复加到 g_fails */
    check_chain_edges();
    return g_fails;
}
