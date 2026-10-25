#include "io.h"

/* COM1（0x3F8）初始化：38400 波特率，8 位数据，无校验，1 位停止位 */
void serial_init(void)
{
    outb(0x3F9U, 0U);          /* 关闭串口中断（等接收处理函数注册后再开） */
    outb(0x3FBU, 0x80U);       /* DLAB=1，允许写波特率分频值 */
    outb(0x3F8U, 0x03U);       /* 分频值 3 -> 38400 波特 */
    outb(0x3F9U, 0x00U);
    outb(0x3FBU, 0x03U);       /* 8N1 */

    /* 清掉上电时残留的接收数据，避免第一下按键是乱码 */
    while ((inb(0x3FDU) & 0x01U) != 0U) (void)inb(0x3F8U);
    outb(0x3F9U, 0x01U);       /* IER: 允许“接收数据可用”中断（IRQ4） */
}

/* 串口接收：有字符可读返回 1 */
int serial_rx_ready(void)
{
    return (inb(0x3FDU) & 0x01U) != 0;
}

/* 串口接收：读一个字符（调用前应先确认 serial_rx_ready） */
char serial_getc(void)
{
    return (char)inb(0x3F8U);
}

void serial_putc(char c)
{
    /* 直接写发送寄存器：QEMU 的 FIFO 会缓冲字符，
       不必轮询 LSR 的 bit5（LSR 读取异常时轮询会卡死） */
    outb(0x3F8U, (unsigned char)c);
}

void serial_puts(const char *s)
{
    while (*s != '\0')
        serial_putc(*s++);
}

/* 输出 32 位十六进制数，固定 8 位，带 0x 前缀 */
void serial_puthex32(unsigned int v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_putc('0');
    serial_putc('x');
    for (int i = 7; i >= 0; i--)
        serial_putc(hex[(v >> (i * 4)) & 0xFU]);
}

/* 输出无符号十进制数 */
void serial_putdec(unsigned int v)
{
    char buf[12];
    int  n = 0;

    if (v == 0U) {
        serial_putc('0');
        return;
    }
    while (v > 0U) {
        buf[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0) {
        serial_putc(buf[--n]);
    }
}
