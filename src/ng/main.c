/*
 * ng WP1-1: 64 位 C 入口。
 *
 * 到这里的先决条件（entry.S 已完成）：分页开启、处于 64 位代码段、
 * 前 1 GiB 恒等映射、栈已就绪。
 *
 * 本 WP 只做一件事：COM1 打印 "ng: hello 64-bit"，然后 hlt 停住。
 * 串口是 QEMU 无显示环境下的唯一证据通道，所以先于一切初始化。
 */

typedef unsigned char u8;
typedef unsigned short u16;

#define COM1 0x3F8 /* COM1 端口基址：0x3F8..0x3FF */

/* 写 8 位端口。%0/%1 用寄存器约束即可，不涉及内存。 */
static inline void outb(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* 读 8 位端口。 */
static inline u8 inb(u16 port)
{
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 115200 8N1，FIFO 打开。UART 寄存器布局是 16550 的经典布局。 */
static void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* 关中断（WP1-1 不处理 IRQ） */
    outb(COM1 + 3, 0x80); /* DLAB=1，下面写波特率除数 */
    outb(COM1 + 0, 0x01); /* divisor = 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* DLAB=0，8 位数据、无校验、1 停止位 */
    outb(COM1 + 2, 0xC7); /* FIFO 使能、清空、14 字节阈值 */
    outb(COM1 + 4, 0x0B); /* DTR + RTS + OUT2 */
}

/* THR 空（LSR bit 5）才写下一个字节，避免丢字符。 */
static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (u8)c);
}

static void serial_puts(const char *s)
{
    while (*s != '\0') {
        serial_putc(*s++);
    }
}

/* 由 entry.S 在 64 位模式下调用；mb_info 暂不使用（保留给 WP1-2 的内存信息）。 */
void kmain(unsigned long mb_info)
{
    (void)mb_info;

    serial_init();
    serial_puts("ng: hello 64-bit\r\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
