/* =============================================================================
 * irq.c - 8259 / PIT / 键盘，以及可注册的 IRQ 框架
 *
 * 结构：
 *   - irq_register(irq, handler, arg, name)：模块把自己的处理函数挂进来；
 *   - irq_mask/unmask：按 IRQ 线单独开关（读改写 PIC 屏蔽字）；
 *   - 伪中断（IRQ7/IRQ15）在分发前识别，避免误发 EOI；
 *   - 时钟中断负责累加 tick，并在 EOI 之后触发调度。
 * ============================================================================= */
#include "interrupt.h"
#include "uaccess.h"
#include "arch/x86/io.h"
#include "../../core/error.h"
#include "../percpu.h"
#include "../proc.h"
#include "include/dev.h"
#include "include/mm.h"
#include "../../lib/kprintf.h"

/* 哪些 IRQ 已经改由 IOAPIC 投递（EOI 要发给 LAPIC 而不是 8259） */
static uint32_t ioapic_routed;
/* 其中哪些是电平触发（LAPIC EOI 之后还要写 IOAPIC EOI）。 */
static uint32_t ioapic_level;

#define LAPIC_EOI_OFFSET 0xB0U

static void lapic_send_eoi(void)
{
    if (apic_base() != 0U) {
        volatile uint32_t *eoi =
            (volatile uint32_t *)(uintptr_t)(apic_base() + LAPIC_EOI_OFFSET);
        *eoi = 0U;
    }
}

/* ---- 8259A 可编程中断控制器端口 ---- */
#define PIC1_CMD   0x20U
#define PIC1_DATA  0x21U
#define PIC2_CMD   0xA0U
#define PIC2_DATA  0xA1U
#define PIC_EOI    0x20U
#define PIC_READ_ISR 0x0BU

/* ---- 8254 可编程定时器端口 ---- */
#define PIT_CH0    0x40U
#define PIT_CMD    0x43U
#define PIT_BASE   1193182U

/* 键盘环形缓冲区大小 */
#define KBD_BUF_SIZE 64U

typedef struct {
    irq_handler_t handler;
    void         *arg;
    const char   *name;
    unsigned int  count;
} irq_slot_t;

static irq_slot_t   slots[IRQ_MAX];
static unsigned int spurious_count;
static volatile unsigned int ticks;

unsigned int timer_ticks(void)
{
    return ticks;
}

static void pit_init(unsigned int hz);

static volatile unsigned char kbd_buf[KBD_BUF_SIZE];
static volatile unsigned int  kbd_head, kbd_tail;

/* ==================== PIC 基础操作 ==================== */
static void io_wait(void)
{
    outb(0x80U, 0U);
}

static void pic_remap(void)
{
    outb(PIC1_CMD, 0x11U); io_wait();
    outb(PIC2_CMD, 0x11U); io_wait();
    outb(PIC1_DATA, 0x20U); io_wait();  /* 主片向量基址 0x20 */
    outb(PIC2_DATA, 0x28U); io_wait();  /* 从片向量基址 0x28 */
    outb(PIC1_DATA, 0x04U); io_wait();  /* 从片接在 IRQ2 */
    outb(PIC2_DATA, 0x02U); io_wait();
    outb(PIC1_DATA, 0x01U); io_wait();  /* 8086 模式 */
    outb(PIC2_DATA, 0x01U); io_wait();

    outb(PIC1_DATA, 0xFFU);             /* 先全部屏蔽 */
    outb(PIC2_DATA, 0xFFU);
}

/* 单独设置某条 IRQ 的屏蔽位 */
static void pic_set_mask(unsigned int irq, bool masked)
{
    unsigned int port = (irq < 8U) ? PIC1_DATA : PIC2_DATA;
    unsigned int bit  = irq & 7U;
    unsigned char value = inb((unsigned short)port);

    if (masked) {
        value = (unsigned char)(value | (1U << bit));
    } else {
        value = (unsigned char)(value & ~(1U << bit));
    }
    outb((unsigned short)port, value);

    /* 从片上任何一条 IRQ 放行时，主片的 IRQ2 必须一起放行 */
    if (irq >= 8U && !masked) {
        unsigned char master = inb(PIC1_DATA);
        outb(PIC1_DATA, (unsigned char)(master & ~(1U << 2)));
    }
}

