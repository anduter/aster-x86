/* =============================================================================
 * lib/string.h - 内核最小 C 字符串/内存函数
 *
 * gcc 在 freestanding 下仍可能为结构体拷贝生成 memcpy/memset 调用，
 * 所以这几个符号必须由内核自己提供。
 * ============================================================================= */
#ifndef LIB_STRING_H
#define LIB_STRING_H

#include "../include/types.h"

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
/* 在 s 中查找 c，返回指针或 NULL */
char  *strchr(const char *s, int c);
/* 把无符号数按 base 转成字符串（含 NUL），返回长度 */
size_t utoa(uint32_t v, char *buf, unsigned base);
/* 有符号十进制 */
size_t itoa(int32_t v, char *buf);

#endif /* LIB_STRING_H */
