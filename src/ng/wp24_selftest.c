/*
 * ng WP2-4: 块设备（ATA PIO + 真实磁盘上的 FAT16）内核接线与自检。
 *
 * 职责：
 *   - 用 PCI 确认 QEMU PIIX3 IDE（8086:7010）存在；QEMU 的兼容（legacy）
 *     模式把主通道固定在 0x1F0/0x3F6，因此驱动直接用这组端口（PCI 找不到
 *     时也回退到它们，见 docs/design/block-ng.md）；
 *   - 注入 inb/outb/inw/outw，IDENTIFY 主盘；
 *   - 读 0 号扇区校验 BPB 签名；写最后一个扇区再读回校验（随后恢复原内容，
 *     重复运行幂等）；
 *   - 用 fake 控制器验证“BSY 永不落地时轮询有界、返回超时、不 panic”；
 *   - ata_blk_bind -> FAT16 挂到 /mnt/disk，读 HELLO.TXT、建/写/读回/stat
 *     NG24.TXT、unlink。
 *
 * 证据行（run-ng 断言）：
 *   ng: ata pci ide found (8086:7010 ...)
 *   ng: ata identify ok (model=..., sectors=...)
 *   ng: ata read ok (lba=0 sig=0xaa55)
 *   ng: ata write ok (lba=..., verify=1 restored=1)
 *   ng: ata timeout rejected (n=...)
 *   ng: disk fat16 mount ok (clusters=...)
 *   ng: disk file rw ok (bytes=600)
 *   ng: WP2-4 selftest PASS               （main.c 补）
 */
#include "wp24.h"

#include "ata.h"
#include "ata_blk.h"
#include "blk.h"
#include "fat16.h"
#include "fs_syscall.h"
#include "io.h"
#include "pci.h"
#include "serial.h"
#include "types.h"
#include "vfs.h"

#define IDE_VENDOR_ID 0x8086u
#define IDE_DEVICE_ID 0x7010u
#define DISK_MOUNT "/mnt/disk"
#define DISK_FILE "/mnt/disk/NG24.TXT"
#define DISK_HELLO "/mnt/disk/HELLO.TXT"

static struct ata_dev g_ata;
static struct fat16 g_disk_fat;
static u8 g_buf[512];
static u8 g_wbuf[1024];
static u8 g_rbuf[1024];

/* ------------------------------------------------------------------------- */
/* 端口 IO 注入                                                               */
/* ------------------------------------------------------------------------- */

static u8 hw_in8(void *ctx, u16 port)
{
    (void)ctx;
    return inb(port);
}

static void hw_out8(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    outb(port, value);
}

static u16 hw_in16(void *ctx, u16 port)
{
    (void)ctx;
    return inw(port);
}

static void hw_out16(void *ctx, u16 port, u16 value)
{
    (void)ctx;
    outw(port, value);
}

static const struct ata_io g_ata_io = {hw_in8, hw_out8, hw_in16, hw_out16,
                                       NULL};

static void pci_out32(void *ctx, u16 port, u32 value)
{
    (void)ctx;
    outl(port, value);
}

static u32 pci_in32(void *ctx, u16 port)
{
    (void)ctx;
    return inl(port);
}

static const struct pci_io_ops g_pci_io = {pci_out32, pci_in32, NULL};

/* ------------------------------------------------------------------------- */
/* 小工具                                                                     */
/* ------------------------------------------------------------------------- */

