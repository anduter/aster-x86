/*
 * ng WP1-7: freestanding 内存原语。
 *
 * 内核用 -ffreestanding -nostdlib 编译，编译器在结构体赋值/大块拷贝时会自己
 * 发 memcpy/memset 调用（WP1-7 的 ELF 段规划结构体就是这么触发的）。这里给出
 * 最小实现，避免为了一个结构体拷贝去链接 libc。
 *
 * 只在内核构建里编译（host 单测用宿主 libc 的同名函数）。
 */
#include "types.h"

void *memcpy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, u64 n)
{
    u8 *d = (u8 *)dst;
    u64 i;

    for (i = 0; i < n; i++) {
        d[i] = (u8)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        u64 i;

        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        while (n > 0) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}