int irq_mask(unsigned int irq)
{
    if (irq >= IRQ_MAX) return E_INVAL;
    /* 已迁到 IOAPIC 的线要同时改重定向表，否则只掩 PIC 不会真正静音。 */
    if ((ioapic_routed & (1U << irq)) != 0U) {
        (void)ioapic_mask(madt_irq_gsi(irq), true);
    }
    pic_set_mask(irq, true);
    return E_OK;
}

int irq_unmask(unsigned int irq)
{
    if (irq >= IRQ_MAX) return E_INVAL;
    if ((ioapic_routed & (1U << irq)) != 0U) {
        (void)ioapic_mask(madt_irq_gsi(irq), false);
    }
    pic_set_mask(irq, false);
    return E_OK;
}

/* ==================== 伪中断检测 ==================== */
/* 读 ISR：对应位为 1 说明是真实中断，为 0 说明是 8259 误报的伪中断 */
static bool irq_is_spurious(unsigned int irq)
{
    if (irq == 7U) {
        outb(PIC1_CMD, PIC_READ_ISR);
        return (inb(PIC1_CMD) & 0x80U) == 0U;
    }
    if (irq == 15U) {
        outb(PIC2_CMD, PIC_READ_ISR);
        return (inb(PIC2_CMD) & 0x80U) == 0U;
    }
    return false;
}

/* ==================== 注册与统计 ==================== */
int irq_register(unsigned int irq, irq_handler_t handler, void *arg, const char *name)
{
    if (irq >= IRQ_MAX || handler == 0 || name == 0) return E_INVAL;
    if (slots[irq].handler != 0) return E_EXIST;

    slots[irq].handler = handler;
    slots[irq].arg = arg;
    slots[irq].name = name;
    slots[irq].count = 0;
    return E_OK;
}

int irq_unregister(unsigned int irq)
{    if (irq >= IRQ_MAX) return E_INVAL;
    if (slots[irq].handler == 0) return E_NOENT;

    pic_set_mask(irq, true);
    slots[irq].handler = 0;
    slots[irq].arg = 0;
    slots[irq].name = 0;
    return E_OK;
}

unsigned int irq_count(unsigned int irq)
{
    return (irq < IRQ_MAX) ? slots[irq].count : 0U;
}

/* 把一条传统 IRQ 从 8259 迁到 IOAPIC：
 *   - IRQ -> GSI 走 MADT Interrupt Source Override（QEMU 的 PIT 是 IRQ0->GSI2）；
 *   - 触发方式/极性按 override flags 解释（0 = ISA 默认：边沿、高有效）；
 *   - 先屏蔽 PIC 线，IOAPIC 编程失败则取消屏蔽回滚到 PIC。 */
int irq_migrate_to_ioapic(unsigned int irq, uint32_t apic_id)
{
    uint8_t vector;
    uint32_t gsi;
    uint16_t flags;
    bool level;
    bool active_low;

    if (irq >= IRQ_MAX || !ioapic_present() || apic_base() == 0U) return E_INVAL;

    vector = (uint8_t)(32U + irq);
    gsi = madt_irq_gsi(irq);
    flags = madt_irq_flags(irq);

    /* ACPI flags: bits0..1 polarity (0=bus,1=high,3=low)，
     *             bits2..3 trigger  (0=bus,1=edge,3=level)。 */
    level      = (((flags >> 2) & 0x3U) == 0x3U);
    active_low = ((flags & 0x3U) == 0x3U);

    pic_set_mask(irq, true);                 /* 1. 关掉 PIC 这一路 */
    if (ioapic_route(gsi, apic_id, vector, false, level, active_low) != E_OK) {
        pic_set_mask(irq, false);            /* 回滚 */
        return E_INVAL;
    }

    ioapic_routed |= (1U << irq);
    if (level) ioapic_level |= (1U << irq);
    else       ioapic_level &= ~(1U << irq);
    kprintf("ioapic: irq%u -> gsi%u apic %u vector=0x%x %s%s(PIC masked, LAPIC EOI)\n",
            irq, (unsigned int)gsi, (unsigned int)apic_id, (unsigned int)vector,
            level ? "level " : "edge ", active_low ? "active-low " : "");
    return E_OK;
}

/* 把已注册处理函数的传统 IRQ 全部迁到 IOAPIC，目标默认 BSP。
 * 跳过 IRQ2：8259 级联线在 IOAPIC 模式下就是 GSI2（PIT 的 override 目标），
 * 不能当作“IRQ2 设备”路由。任何一条失败只影响该条（保留 PIC 回退），
 * 返回成功迁移的条数。 */
