/* =============================================================================
 * verify_support.c - 启动签名验证器的最小运行时
 *
 * 不依赖内核 heap/kprintf；失败时只向 COM1 输出一行并停机。
 * 这里只用 unsigned int / unsigned char，避免 stdint 与内核 types.h
 * 对 uint32_t 的 typedef 冲突。
 * ============================================================================= */
#include "verify_support.h"

void *memcpy(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n-- != 0U) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n-- != 0U) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n-- != 0U) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

static void serial_putc(char c)
{
    unsigned char status;
    unsigned short port;

    port = 0x3FDU;
    do {
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"(port));
    } while ((status & 0x20U) == 0U);

    port = 0x3F8U;
    __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)c), "Nd"(port));
}

void boot_verify_ok(void)
{
    const char *s = "boot: KERNEL SIGNATURE OK\n";

    while (*s != '\0') serial_putc(*s++);
}

void boot_verify_fail(void)
{
    const char *s = "boot: KERNEL SIGNATURE INVALID\n";

    while (*s != '\0') serial_putc(*s++);
    for (;;) __asm__ volatile ("cli; hlt");
}
