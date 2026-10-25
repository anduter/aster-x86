#ifndef IO_H
#define IO_H

/* 端口 I/O 指令封装 */
static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0,%1" :: "a"(val), "d"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char ret;
    __asm__ volatile("inb %1,%0" : "=a"(ret) : "d"(port));
    return ret;
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short ret;
    __asm__ volatile("inw %1,%0" : "=a"(ret) : "d"(port));
    return ret;
}

static inline void outw(unsigned short port, unsigned short val)
{
    __asm__ volatile("outw %0,%1" :: "a"(val), "d"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int ret;
    __asm__ volatile("inl %1,%0" : "=a"(ret) : "d"(port));
    return ret;
}

static inline void outl(unsigned short port, unsigned int val)
{
    __asm__ volatile("outl %0,%1" :: "a"(val), "d"(port));
}

/* COM1 串口输出（调试日志通道） */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

/* COM1 串口输入（IRQ4，字符会灌进键盘缓冲区） */
int  serial_rx_ready(void);
char serial_getc(void);

/* 串口输出 32 位十六进制数，固定 8 位，带 0x 前缀 */
void serial_puthex32(unsigned int v);
/* 串口输出无符号十进制数 */
void serial_putdec(unsigned int v);

#endif