int irq_migrate_all_to_ioapic(void)
{
    uint32_t routed = 0;
    uint32_t needed = 0;
    uint32_t bsp = apic_id_from_cpuid();

    if (!ioapic_present() || apic_base() == 0U) {
        kprintln("ioapic: not available, legacy IRQs stay on PIC");
        return 0;
    }

    for (unsigned int irq = 0; irq < IRQ_MAX; irq++) {
        if (irq == 2U) continue;                 /* cascade: not a device GSI */
        if (slots[irq].handler == 0) continue;
        needed++;
        if ((ioapic_routed & (1U << irq)) != 0U) { routed++; continue; }

        if (irq_migrate_to_ioapic(irq, bsp) == E_OK) {
            routed++;
        } else {
            kprintf("ioapic: irq%u stays on PIC (fallback)\n", irq);
        }
    }

    /* 只有全部设备都走 IOAPIC 时才掩掉 PIC 级联线；否则保留从片回退路径。 */
    if (routed > 0U && routed == needed) pic_set_mask(2U, true);

    kprintf("ioapic: legacy IRQ routing done (%u irq(s) via IOAPIC, PIC fallback armed)\n",
            (unsigned int)routed);
    return (int)routed;
}

int irq_is_ioapic_routed(unsigned int irq)
{
    if (irq >= IRQ_MAX) return 0;
    return (ioapic_routed & (1U << irq)) != 0U;
}

const char *irq_name(unsigned int irq)
{
    if (irq >= IRQ_MAX) return "invalid";
    return (slots[irq].name != 0) ? slots[irq].name : "unused";
}

unsigned int irq_spurious_count(void)
{
    return spurious_count;
}

