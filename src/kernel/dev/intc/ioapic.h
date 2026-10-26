#ifndef CORE_IOAPIC_H
#define CORE_IOAPIC_H

#include "../../include/types.h"

/* IOAPIC：把 GSI 路由到某个 CPU 的 LAPIC 向量。
 * 初始化时做“掩码状态下的路由自检”；传统 IRQ 由 irq.c 逐条迁移，
 * 失败的单条保留 8259 PIC 回退。 */
int  ioapic_init(void);
int  ioapic_present(void);
uint32_t ioapic_gsi_count(void);

/* 路由一条 GSI：apic_id 为目标 CPU，vector 为 IDT 向量，
 * level=true 表示电平触发，active_low=true 表示低电平有效。 */
int  ioapic_route(uint32_t gsi, uint32_t apic_id, uint8_t vector,
                  bool masked, bool level, bool active_low);
int  ioapic_mask(uint32_t gsi, bool masked);
uint32_t ioapic_read_entry_low(uint32_t gsi);
/* 电平触发中断在 LAPIC EOI 之后还要给 IOAPIC 写 EOI 寄存器（0x40）。 */
int  ioapic_eoi(uint8_t vector);

#endif
