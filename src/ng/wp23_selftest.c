/*
 * ng WP2-3: 显示 + 输入内核接线与自检（见 wp23.h）。
 *
 * 证据行（stdvga 路径）：
 *   ng: pci vga found
 *   ng: pci vga 1234:1111 cmd=0x107 bar0=0xfd000000 size=16777216
 *   ng: vbe id=0xb0c5
 *   ng: vbe mode 1280x800x32 ok
 *   ng: vbe pitch=5120 virt=1280x800
 *   ng: lfb map ok (base=0xfd000000 pages=1000)
 *   ng: lfb pte=0x... (pcd/pwt)
 *   ng: fb draw ok (readback)
 *   ng: fb console cols=160 rows=50
 *   ng: kbd init ok
 *   ng: input event ok (n=5)
 *
 * 降级路径（-vga none / 无 Bochs VBE）：
 *   ng: pci vga missing
 *   ng: vbe unavailable (...), vga text fallback
 *   ng: vga text fallback ok (0xB8000)
 *   ng: kbd init ok / ng: input event ok / ng: WP2-3 selftest PASS
 *
 * LFB 映射：0xFD000000 之类的 MMIO 落在 0..1 GiB 恒等映射之外，pg_map() 会按需
 * 分配 PDPT/PD/PT，把 BAR0 逐 4 KiB 恒等映射成 RW + PCD + PWT + NX（MMIO 不走
 * 可缓存路径）。映射只进内核 PML4；ring3 地址空间（useras）只共享 0..1 GiB，
 * 显示在 E3 之前不做用户态直写。
 */
#include "wp23.h"

#include "fb.h"
#include "input.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include "proc.h"
#include "serial.h"
#include "thread.h"
#include "vbe.h"
#include "vga_text.h"

#define VGA_VENDOR_ID 0x1234u
#define VGA_DEVICE_ID 0x1111u

#define VBE_PREF_W 1280u
#define VBE_PREF_H 800u
#define VBE_FALLBACK_W 1024u
#define VBE_FALLBACK_H 768u
#define VBE_PREF_BPP 32u

/* 只允许映射 16 MiB 以内的 BAR，防止把奇怪的地址整段吃掉。 */
#define LFB_MAP_MAX (16ULL * 1024ULL * 1024ULL)
#define LFB_ASSIGN_BASE 0xFD000000ULL

/* ------------------------------------------------------------------------- */
/* 端口 IO 注入                                                               */
/* ------------------------------------------------------------------------- */

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

static void vbe_outw(void *ctx, u16 port, u16 value)
{
    (void)ctx;
    outw(port, value);
}

static u16 vbe_inw(void *ctx, u16 port)
{
    (void)ctx;
    return inw(port);
}

static u8 ps2_inb(void *ctx, u16 port)
{
    (void)ctx;
    return inb(port);
}

static void ps2_outb(void *ctx, u16 port, u8 value)
{
    (void)ctx;
    outb(port, value);
}

static struct pci_io_ops g_pci_io = {pci_out32, pci_in32, NULL};
static struct vbe_io g_vbe_io = {vbe_outw, vbe_inw, NULL};
static struct input_io g_input_io = {ps2_inb, ps2_outb, NULL};

static struct fb_info g_fb;
static struct vga_text g_text;
static struct input_state g_input;
static int g_fb_ready;
static int g_text_ready;

/*
 * WP5-0h：内核输入服务。键盘（g_input）+ 鼠标（g_mouse）由 sched_yield()
 * 调度点上的 ng_input_poll() 轮询（readkey 对 boot 同步 ring3 演示兜底），
 * 统一推进 g_events；syscall 96 只做非阻塞出队。readkey（syscall 30）继续
 * 从 g_input.ring 读，保留旧 ABI；但 displayd 认领输入后，其它进程的
 * readkey 返回 -EAGAIN 并让出一轮，避免 shell 忙等抢键。
 */