/* ==================== 键盘 ==================== */
/* set 1 扫描码 → ASCII（小写/无 Shift 状态） */
static const char kbd_map[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2',  '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

/* Shift 状态下的符号 */
static const char kbd_shift_map[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A', 'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2',  '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static bool kbd_shift;
static bool kbd_caps;
static bool kbd_ctrl;
static bool kbd_extended;

static void kbd_push(char c)
{
    unsigned int next = (kbd_head + 1U) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = (unsigned char)c;
        kbd_head = next;
    }
    /* 有键按下：唤醒正在等待键盘的线程 */
    proc_wakeup(PROC_WAIT_KBD);
}

/* 扫描码 → 字符；返回 0 表示这个键不产生字符 */
static char kbd_translate(unsigned char sc)
{
    /* 扩展键前缀 0xE0：方向键、Home 等，当前忽略但必须吃掉后续码 */
    if (sc == 0xE0U) {
        kbd_extended = true;
        return 0;
    }
    if (kbd_extended) {
        kbd_extended = false;
        return 0;
    }

    /* 修饰键（按下/松开） */
    if (sc == 0x2AU || sc == 0x36U) { kbd_shift = true;  return 0; }
    if (sc == 0xAAU || sc == 0xB6U) { kbd_shift = false; return 0; }
    if (sc == 0x1DU)                { kbd_ctrl = true;   return 0; }
    if (sc == 0x9DU)                { kbd_ctrl = false;  return 0; }
    if (sc == 0x3AU)                { kbd_caps = !kbd_caps; return 0; }

    if (sc & 0x80U) return 0;           /* 其它键的松开事件忽略 */

    /* Ctrl+C：产生 ETX，由键盘中断转成给前台进程的 SIGINT */
    if (kbd_ctrl && sc == 0x2EU) return 3;

    char c = kbd_map[sc];
    if (c == 0) return 0;

    /* 字母：Shift 与 CapsLock 异或决定大小写 */
    if (c >= 'a' && c <= 'z') {
        bool upper = kbd_shift != kbd_caps;
        return upper ? (char)(c - 'a' + 'A') : c;
    }

    /* 符号：Shift 用上档表 */
    if (kbd_shift) {
        char s = kbd_shift_map[sc];
        if (s != 0) return s;
    }
    return c;
}

/* ==================== 中断处理函数 ==================== */
/* PIT 频率自检状态（见 timer_handler 的 irq: pit rate 行）。 */
static uint32_t pit_rate_rtc0;
static bool     pit_rate_armed;

/* SMP AP tick 偏斜自检：AP 定时器在 tick 150 完成校准并把自己的 per-CPU
 * tick 对齐到 BSP（apic.c），距 2s 检查只剩 50 个 tick，校准误差被限制在
 * 很短的窗口里。比较各 CPU 绝对 tick，落在 BSP 的 ±10% 内即通过。 */
static void smp_tick_skew_check(void)
{
    uint32_t bsp;
    uint32_t max_pct = 0;

    if (smp_cpu_count <= 1U) return;

    bsp = per_cpu[0].ticks;
    if (bsp == 0U) return;

    for (uint32_t c = 1; c < SMP_MAX_CPUS; c++) {
        uint32_t ap, diff, pct;

        if (!per_cpu[c].online) continue;
        ap = per_cpu[c].ticks;
        diff = (ap > bsp) ? (ap - bsp) : (bsp - ap);
        pct = diff * 100U / bsp;
        if (pct > max_pct) max_pct = pct;
    }

    if (max_pct <= 10U) {
        kprintf("smp: ap tick skew ok (max_delta=%u%%, limit=10%%, window=2s)\n",
                (unsigned int)max_pct);
    } else {
        kprintf("smp: ap tick skew FAIL (max_delta=%u%%, bsp=%u ap=%u ap2=%u ap3=%u)\n",
                (unsigned int)max_pct, per_cpu[0].ticks, per_cpu[1].ticks,
                per_cpu[2].online ? per_cpu[2].ticks : 0U,
                per_cpu[3].online ? per_cpu[3].ticks : 0U);
    }
}

static void timer_handler(void *arg)
{
    (void)arg;
    ticks++;

    /* 每 CPU tick：BSP 走 PIT（本函数），AP 走 LAPIC timer（smp_timer_tick）。
     * 全局 ticks 继续供 uptime/soak 使用，语义不受影响。 */
    this_cpu()->ticks++;

    /* LAPIC/NMI 校准 + AP tick 对齐（apic.c）。放在 tick 自检之前：
     * tick 200 会先把 AP tick 对齐 BSP，再打印 tick check。 */
    apic_tick();

    /* 启动 2 秒后打印每颗 CPU 的 tick，用于验证所有 AP 定时器都已校准到
     * ~100Hz（-smp 4 时四颗核都要有数）。 */
    if (ticks == 200U && smp_cpu_count > 1U) {
        kprintf("smp: tick check after 2s bsp=%u ap=%u ap2=%u ap3=%u (online=%u)\n",
                per_cpu[0].ticks, per_cpu[1].ticks,
                per_cpu[2].online ? per_cpu[2].ticks : 0U,
                per_cpu[3].online ? per_cpu[3].ticks : 0U,
                (unsigned int)smp_cpu_count);
        smp_tick_skew_check();
    }

    /* 浸泡测试：每 60 秒往串口打一行运行状态（只走串口，不刷屏幕）。
     * 观察 free_pages 是否持续下降、procs 是否泄漏。 */
    if ((ticks % (TIMER_HZ * 60U)) == 0U) {
        unsigned int sec = ticks / TIMER_HZ;
        char line[128];

        /* 整行一次性走 console 锁，避免和 AP 上的输出交错。 */
        ksnprintf(line, sizeof(line),
                  "soak: uptime=%u s free_pages=%u procs=%u faults=%u\n",
                  sec, (unsigned int)pmm.free_pages, (unsigned int)proc_count(),
                  (unsigned int)pmm_faults);
        console_serial_line(line);
    }

    /* PIT 频率自检（v0.74 tick 2x 回归）：tick 200 时记下 RTC 秒，tick 1400
     * 再读一次。1200 tick 应约等于 12 个 RTC 墙钟秒；如果每秒来了两次中断，
     * 这里只有 ~6s，打印 FAIL 而不是让 uptime/心跳/超时全部跑快。
     * 容差放宽到 8..18s：TCG/CI 负载高时定时器投递会被延迟（实测 13.6s），
     * 但 2x 回归的 ~6s 仍然远离下界。 */
    if (ticks == 200U) {
        pit_rate_rtc0 = rtc_now();
        pit_rate_armed = true;
    } else if (ticks == 1400U && pit_rate_armed) {
        uint32_t sec = rtc_now() - pit_rate_rtc0;

        pit_rate_armed = false;
        if (sec >= 8U && sec <= 18U) {
            kprintf("irq: pit rate ok (1200 ticks / %u s)\n", (unsigned int)sec);
        } else {
            kprintf("irq: pit rate FAIL (1200 ticks / %u s, expect ~12)\n",
                    (unsigned int)sec);
        }
    }

    /* 唤醒到期的睡眠者 */
    proc_wake_sleepers(ticks);
}

static void keyboard_handler(void *arg)
{
    (void)arg;

    unsigned char sc = inb(0x60U);
    char c = kbd_translate(sc);

    /* Ctrl+C：不当作字符，转成给前台子进程的 SIGINT */
    if ((unsigned char)c == 3U) {
        proc_console_int();
        return;
    }

    if (c != 0) {
        kbd_push(c);
    }
}

/* COM1 接收：把串口输入也灌进键盘缓冲区，于是 shell 可以走串口交互。
 * 串口用 CR 表示回车、DEL 表示退格，这里统一成 '\n' 和 '\b'。 */
static void serial_rx_handler(void *arg)
{
    (void)arg;

    for (uint32_t i = 0; i < 16U; i++) {          /* 一次最多搬 16 字节，避免饿死别的中断 */
        char c;

        if (!serial_rx_ready()) break;
        c = serial_getc();
        if ((unsigned char)c == (unsigned char)'\r') c = '\n';
        if ((unsigned char)c == 0x7FU) c = '\b';
        if (c != 0) kbd_push(c);
    }
}

/* ==================== PS/2 鼠标（IRQ12） ==================== */
static int      mouse_x, mouse_y;
static uint8_t  mouse_cycle;
static uint8_t  mouse_packet[3];
static int      mouse_ready;

static void mouse_wait_write(void)
{
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((inb(0x64U) & 0x02U) == 0U) return;
    }
}

