/*
 * ng WP2-3 host 单测：L1D 设备域纯逻辑（PCI / VBE / 8x16 字体 / 帧缓冲 /
 * VGA 文本 / PS/2 scancode 映射与环形缓冲）。
 *
 * 这些驱动不 include 串口/页表/PMM，端口 IO 通过注入的 ops 提供，所以同一份
 * 源码在 host 上编译运行；内核里只换一套 ops + 把 BAR 映射进页表。
 */
#include <string.h>

#include "fb.h"
#include "font8x16.h"
#include "input.h"
#include "pci.h"
#include "test_util.h"
#include "vbe.h"
#include "vga_text.h"

/* ------------------------------------------------------------------------- */
/* fake PCI 配置空间：bus0 dev2 func0 = 1234:1111                              */
/* ------------------------------------------------------------------------- */

struct fake_pci {
    u32 sel;
    u32 cmd_status;
    u32 bar[6];
    u32 mask[6];
};

static struct fake_pci fc;

static int fake_bar_index(u32 off, u32 *idx)
{
    if (off >= 0x10u && off <= 0x24u && (off & 0x3u) == 0) {
        *idx = (off - 0x10u) / 4u;
        return 1;
    }
    return 0;
}

static u32 fake_pci_in32(void *ctx, u16 port)
{
    u32 off;
    u32 idx;

    (void)ctx;
    if (port == PCI_CONFIG_ADDRESS_PORT) {
        return fc.sel;
    }
    if (port != PCI_CONFIG_DATA_PORT) {
        return 0xFFFFFFFFu;
    }
    if (((fc.sel >> 16) & 0xFFu) != 0u || ((fc.sel >> 11) & 0x1Fu) != 2u ||
        ((fc.sel >> 8) & 0x7u) != 0u) {
        return 0xFFFFFFFFu;
    }
    off = fc.sel & 0xFCu;
    if (off == 0x00u) {
        return 0x11111234u;
    }
    if (off == 0x04u) {
        return fc.cmd_status;
    }
    if (off == 0x08u) {
        return 0x03000000u; /* class 0x03 (display), subclass 0 */
    }
    if (off == 0x0Cu) {
        return 0x00000000u;
    }
    if (fake_bar_index(off, &idx)) {
        return fc.bar[idx];
    }
    return 0u;
}

static void fake_pci_out32(void *ctx, u16 port, u32 value)
{
    u32 off;
    u32 idx;

    (void)ctx;
    if (port == PCI_CONFIG_ADDRESS_PORT) {
        fc.sel = value;
        return;
    }
    if (port != PCI_CONFIG_DATA_PORT) {
        return;
    }
    off = fc.sel & 0xFCu;
    if (off == 0x04u) {
        fc.cmd_status = (fc.cmd_status & 0xFFFF0000u) | (value & 0xFFFFu);
        return;
    }
    if (fake_bar_index(off, &idx)) {
        fc.bar[idx] = (value == 0xFFFFFFFFu) ? fc.mask[idx] : value;
    }
}

static void fake_pci_reset(void)
{
    u32 i;

    memset(&fc, 0, sizeof(fc));
    for (i = 0; i < 6u; i++) {
        fc.mask[i] = 0xFFFFFFFFu;
    }
}

/* ------------------------------------------------------------------------- */
/* PCI                                                                       */
/* ------------------------------------------------------------------------- */

