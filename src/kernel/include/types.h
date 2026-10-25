/* =============================================================================
 * include/types.h - 基础类型（接口层）
 *
 * 定宽整数由内核自带的 <stdint.h> 提供（src/kernel/stdint.h，经 -Isrc/kernel
 * 覆盖工具链版本，保证 uint32_t == unsigned int == 指针宽度）。
 * 本文件补充内核特有的类型与宏转发。
 * ============================================================================= */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include "stdint.h"

/* size_t/ssize_t：32 位平坦模型下与 unsigned int 同宽同类型 */
typedef unsigned int        size_t;
typedef signed int          ssize_t;

/* 语义化地址类型：32 位平坦模型下都是 32 位 */
typedef uint32_t            phys_addr_t;
typedef uint32_t            virt_addr_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "macros.h"

#endif /* TYPES_H */