static void mouse_write(uint8_t v)
{
    mouse_wait_write();
    outb(0x64U, 0xD4U);         /* 下一条 0x60 数据发给辅助设备（鼠标） */
    mouse_wait_write();
    outb(0x60U, v);
}

int mouse_init(void)
{
    uint8_t status;

    /* 打开辅助设备（鼠标） */
    mouse_wait_write();
    outb(0x64U, 0xA8U);

    /* 读控制器配置字节，打开 IRQ12、清掉“禁用鼠标时钟”位 */
    mouse_wait_write();
    outb(0x64U, 0x20U);
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((inb(0x64U) & 0x01U) != 0U) break;
    }
    status = inb(0x60U);
    status = (uint8_t)((status | 0x02U) & ~0x20U);
    mouse_wait_write();
    outb(0x64U, 0x60U);
    mouse_wait_write();
    outb(0x60U, status);

    mouse_write(0xF6U);             /* 默认采样率/分辨率 */
    mouse_write(0xF4U);             /* 允许数据上报 */

    mouse_x = (int)fb_w / 2;
    mouse_y = (int)fb_h / 2;
    mouse_cycle = 0;
    mouse_ready = 1;

    kprintln("mouse: PS/2 mouse enabled (irq12)");
    return 0;
}

int mouse_present(void) { return mouse_ready; }

void mouse_get(int *x, int *y, int *buttons)
{
    if (x != 0) *x = mouse_x;
    if (y != 0) *y = mouse_y;
    if (buttons != 0) *buttons = mouse_packet[0] & 0x07U;
}

static void mouse_handler(void *arg)
{
    (void)arg;

    uint8_t data = inb(0x60U);

    switch (mouse_cycle) {
    case 0:
        if ((data & 0x08U) == 0U) return;       /* 第一位必须置位 */
        mouse_packet[0] = data;
        mouse_cycle = 1;
        break;
    case 1:
        mouse_packet[1] = data;
        mouse_cycle = 2;
        break;
    case 2: {
        int dx, dy;

        mouse_packet[2] = data;
        mouse_cycle = 0;

        dx = (int)mouse_packet[1];
        dy = (int)mouse_packet[2];
        if (mouse_packet[0] & 0x10U) dx -= 256;  /* X 符号位 */
        if (mouse_packet[0] & 0x20U) dy -= 256;  /* Y 符号位 */

        mouse_x += dx;
        mouse_y -= dy;                          /* 屏幕 y 向下 */

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int)fb_w) mouse_x = (int)fb_w - 1;
        if (mouse_y >= (int)fb_h) mouse_y = (int)fb_h - 1;

        if (dx != 0 || dy != 0 || (mouse_packet[0] & 0x07U) != 0U) {
            /* 坐标通过 SYS_MOUSE 暴露给 shell；不抢状态栏，避免和心跳闪烁 */
        }
        break;
    }
    default:
        mouse_cycle = 0;
        break;
    }
}