static void test_pci(void)
{
    struct pci_io_ops io;
    struct pci_loc loc;
    struct pci_bar bar;
    u32 v;
    u16 h;
    u8 b;
    int ok;

    fake_pci_reset();
    io.out32 = fake_pci_out32;
    io.in32 = fake_pci_in32;
    io.ctx = NULL;

    CHECK(pci_cfg_addr(0, 2, 0, 0x10) == 0x80001010u);
    CHECK(pci_cfg_addr(0, 0, 0, 0x00) == 0x80000000u);
    CHECK(pci_cfg_addr(1, 3, 2, 0x08) == 0x80011A08u);
    CHECK(pci_cfg_addr(0, 31, 7, 0xFC) == 0x8000FFFCu);
    CHECK(pci_cfg_addr(0, 32, 0, 0) == 0u);
    CHECK(pci_cfg_addr(0, 0, 8, 0) == 0u);
    CHECK(pci_cfg_addr(0, 0, 0, 2) == 0u);

    CHECK(pci_cfg_read32(&io, 0, 2, 0, 0x00, &v) == 0);
    CHECK(v == 0x11111234u);
    CHECK(pci_cfg_read16(&io, 0, 2, 0, 0x00, &h) == 0 && h == 0x1234u);
    CHECK(pci_cfg_read16(&io, 0, 2, 0, 0x02, &h) == 0 && h == 0x1111u);
    CHECK(pci_cfg_read8(&io, 0, 2, 0, 0x00, &b) == 0 && b == 0x34u);
    CHECK(pci_cfg_read8(&io, 0, 2, 0, 0x03, &b) == 0 && b == 0x11u);
    CHECK(pci_cfg_read32(NULL, 0, 0, 0, 0, &v) == -1);

    CHECK(pci_find(&io, 0x1234u, 0x1111u, &loc) == 0);
    CHECK(loc.bus == 0 && loc.dev == 2 && loc.func == 0);
    CHECK(pci_find(&io, 0x1234u, 0x9999u, &loc) == -1);
    CHECK(pci_find_class(&io, 0x03u, 0x00u, &loc) == 0);
    CHECK(pci_find_class(&io, 0x02u, 0xFFu, &loc) == -1);

    fc.cmd_status = 0x00050000u;
    CHECK(pci_command(&io, &loc, &ok) == 0 && ok);
    CHECK(pci_enable(&io, &loc, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER) == 0);
    CHECK((fc.cmd_status & 0xFFFFu) == 0x0006u);
    CHECK((fc.cmd_status >> 16) == 0x0005u); /* status 高位保留 */
    CHECK(pci_command(&io, &loc, &ok) == 0x0006u && ok);

    CHECK(pci_bar_size(0u, 0) == 0u);
    CHECK(pci_bar_size(0xFFFFF000u, 0) == 0x1000u);
    CHECK(pci_bar_size(0xFFFF0000u, 0) == 0x10000u);
    CHECK(pci_bar_size(0xFFFFFF00u, 0) == 0x100u);
    CHECK(pci_bar_size(0xFFFFFFC0u, 1) == 0x40u);
    CHECK(pci_bar_size(0xFFFFFFF0u, 0) == 0x10u);

    /* 32 位 prefetchable 内存 BAR：0xFD000000 / 16 MiB。 */
    fake_pci_reset();
    fc.bar[0] = 0xFD000008u;
    fc.mask[0] = 0xFF000000u;
    CHECK(pci_read_bar(&io, &loc, 0, &bar) == 0);
    CHECK(bar.base == 0xFD000000u);
    CHECK(bar.size == 16u * 1024u * 1024u);
    CHECK(bar.is_io == 0 && bar.is_64 == 0 && bar.prefetch == 1);
    CHECK(bar.raw_lo == 0xFD000008u);
    CHECK(fc.bar[0] == 0xFD000008u); /* 探测后原值已恢复 */

    /* IO BAR：0xC000 / 64 字节。 */
    fake_pci_reset();
    fc.bar[1] = 0xC001u;
    fc.mask[1] = 0xFFFFFFC0u;
    CHECK(pci_read_bar(&io, &loc, 1, &bar) == 0);
    CHECK(bar.is_io == 1);
    CHECK(bar.base == 0xC000u);
    CHECK(bar.size == 0x40u);

    /* 64 位内存 BAR：0xE0000000 / 4 KiB，高 32 位为 0。 */
    fake_pci_reset();
    fc.bar[2] = 0xE0000004u;
    fc.bar[3] = 0u;
    fc.mask[2] = 0xFFFFF000u;
    fc.mask[3] = 0xFFFFFFFFu;
    CHECK(pci_read_bar(&io, &loc, 2, &bar) == 0);
    CHECK(bar.is_64 == 1);
    CHECK(bar.base == 0xE0000000u);
    CHECK(bar.size == 0x1000u);
    CHECK(fc.bar[2] == 0xE0000004u && fc.bar[3] == 0u);

    /* 未实现的 BAR（全 0）。 */
    fake_pci_reset();
    CHECK(pci_read_bar(&io, &loc, 5, &bar) == -1);
}

/* ------------------------------------------------------------------------- */
/* VBE                                                                       */
/* ------------------------------------------------------------------------- */

struct fake_vbe {
    u16 idx;
    u16 regs[16];
    int ignore_writes;
};

static struct fake_vbe fv;

static void fake_vbe_outw(void *ctx, u16 port, u16 value)
{
    (void)ctx;
    if (port == VBE_PORT_INDEX) {
        fv.idx = (u16)(value & 0xFu);
        return;
    }
    if (port == VBE_PORT_DATA && !fv.ignore_writes && fv.idx < 16u) {
        fv.regs[fv.idx] = value;
    }
}

static u16 fake_vbe_inw(void *ctx, u16 port)
{
    (void)ctx;
    if (port == VBE_PORT_INDEX) {
        return fv.idx;
    }
    if (port == VBE_PORT_DATA) {
        if (fv.ignore_writes) {
            return 0xFFFFu; /* 设备不回显：用于验证 EVERIFY 路径 */
        }
        return (fv.idx < 16u) ? fv.regs[fv.idx] : 0xFFFFu;
    }
    return 0xFFFFu;
}

