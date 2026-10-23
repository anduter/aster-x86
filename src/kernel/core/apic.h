#ifndef CORE_APIC_H
#define CORE_APIC_H

#include "../types.h"

/* 探测本地 APIC（CPUID.1:EDX[9]）并读取 IA32_APIC_BASE（MSR 0x1B）。
 * 只做探测和日志：中断仍然走 8259 PIC，不切换 APIC 模式。 */
int apic_init(void);
/* 映射 LAPIC 页并打开 APIC（AP 启动前调用；不启用定时器/看门狗）。 */
int apic_prepare(void);
/* AP 专用：打开自己的 LAPIC、屏蔽 LINT0/1，避免 PIC 中断打到 AP。 */
int apic_prepare_ap(void);
/* AP 周期 timer；vector 由 IDT 安装，count 为初始计数。 */
int apic_start_timer_ap(uint32_t vector, uint32_t count);
/* 写 LAPIC EOI。 */
void apic_eoi(void);

uint32_t apic_base(void);      /* LAPIC MMIO 物理地址，0 表示没有 */
int      apic_present(void);

/* CPUID.1:EBX[31:24] 里的初始 APIC ID（不需要映射 LAPIC 页） */
uint32_t apic_id_from_cpuid(void);

/* 读取 64 位 MSR 的低 32 位 */
uint32_t rdmsr_low(uint32_t msr);
void     wrmsr_low(uint32_t msr, uint32_t value);

/* NMI 看门狗：映射 LAPIC、打开 APIC、校准 LAPIC 定时器并用 NMI 投递 */
int  apic_watchdog_start(void);
void apic_tick(void);        /* 由 PIT 中断每 tick 调用（校准阶段） */

/* 给目标 APIC 发一条 IPI（low = ICR 低 32 位，如 0x4500=INIT、0x4600|vec=SIPI） */
int  apic_send_ipi(uint32_t dest_apic_id, uint32_t low);
uint32_t apic_id(void);      /* 当前 CPU 的 LAPIC ID */

#endif