/* ==================== 初始化与分发 ==================== */
void interrupt_init(void)
{
    idt_init();
    uaccess_init();         /* 注册 #PF 恢复钩子（安全访问用户内存） */
    pic_remap();
    pit_init(TIMER_HZ);

    /* 处理函数由各模块注册，irq 层不再硬编码 */
    irq_register(0, timer_handler,   0, "timer");
    irq_register(1, keyboard_handler, 0, "keyboard");
    irq_register(4, serial_rx_handler, 0, "serial");
    irq_register(12, mouse_handler,   0, "mouse");

    irq_unmask(0);
    irq_unmask(1);
    irq_unmask(4);          /* COM1 接收中断 */
    irq_unmask(12);         /* PS/2 鼠标 */
    irq_unmask(2);          /* 级联：从片（IRQ8..15）必须放行 IRQ2 */
}

void irq_dispatch(interrupt_frame_t *f)
{
    unsigned int irq = f->vec - 32U;

    if (irq >= IRQ_MAX) {
        return;
    }

    /* 伪中断：8259 没有真正的中断请求，不能发 EOI（从片伪中断要给主片发） */
    if ((irq == 7U || irq == 15U) && irq_is_spurious(irq)) {
        spurious_count++;
        if (irq == 15U) {
            outb(PIC1_CMD, PIC_EOI);
        }
        return;
    }

    if (slots[irq].handler != 0) {
        slots[irq].count++;
        slots[irq].handler(slots[irq].arg);
    } else {
        /* 没有处理函数的 IRQ 直接屏蔽，避免反复触发把系统拖死 */
        pic_set_mask(irq, true);
    }

    /* EOI：从片上的中断要两个 PIC 都通知 */
    /* EOI：IOAPIC 投递的中断走 LAPIC EOI；电平触发还要给 IOAPIC 写 EOI，
     * 否则电平线保持有效会立刻重触发。其余走 8259（从片要两个都通知）。 */
    if ((ioapic_routed & (1U << irq)) != 0U) {
        if ((ioapic_level & (1U << irq)) != 0U) {
            (void)ioapic_eoi((uint8_t)f->vec);
        }
        lapic_send_eoi();
    } else {
        if (irq >= 8U) {
            outb(PIC2_CMD, PIC_EOI);
        }
        outb(PIC1_CMD, PIC_EOI);
    }

    /* 时钟中断在 EOI 之后做调度决策（可能切换栈，不再返回本函数） */
    if (irq == 0U) {
        sched_tick();
    }

    /* 返回用户态前处理待处理信号（可能终止或跳到用户 handler） */
    if ((f->cs & 3U) == 3U) {
        proc_check_signals(f);
    }
}

unsigned int timer_hz(void)
{
    return TIMER_HZ;
}

int keyboard_getchar(void)
{
    if (kbd_tail == kbd_head) return -1;

    int c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1U) % KBD_BUF_SIZE;
    return c;
}

/* PIT 通道 0：模式 3，频率 hz */
static void pit_init(unsigned int hz)
{
    unsigned int divisor = PIT_BASE / hz;

    if (divisor == 0U) divisor = 1U;
    if (divisor > 0xFFFFU) divisor = 0xFFFFU;

    /* 通道 0 用 mode 2（rate generator）：每个周期只输出一个脉冲/上升沿。
     *
     * 0.74 把 IRQ0 迁到 IOAPIC 后 guest tick 变成墙钟的 ~2 倍：旧代码用
     * mode 3（方波），OUT 每周期翻转两次；8259 的边沿检测只认一次上升沿，
     * 而 IOAPIC 的 edge 重定向表把两次电平变化都当成一次触发，于是 PIT
     * 100Hz → 200 次向量 0x20（QMP 实测 irq0 计数=ticks，LINT0 屏蔽无效，
     * 只有 IOAPIC 是唯一来源）。mode 2 在两种投递路径下都是 1 次/周期。
     * tick 频率自检见 timer_handler 的 "irq: pit rate ok" 行。 */
    outb(PIT_CMD, 0x34U);
    outb(PIT_CH0, (unsigned char)(divisor & 0xFFU));
    outb(PIT_CH0, (unsigned char)((divisor >> 8) & 0xFFU));
}
