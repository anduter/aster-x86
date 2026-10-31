/*
 * ng WP2-7: parity 内核 harness（100 项，E2 出口）。
 *
 * 职责只有“接线 + 打印”：
 *   - case 表与断言体来自 tests/parity/parity_cases.h + parity_body.inc，
 *     与 host 单测（tests/unit-ng/test_parity.c）**同一份**，所以两边不会
 *     出现“host 过、内核不过”的语义漂移；
 *   - ATA（56..60）走真实 legacy 端口（与 wp24 相同的注入方式）；
 *   - 显示/输入（96..100）用 wp23 的只读诊断接口，验证“当前实际走的是
 *     VBE 帧缓冲还是 VGA 文本降级”，不做假设；
 *   - shell（86..95）调用 src/apps/shell/shell.c 的 tokenizer/cwd 解析。
 *     这份 shell.c 以 -DNG_HOST_TEST 单独编译成内核对象（parity_shell.o）：
 *     只用到纯字符串钩子 sh_test_tokenize/sh_test_resolve，不会执行
 *     shell_main，也不会触发 sh_host_syscall（这里给一个 -ENOSYS 兜底）。
 *
 * 失败不 abort：每项独立，打印 FAIL + 原因细节，最后汇总失败数。
 */
#include "parity_selftest.h"

#include "ata.h"
#include "auth.h"
#include "fb.h"
#include "fs_syscall.h"
#include "input.h"
#include "io.h"
#include "log.h"
#include "parity_cases.h"
#include "pkg.h"
#include "serial.h"
#include "types.h"
#include "vga_text.h"
#include "vfs.h"
#include "wp23.h"

#include "parity_body.inc"

static const struct parity_case g_parity_cases[] = {PARITY_CASE_LIST(PARITY_ROW)};

/* ------------------------------------------------------------------------- */
/* VFS 上下文（共享体通过这个拿 live ctx）                                   */
/* ------------------------------------------------------------------------- */

struct vfs_ctx *parity_vfs_ctx(void)
{
    return fs_sys_ctx();
}

/* ------------------------------------------------------------------------- */
/* shell.c 的 host syscall 兜底：parity 只调 tokenizer/resolve，不会到这里。  */
/* ------------------------------------------------------------------------- */

i64 sh_host_syscall(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    (void)nr;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    return -38; /* -ENOSYS */
}

/* ------------------------------------------------------------------------- */
/* ATA（56..60）：真实 primary master（QEMU IDE 盘）                          */
/* ------------------------------------------------------------------------- */

static struct ata_dev g_pata;
static int g_pata_state; /* 0=未试, 1=就绪, -1=失败 */
static u8 g_ata_a[512];
static u8 g_ata_b[512];
static u8 g_ata_save[512];

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

static const struct ata_io g_pata_io = {hw_in8, hw_out8, hw_in16, hw_out16, 0};

static int p_ata_ready(void)
{
    if (g_pata_state == 0) {
        ata_dev_init(&g_pata, &g_pata_io, 0, 0, ATA_DRIVE_MASTER);
        g_pata_state = (ata_identify(&g_pata) == ATA_OK) ? 1 : -1;
    }
    return (g_pata_state == 1) ? 1 : 0;
}

