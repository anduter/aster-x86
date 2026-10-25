/* =============================================================================
 * lib/string.c - 内核最小 C 字符串/内存函数实现
 * ============================================================================= */
#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- > 0) {
        if (*a != *b) {
            return (int)(unsigned char)*a - (int)(unsigned char)*b;
        }
        if (*a == '\0') {
            return 0;
        }
        a++;
        b++;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') {
        /* copy */
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    while (*s != '\0') {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    return (ch == '\0') ? (char *)s : NULL;
}

size_t utoa(uint32_t v, char *buf, unsigned base)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[33];
    size_t n = 0;
    size_t i;

    if (base < 2U || base > 16U) {
        buf[0] = '\0';
        return 0;
    }
    if (v == 0U) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (v > 0U) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1U - i];
    }
    buf[n] = '\0';
    return n;
}

size_t itoa(int32_t v, char *buf)
{
    if (v < 0) {
        buf[0] = '-';
        /* 注意：-2147483648 取负会溢出，按无符号处理 */
        return 1U + utoa((uint32_t)(-(int64_t)v), buf + 1, 10U);
    }
    return utoa((uint32_t)v, buf, 10U);
}
