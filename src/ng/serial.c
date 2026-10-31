/*
 * ng WP1-2: COM1 16550 串口。
 *
 * 从 WP1-1 的 main.c 拆出来：页表初始化要打印大量证据行，串口不能再和 kmain
 * 挤在一个文件里。初始化/收发逻辑与 WP1-1 完全一致（115200 8N1、FIFO、轮询 THR）。
 */
#include "serial.h"

#include "io.h"

#define COM1 0x3F8 /* COM1 端口基址：0x3F8..0x3FF */

/* 115200 8N1，FIFO 打开。UART 寄存器布局是 16550 的经典布局。 */
void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* 关中断（内核还不处理 IRQ） */
    outb(COM1 + 3, 0x80); /* DLAB=1，下面写波特率除数 */
    outb(COM1 + 0, 0x01); /* divisor = 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* DLAB=0，8 位数据、无校验、1 停止位 */
    outb(COM1 + 2, 0xC7); /* FIFO 使能、清空、14 字节阈值 */
    outb(COM1 + 4, 0x0B); /* DTR + RTS + OUT2 */
}

/* THR 空（LSR bit 5）才写下一个字节，避免丢字符。 */
void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (u8)c);
}

void serial_puts(const char *s)
{
    while (*s != '\0') {
        serial_putc(*s++);
    }
}

/* 0x 前缀 + 最少位数；0 打印成 "0x0"。 */
void serial_puthex(u64 value)
{
    static const char digits[] = "0123456789abcdef";
    int shift = 60;

    serial_puts("0x");
    while (shift > 0 && ((value >> shift) & 0xF) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> shift) & 0xF]);
    }
}

void serial_putdec(u64 value)
{
    char buf[21];
    int i = 0;

    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}