static void test_vbe(void)
{
    struct vbe_io io;
    struct vbe_mode mode;
    u16 id = 0;

    memset(&fv, 0, sizeof(fv));
    io.outw = fake_vbe_outw;
    io.inw = fake_vbe_inw;
    io.ctx = NULL;

    fv.regs[VBE_DISPI_INDEX_ID] = 0xB0C5u;
    CHECK(vbe_probe(&io, &id) == VBE_OK);
    CHECK(id == 0xB0C5u);
    fv.regs[VBE_DISPI_INDEX_ID] = 0x1234u;
    CHECK(vbe_probe(&io, &id) == VBE_ENODEV);
    CHECK(vbe_probe(NULL, &id) == VBE_ENODEV);

    fv.regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = 256u;
    CHECK(vbe_video_mem_bytes(&io) == 16u * 1024u * 1024u);

    CHECK(vbe_mode_info(1280, 800, 32, &mode) == VBE_OK);
    CHECK(mode.width == 1280 && mode.height == 800 && mode.bpp == 32);
    CHECK(mode.pitch == 5120u);
    CHECK(mode.size_bytes == 5120u * 800u);
    CHECK(mode.virt_width == 1280 && mode.virt_height == 800);
    CHECK(vbe_mode_info(1024, 768, 32, &mode) == VBE_OK && mode.pitch == 4096u);
    CHECK(vbe_mode_info(320, 200, 8, &mode) == VBE_OK);
    CHECK(vbe_mode_info(100, 768, 32, &mode) == VBE_EBADMODE);
    CHECK(vbe_mode_info(1024, 768, 12, &mode) == VBE_EBADMODE);
    CHECK(vbe_mode_info(1024, 768, 32, NULL) == VBE_EBADMODE);

    memset(fv.regs, 0, sizeof(fv.regs));
    fv.ignore_writes = 0;
    CHECK(vbe_set_mode(&io, 1280, 800, 32, &mode) == VBE_OK);
    CHECK(fv.regs[VBE_DISPI_INDEX_XRES] == 1280u);
    CHECK(fv.regs[VBE_DISPI_INDEX_YRES] == 800u);
    CHECK(fv.regs[VBE_DISPI_INDEX_BPP] == 32u);
    CHECK(fv.regs[VBE_DISPI_INDEX_VIRT_WIDTH] == 1280u);
    CHECK(fv.regs[VBE_DISPI_INDEX_VIRT_HEIGHT] == 800u);
    CHECK(fv.regs[VBE_DISPI_INDEX_ENABLE] ==
          (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED));
    CHECK(mode.width == 1280 && mode.pitch == 5120u);

    vbe_disable(&io);
    CHECK(fv.regs[VBE_DISPI_INDEX_ENABLE] == VBE_DISPI_DISABLED);

    fv.ignore_writes = 1;
    CHECK(vbe_set_mode(&io, 1024, 768, 32, &mode) == VBE_EVERIFY);
    fv.ignore_writes = 0;

    CHECK(vbe_set_mode(&io, 1280, 800, 32, NULL) == VBE_OK);
    CHECK(vbe_set_mode(&io, 0, 0, 32, &mode) == VBE_EBADMODE);
}

/* ------------------------------------------------------------------------- */
/* 字体                                                                      */
/* ------------------------------------------------------------------------- */

static void test_font(void)
{
    const u8 *g;
    u32 i;
    u32 nonblank = 0;

    CHECK(NG_FONT_GLYPHS == 95u);
    CHECK(NG_FONT_W == 8u && NG_FONT_H == 16u);
    CHECK(NG_FONT_FIRST == 0x20u && NG_FONT_LAST == 0x7Eu);

    g = font8x16_glyph(' ');
    CHECK(g == ng_font8x16[0]);
    CHECK(font8x16_is_blank(' ') == 1);
    CHECK(font8x16_glyph('A') == ng_font8x16['A' - 0x20]);
    CHECK(font8x16_is_blank('A') == 0);
    CHECK(font8x16_is_blank('0') == 0);

    /* 'A' 的基线行 0x0C/0x1E/0x33/0x3F 纵向展开后的固定形状。 */
    g = font8x16_glyph('A');
    CHECK(g[0] == 0x0Cu && g[1] == 0x0Cu);
    CHECK(g[2] == 0x1Eu && g[3] == 0x1Eu);
    CHECK(g[4] == 0x33u && g[5] == 0x33u);
    CHECK(g[6] == 0x33u && g[7] == 0x33u);
    CHECK(g[8] == 0x3Fu && g[15] == 0x00u);

    /* 越界字符回退到 '?'，指针仍在表内。 */
    CHECK(font8x16_glyph(0x1Fu) == font8x16_glyph('?'));
    CHECK(font8x16_glyph(0x7Fu) == font8x16_glyph('?'));

    for (i = 0x21u; i <= 0x7Eu; i++) {
        if (!font8x16_is_blank((u8)i)) {
            nonblank++;
        }
    }
    CHECK(nonblank == 94u); /* 除空格以外的 94 个可见字符都有像素 */
    CHECK(ng_font8x16[95u - 1u][15] == 0x00u); /* '~' 底行留白 */
}

/* ------------------------------------------------------------------------- */
/* PS/2 键映射 + 环形缓冲 + 控制器轮询                                        */
/* ------------------------------------------------------------------------- */

struct fake_kbd {
    u8 data;
    int has_data;
    int out_cnt;
    u32 init_cmds;
};

static struct fake_kbd fk;

static u8 fake_kbd_inb(void *ctx, u16 port)
{
    (void)ctx;
    if (port == PS2_STATUS_PORT) {
        return fk.has_data ? PS2_STATUS_OUTPUT_FULL : 0u;
    }
    if (port == PS2_DATA_PORT) {
        u8 v = fk.data;

        fk.has_data = 0;
        return v;
    }
    return 0xFFu;
}

