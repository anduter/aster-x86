/*
 * ng 基础类型（WP1-2）。
 *
 * 新内核不链接 libc（-ffreestanding -nostdlib），所以自己给一套定宽类型；
 * 只用在这些头里，不引入 stdint.h，避免依赖宿主/交叉编译器的 libc 头。
 */
#ifndef NG_TYPES_H
#define NG_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

/* 64 位下指针宽度 == 整数宽度；别名便于 (usize)ptr 转型。 */
typedef u64 usize;
typedef i64 isize;

#define NULL ((void *)0)

#endif /* NG_TYPES_H */
