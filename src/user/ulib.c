/* =============================================================================
 * ulib.c - 用户态最小 C 库
 *
 * 提供 printf / puts / malloc / 字符串函数，迁移期建立在 int 0x81 之上：
 *   - 输出走 SYS_WRITE（fd=1）；
 *   - malloc 走 SYS_BRK 扩展用户堆（0x40080000 起，向上增长）。
 * ============================================================================= */
#include "ulib.h"

int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static usize utoa(unsigned int v, char *buf, unsigned int base)
{
    char tmp[16];
    usize n = 0, i = 0;

    if (v == 0U) {
        buf[0] = '0';
        return 1;
    }
    while (v > 0U) {
        unsigned int d = v % base;
        tmp[n++] = (d < 10U) ? (char)('0' + d) : (char)('a' + d - 10U);
        v /= base;
    }
    while (n > 0U) buf[i++] = tmp[--n];
    return i;
}

int fd_read(int fd, void *buf, int len)
{
    return syscall3(SYS_FREAD, fd, (int)buf, len);
}

int fd_write(int fd, const void *buf, int len)
{
    return syscall3(SYS_FWRITE, fd, (int)buf, len);
}

int putchar(int c)
{
    char ch = (char)c;
    fd_write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    usize n = strlen(s);
    fd_write(1, s, (int)n);
    putchar('\n');
    return (int)n + 1;
}

/* 把 tmp 里的字符串按 width/zero 填充后追加到 out */
static int append_padded(char *out, int n, const char *tmp, int len,
                         int width, int zero)
{
    int pad = (width > len) ? (width - len) : 0;

    for (int i = 0; i < pad && n < 240; i++) out[n++] = zero ? '0' : ' ';
    for (int i = 0; i < len && n < 240; i++) out[n++] = tmp[i];
    return n;
}

int printf(const char *fmt, ...)
{
    char out[256];
    int n = 0;
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);

    for (; *fmt != '\0' && n < 240; fmt++) {
        int zero = 0, width = 0;
        char tmp[40];
        int len;

        if (*fmt != '%') {
            out[n++] = *fmt;
            continue;
        }
        fmt++;

        /* 解析 "%0Nd" 里的 '0' 标志和宽度 */
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            len = 0;
            while (s[len] != '\0' && len < (int)sizeof(tmp)) len++;
            for (int i = 0; i < len && n < 240; i++) out[n++] = s[i];
            break;
        }
        case 'd': {
            int v = __builtin_va_arg(ap, int);
            unsigned int uv;
            int neg = 0;

            if (v < 0) { neg = 1; uv = (unsigned int)(-v); } else { uv = (unsigned int)v; }
            len = (int)utoa(uv, tmp, 10U);
            if (neg) {
                if (len + 1 < (int)sizeof(tmp)) {
                    for (int i = len; i > 0; i--) tmp[i] = tmp[i - 1];
                    tmp[0] = '-';
                    len++;
                }
            }
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'u': {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            len = (int)utoa(v, tmp, 10U);
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'x': {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            len = (int)utoa(v, tmp, 16U);
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'c': {
            tmp[0] = (char)__builtin_va_arg(ap, int);
            n = append_padded(out, n, tmp, 1, width, zero);
            break;
        }
        case 'p': {
            unsigned int v = (unsigned int)(usize)__builtin_va_arg(ap, void *);
            out[n++] = '0';
            out[n++] = 'x';
            len = (int)utoa(v, tmp, 16U);
            for (int i = 0; i < len && n < 240; i++) out[n++] = tmp[i];
            break;
        }
        default:
            out[n++] = '%';
            if (*fmt != '\0') out[n++] = *fmt;
            break;
        }
    }

    __builtin_va_end(ap);

    syscall3(SYS_FWRITE, 1, (int)out, n);
    return n;
}

/* ---- 堆：向上增长的 bump 分配器，free 是空操作 ---- */
static char *heap_ptr;
static char *heap_end;

void *malloc(usize n)
{
    char *p;
    int brk;

    n = (n + 7U) & ~7U;
    if (n == 0U) n = 8U;

    if (heap_ptr == 0) {
        brk = syscall3(SYS_BRK, 0, 0, 0);
        if (brk < 0) return 0;
        heap_ptr = (char *)(usize)brk;
        heap_end = heap_ptr;
    }

    if (heap_ptr + n > heap_end) {
        brk = syscall3(SYS_BRK, (int)n, 0, 0);
        if (brk < 0) return 0;
        heap_end = (char *)(usize)brk;
    }

    p = heap_ptr;
    heap_ptr += n;
    return p;
}

void free(void *p)
{
    (void)p;            /* bump 分配器不回收 */
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
    char *p = d;
    while ((*d++ = *s++) != '\0') { }
    return p;
}

void *memcpy(void *d, const void *s, usize n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n-- > 0U) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, usize n)
{
    unsigned char *dp = (unsigned char *)d;
    while (n-- > 0U) *dp++ = (unsigned char)c;
    return d;
}
