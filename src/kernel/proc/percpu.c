/* =============================================================================
 * core/percpu.c - 每 CPU 状态骨架
 *
 * SMP 第一步：把“当前 CPU”抽象出来。现在只有 BSP 在线（cpu0），
 * 但所有新代码都应该通过 this_cpu() 拿状态，而不是全局变量，
 * 这样以后 AP 上线时不需要再改一遍调用点。
 * ============================================================================= */
#include "percpu.h"
#include "include/dev.h"
#include "include/kernel.h"

per_cpu_t per_cpu[SMP_MAX_CPUS];
uint32_t  smp_cpu_count;
uint32_t  smp_ap_ready;

/* 用 APIC ID 找当前 CPU 的逻辑编号；AP 登记后即可正确返回。 */
uint32_t smp_cpu_id(void)
{
    uint32_t id = apic_id_from_cpuid();

    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        if (per_cpu[i].online && per_cpu[i].apic_id == id) return i;
    }
    return 0U;
}

per_cpu_t *this_cpu(void)
{
    return &per_cpu[smp_cpu_id()];
}

void percpu_init_bsp(uint32_t apic_id, uint32_t discovered)
{
    for (uint32_t i = 0; i < SMP_MAX_CPUS; i++) {
        per_cpu[i].id = i;
        per_cpu[i].apic_id = 0;
        per_cpu[i].online = false;
        per_cpu[i].ticks = 0;
        per_cpu[i].ipi = 0;
        per_cpu[i].tlb_seen = 0;
        per_cpu[i].runq_mask = 0;
        per_cpu[i].preempt_off = 0;
        per_cpu[i].ready = false;
        per_cpu[i].current = 0;
        per_cpu[i].bootstrap_esp = 0;
        per_cpu[i].idle_active = false;
    }

    if (discovered == 0U) discovered = 1U;
    if (discovered > SMP_MAX_CPUS) discovered = SMP_MAX_CPUS;

    per_cpu[0].apic_id = apic_id;
    per_cpu[0].online = true;
    smp_cpu_count = 1;

    kprintf("smp: BSP online (apic id=%u), %u cpu(s) discovered, AP startup pending\n",
            (unsigned int)apic_id, (unsigned int)discovered);
}

void preempt_disable(void)
{
    per_cpu_t *c = this_cpu();

    if (c->preempt_off < 0xFFFFFFFFU) c->preempt_off++;
    __asm__ volatile("" ::: "memory");
}

void preempt_enable(void)
{
    per_cpu_t *c = this_cpu();

    if (c->preempt_off > 0U) c->preempt_off--;
    __asm__ volatile("" ::: "memory");
}
