/*
 * ng WP2-7 host 单测：parity 100 项里可 host 化的 95 项。
 *
 * 与内核共用 tests/parity/parity_cases.h（case 表）+ parity_body.inc（断言体），
 * 所以 VFS/ramfs/FAT16/auth/log/pkg/shell 的语义只有一份实现：
 *   - VFS/ramfs/auth/pkg：自建 ramfs 根（64 页静态页池）+ 真实 L1 服务；
 *   - FAT16：64 扇区内存合成镜像（与内核同一构造代码）；
 *   - ATA：fake 控制器（寄存器级模拟 IDENTIFY/READ/WRITE/FLUSH），与
 *     test_ata.c 的 fake 同构但独立，避免测试间共享状态；
 *   - shell：shell.c 以 -DNG_HOST_TEST 编译，调 sh_test_tokenize/resolve。
 *
 * 显示/输入 5 项（96..100）需要 QEMU 里的真实 VBE/PS2 状态，host 跳过；
 * 内核自检会跑满 100 项。unit-ng 的 check 数因此 +95。
 */
#define NG_PARITY_HOST 1

#include <stdio.h>
#include <string.h>

#include "ata.h"
#include "auth.h"
#include "log.h"
#include "parity_cases.h"
#include "pkg.h"
#include "ramfs.h"
#include "test_util.h"
#include "vfs.h"

#include "parity_body.inc"

/* ------------------------------------------------------------------------- */
/* 页池（ramfs 的数据页）                                                     */
/* ------------------------------------------------------------------------- */

#define P_POOL_PAGES 64u

static u64 g_pool[P_POOL_PAGES * 512u] __attribute__((aligned(4096)));
static u8 g_pool_used[P_POOL_PAGES];

static void *p_pool_alloc(void *ctx)
{
    u32 i;

    (void)ctx;
    for (i = 0; i < P_POOL_PAGES; i++) {
        if (g_pool_used[i] == 0u) {
            g_pool_used[i] = 1u;
            memset(&g_pool[i * 512u], 0, 4096);
            return &g_pool[i * 512u];
        }
    }
    return 0;
}

