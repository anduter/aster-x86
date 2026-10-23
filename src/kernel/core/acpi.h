#ifndef CORE_ACPI_H
#define CORE_ACPI_H

#include "../types.h"

/* 扫描 ACPI 表，找到 FADT 的 PM1a 控制寄存器；成功返回 1 */
int acpi_init(void);

/* 关机（S5）：写 PM1a_CNT 的 SLP_TYP|SLP_EN，成功不返回 */
void acpi_poweroff(void) __attribute__((noreturn));

/* 是否找到了 ACPI 关机寄存器 */
int acpi_available(void);

/* 按签名查找 ACPI 表（如 "APIC"），找不到返回 0 */
const unsigned char *acpi_find_table(const char *sig);

#endif
