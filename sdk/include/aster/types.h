/*
 * Aster SDK alpha —— 基础类型（ABI 1.0 未冻结）
 *
 * 设计约束（与 ng 内核一致）：
 *   - 不依赖 libc，所以不 include <stdint.h>/<stddef.h>/<stdbool.h>；定宽类型
 *     在这里自己给。size_t/ssize_t 用编译器的 __SIZE_TYPE__/__PTRDIFF_TYPE__，
 *     这样 32/64 位宿主都不会和将来链接的 C 运行库打架。
 *   - SDK alpha 只支持 x86_64 SysV 调用约定（syscall 内联汇编见 syscall.h）。
 *   - 这是 **alpha**：头文件里的常量、结构布局和调用号都可能在 ABI 1.0 冻结
 *     （WP4-1）前调整；升级内核后请重跑 tools/sdk-check.ps1 并读 CHANGELOG。
 */
#ifndef ASTER_TYPES_H
#define ASTER_TYPES_H

/* ---- 定宽整数 ------------------------------------------------------------ */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

/* 指针宽度别名：写 (usize)ptr 比 (u64)(unsigned long)ptr 干净。 */
typedef u64 usize;
typedef i64 isize;

/* ---- C 标准类型（freestanding 时仍然需要） ------------------------------ */
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;

#if defined(__cplusplus)
/* C++ 里 bool/true/false 是关键字。 */
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
/* C23：bool/true/false 是关键字，不能再 typedef/define。 */
#else
typedef _Bool bool;
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- SDK 版本 ------------------------------------------------------------ */
/*
 * SDK 版本与内核 ABI 版本是两件事：
 *   - ASTER_SDK_VERSION 是这套头文件/示例的版本；
 *   - ASTER_ABI_VERSION 是 syscall 调用号+结构布局的版本，目前仍是 1.0 草案，
 *     **未冻结**。alpha 阶段不保证二进制兼容。
 */
#define ASTER_SDK_VERSION "0.1.0-alpha"
#define ASTER_ABI_VERSION 0x00010000u

#endif /* ASTER_TYPES_H */
