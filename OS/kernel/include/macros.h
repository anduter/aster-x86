/* =============================================================================
 * include/macros.h - 通用工具宏（接口层）
 *
 * 容量、数组长度、对齐、最值、未使用参数。纯宏、无类型依赖，所有模块可用。
 * 由 include/types.h 包含；各模块通过 #include "types.h"（接口层）使用。
 * ============================================================================= */
#ifndef MACROS_H
#define MACROS_H

/* 常用容量单位 */
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

#endif /* MACROS_H */
