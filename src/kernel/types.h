/* =============================================================================
 * types.h - 全局基础类型与工具宏
 *
 * 全项目唯一的类型来源。各 .c 不再各自 typedef，改为一律 #include "types.h"。
 *
 * 注意：这里不使用 <stdint.h>。在 i686-elf 上 uint32_t 会被定义成
 * "unsigned long"，而内核现有接口大量使用 "unsigned int"，两者虽然都是
 * 32 位，但类型不同会触发函数原型冲突。统一用下面这套定义可以保证
 * uint32_t == unsigned int == 指针宽度，整个 32 位平坦模型下没有歧义。
 * ============================================================================= */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>

typedef unsigned char       uint8_t;
typedef signed char         int8_t;
typedef unsigned short      uint16_t;
typedef signed short        int16_t;
typedef unsigned int        uint32_t;
typedef signed int          int32_t;
typedef unsigned long long  uint64_t;
typedef signed long long    int64_t;

typedef unsigned int        size_t;
typedef signed int          ssize_t;
typedef unsigned int        uintptr_t;
typedef signed int          intptr_t;

/* 语义化地址类型：32 位平坦模型下都是 32 位 */
typedef uint32_t            phys_addr_t;
typedef uint32_t            virt_addr_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 常用常量 */
#define KiB(x)         ((x) * 1024U)
#define MiB(x)         ((x) * 1024U * 1024U)
#define GiB(x)         ((x) * 1024U * 1024U * 1024U)

/* 数组元素个数 */
#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* 对齐：a 必须是 2 的幂 */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1U)) & ~((a) - 1U))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1U))

/* 最小/最大 */
#define MIN(a, b)      (((a) < (b)) ? (a) : (b))
#define MAX(a, b)      (((a) > (b)) ? (a) : (b))

/* 未使用参数（-Wextra 下抑制告警） */
#define UNUSED(x)      ((void)(x))

#endif /* TYPES_H */