static struct input_queue g_events;
static struct input_mouse g_mouse;
static struct input_owner g_owner;
static int g_input_ready;
static int g_mouse_ready;
static u32 g_input_aux_bytes;
static u32 g_input_key_bytes;
static u32 g_input_poll_rounds;
static u32 g_mouse_moves_seen;
static u32 g_mouse_buttons_seen;
static u32 g_key_print;
static u32 g_mouse_print;

/* ------------------------------------------------------------------------- */
/* LFB 映射                                                                   */
/* ------------------------------------------------------------------------- */

static u64 lfb_flags(void)
{
    return PG_WRITE | PG_PCD | PG_PWT | (pg_nx_on() ? PG_NX : 0);
}

static int lfb_map(u64 base, u64 size)
{
    u64 start = base & ~0xFFFULL;
    u64 end = (base + size + 0xFFFULL) & ~0xFFFULL;
    u64 flags = lfb_flags();
    u64 va;
    u32 pages = 0;
    u64 pte;

    if (size == 0 || end <= start || (end - start) > LFB_MAP_MAX) {
        return -1;
    }
    for (va = start; va < end; va += 0x1000ULL) {
        int rc = pg_map(va, va, flags);

        if (rc == PG_EBIGPAGE) {
            /* 已经由 2 MiB 恒等大页覆盖：确认身份映射即可（< 1 GiB 的 BAR）。 */
            u64 e = pg_walk(va);

            if ((e & (PG_PRESENT | PG_PS)) != (PG_PRESENT | PG_PS) ||
                (e & PG_ADDR_MASK) != (va & ~((1ULL << 21) - 1ULL))) {
                return -1;
            }
        } else if (rc != PG_OK) {
            return -1;
        }
        pages++;
    }

    pte = pg_walk(start);
    if ((pte & (PG_PRESENT | PG_WRITE)) != (PG_PRESENT | PG_WRITE)) {
        return -1;
    }
    if ((pte & PG_PS) == 0 &&
        (pte & (PG_PCD | PG_PWT)) != (PG_PCD | PG_PWT)) {
        return -1;
    }

    serial_puts("ng: lfb map ok (base=");
    serial_puthex(start);
    serial_puts(" pages=");
    serial_putdec((u64)pages);
    serial_puts(")\r\n");
    serial_puts("ng: lfb pte=");
    serial_puthex(pte);
    serial_puts(" (pcd/pwt)\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 帧缓冲自检：棋盘/矩形/文字 + 读回像素                                     */
/* ------------------------------------------------------------------------- */

static int fb_draw_selftest(void)
{
    int fails = 0;

    fb_fill(&g_fb, FB_DARK);

    /* 字形 'A'：8x8 基线第 2 行是 0x33（bit0=1），纵向展开后第 4 行 bit0=1。 */
    fb_glyph(&g_fb, 16, 40, 'A', FB_WHITE, FB_DARK);
    if (fb_read_pixel(&g_fb, 16, 44) != FB_WHITE) {
        fails++;
    }
    if (fb_read_pixel(&g_fb, 23, 40) != FB_DARK) {
        fails++;
    }

    /* 棋盘：64x64、8 像素格，(0,0)=绿，(1,0)=蓝，(0,1)=蓝。 */
    fb_checkerboard(&g_fb, 32, 64, 64, 64, 8, FB_GREEN, FB_BLUE);
    if (fb_read_pixel(&g_fb, 36, 68) != FB_GREEN) {
        fails++;
    }
    if (fb_read_pixel(&g_fb, 44, 68) != FB_BLUE) {
        fails++;
    }
    if (fb_read_pixel(&g_fb, 36, 76) != FB_BLUE) {
        fails++;
    }

    /* 实心矩形 + 外侧背景读回。 */
    fb_rect(&g_fb, 200, 0, 16, 16, FB_RED);
    if (fb_read_pixel(&g_fb, 200, 0) != FB_RED ||
        fb_read_pixel(&g_fb, 215, 15) != FB_RED) {
        fails++;
    }
    if (fb_read_pixel(&g_fb, 216, 0) != FB_DARK) {
        fails++;
    }

    /* 文本控制台：从 y=200 开始打印，再读回 'A'/'V' 的像素。 */
    g_fb.cursor_x = 0;
    g_fb.cursor_y = 200;
    fb_set_colors(&g_fb, FB_WHITE, FB_DARK);
    fb_print(&g_fb, "Aster ng WP2-3 framebuffer console\r\n");
    fb_print(&g_fb, "VBE 1280x800x32, embedded 8x16 font\r\n");
    fb_print(&g_fb, "ng: WP2-3 selftest\r\n");
    if (fb_read_pixel(&g_fb, 0, 204) != FB_WHITE) {
        fails++;
    }
    if (fb_read_pixel(&g_fb, 0, 200 + 16 + 4) != FB_WHITE) {
        fails++;
    }

    if (fails == 0) {
        u32 cols;
        u32 rows;

        g_fb_ready = 1;
        serial_puts("ng: fb draw ok (readback)\r\n");
        fb_grid(&g_fb, &cols, &rows);
        serial_puts("ng: fb console cols=");
        serial_putdec((u64)cols);
        serial_puts(" rows=");
        serial_putdec((u64)rows);
        serial_puts("\r\n");
    } else {
        serial_puts("ng: fb draw FAIL (n=");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 显示路径                                                                   */
/* ------------------------------------------------------------------------- */

static int setup_vbe_vga(struct pci_loc *loc)
{
    struct pci_bar bar;
    struct vbe_mode mode;
    u16 vbe_id = 0;
    u16 cmd;
    int cmd_ok;
    int rc;

    if (pci_read_bar(&g_pci_io, loc, 0, &bar) != 0 || bar.size == 0) {
        serial_puts("ng: pci vga bar0 FAIL\r\n");
        return -1;
    }
    if (bar.base == 0) {
        /* QEMU -kernel 通常已预置 BAR；没有预置就自己放到传统 VGA LFB 地址。 */
        u32 val = (u32)LFB_ASSIGN_BASE | (bar.prefetch ? 0x8u : 0u);
        u32 raw = 0;

        (void)pci_cfg_write32(&g_pci_io, loc->bus, loc->dev, loc->func, 0x10, val);
        (void)pci_cfg_read32(&g_pci_io, loc->bus, loc->dev, loc->func, 0x10, &raw);
        bar.base = (u64)(raw & 0xFFFFFFF0u);
    }

    cmd = pci_command(&g_pci_io, loc, &cmd_ok);
    serial_puts("ng: pci vga 1234:1111 cmd=");
    serial_puthex((u64)cmd);
    serial_puts(" bar0=");
    serial_puthex(bar.base);
    serial_puts(" size=");
    serial_putdec(bar.size);
    serial_puts("\r\n");

    if (vbe_probe(&g_vbe_io, &vbe_id) != VBE_OK) {
        serial_puts("ng: vbe unavailable (no bochs dispi id)\r\n");
        return -1;
    }
    serial_puts("ng: vbe id=");
    serial_puthex((u64)vbe_id);
    serial_puts(" mem=");
    serial_putdec((u64)vbe_video_mem_bytes(&g_vbe_io));
    serial_puts("\r\n");

    rc = vbe_set_mode(&g_vbe_io, (u16)VBE_PREF_W, (u16)VBE_PREF_H,
                      (u16)VBE_PREF_BPP, &mode);
    if (rc != VBE_OK) {
        serial_puts("ng: vbe 1280x800x32 rejected (rc=");
        serial_putdec((u64)(0 - rc));
        serial_puts("), retry 1024x768x32\r\n");
        rc = vbe_set_mode(&g_vbe_io, (u16)VBE_FALLBACK_W, (u16)VBE_FALLBACK_H,
                          (u16)VBE_PREF_BPP, &mode);
    }
    if (rc != VBE_OK) {
        serial_puts("ng: vbe mode FAIL (rc=");
        serial_putdec((u64)(0 - rc));
        serial_puts(")\r\n");
        vbe_disable(&g_vbe_io);
        return -1;
    }

    if (mode.width == VBE_PREF_W) {
        serial_puts("ng: vbe mode 1280x800x32 ok\r\n");
    } else {
        serial_puts("ng: vbe mode 1024x768x32 ok\r\n");
        serial_puts("ng: vbe (1280x800 unavailable, using 1024x768)\r\n");
    }
    serial_puts("ng: vbe pitch=");
    serial_putdec((u64)mode.pitch);
    serial_puts(" virt=");
    serial_putdec((u64)mode.virt_width);
    serial_puts("x");
    serial_putdec((u64)mode.virt_height);
    serial_puts("\r\n");

    if (bar.size < (u64)mode.size_bytes) {
        serial_puts("ng: lfb bar too small for mode\r\n");
        vbe_disable(&g_vbe_io);
        return -1;
    }
    if (lfb_map(bar.base, (u64)mode.size_bytes) != 0) {
        serial_puts("ng: lfb map FAIL\r\n");
        vbe_disable(&g_vbe_io);
        return -1;
    }
    if (fb_init(&g_fb, (void *)(usize)bar.base, mode.width, mode.height,
                mode.pitch, mode.bpp) != FB_OK) {
        serial_puts("ng: fb init FAIL\r\n");
        vbe_disable(&g_vbe_io);
        return -1;
    }
    return 0;
}

static int setup_vga_text(void)
{
    vga_text_init(&g_text, (void *)(usize)VGA_TEXT_ADDR);
    vga_text_clear(&g_text);
    vga_text_puts(&g_text, "Aster ng WP2-3: stdvga/VBE unavailable\r\n");
    vga_text_puts(&g_text, "VGA text fallback at 0xB8000 (system usable)\r\n");

    if (vga_text_char_at(&g_text, 0, 0) != 'A' ||
        vga_text_attr_at(&g_text, 0, 0) != VGA_TEXT_ATTR ||
        vga_text_char_at(&g_text, 1, 0) != 'V') {
        serial_puts("ng: vga text fallback FAIL\r\n");
        return -1;
    }
    serial_puts("ng: vga text fallback ok (0xB8000)\r\n");
    g_text_ready = 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* PS/2 输入自检（纯解码路径，不依赖是否真的按了键）                          */
/* ------------------------------------------------------------------------- */

static int input_decode_selftest(void)
{
    struct input_state t;
    struct input_event ev;
    int fails = 0;
    int n = 0;

    input_init(&t);

    /* 'a' 按下 -> 一个事件；释放不产生事件。 */
    if (input_feed(&t, 0x1Eu) != 1 ||
        input_next(&t, &ev) != 0 || ev.ascii != 'a' || ev.scancode != 0x1Eu ||
        ev.pressed != 1 || ev.flags != 0) {
        fails++;
    } else {
        n++;
    }
    if (input_feed(&t, 0x9Eu) != 0 || input_next(&t, &ev) != -1) {
        fails++;
    }

    /* Shift + 'a' -> 'A'。 */
    (void)input_feed(&t, 0x2Au);
    if (input_feed(&t, 0x1Eu) != 1 || input_next(&t, &ev) != 0 ||
        ev.ascii != 'A' || (ev.flags & INPUT_F_SHIFT) == 0) {
        fails++;
    } else {
        n++;
    }
    (void)input_feed(&t, 0xAAu);

    /* CapsLock 按下 -> 后续字母大写；再按一次恢复。 */
    (void)input_feed(&t, 0x3Au);
    if (input_feed(&t, 0x1Eu) != 1 || input_next(&t, &ev) != 0 ||
        ev.ascii != 'A' || (ev.flags & INPUT_F_CAPS) == 0) {
        fails++;
    } else {
        n++;
    }
    (void)input_feed(&t, 0x3Au);
    if (input_feed(&t, 0x1Eu) != 1 || input_next(&t, &ev) != 0 || ev.ascii != 'a') {
        fails++;
    } else {
        n++;
    }

    /* 扩展前缀：0xE0 0x50 = 方向键下，scancode 保留、ascii=0、EXT 标志。 */
    (void)input_feed(&t, 0xE0u);
    if (input_feed(&t, 0x50u) != 1 || input_next(&t, &ev) != 0 ||
        ev.scancode != 0x50u || ev.ascii != 0 || (ev.flags & INPUT_F_EXT) == 0) {
        fails++;
    } else {
        n++;
    }

    /* Shift + '1' -> '!'（键位映射覆盖）。 */
    (void)input_feed(&t, 0x2Au);
    if (input_feed(&t, 0x02u) != 1 || input_next(&t, &ev) != 0 || ev.ascii != '!') {
        fails++;
    } else {
        n++;
    }

    if (t.ring.count != 0 || t.ring.lost != 0 || t.releases == 0) {
        fails++;
    }
    if (fails == 0) {
        serial_puts("ng: input event ok (n=");
        serial_putdec((u64)n);
        serial_puts(")\r\n");
    } else {
        serial_puts("ng: input event FAIL (n=");
        serial_putdec((u64)fails);
        serial_puts(")\r\n");
    }
    return fails;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

int wp23_selftest(void)
{
    int fails = 0;
    struct pci_loc loc;
    int have_vga;
    int display_ok = 0;
    int kbd_ok;

    input_init(&g_input);

    have_vga = (pci_find(&g_pci_io, VGA_VENDOR_ID, VGA_DEVICE_ID, &loc) == 0);
    if (have_vga) {
        serial_puts("ng: pci vga found\r\n");
        if (pci_enable(&g_pci_io, &loc, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER) != 0) {
            serial_puts("ng: pci vga enable FAIL\r\n");
            fails++;
        }
        if (setup_vbe_vga(&loc) == 0) {
            if (fb_draw_selftest() == 0) {
                display_ok = 1;
            } else {
                fails++;
            }
        }
        if (!display_ok) {
            /* stdvga 存在但 VBE/映射失败：关 VBE 回文本，别把屏幕留在半配置态。 */
            vbe_disable(&g_vbe_io);
            serial_puts("ng: vbe unavailable, vga text fallback\r\n");
        }
    } else {
        serial_puts("ng: pci vga missing\r\n");
        serial_puts("ng: vbe unavailable (no 1234:1111), vga text fallback\r\n");
    }

    if (!display_ok) {
        if (setup_vga_text() != 0) {
            fails++;
        }
    }

    kbd_ok = (ps2_init(&g_input, &g_input_io) == 0);
    if (kbd_ok) {
        serial_puts("ng: kbd init ok\r\n");
    } else {
        serial_puts("ng: kbd init FAIL (no i8042 ack)\r\n");
        fails++;
    }

    /* WP5-0h：aux 口 + 3 字节包鼠标。QEMU/wstdvga 路径必须 ACK。 */
    input_queue_reset(&g_events);
    input_mouse_reset(&g_mouse);
    input_owner_reset(&g_owner);
    g_input_aux_bytes = 0;
    g_input_key_bytes = 0;
    g_input_poll_rounds = 0;
    g_mouse_moves_seen = 0;
    g_mouse_buttons_seen = 0;
    if (ps2_init_mouse(&g_mouse, &g_input_io) == 0) {
        g_mouse_ready = 1;
        serial_puts("ng: mouse init ok (aux=0x60 irq12=1 f4/f3/e8)\r\n");
    } else {
        g_mouse_ready = 0;
        serial_puts("ng: mouse init FAIL (no aux ack)\r\n");
        fails++;
    }
    g_input_ready = 1;
    ng_input_poll();

    fails += input_decode_selftest();
    return fails;
}

/* ------------------------------------------------------------------------- */
/* WP5-0h：内核输入服务                                                       */
/* ------------------------------------------------------------------------- */

static int ng_input_owner_alive(u32 pid)
{
    const struct proc *p;

    if (pid == 0u) {
        return 0;
    }
    p = proc_slot(pid);
    if (p == NULL) {
        return 0;
    }
    return (p->state != PROC_STATE_FREE && p->state != PROC_STATE_ZOMBIE) ? 1
                                                                          : 0;
}

/*
 * 输入服务轮询：sched_yield() 是主轮询点（所有阻塞/让出都经过），readkey
 * 在 boot 的同步 ring3 演示里做兜底。i8042 状态 bit5 区分 aux（鼠标）和
 * 键盘；键盘字节仍喂旧解码器（readkey 兼容），同时复制一份进 g_events 给
 * syscall 96。没有 IRQ 也能工作。
 */
void ng_input_poll(void)
{
    u32 n;

    if (g_input_ready == 0) {
        return;
    }
    g_input_poll_rounds++;
    for (n = 0; n < INPUT_POLL_MAX; n++) {
        u8 status = g_input_io.inb(g_input_io.ctx, PS2_STATUS_PORT);
        u8 data;

        if (status == 0xFFu || (status & PS2_STATUS_OUTPUT_FULL) == 0u) {
            return;
        }
        data = g_input_io.inb(g_input_io.ctx, PS2_DATA_PORT);
        if ((status & PS2_STATUS_AUX) != 0u) {
            u32 packets = g_mouse.packets;

            g_input_aux_bytes++;
            (void)input_mouse_feed(&g_mouse, data, &g_events);
            if (g_mouse.packets != packets && g_mouse_print < 4u) {
                g_mouse_print++;
                serial_puts("ng: input poll mouse ok (packets=");
                serial_putdec(g_mouse.packets);
                serial_puts(" rounds=");
                serial_putdec(g_input_poll_rounds);
                serial_puts(" idle=");
                serial_putdec(sched_idle_runs());
                serial_puts(")\r\n");
            }
        } else {
            struct input_event ev;
            int rc;

            g_input_key_bytes++;
            rc = input_feed_ev(&g_input, data, &ev);
            if (rc == 1) {
                struct input_packet pk;

                pk.type = INPUT_EV_KEY;
                pk.key = (u32)ev.ascii;
                pk.x = 0;
                pk.y = 0;
                pk.buttons = g_mouse.buttons;
                pk.scancode = (u32)ev.scancode;
                pk.flags = (u32)ev.flags;
                (void)input_queue_push(&g_events, &pk);
                if (g_key_print < 4u) {
                    g_key_print++;
                    serial_puts("ng: input poll key ok (key=");
                    serial_puthex((u32)ev.ascii);
                    serial_puts(" scancode=");
                    serial_puthex((u32)ev.scancode);
                    serial_puts(" keys=");
                    serial_putdec(g_input_key_bytes);
                    serial_puts(" rounds=");
                    serial_putdec(g_input_poll_rounds);
                    serial_puts(" idle=");
                    serial_putdec(sched_idle_runs());
                    serial_puts(")\r\n");
                }
            }
        }
    }
}

/* syscall 96：非阻塞取一个统一输入事件。0=取到，-EAGAIN=没有。 */
int ng_input_event(struct input_packet *out)
{
    if (out == NULL) {
        return NG_EAGAIN;
    }
    if (input_queue_pop(&g_events, out) != 0) {
        return NG_EAGAIN;
    }
    if (out->type == INPUT_EV_MOVE) {
        g_mouse_moves_seen++;
    } else if (out->type == INPUT_EV_BUTTON) {
        g_mouse_buttons_seen++;
    }
    return 0;
}

/*
 * syscall 97：输入所有权。pid=0 表示当前进程；flags bit0=释放。
 * 返回 0 / -1(EPERM，当前不是进程) / -16(EBUSY，已有存活属主)。数值与
 * ABI 1.0 的 EPERM/EBUSY 一致。
 */
i64 ng_input_claim(u64 pid_arg, u64 flags)
{
    u32 pid = (u32)pid_arg;
    int alive;

    if (pid == 0u) {
        pid = proc_getpid();
    }
    if (pid == 0u) {
        return -1;
    }
    if ((flags & 1u) != 0u) {
        return (input_owner_release(&g_owner, pid) == 0) ? 0 : -1;
    }
    alive = ng_input_owner_alive(g_owner.pid);
    if (input_owner_claim(&g_owner, pid, alive) != 0) {
        return -16;
    }
    /* 认领成功：清掉旧 readkey 环与已入队的旧事件（认领前的按键不属于 displayd）。 */
    input_ring_reset(&g_input.ring);
    input_queue_reset(&g_events);
    serial_puts("ng: input claim ok (pid=");
    serial_putdec(pid);
    serial_puts(" polls=");
    serial_putdec(g_input_poll_rounds);
    serial_puts(" keys=");
    serial_putdec(g_input_key_bytes);
    serial_puts(" aux=");
    serial_putdec(g_input_aux_bytes);
    serial_puts(" idle=");
    serial_putdec(sched_idle_runs());
    serial_puts(")\r\n");
    return 0;
}

/*
 * WP5-0h：只有“真正的进程线程”可以在 syscall 里让出。boot 线程 / init（pid 1）
 * 的 ring3 演示没有独立 syscall 栈与 PML4 恢复上下文（proc_init 只把 init
 * 挂到 boot TCB 上，没设 t->syscall_top/t->pml4），在里面调度会把 CR3 切回
 * 内核表，SYSRET 回用户态时取指失败（实测 #PF）。所以 readkey/输入路径只对
 * 非 boot 线程 yield。
 */
static int ng_input_can_yield(void)
{
    thread_t *t = thread_self();

    if (t == NULL) {
        return 0;
    }
    return ((t->flags & THREAD_F_BOOT) != 0) ? 0 : 1;
}

u64 ng_input_readkey(void)
{
    struct input_event ev;
    u32 pid = proc_getpid();
    int alive = ng_input_owner_alive(g_owner.pid);

    if (input_owner_may_read(&g_owner, pid, alive) == 0) {
        /*
         * 不是输入属主：让出一轮，避免 shell 在 -EAGAIN 上空转饿死 displayd。
         * boot/init 的同步 ring3 演示不能在这里调度（见上）。
         */
        if (ng_input_can_yield()) {
            (void)thread_sleep(0);
        }
        return (u64)(i64)NG_EAGAIN;
    }
    /*
     * 兜底轮询：sched_yield() 是主轮询点，但 boot 的同步 ring3 演示没有调度
     * 点（不能 yield），所以 readkey 自己也把控制器里的字节收进队列。
     */
    ng_input_poll();
    if (input_next(&g_input, &ev) != 0) {
        if (ng_input_can_yield()) {
            (void)thread_sleep(0);
        }
        return (u64)(i64)NG_EAGAIN;
    }
    return ((u64)ev.flags << 24) | ((u64)ev.ascii << 16) | (u64)ev.scancode;
}

/* 诊断用（parity/claims 读计数，不改变状态）。 */
u32 ng_input_aux_bytes(void)
{
    return g_input_aux_bytes;
}

u32 ng_input_key_bytes(void)
{
    return g_input_key_bytes;
}

u32 ng_input_poll_rounds(void)
{
    return g_input_poll_rounds;
}

u32 ng_input_mouse_moves(void)
{
    return g_mouse_moves_seen;
}

u32 ng_input_mouse_buttons(void)
{
    return g_mouse_buttons_seen;
}

u32 ng_input_owner_pid(void)
{
    return g_owner.pid;
}

u32 ng_input_owner_denials(void)
{
    return g_owner.denials;
}

int ng_input_mouse_ready(void)
{
    return g_mouse_ready;
}

/* ------------------------------------------------------------------------- */
/* WP2-7 parity 诊断（只读）                                                  */
/* ------------------------------------------------------------------------- */

const struct fb_info *ng_display_fb(void)
{
    return (g_fb_ready != 0) ? &g_fb : NULL;
}

const struct vga_text *ng_display_text(void)
{
    return (g_text_ready != 0) ? &g_text : NULL;
}

const struct input_state *ng_input_state(void)
{
    return &g_input;
}