static void fake_kbd_outb(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    if (port == PS2_DATA_PORT && value == PS2_KBD_ENABLE_SCAN) {
        fk.data = PS2_ACK;
        fk.has_data = 1;
        fk.init_cmds++;
    }
}

static void test_input(void)
{
    struct input_io io;
    struct input_state st;
    struct input_event ev;
    u32 i;

    /* 纯映射 */
    CHECK(input_scancode_to_ascii(0x1E, 0) == 'a');
    CHECK(input_scancode_to_ascii(0x1E, INPUT_F_SHIFT) == 'A');
    CHECK(input_scancode_to_ascii(0x1E, INPUT_F_CAPS) == 'A');
    CHECK(input_scancode_to_ascii(0x1E, INPUT_F_SHIFT | INPUT_F_CAPS) == 'a');
    CHECK(input_scancode_to_ascii(0x02, 0) == '1');
    CHECK(input_scancode_to_ascii(0x02, INPUT_F_SHIFT) == '!');
    CHECK(input_scancode_to_ascii(0x02, INPUT_F_CAPS) == '1');
    CHECK(input_scancode_to_ascii(0x0C, INPUT_F_SHIFT) == '_');
    CHECK(input_scancode_to_ascii(0x1C, 0) == 13);
    CHECK(input_scancode_to_ascii(0x0E, 0) == 8);
    CHECK(input_scancode_to_ascii(0x0F, 0) == 9);
    CHECK(input_scancode_to_ascii(0x39, 0) == ' ');
    CHECK(input_scancode_to_ascii(0x2B, 0) == '\\');
    CHECK(input_scancode_to_ascii(0x2B, INPUT_F_SHIFT) == '|');
    CHECK(input_scancode_to_ascii(0x33, 0) == ',');
    CHECK(input_scancode_to_ascii(0x33, INPUT_F_SHIFT) == '<');
    CHECK(input_scancode_to_ascii(0x01, 0) == 27);
    CHECK(input_scancode_to_ascii(0x60, 0) == 0);
    CHECK(input_scancode_to_ascii(0xFF, 0) == 0);

    /* 解码：按下入环、释放只更新状态；修饰键成对生效。 */
    input_init(&st);
    CHECK(input_feed(&st, 0x1E) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'a' && ev.pressed == 1 &&
          ev.flags == 0);
    CHECK(input_feed(&st, 0x9E) == 0);
    CHECK(input_next(&st, &ev) == -1);

    CHECK(input_feed(&st, 0x2A) == 0);
    CHECK(input_feed(&st, 0x1E) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'A' &&
          (ev.flags & INPUT_F_SHIFT) != 0);
    CHECK(input_feed(&st, 0xAA) == 0);
    CHECK(input_feed(&st, 0x1E) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'a');

    CHECK(input_feed(&st, 0x3A) == 0); /* caps on */
    CHECK(input_feed(&st, 0x1E) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'A' &&
          (ev.flags & INPUT_F_CAPS) != 0);
    CHECK(input_feed(&st, 0xBA) == 0); /* caps release 不翻转 */
    CHECK(input_feed(&st, 0x3A) == 0); /* caps off */
    CHECK(input_feed(&st, 0x1E) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'a');

    CHECK(input_feed(&st, 0x1D) == 0); /* ctrl down */
    CHECK(input_feed(&st, 0x2E) == 1);
    CHECK(input_next(&st, &ev) == 0 && (ev.flags & INPUT_F_CTRL) != 0);
    CHECK(input_feed(&st, 0x9D) == 0);

    CHECK(input_feed(&st, 0xE0) == 0);
    CHECK(input_feed(&st, 0x50) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.scancode == 0x50 && ev.ascii == 0 &&
          (ev.flags & INPUT_F_EXT) != 0);

    /* E1（Pause）整段忽略。 */
    CHECK(input_feed(&st, 0xE1) == 0);
    for (i = 0; i < 5u; i++) {
        CHECK(input_feed(&st, (u8)(0x1D + i)) == 0);
    }
    CHECK(input_next(&st, &ev) == -1);

    /* 环容量：128 个事件后满，第 129 个丢进 lost；清空后重新可用。 */
    input_init(&st);
    for (i = 0; i < INPUT_RING_SIZE; i++) {
        CHECK(input_feed(&st, 0x1E) == 1);
    }
    CHECK(input_feed(&st, 0x1E) == -1);
    CHECK(st.ring.lost == 1u);
    for (i = 0; i < INPUT_RING_SIZE; i++) {
        CHECK(input_next(&st, &ev) == 0);
    }
    CHECK(input_next(&st, &ev) == -1);
    CHECK(input_feed(&st, 0x1E) == 1);

    /* 控制器轮询：一次把状态 bit0 置位的字节喂进解码器。 */
    memset(&fk, 0, sizeof(fk));
    io.inb = fake_kbd_inb;
    io.outb = fake_kbd_outb;
    io.ctx = NULL;
    fk.data = 0x1E;
    fk.has_data = 1;
    input_init(&st);
    CHECK(input_poll(&st, &io, INPUT_POLL_MAX) == 1);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'a');
    CHECK(input_poll(&st, &io, INPUT_POLL_MAX) == 0);

    /* ps2_init：0xF4 -> ACK -> 0；无控制器（0xFF）-> -1。 */
    memset(&fk, 0, sizeof(fk));
    CHECK(ps2_init(&st, &io) == 0);
    CHECK(fk.init_cmds == 1u);
    fk.init_cmds = 0;
    fk.has_data = 0;
    fk.data = 0xFF;
    /* 用 status==0xFF 模拟“没有 i8042”：见下面的专用 fake。 */
    CHECK(ps2_init(NULL, &io) == -1);
}

