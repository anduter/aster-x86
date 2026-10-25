/* =============================================================================
 * stdint.h - 内核 ABI 的 <stdint.h>（替代工具链版本）
 *
 * 为什么需要它：i686-elf 工具链的 <stdint.h> 把 uint32_t 定义成
 * "unsigned long"，而内核接口大量使用 "unsigned int"；两者都是 32 位、ABI
 * 相同，但类型不同会触发函数原型冲突与可变参数 UB。
 *
 * 本文件位于 src/kernel/，构建使用 -Isrc/kernel（Makefile 为 -I$(KERNEL_DIR)），
 * 因此 <> 形式的 #include <stdint.h> 会优先找到这里，得到与内核一致的 ABI：
 *   uint32_t == unsigned int，int32_t == signed int，指针宽度 == 32 位。
 *
 * 注意：这是内核头，不是 libc 头；用户态程序不要包含它（用户态用 abi.h）。
 * ============================================================================= */
#ifndef KERNEL_STDINT_H
#define KERNEL_STDINT_H

typedef unsigned char       uint8_t;
typedef signed char         int8_t;
typedef unsigned short      uint16_t;
typedef signed short        int16_t;
typedef unsigned int        uint32_t;
typedef signed int          int32_t;
typedef unsigned long long  uint64_t;
typedef signed long long    int64_t;

typedef unsigned int        uintptr_t;
typedef signed int          intptr_t;

typedef unsigned long long  uintmax_t;
typedef signed long long    intmax_t;

/* least / fast 变体：32 位平坦模型下与定宽类型相同 */
typedef uint8_t   uint_least8_t;
typedef int8_t    int_least8_t;
typedef uint16_t  uint_least16_t;
typedef int16_t   int_least16_t;
typedef uint32_t  uint_least32_t;
typedef int32_t   int_least32_t;
typedef uint64_t  uint_least64_t;
typedef int64_t   int_least64_t;

typedef uint8_t   uint_fast8_t;
typedef int8_t    int_fast8_t;
typedef uint16_t  uint_fast16_t;
typedef int16_t   int_fast16_t;
typedef uint32_t  uint_fast32_t;
typedef int32_t   int_fast32_t;
typedef uint64_t  uint_fast64_t;
typedef int64_t   int_fast64_t;

/* 极限值 */
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255U
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535U
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define SIZE_MAX    UINT32_MAX

#define INT8_C(x)   (x)
#define INT16_C(x)  (x)
#define INT32_C(x)  (x)
#define INT64_C(x)  (x##LL)
#define UINT8_C(x)  (x)
#define UINT16_C(x) (x)
#define UINT32_C(x) (x##U)
#define UINT64_C(x) (x##ULL)

#endif /* KERNEL_STDINT_H */
