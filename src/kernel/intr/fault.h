#ifndef INTR_FAULT_H
#define INTR_FAULT_H

#include "../types.h"

/* 通用异常修复表：登记一段 [start,end) 指令范围，缺页时把 eip 改到 fixup。
 * 和 uaccess 的“跳过 rep movsb”相比，这是 Linux 风格的 fixup 表，
 * 任何指令序列都可以登记。 */
int      fault_register(uint32_t start, uint32_t end, uint32_t fixup);
uint32_t fault_lookup(uint32_t eip);

#endif
