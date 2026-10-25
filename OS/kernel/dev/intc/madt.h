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

#endif