/* ------------------------------------------------------------------------- */
/* VGA 文本兜底                                                              */
/* ------------------------------------------------------------------------- */

static void test_vga_text(void)
{
    static u16 vram[VGA_TEXT_COLS * VGA_TEXT_ROWS];
    struct vga_text t;
    u32 i;

    vga_text_init(&t, vram);
    vga_text_clear(&t);
    CHECK(vga_text_char_at(&t, 0, 0) == ' ');
    CHECK(vga_text_attr_at(&t, 0, 0) == VGA_TEXT_ATTR);

    vga_text_puts(&t, "AB");
    CHECK(vga_text_char_at(&t, 0, 0) == 'A');
    CHECK(vga_text_char_at(&t, 0, 1) == 'B');
    CHECK(vga_text_char_at(&t, 0, 2) == ' ');
    CHECK(t.col == 2 && t.row == 0);

    vga_text_putc(&t, '\n');
    vga_text_puts(&t, "CD");
    CHECK(vga_text_char_at(&t, 1, 0) == 'C');
    CHECK(vga_text_char_at(&t, 1, 1) == 'D');

    vga_text_putc(&t, '\r');
    CHECK(t.col == 0);
    CHECK(vga_text_char_at(&t, 0, 79) == 0 || vga_text_char_at(&t, 0, 79) == ' ');

    /* 行尾自动换行：第 1 行剩余 78 列写满后到第 2 行。 */
    vga_text_clear(&t);
    for (i = 0; i < VGA_TEXT_COLS; i++) {
        vga_text_putc(&t, 'x');
    }
    CHECK(t.row == 1 && t.col == 0);
    CHECK(vga_text_char_at(&t, 0, VGA_TEXT_COLS - 1) == 'x');
    CHECK(vga_text_char_at(&t, 1, 0) == ' ');

    /* 最后一行再换行 -> 滚屏。 */
    for (i = 0; i < VGA_TEXT_COLS * VGA_TEXT_ROWS; i++) {
        vga_text_putc(&t, 'y');
    }
    vga_text_putc(&t, 'z');
    CHECK(t.row == VGA_TEXT_ROWS - 1);
    CHECK(vga_text_char_at(&t, VGA_TEXT_ROWS - 1, 0) == 'z');
    CHECK(vga_text_char_at(&t, 0, 0) == 'y');

    CHECK(vga_text_char_at(&t, VGA_TEXT_ROWS, 0) == 0);
    CHECK(vga_text_attr_at(&t, VGA_TEXT_ROWS, 0) == 0);
}

/* ------------------------------------------------------------------------- */
/* 帧缓冲                                                                    */
/* ------------------------------------------------------------------------- */