int parity_ata_case(int which)
{
    u32 i;

    switch (which) {
    case 0:
        PCHK(p_ata_ready(), "identify");
        PEQ(g_pata.present, 1, "present");
        PCHK(g_pata.sectors > 0u, "sectors > 0");
        PCHK(g_pata.model[0] != '\0', "model non-empty");
        PEQ(g_pata.lba28, 1, "lba28");
        return PARITY_PASS;
    case 1:
        PCHK(p_ata_ready(), "identify");
        PEQ(ata_read28(&g_pata, 0, g_ata_a, 1), ATA_OK, "read lba0");
        PEQ(ata_read28(&g_pata, 0, g_ata_b, 1), ATA_OK, "read lba0 again");
        PMEM(g_ata_a, g_ata_b, 512, "stable read");
        PEQ(g_ata_a[510], 0x55, "BPB sig lo");
        PEQ(g_ata_a[511], 0xAA, "BPB sig hi");
        return PARITY_PASS;
    case 2:
        PCHK(p_ata_ready(), "identify");
        PEQ(ata_read28(&g_pata, 0, 0, 1), ATA_EINVAL, "NULL buffer");
        PEQ(ata_read28(&g_pata, 0, g_ata_a, 0), ATA_EINVAL, "count 0");
        PEQ(ata_read28(&g_pata, 0, g_ata_a, 9), ATA_EINVAL, "count 9");
        PEQ(ata_read28(&g_pata, g_pata.sectors, g_ata_a, 1), ATA_EINVAL,
            "lba at end");
        PEQ(ata_read28(&g_pata, g_pata.sectors - 1u, g_ata_a, 2), ATA_EINVAL,
            "cross end");
        PEQ(ata_read28(0, 0, g_ata_a, 1), ATA_EINVAL, "NULL dev");
        return PARITY_PASS;
    case 3: {
        u32 last;

        PCHK(p_ata_ready(), "identify");
        last = g_pata.sectors - 1u;
        PEQ(ata_read28(&g_pata, last, g_ata_save, 1), ATA_OK, "save last");
        for (i = 0; i < 512u; i++) {
            g_ata_a[i] = (u8)(i * 3u + 0x5Au);
        }
        PEQ(ata_write28(&g_pata, last, g_ata_a, 1), ATA_OK, "write last");
        PEQ(ata_read28(&g_pata, last, g_ata_b, 1), ATA_OK, "read back");
        PMEM(g_ata_a, g_ata_b, 512, "write verified");
        PEQ(ata_write28(&g_pata, last, g_ata_save, 1), ATA_OK, "restore last");
        PEQ(ata_read28(&g_pata, last, g_ata_b, 1), ATA_OK, "read restored");
        PMEM(g_ata_save, g_ata_b, 512, "restore verified");
        return PARITY_PASS;
    }
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
        PEQ(ata_devsel(ATA_DRIVE_MASTER, 0x0F000000u),
            (int)(ATA_LBA_SELECT | 0x0Fu), "devsel lba high");
        PEQ(ata_status_class(ATA_SR_BSY), 1, "status bsy");
        PEQ(ata_status_class(ATA_SR_ERR), 2, "status err");
        PEQ(ata_status_class(ATA_SR_DRQ), 3, "status drq");
        PEQ(ata_status_class(ATA_SR_DRDY), 0, "status drdy");
        return PARITY_PASS;
    default:
        p_why_add("bad ata case");
        return PARITY_FAIL;
    }
}

/* ------------------------------------------------------------------------- */
/* 显示 / 输入（96..100）：真实当前路径                                       */
/* ------------------------------------------------------------------------- */

int parity_disp_case(int which)
{
    const struct fb_info *fb = ng_display_fb();
    const struct vga_text *tx = ng_display_text();
    const struct input_state *in = ng_input_state();

    switch (which) {
    case 0:
        if (fb != 0) {
            PCHK(fb->base != 0, "fb base");
            PCHK(fb->width >= 1024u && fb->height >= 768u, "fb resolution");
            PEQ(fb->bpp, 32, "fb bpp");
            PCHK(fb->pitch >= fb->width * 4u, "fb pitch");
            PCHK(tx == 0, "text fallback inactive");
        } else {
            PCHK(tx != 0, "text fallback");
            PEQ(tx->cols, 80, "text cols");
            PEQ(tx->rows, 25, "text rows");
        }
        return PARITY_PASS;
    case 1: {
        u32 x;
        u32 y;
        u32 saved;

        if (fb == 0) {
            PCHK(tx != 0, "text fallback");
            PEQ(vga_text_char_at(tx, 0, 0), 'A', "text cell data");
            PEQ(vga_text_attr_at(tx, 0, 0), VGA_TEXT_ATTR, "text cell attr");
            return PARITY_PASS;
        }
        x = fb->width - 2u;
        y = fb->height - 2u;
        saved = fb_read_pixel(fb, x, y);
        fb_pixel((struct fb_info *)fb, x, y, FB_RED);
        PEQ(fb_read_pixel(fb, x, y), FB_RED, "pixel readback");
        fb_pixel((struct fb_info *)fb, x, y, saved);
        PEQ(fb_read_pixel(fb, x, y), saved, "pixel restored");
        return PARITY_PASS;
    }
    case 2: {
        u32 cols;
        u32 rows;

        if (fb == 0) {
            PCHK(tx != 0, "text fallback");
            PCHK(tx->row < tx->rows && tx->col < tx->cols, "text cursor");
            return PARITY_PASS;
        }
        fb_grid(fb, &cols, &rows);
        PEQ(cols, fb->width / FB_CELL_W, "grid cols");
        PEQ(rows, fb->height / FB_CELL_H, "grid rows");
        PCHK(fb->cursor_x / FB_CELL_W < cols, "cursor x");
        PCHK(fb->cursor_y / FB_CELL_H < rows, "cursor y");
        return PARITY_PASS;
    }
    case 3:
        PCHK(in != 0, "input state");
        PEQ(in->ring.count, 0, "ring empty");
        PEQ((int)(i64)ng_input_readkey(), NG_EAGAIN, "readkey EAGAIN");
        return PARITY_PASS;
    case 4: {
        struct input_state st;
        struct input_event ev;

        input_init(&st);
        PEQ(input_feed(&st, 0x1Eu), 1, "feed a");
        PEQ(input_next(&st, &ev), 0, "pop a");
        PEQ(ev.ascii, 'a', "ascii a");
        PEQ(ev.flags, 0, "no mods");
        (void)input_feed(&st, 0x2Au);
        PEQ(input_feed(&st, 0x1Eu), 1, "feed shift+a");
        PEQ(input_next(&st, &ev), 0, "pop shift+a");
        PEQ(ev.ascii, 'A', "ascii A");
        PCHK((ev.flags & INPUT_F_SHIFT) != 0u, "shift flag");
        (void)input_feed(&st, 0xAAu);
        (void)input_feed(&st, 0x3Au);
        PEQ(input_feed(&st, 0x1Eu), 1, "feed caps+a");
        PEQ(input_next(&st, &ev), 0, "pop caps+a");
        PEQ(ev.ascii, 'A', "caps ascii A");
        PCHK((ev.flags & INPUT_F_CAPS) != 0u, "caps flag");
        (void)input_feed(&st, 0x3Au);
        (void)input_feed(&st, 0x1Du);
        PEQ(input_feed(&st, 0x2Eu), 1, "feed ctrl+c");
        PEQ(input_next(&st, &ev), 0, "pop ctrl+c");
        PEQ(ev.ascii, 'c', "ctrl ascii c");
        PCHK((ev.flags & INPUT_F_CTRL) != 0u, "ctrl flag");
        (void)input_feed(&st, 0x9Du);
        PEQ(input_feed(&st, 0x9Eu), 0, "release ignored");
        PEQ(st.ring.count, 0, "ring drained");
        PEQ(st.ring.lost, 0, "no lost events");
        return PARITY_PASS;
    }
    default:
        p_why_add("bad display case");
        return PARITY_FAIL;
    }
}

