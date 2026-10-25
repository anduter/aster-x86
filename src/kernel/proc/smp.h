#ifndef CORE_SMP_H
#define CORE_SMP_H

#include "../include/types.h"

/* 启动 MADT 中列出的 AP：进入 32 位保护模式、登记 per-CPU、
 * 装好自己的 GDT/TSS 和共享 IDT，然后进入可被 timer/IPI 唤醒的 idle。 */
void smp_start_aps(void);

/* AP 入口内部使用：把当前 AP 登记到 per_cpu[]，返回逻辑 CPU 槽。 */
uint32_t smp_register_ap(uint32_t apic_id);
/* AP LAPIC timer 向量处理：per-CPU tick + EOI，再尝试从本 CPU runqueue 取活。 */
void smp_timer_tick(void);
/* AP IPI 向量处理：per-CPU IPI 计数 + TLB 刷新 + EOI，并唤醒本 CPU 调度器。 */
void smp_ipi_tick(void);

/* BSP 校准 LAPIC 后调用：把 10ms 计数交给 AP，下次 AP timer 中断时重装 */
void smp_ap_timer_reload(uint32_t count);
/* 向所有在线 AP 广播 TLB shootdown，并等待 ack。返回 ack 的 CPU 数。 */
uint32_t smp_tlb_shootdown(void);

#endif
