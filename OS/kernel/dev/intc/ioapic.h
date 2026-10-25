#ifndef CORE_IOAPIC_H
#define CORE_IOAPIC_H

#include "../../include/types.h"

/* IOAPIC：把 GSI 路由到某个 CPU 的 LAPIC 向量。
 * 目前只做初始化、寄存器读写和“掩码状态下的路由自检”，
 * 传统 IRQ 仍然走 8259 PIC（渐进迁移）。 */
int  ioapic_init(void);
int  ioapic_present(void);
uint32_t ioapic_gsi_count(void);

/* 路由一条 GSI：apic_id 为目标 CPU，vector 为 IDT 向量，level=true 表示电平触发 */
int  ioapic_route(uint32_t gsi, uint32_t apic_id, uint8_t vector,
                  bool masked, bool level);
int  ioapic_mask(uint32_t gsi, bool masked);
uint32_t ioapic_read_entry_low(uint32_t gsi);

#endif