static void p_pool_free(void *ctx, void *page)
{
    u32 i;

    (void)ctx;
    for (i = 0; i < P_POOL_PAGES; i++) {
        if ((void *)&g_pool[i * 512u] == page) {
            g_pool_used[i] = 0u;
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 环境                                                                      */
/* ------------------------------------------------------------------------- */

static struct ramfs g_ramfs;
static struct vfs_ctx g_ctx;

struct vfs_ctx *parity_vfs_ctx(void)
{
    return &g_ctx;
}

/* ------------------------------------------------------------------------- */
/* fake ATA 控制器（256 KiB 盘，512 扇区）                                    */
/* ------------------------------------------------------------------------- */

#define FAKE_SECTORS 512u

enum {
    FAKE_IDLE = 0,
    FAKE_IDENT = 1,
    FAKE_READ = 2,
    FAKE_WRITE = 3,
    FAKE_DONE = 4
};

struct fake_ata {
    int phase;
    u8 status;
    u32 lba;
    u32 count;
    u32 sec;
    u32 word;
    u16 id[ATA_IDENTIFY_WORDS];
    u8 buf[512];
};

static struct fake_ata g_fa;
static u8 g_fake_disk[FAKE_SECTORS * 512u];

static void fake_sync_status(void)
{
    u8 st = ATA_SR_DRDY;

    if (g_fa.phase == FAKE_IDENT || g_fa.phase == FAKE_READ ||
        g_fa.phase == FAKE_WRITE) {
        st |= ATA_SR_DRQ;
    }
    g_fa.status = st;
}

static void fake_build_id(void)
{
    char model[40];
    u32 i;

    memset(g_fa.id, 0, sizeof(g_fa.id));
    memset(model, ' ', sizeof(model));
    memcpy(model, "PARITY FAKE DISK", 16);
    g_fa.id[0] = 0x0040u;
    g_fa.id[49] = 0x0200u; /* LBA supported */
    g_fa.id[60] = (u16)(FAKE_SECTORS & 0xFFFFu);
    g_fa.id[61] = (u16)((FAKE_SECTORS >> 16) & 0xFFFFu);
    g_fa.id[83] = 0; /* no LBA48 */
    for (i = 0; i < 20u; i++) {
        g_fa.id[27u + i] =
            (u16)(((u16)(u8)model[i * 2u] << 8) | (u16)(u8)model[i * 2u + 1u]);
    }
}

static void fake_command(u8 cmd)
{
    if (cmd == ATA_CMD_IDENTIFY) {
        fake_build_id();
        g_fa.phase = FAKE_IDENT;
        g_fa.word = 0;
        g_fa.sec = 0;
    } else if (cmd == ATA_CMD_READ_SECTORS) {
        g_fa.phase = FAKE_READ;
        g_fa.sec = 0;
        g_fa.word = 0;
        memcpy(g_fa.buf, g_fake_disk + (u64)g_fa.lba * 512u, 512u);
    } else if (cmd == ATA_CMD_WRITE_SECTORS) {
        g_fa.phase = FAKE_WRITE;
        g_fa.sec = 0;
        g_fa.word = 0;
        memset(g_fa.buf, 0, sizeof(g_fa.buf));
    } else if (cmd == ATA_CMD_FLUSH_CACHE) {
        g_fa.phase = FAKE_IDLE;
    }
    fake_sync_status();
}

static u8 fake_in8(void *ctx, u16 port)
{
    (void)ctx;
    if (port == ATA_PRIMARY_IO_BASE + ATA_REG_STATUS ||
        port == ATA_PRIMARY_CTRL_BASE + ATA_REG_ALTSTATUS) {
        return g_fa.status;
    }
    return 0;
}

static void fake_out8(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    if (port == ATA_PRIMARY_IO_BASE + ATA_REG_HDDEVSEL) {
        g_fa.lba = ((u32)(value & 0x0Fu)) << 24;
    } else if (port == ATA_PRIMARY_IO_BASE + ATA_REG_SECCOUNT0) {
        g_fa.count = value;
    } else if (port == ATA_PRIMARY_IO_BASE + ATA_REG_LBA0) {
        g_fa.lba = (g_fa.lba & ~0xFFu) | value;
    } else if (port == ATA_PRIMARY_IO_BASE + ATA_REG_LBA1) {
        g_fa.lba = (g_fa.lba & ~0xFF00u) | ((u32)value << 8);
    } else if (port == ATA_PRIMARY_IO_BASE + ATA_REG_LBA2) {
        g_fa.lba = (g_fa.lba & ~0xFF0000u) | ((u32)value << 16);
    } else if (port == ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND) {
        fake_command(value);
    }
}

static u16 fake_in16(void *ctx, u16 port)
{
    (void)ctx;
    if (port != ATA_PRIMARY_IO_BASE + ATA_REG_DATA) {
        return 0;
    }
    if (g_fa.phase == FAKE_IDENT) {
        u16 w = (g_fa.word < ATA_IDENTIFY_WORDS) ? g_fa.id[g_fa.word] : 0;

        g_fa.word++;
        if (g_fa.word >= ATA_IDENTIFY_WORDS) {
            g_fa.phase = FAKE_DONE;
            fake_sync_status();
        }
        return w;
    }
    if (g_fa.phase == FAKE_READ) {
        u16 w = (u16)((u16)g_fa.buf[g_fa.word * 2u] |
                      ((u16)g_fa.buf[g_fa.word * 2u + 1u] << 8));

        g_fa.word++;
        if (g_fa.word >= 256u) {
            g_fa.sec++;
            if (g_fa.sec >= g_fa.count) {
                g_fa.phase = FAKE_DONE;
                fake_sync_status();
            } else {
                g_fa.word = 0;
                memcpy(g_fa.buf,
                       g_fake_disk + ((u64)g_fa.lba + g_fa.sec) * 512u, 512u);
            }
        }
        return w;
    }
    return 0;
}

static void fake_out16(void *ctx, u16 port, u16 value)
{
    (void)ctx;
    if (port != ATA_PRIMARY_IO_BASE + ATA_REG_DATA) {
        return;
    }
    if (g_fa.phase == FAKE_WRITE) {
        g_fa.buf[g_fa.word * 2u] = (u8)(value & 0xFFu);
        g_fa.buf[g_fa.word * 2u + 1u] = (u8)(value >> 8);
        g_fa.word++;
        if (g_fa.word >= 256u) {
            memcpy(g_fake_disk + ((u64)g_fa.lba + g_fa.sec) * 512u, g_fa.buf,
                   512u);
            g_fa.sec++;
            if (g_fa.sec >= g_fa.count) {
                g_fa.phase = FAKE_DONE;
                fake_sync_status();
            } else {
                g_fa.word = 0;
                memset(g_fa.buf, 0, sizeof(g_fa.buf));
            }
        }
    }
}

static const struct ata_io g_fake_io = {fake_in8, fake_out8, fake_in16,
                                        fake_out16, 0};

static struct ata_dev g_fa_dev;
static u8 g_ata_a[512];
static u8 g_ata_b[512];
static u8 g_ata_save[512];

static void fake_disk_init(void)
{
    u32 i;

    memset(&g_fa, 0, sizeof(g_fa));
    memset(g_fake_disk, 0, sizeof(g_fake_disk));
    for (i = 0; i < FAKE_SECTORS * 512u; i++) {
        g_fake_disk[i] = (u8)(i * 7u + 0x11u);
    }
    g_fake_disk[510] = 0x55;
    g_fake_disk[511] = 0xAA;
    fake_sync_status();
    ata_dev_init(&g_fa_dev, &g_fake_io, 0, 0, ATA_DRIVE_MASTER);
    (void)ata_identify(&g_fa_dev);
}

int parity_ata_case(int which)
{
    u32 i;

    switch (which) {
    case 0:
        PEQ(ata_identify(&g_fa_dev), ATA_OK, "identify");
        PEQ(g_fa_dev.present, 1, "present");
        PEQ(g_fa_dev.sectors, (int)FAKE_SECTORS, "sectors");
        PCHK(g_fa_dev.model[0] != '\0', "model non-empty");
        PEQ(g_fa_dev.lba28, 1, "lba28");
        return PARITY_PASS;
    case 1:
        PCHK(ata_identify(&g_fa_dev) == ATA_OK, "identify");
        PEQ(ata_read28(&g_fa_dev, 0, g_ata_a, 1), ATA_OK, "read lba0");
        PEQ(ata_read28(&g_fa_dev, 0, g_ata_b, 1), ATA_OK, "read lba0 again");
        PMEM(g_ata_a, g_ata_b, 512, "stable read");
        PEQ(g_ata_a[510], 0x55, "BPB sig lo");
        PEQ(g_ata_a[511], 0xAA, "BPB sig hi");
        return PARITY_PASS;
    case 2:
        PCHK(ata_identify(&g_fa_dev) == ATA_OK, "identify");
        PEQ(ata_read28(&g_fa_dev, 0, 0, 1), ATA_EINVAL, "NULL buffer");
        PEQ(ata_read28(&g_fa_dev, 0, g_ata_a, 0), ATA_EINVAL, "count 0");
        PEQ(ata_read28(&g_fa_dev, 0, g_ata_a, 9), ATA_EINVAL, "count 9");
        PEQ(ata_read28(&g_fa_dev, FAKE_SECTORS, g_ata_a, 1), ATA_EINVAL,
            "lba at end");
        PEQ(ata_read28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_a, 2), ATA_EINVAL,
            "cross end");
        PEQ(ata_read28(0, 0, g_ata_a, 1), ATA_EINVAL, "NULL dev");
        return PARITY_PASS;
    case 3:
        PCHK(ata_identify(&g_fa_dev) == ATA_OK, "identify");
        PEQ(ata_read28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_save, 1), ATA_OK,
            "save last");
        for (i = 0; i < 512u; i++) {
            g_ata_a[i] = (u8)(i * 3u + 0x5Au);
        }
        PEQ(ata_write28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_a, 1), ATA_OK,
            "write last");
        PEQ(ata_read28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_b, 1), ATA_OK,
            "read back");
        PMEM(g_ata_a, g_ata_b, 512, "write verified");
        PEQ(ata_write28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_save, 1), ATA_OK,
            "restore last");
        PEQ(ata_read28(&g_fa_dev, FAKE_SECTORS - 1u, g_ata_b, 1), ATA_OK,
            "read restored");
        PMEM(g_ata_save, g_ata_b, 512, "restore verified");
        return PARITY_PASS;
    case 4:
        PEQ(ata_lba_range_ok(0, 1, 1000), 1, "range ok");
        PEQ(ata_lba_range_ok(999, 1, 1000), 1, "last sector ok");
        PEQ(ata_lba_range_ok(1000, 1, 1000), 0, "lba == sectors");
        PEQ(ata_lba_range_ok(999, 2, 1000), 0, "cross end");
        PEQ(ata_lba_range_ok(0, 0, 1000), 0, "count 0");
        PEQ(ata_lba_range_ok(0, 9, 1000), 0, "count 9");
        PEQ(ata_lba_range_ok(ATA_LBA28_MAX + 1u, 1, ATA_LBA28_SECTORS), 0,
            "lba28 overflow");
        PEQ(ata_devsel(ATA_DRIVE_MASTER, 0), ATA_LBA_SELECT, "devsel master");
        PEQ(ata_devsel(ATA_DRIVE_SLAVE, 0), (int)(ATA_LBA_SELECT | 0x10u),
            "devsel slave");
        PEQ(ata_status_class(ATA_SR_DRDY), 0, "status idle");
        PEQ(ata_status_class(ATA_SR_BSY), 1, "status bsy");
        PEQ(ata_status_class(ATA_SR_ERR), 2, "status err");
        PEQ(ata_status_class(ATA_SR_DRQ), 3, "status drq");
        return PARITY_PASS;
    default:
        p_why_add("bad ata case");
        return PARITY_FAIL;
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                      */
/* ------------------------------------------------------------------------- */

