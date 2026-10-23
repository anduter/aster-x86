#ifndef CORE_SMP_H
#define CORE_SMP_H

#include "../types.h"

/* 启动 MADT 中列出的 AP：进入 32 位保护模式、登记 per-CPU、idle。
 * 不启动 SMP 调度；当前仅 BSP 调度进程。 */
void smp_start_aps(void);

/* AP 入口内部使用：把当前 AP 登记到 per_cpu[]，返回逻辑 CPU 槽。 */
uint32_t smp_register_ap(uint32_t apic_id);
/* AP LAPIC timer 向量处理：per-CPU tick + EOI。 */
void smp_timer_tick(void);
/* AP IPI 向量处理：per-CPU IPI 计数 + EOI。 */
void smp_ipi_tick(void);
/* 向所有在线 AP 广播 TLB shootdown，并等待 ack。返回 ack 的 CPU 数。 */
uint32_t smp_tlb_shootdown(void);

#endif