static void test_fb(void)
{
    static u32 fb32[64 * 32];
    static u8 fb24[16 * 8 * 3];
    static u16 fb16[32 * 16];
    struct fb_info fb;
    u32 x;
    u32 y;
    u32 cols;
    u32 rows;

    memset(fb32, 0, sizeof(fb32));
    CHECK(fb_init(&fb, fb32, 64, 32, 64u * 4u, 32) == FB_OK);
    CHECK(fb_init(NULL, fb32, 64, 32, 256, 32) == FB_EINVAL);
    CHECK(fb_init(&fb, NULL, 64, 32, 256, 32) == FB_EINVAL);
    CHECK(fb_init(&fb, fb32, 64, 32, 100, 32) == FB_EINVAL);
    CHECK(fb_init(&fb, fb32, 64, 32, 256, 8) == FB_EINVAL);

    memset(fb32, 0, sizeof(fb32));
    fb_init(&fb, fb32, 64, 32, 256, 32);
    CHECK(fb_pack(0x123456u, 32) == 0x123456u);
    CHECK(fb_pack(0xFF0000u, 16) == 0xF800u);
    CHECK(fb_pack(0x00FF00u, 16) == 0x07E0u);
    CHECK(fb_pack(0x0000FFu, 16) == 0x001Fu);

    fb_pixel(&fb, 3, 2, 0x00FF00u);
    CHECK(fb_read_pixel(&fb, 3, 2) == 0x00FF00u);
    CHECK(fb_read_pixel(&fb, 64, 0) == 0);
    fb_pixel(&fb, 64, 0, 0xFFFFFFu); /* 越界忽略，不崩 */

    fb_fill(&fb, 0x000102u);
    CHECK(fb_read_pixel(&fb, 0, 0) == 0x000102u);
    CHECK(fb_read_pixel(&fb, 63, 31) == 0x000102u);

    fb_rect(&fb, 60, 30, 10, 10, FB_RED); /* 自动裁剪 */
    CHECK(fb_read_pixel(&fb, 60, 30) == FB_RED);
    CHECK(fb_read_pixel(&fb, 63, 31) == FB_RED);

    fb_checkerboard(&fb, 0, 0, 32, 16, 8, FB_GREEN, FB_BLUE);
    CHECK(fb_read_pixel(&fb, 4, 4) == FB_GREEN);
    CHECK(fb_read_pixel(&fb, 12, 4) == FB_BLUE);
    CHECK(fb_read_pixel(&fb, 4, 12) == FB_BLUE);

    /* 字形 'A'：第 4 行 bit0=1 -> 前景；第 0 行 bit7=0 -> 背景。 */
    fb_fill(&fb, FB_BLACK);
    fb_glyph(&fb, 0, 0, 'A', FB_WHITE, FB_BLACK);
    CHECK(fb_read_pixel(&fb, 0, 4) == FB_WHITE);
    CHECK(fb_read_pixel(&fb, 7, 0) == FB_BLACK);

    /* 文本控制台换行 + 光标。 */
    fb_fill(&fb, FB_BLACK);
    fb_init(&fb, fb32, 64, 32, 256, 32);
    fb_set_colors(&fb, FB_WHITE, FB_BLACK);
    fb_print(&fb, "AB\nC");
    CHECK(fb_read_pixel(&fb, 0, 4) == FB_WHITE);  /* 'A' */
    CHECK(fb_read_pixel(&fb, 9, 4) == FB_WHITE);  /* 'B'（0x66 的 bit1） */
    CHECK(fb_read_pixel(&fb, 0, 16 + 4) == FB_WHITE); /* 'C' 第二行 */
    CHECK(fb.cursor_x == 8 && fb.cursor_y == 16);
    fb_grid(&fb, &cols, &rows);
    CHECK(cols == 8u && rows == 2u);

    /* 滚屏：两行屏，第二行写满后滚一行。 */
    fb_init(&fb, fb32, 64, 32, 256, 32);
    fb_fill(&fb, FB_BLACK);
    fb_glyph(&fb, 0, 0, 'A', FB_WHITE, FB_BLACK);
    fb_glyph(&fb, 0, 16, 'B', FB_WHITE, FB_BLACK);
    fb.cursor_x = 0;
    fb.cursor_y = 16;
    fb_newline(&fb);
    CHECK(fb.cursor_y == 16u);
    CHECK(fb_read_pixel(&fb, 0, 4) == FB_BLACK);        /* 'A' 被滚走 */
    CHECK(fb_read_pixel(&fb, 0, 4) == FB_BLACK);
    CHECK(fb_read_pixel(&fb, 0, 16 + 4) == FB_BLACK);   /* 底部已清空 */
    CHECK(fb.cursor_x == 0 && fb.cursor_y == 16);

    /* 16bpp RGB565。 */
    memset(fb16, 0, sizeof(fb16));
    CHECK(fb_init(&fb, fb16, 32, 16, 64, 16) == FB_OK);
    fb_pixel(&fb, 1, 1, FB_RED);
    CHECK(fb16[32 + 1] == 0xF800u);
    CHECK(fb_read_pixel(&fb, 1, 1) == 0xF80000u);

    /* 24bpp BGR 字节序。 */
    memset(fb24, 0, sizeof(fb24));
    CHECK(fb_init(&fb, fb24, 16, 8, 48, 24) == FB_OK);
    fb_pixel(&fb, 1, 1, 0x123456u);
    CHECK(fb24[48 + 3] == 0x56u && fb24[48 + 4] == 0x34u && fb24[48 + 5] == 0x12u);
    CHECK(fb_read_pixel(&fb, 1, 1) == 0x123456u);

    /* 光标读回。 */
    fb_init(&fb, fb32, 64, 32, 256, 32);
    CHECK(fb.cursor_x == 0 && fb.cursor_y == 0);
    (void)x;
    (void)y;
}

/* ------------------------------------------------------------------------- */
/* WP5-0h：统一输入服务（鼠标解码 / 事件队列 / 所有权 / aux 初始化）          */
/* ------------------------------------------------------------------------- */

struct fake_mouse_ctl {
    u8 data;
    int has_data;
    int fail_ack;
    u8 seq[32];
    u32 seq_n;
};

static struct fake_mouse_ctl fm;

static u8 fake_mouse_inb(void *ctx, u16 port)
{
    (void)ctx;
    if (port == PS2_STATUS_PORT) {
        return fm.has_data ? PS2_STATUS_OUTPUT_FULL : 0u;
    }
    if (port == PS2_DATA_PORT) {
        u8 v = fm.data;

        fm.has_data = 0;
        return v;
    }
    return 0xFFu;
}