static int parity_host_setup(void)
{
    struct fs_env env;
    int rc;

    memset(g_pool_used, 0, sizeof(g_pool_used));
    env.page_alloc = p_pool_alloc;
    env.page_free = p_pool_free;
    env.ctx = 0;

    vfs_reset();
    ramfs_init(&g_ramfs, &env);
    vfs_ctx_init(&g_ctx);
    if (vfs_mount("/", ramfs_ops(), &g_ramfs, ramfs_root(&g_ramfs)) != VFS_OK) {
        return -1;
    }
    rc = vfs_mkdir(&g_ctx, "/parity", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    rc = vfs_mkdir(&g_ctx, "/parity/vfs", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    rc = vfs_mkdir(&g_ctx, "/parity/ram", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    if (auth_init(&g_ctx) != AUTH_OK) {
        return -1;
    }
    if (pkg_init(&g_ctx) != PKG_OK) {
        return -1;
    }
    (void)log_init();
    return 0;
}

int test_parity(void)
{
    static const struct parity_case cases[] = {PARITY_CASE_LIST(PARITY_ROW)};
    u32 n = (u32)(sizeof(cases) / sizeof(cases[0]));
    u32 i;
    u32 ran = 0;
    int fails = 0;
    int rc;

    printf("=== parity (host, %d cases; display/input kernel-only) ===\n",
           (int)PARITY_TOTAL);
    rc = parity_host_setup();
    if (rc != 0) {
        printf("parity: setup FAIL (rc=%d)\n", rc);
        CHECK(rc == 0);
        return 1;
    }
    fake_disk_init();

    for (i = 0; i < n; i++) {
        int ok;

        if (cases[i].id != (int)(i + 1u)) {
            printf("parity: id gap at index %u\n", i);
            fails++;
            break;
        }
        if (cases[i].id >= PARITY_DISP_FIRST) {
            continue;
        }
        ok = parity_run_one(cases[i].id);
        ran++;
        if (ok == PARITY_PASS) {
            printf("parity: %03d/%03d [%s] %s ... ok\n", cases[i].id,
                   (int)PARITY_TOTAL, cases[i].domain, cases[i].desc);
        } else {
            printf("parity: %03d/%03d [%s] %s ... FAIL\n", cases[i].id,
                   (int)PARITY_TOTAL, cases[i].domain, cases[i].desc);
            if (parity_why()[0] != '\0') {
                printf("  parity detail: %s\n", parity_why());
            }
            fails++;
        }
        ut_check(ok == PARITY_PASS, cases[i].desc, __FILE__, __LINE__);
    }

    printf("parity(host): %u/%u PASS%s%s\n", ran - (u32)fails, ran,
           (fails == 0) ? "" : " FAIL",
           " (5 display/input cases are kernel-only)");
    return fails;
}
