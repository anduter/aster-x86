#ifndef CORE_MADT_H
#define CORE_MADT_H

#include "../../include/types.h"

/* ACPI MADT（"APIC" 表）：CPU 列表与 IOAPIC 信息 */
int      madt_init(void);
uint32_t madt_cpu_count(void);
uint32_t madt_cpu_apic_id(uint32_t index);
int      madt_cpu_enabled(uint32_t index);
uint32_t madt_ioapic_base(void);
uint32_t madt_ioapic_gsi_base(void);

/* Interrupt Source Override（MADT type 2）：ISA IRQ -> GSI 的映射与触发
 * 属性。没有 override 时返回恒等映射（GSI == IRQ，flags == 0）。 */
uint32_t madt_irq_gsi(uint32_t irq);
uint16_t madt_irq_flags(uint32_t irq);
uint32_t madt_irq_override_count(void);

#endif