static void fake_mouse_outb(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    if (port == PS2_STATUS_PORT && value == 0x20u) {
        fm.data = 0x00u; /* 配置字节 */
        fm.has_data = 1;
        return;
    }
    if (port == PS2_DATA_PORT) {
        if (fm.seq_n < (u32)sizeof(fm.seq)) {
            fm.seq[fm.seq_n++] = value;
        }
        if (!fm.fail_ack) {
            fm.data = PS2_ACK;
            fm.has_data = 1;
        }
    }
}

static void test_input_service(void)
{
    struct input_io io;
    struct input_mouse ms;
    struct input_queue q;
    struct input_packet pk;
    struct input_owner owner;
    struct input_state st;
    struct input_event ev;
    u8 pkt[3];
    u32 b = 0;
    i32 dx = 0;
    i32 dy = 0;
    u32 i;

    /* ---- 3 字节包解码：符号位 / 按键位 / 屏幕 y 方向 ---- */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x08u;
    pkt[1] = 10;
    pkt[2] = 10;
    CHECK(input_mouse_decode(pkt, &b, &dx, &dy) == 0);
    CHECK(b == 0u && dx == 10 && dy == -10);

    pkt[0] = 0x39u; /* left + 两个符号位（bit4/bit5） */
    pkt[1] = 0x80u;
    pkt[2] = 0x80u;
    CHECK(input_mouse_decode(pkt, &b, &dx, &dy) == 0);
    CHECK(b == INPUT_BTN_LEFT && dx == -128 && dy == 128);

    pkt[0] = 0x0Eu; /* right + middle */
    pkt[1] = 0xFFu;
    pkt[2] = 0x01u;
    CHECK(input_mouse_decode(pkt, &b, &dx, &dy) == 0);
    CHECK(b == (INPUT_BTN_RIGHT | INPUT_BTN_MIDDLE) && dx == 255 && dy == -1);

    pkt[0] = 0x48u; /* X overflow */
    CHECK(input_mouse_decode(pkt, &b, &dx, &dy) == -1);
    pkt[0] = 0x88u; /* Y overflow */
    CHECK(input_mouse_decode(pkt, &b, &dx, &dy) == -1);
    CHECK(input_mouse_decode(NULL, &b, &dx, &dy) == -1);

    /* ---- 喂字节状态机：失步/ACK 丢弃、凑满 3 字节才出事件 ---- */
    input_mouse_reset(&ms);
    input_queue_reset(&q);
    CHECK(input_mouse_feed(&ms, 0x00u, &q) == 0);  /* bit3=0 失步 */
    CHECK(input_mouse_feed(&ms, PS2_ACK, &q) == 0); /* ACK 在包边界忽略 */
    CHECK(input_mouse_feed(&ms, 0x08u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 10u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 20u, &q) == 1);
    CHECK(input_queue_pop(&q, &pk) == 0);
    CHECK(pk.type == INPUT_EV_MOVE && pk.x == 10 && pk.y == -20 &&
          pk.buttons == 0u);
    CHECK(ms.packets == 1u && ms.moves == 1u && ms.button_events == 0u);

    /* 左键按下（无位移）：只出 BUTTON。 */
    CHECK(input_mouse_feed(&ms, 0x09u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 0u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 0u, &q) == 1);
    CHECK(input_queue_pop(&q, &pk) == 0);
    CHECK(pk.type == INPUT_EV_BUTTON && pk.key == INPUT_BTN_LEFT &&
          pk.buttons == INPUT_BTN_LEFT);
    CHECK(ms.button_events == 1u && ms.buttons == INPUT_BTN_LEFT);

    /* 左键释放。 */
    CHECK(input_mouse_feed(&ms, 0x08u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 0u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 0u, &q) == 1);
    CHECK(input_queue_pop(&q, &pk) == 0 && pk.type == INPUT_EV_BUTTON &&
          pk.buttons == 0u);
    CHECK(ms.button_events == 2u && ms.buttons == 0u);

    /* 右键 + 位移同一包：先 BUTTON 后 MOVE。 */
    CHECK(input_mouse_feed(&ms, 0x2Au, &q) == 0);
    CHECK(input_mouse_feed(&ms, 5u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 0xF6u, &q) == 2);
    CHECK(input_queue_pop(&q, &pk) == 0 && pk.type == INPUT_EV_BUTTON &&
          pk.buttons == INPUT_BTN_RIGHT);
    CHECK(input_queue_pop(&q, &pk) == 0 && pk.type == INPUT_EV_MOVE &&
          pk.x == 5 && pk.y == 10 && pk.buttons == INPUT_BTN_RIGHT);
    CHECK(ms.moves == 2u && ms.button_events == 3u);

    /* 溢出包：不出事件、计数，再同步恢复。 */
    CHECK(input_mouse_feed(&ms, 0x48u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 1u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 2u, &q) == 0);
    CHECK(ms.overflows == 1u);
    CHECK(input_queue_pop(&q, &pk) == -1);
    CHECK(input_mouse_feed(&ms, 0x0Au, &q) == 0); /* 保持右键按下，只出 MOVE */
    CHECK(input_mouse_feed(&ms, 1u, &q) == 0);
    CHECK(input_mouse_feed(&ms, 1u, &q) == 1);
    CHECK(input_queue_pop(&q, &pk) == 0 && pk.type == INPUT_EV_MOVE &&
          pk.buttons == INPUT_BTN_RIGHT);

    /* ---- 事件队列 FIFO / 满丢弃 / reset ---- */
    input_queue_reset(&q);
    CHECK(q.count == 0u && q.lost == 0u);
    memset(&pk, 0, sizeof(pk));
    pk.type = INPUT_EV_KEY;
    for (i = 0; i < INPUT_QUEUE_SIZE; i++) {
        pk.key = i;
        CHECK(input_queue_push(&q, &pk) == 0);
    }
    CHECK(q.count == INPUT_QUEUE_SIZE);
    CHECK(input_queue_push(&q, &pk) == -1);
    CHECK(q.lost == 1u);
    for (i = 0; i < INPUT_QUEUE_SIZE; i++) {
        CHECK(input_queue_pop(&q, &pk) == 0 && pk.key == i);
    }
    CHECK(input_queue_pop(&q, &pk) == -1);
    CHECK(input_queue_push(NULL, &pk) == -1);
    CHECK(input_queue_pop(NULL, &pk) == -1);
    input_queue_reset(&q);
    CHECK(q.count == 0u && q.head == 0u && q.tail == 0u);

    /* ---- 所有权状态机 ---- */
    input_owner_reset(&owner);
    CHECK(owner.pid == 0u);
    CHECK(input_owner_may_read(&owner, 0u, 0) == 1);
    CHECK(input_owner_claim(&owner, 0u, 0) == -1);
    CHECK(input_owner_claim(&owner, 7u, 0) == 0);
    CHECK(owner.pid == 7u && owner.claims == 1u);
    CHECK(input_owner_may_read(&owner, 7u, 1) == 1);
    CHECK(input_owner_may_read(&owner, 8u, 1) == 0);
    CHECK(owner.denials == 1u);
    CHECK(input_owner_claim(&owner, 8u, 1) == -1);
    CHECK(owner.denials == 2u && owner.pid == 7u);
    CHECK(input_owner_claim(&owner, 7u, 1) == 0);
    CHECK(owner.claims == 2u);
    CHECK(input_owner_release(&owner, 8u) == -1);
    CHECK(input_owner_release(&owner, 7u) == 0);
    CHECK(owner.pid == 0u && owner.releases == 1u);
    CHECK(input_owner_release(&owner, 7u) == -1);

    /* 属主死了：may_read 自动回收；新进程可重新认领。 */
    CHECK(input_owner_claim(&owner, 9u, 0) == 0);
    CHECK(input_owner_may_read(&owner, 10u, 0) == 1);
    CHECK(owner.pid == 0u && owner.releases == 2u);
    CHECK(input_owner_claim(&owner, 9u, 0) == 0);
    CHECK(input_owner_claim(&owner, 10u, 0) == 0);
    CHECK(owner.pid == 10u);
    CHECK(input_owner_release(&owner, 10u) == 0);

    /* ---- input_feed_ev：事件既进旧环也回填 *out ---- */
    input_init(&st);
    memset(&ev, 0, sizeof(ev));
    CHECK(input_feed_ev(&st, 0x1Eu, &ev) == 1);
    CHECK(ev.ascii == 'a' && ev.scancode == 0x1Eu);
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'a');
    CHECK(input_feed_ev(&st, 0x9Eu, &ev) == 0);
    CHECK(input_feed_ev(&st, 0x2Au, &ev) == 0);
    CHECK(input_feed_ev(&st, 0x1Eu, &ev) == 1 && ev.ascii == 'A');
    CHECK(input_next(&st, &ev) == 0 && ev.ascii == 'A');

    /* ---- ps2_init_mouse：aux 初始化序列 0xF6/F3/0x64/E8/3/F4 ---- */
    memset(&fm, 0, sizeof(fm));
    io.inb = fake_mouse_inb;
    io.outb = fake_mouse_outb;
    io.ctx = NULL;
    input_mouse_reset(&ms);
    CHECK(ps2_init_mouse(&ms, &io) == 0);
    CHECK(fm.seq_n >= 7u);
    CHECK(fm.seq[0] == 0x02u);       /* 配置字节 */
    CHECK(fm.seq[1] == 0xF6u);       /* set defaults */
    CHECK(fm.seq[2] == 0xF3u);       /* set sample rate */
    CHECK(fm.seq[3] == 100u);
    CHECK(fm.seq[4] == 0xE8u);       /* set resolution */
    CHECK(fm.seq[5] == 3u);
    CHECK(fm.seq[6] == 0xF4u);       /* enable reporting */
    CHECK(ms.packets == 0u);
    CHECK(ps2_init_mouse(NULL, &io) == -1);

    /* 不 ACK 的控制器：init 失败（不能假装鼠标就绪）。 */
    fm.fail_ack = 1;
    input_mouse_reset(&ms);
    CHECK(ps2_init_mouse(&ms, &io) == -1);
}

void test_drivers(void)
{
    test_pci();
    test_vbe();
    test_font();
    test_input();
    test_input_service();
    test_vga_text();
    test_fb();
}