/* ------------------------------------------------------------------------- */
/* 打印 / 入口                                                               */
/* ------------------------------------------------------------------------- */

static void put3(int v)
{
    if (v < 0) {
        serial_putc('-');
        v = -v;
    }
    serial_putc((char)('0' + ((v / 100) % 10)));
    serial_putc((char)('0' + ((v / 10) % 10)));
    serial_putc((char)('0' + (v % 10)));
}

static int parity_prepare_tree(void)
{
    struct vfs_ctx *c = parity_vfs_ctx();
    int rc;

    if (c == 0) {
        return -1;
    }
    rc = vfs_mkdir(c, "/parity", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    rc = vfs_mkdir(c, "/parity/vfs", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    rc = vfs_mkdir(c, "/parity/ram", 0755);
    if (rc != VFS_OK && rc != VFS_EEXIST) {
        return rc;
    }
    return 0;
}

int parity_selftest(void)
{
    u32 n = (u32)(sizeof(g_parity_cases) / sizeof(g_parity_cases[0]));
    u32 i;
    int fails = 0;
    int rc;

    serial_puts("ng: parity suite (100 items, E2 gate)\r\n");
    rc = parity_prepare_tree();
    if (rc != 0) {
        serial_puts("ng: parity setup FAIL (rc=");
        serial_putdec((u64)(0 - rc));
        serial_puts(")\r\n");
        return 1;
    }
    if (n != (u32)PARITY_TOTAL) {
        serial_puts("ng: parity table size mismatch\r\n");
        return 1;
    }

    for (i = 0; i < n; i++) {
        int ok;

        if (g_parity_cases[i].id != (int)(i + 1u)) {
            serial_puts("ng: parity id gap\r\n");
            fails++;
            break;
        }
        ok = parity_run_one(g_parity_cases[i].id);
        if (ok != PARITY_PASS) {
            fails++;
        }
        serial_puts("parity: ");
        put3(g_parity_cases[i].id);
        serial_puts("/");
        put3((int)PARITY_TOTAL);
        serial_puts(" [");
        serial_puts(g_parity_cases[i].domain);
        serial_puts("] ");
        serial_puts(g_parity_cases[i].desc);
        if (ok == PARITY_PASS) {
            serial_puts(" ... ok\r\n");
        } else {
            serial_puts(" ... FAIL\r\n");
            if (parity_why()[0] != '\0') {
                serial_puts("  parity detail: ");
                serial_puts(parity_why());
                serial_puts("\r\n");
            }
        }
    }

    serial_puts("ng: parity ");
    serial_putdec((u64)((int)PARITY_TOTAL - fails));
    serial_puts("/");
    serial_putdec((u64)PARITY_TOTAL);
    if (fails == 0) {
        serial_puts(" PASS\r\n");
        (void)log_write("selftest", "parity 100/100 PASS");
    } else {
        serial_puts(" FAIL\r\n");
        (void)log_write("selftest", "parity FAIL");
    }
    return fails;
}
