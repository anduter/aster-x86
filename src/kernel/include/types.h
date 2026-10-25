/* =============================================================================
 * include/types.h - 基础类型（接口层）
 *
 * 全项目唯一的类型定义来源。各模块统一 #include "types.h"（根目录 types.h
 * 转发到本文件），不再各自 typedef。
 *
 * 不使用 <stdint.h>：i686-elf 上 uint32_t 会被定义成 "unsigned long"，而内核
 * 接口大量使用 "unsigned int"；两者都是 32 位但类型不同，会触发函数原型冲突。
 * 这里统一成 uint32_t == unsigned int == 指针宽度。
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

#endif /* TYPES_H */