static void kv_zero(void *dst, u32 n)
{
    u8 *d = (u8 *)dst;
    u32 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
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

static int name_eq(const char *a, const char *b)
{
    u32 i = 0;

    while (a[i] != '\0' || b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void fail_item(const char *what)
{
    serial_puts("ng: ");
    serial_puts(what);
    serial_puts(" FAIL\r\n");
}

/* ------------------------------------------------------------------------- */
/* PCI：确认 PIIX3 IDE（找不到也继续用 legacy 端口）                          */
/* ------------------------------------------------------------------------- */

static int ata_probe_pci(void)
{
    struct pci_loc loc;

    if (pci_find(&g_pci_io, IDE_VENDOR_ID, IDE_DEVICE_ID, &loc) != 0) {
        serial_puts("ng: ata pci ide missing (8086:7010), legacy ports in use\r\n");
        return 0;
    }
    (void)pci_enable(&g_pci_io, &loc, PCI_CMD_IO_SPACE);
    serial_puts("ng: ata pci ide found (8086:7010 bus=");
    serial_putdec((u64)loc.bus);
    serial_puts(" dev=");
    serial_putdec((u64)loc.dev);
    serial_puts(" func=");
    serial_putdec((u64)loc.func);
    serial_puts(")\r\n");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* ATA 自检：读 0 号扇区 / 写末扇区并恢复                                    */
/* ------------------------------------------------------------------------- */

static int ata_read_selftest(void)
{
    u16 sig;

    kv_zero(g_buf, (u32)sizeof(g_buf));
    if (ata_read28(&g_ata, 0, g_buf, 1) != ATA_OK) {
        fail_item("ata read");
        return 1;
    }
    sig = (u16)((u16)g_buf[510] | ((u16)g_buf[511] << 8));
    if (g_buf[510] != 0x55u || g_buf[511] != 0xAAu ||
        (u16)((u16)g_buf[11] | ((u16)g_buf[12] << 8)) != 512u) {
        fail_item("ata read");
        return 1;
    }
    serial_puts("ng: ata read ok (lba=0 sig=");
    serial_puthex((u64)sig);
    serial_puts(")\r\n");
    return 0;
}

static int ata_write_selftest(void)
{
    u32 last = g_ata.sectors - 1u;
    u8 orig[512];
    u32 i;

    if (ata_read28(&g_ata, last, orig, 1) != ATA_OK) {
        fail_item("ata write");
        return 1;
    }
    for (i = 0; i < (u32)sizeof(g_buf); i++) {
        g_buf[i] = (u8)(0xA5u ^ i);
    }
    if (ata_write28(&g_ata, last, g_buf, 1) != ATA_OK) {
        fail_item("ata write");
        return 1;
    }
    kv_zero(g_rbuf, (u32)sizeof(g_buf));
    if (ata_read28(&g_ata, last, g_rbuf, 1) != ATA_OK ||
        !kv_eq(g_rbuf, g_buf, (u32)sizeof(g_buf))) {
        (void)ata_write28(&g_ata, last, orig, 1);
        fail_item("ata write");
        return 1;
    }
    /* 恢复原内容：同一镜像重复跑 run-ng 不受影响。 */
    if (ata_write28(&g_ata, last, orig, 1) != ATA_OK) {
        fail_item("ata write");
        return 1;
    }
    serial_puts("ng: ata write ok (lba=");
    serial_putdec((u64)last);
    serial_puts(" verify=1 restored=1)\r\n");
    return 0;
}

/*
 * 超时路径：fake 控制器永远返回 BSY=1。poll_limit=64，必须有限返回
 * ATA_ETIMEDOUT 且计数 +1 —— 证明驱动不会在坏设备上无限等/panic。
 */
static u8 fog_in8(void *ctx, u16 port)
{
    (void)ctx;
    (void)port;
    return ATA_SR_BSY;
}

static void fog_out8(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    (void)port;
    (void)value;
}

static u16 fog_in16(void *ctx, u16 port)
{
    (void)ctx;
    (void)port;
    return 0;
}

static void fog_out16(void *ctx, u16 port, u16 value)
{
    (void)ctx;
    (void)port;
    (void)value;
}

static const struct ata_io g_fog_io = {fog_in8, fog_out8, fog_in16, fog_out16,
                                       NULL};

static int ata_timeout_selftest(void)
{
    struct ata_dev dev;
    int rc;

    ata_dev_init(&dev, &g_fog_io, ATA_PRIMARY_IO_BASE, ATA_PRIMARY_CTRL_BASE,
                 ATA_DRIVE_MASTER);
    dev.poll_limit = 64u;
    rc = ata_identify(&dev);
    if (rc != ATA_ETIMEDOUT || dev.timeouts != 1u || dev.present != 0u) {
        serial_puts("ng: ata timeout n=");
        serial_putdec((u64)rc);
        serial_puts("\r\n");
        fail_item("ata timeout rejected");
        return 1;
    }
    serial_puts("ng: ata timeout rejected (n=");
    serial_putdec((u64)dev.poll_limit);
    serial_puts(")\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 真实磁盘上的 FAT16：挂载 / 文件读写                                        */
/* ------------------------------------------------------------------------- */

static int disk_mount_selftest(struct vfs_ctx *c)
{
    const struct blk_env *blk;
    struct vfs_stat st;
    int rc;

    if (ata_blk_bind(&g_ata) != 0) {
        fail_item("disk fat16 mount");
        return 1;
    }
    blk = ata_blk_env();
    if (blk == NULL || blk->read == NULL || blk->write == NULL ||
        blk->sectors != (u64)g_ata.sectors) {
        fail_item("disk fat16 mount");
        return 1;
    }
    rc = vfs_mkdir(c, "/mnt", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        fail_item("disk fat16 mount");
        return 1;
    }
    rc = vfs_mkdir(c, DISK_MOUNT, 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        fail_item("disk fat16 mount");
        return 1;
    }
    fat16_init(&g_disk_fat);
    rc = fat16_mount(&g_disk_fat, blk);
    if (rc != VFS_OK) {
        serial_puts("ng: disk fat16 mount rc=");
        serial_putdec((u64)(-rc));
        serial_puts("\r\n");
        fail_item("disk fat16 mount");
        return 1;
    }
    if (fat16_total_sectors(&g_disk_fat) != g_ata.sectors) {
        fail_item("disk fat16 mount");
        return 1;
    }
    rc = vfs_mount(DISK_MOUNT, fat16_ops(), &g_disk_fat,
                   fat16_root(&g_disk_fat));
    if (rc != VFS_OK || vfs_stat(c, DISK_MOUNT, &st) != VFS_OK ||
        st.type != VFS_NODE_DIR) {
        fail_item("disk fat16 mount");
        return 1;
    }
    serial_puts("ng: disk fat16 mount ok (clusters=");
    serial_putdec((u64)fat16_cluster_count(&g_disk_fat));
    serial_puts(")\r\n");
    return 0;
}

static int disk_file_read(struct vfs_ctx *c)
{
    static const char hello[] = "hello from fat16\r\n";
    struct vfs_stat st;
    struct vfs_dirent ents[8];
    int fd;
    int n;
    int have_hello = 0;
    int have_dir = 0;
    int i;

    fd = vfs_open(c, DISK_HELLO, VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("disk file rw");
        return 1;
    }
    kv_zero(g_rbuf, (u32)sizeof(g_rbuf));
    n = vfs_read(c, fd, g_rbuf, 32);
    if (vfs_close(c, fd) != VFS_OK || n != 18 || !kv_eq(g_rbuf, hello, 18)) {
        fail_item("disk file rw");
        return 1;
    }
    if (vfs_stat(c, DISK_HELLO, &st) != VFS_OK || st.size != 18u) {
        fail_item("disk file rw");
        return 1;
    }
    /* 镜像在多次 run-ng 之间会保留 ring3 建的 DISK.TXT，所以只要求集合里
     * 有 HELLO.TXT 和 DIR，不要求条目总数固定。 */
    n = vfs_listdir(c, DISK_MOUNT, ents, 8);
    if (n < 2 || n > 8) {
        fail_item("disk file rw");
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (ents[i].type == VFS_NODE_FILE && name_eq(ents[i].name, "HELLO.TXT")) {
            have_hello = 1;
        }
        if (ents[i].type == VFS_NODE_DIR && name_eq(ents[i].name, "DIR")) {
            have_dir = 1;
        }
    }
    if (!have_hello || !have_dir) {
        fail_item("disk file rw");
        return 1;
    }
    return 0;
}

static int disk_file_write(struct vfs_ctx *c)
{
    struct vfs_stat st;
    int fd;
    u32 i;

    for (i = 0; i < 600u; i++) {
        g_wbuf[i] = (u8)(i * 7u + 0x21u);
    }
    fd = vfs_open(c, DISK_FILE, VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC, 0644);
    if (fd <= 0) {
        fail_item("disk file rw");
        return 1;
    }
    if (vfs_write(c, fd, g_wbuf, 600) != 600 ||
        vfs_close(c, fd) != VFS_OK) {
        fail_item("disk file rw");
        return 1;
    }
    if (vfs_stat(c, DISK_FILE, &st) != VFS_OK || st.size != 600u) {
        fail_item("disk file rw");
        return 1;
    }
    fd = vfs_open(c, DISK_FILE, VFS_O_RDONLY, 0);
    if (fd <= 0) {
        fail_item("disk file rw");
        return 1;
    }
    kv_zero(g_rbuf, 700);
    if (vfs_read(c, fd, g_rbuf, 700) != 600 ||
        !kv_eq(g_rbuf, g_wbuf, 600) || vfs_close(c, fd) != VFS_OK) {
        fail_item("disk file rw");
        return 1;
    }
    if (vfs_unlink(c, DISK_FILE) != VFS_OK ||
        vfs_stat(c, DISK_FILE, &st) != VFS_ENOENT) {
        fail_item("disk file rw");
        return 1;
    }
    return 0;
}

static int disk_file_selftest(struct vfs_ctx *c)
{
    if (disk_file_read(c) != 0) {
        return 1;
    }
    if (disk_file_write(c) != 0) {
        return 1;
    }
    serial_puts("ng: disk file rw ok (bytes=600)\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int wp24_selftest(void)
{
    struct vfs_ctx *c;
    int fails = 0;

    (void)ata_probe_pci();

    ata_dev_init(&g_ata, &g_ata_io, ATA_PRIMARY_IO_BASE,
                 ATA_PRIMARY_CTRL_BASE, ATA_DRIVE_MASTER);
    if (ata_identify(&g_ata) != ATA_OK) {
        fail_item("ata identify");
        return 1;
    }
    serial_puts("ng: ata identify ok (model=");
    serial_puts(g_ata.model);
    serial_puts(" sectors=");
    serial_putdec((u64)g_ata.sectors);
    serial_puts(" lba28=1 lba48=");
    serial_putdec((u64)g_ata.lba48);
    serial_puts(")\r\n");

    fails += ata_read_selftest();
    if (fails == 0) {
        fails += ata_write_selftest();
    }
    fails += ata_timeout_selftest();

    c = fs_sys_ctx();
    if (fails == 0) {
        fails += disk_mount_selftest(c);
        if (fails == 0) {
            fails += disk_file_selftest(c);
        }
    }

    serial_puts("ng: ata stat reads=");
    serial_putdec((u64)g_ata.reads);
    serial_puts(" writes=");
    serial_putdec((u64)g_ata.writes);
    serial_puts(" timeouts=");
    serial_putdec((u64)g_ata.timeouts);
    serial_puts(" errors=");
    serial_putdec((u64)g_ata.errors);
    serial_puts("\r\n");
    return fails;
}
