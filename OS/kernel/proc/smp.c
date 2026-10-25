/* =============================================================================
 * core/smp.c - AP 启动（最小多核 bring-up）
 *
 * 当前只把 AP 拉进 32 位保护模式、登记 per-CPU 状态，然后进入可被
 * timer/IPI 唤醒的 idle；AP 上线后由 sched.c 按每 CPU runqueue 参与调度。
 * ============================================================================= */
#include "smp.h"
#include "include/dev.h"
#include "include/dev.h"
#include "percpu.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/proc.h"
#include "include/proc.h"
#include "../proc/proc.h"
#include "include/mm.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "include/kernel.h"

#define AP_TRAMPOLINE_PA  0x00008000U
#define AP_DATA_PA        0x00007FF0U
#define AP_STACK_PAGES    16U
#define AP_STACK_BYTES    (AP_STACK_PAGES * 4096U)

extern uint8_t ap_trampoline_blob_start[];
extern uint8_t ap_trampoline_blob_end[];
extern void ap_entry(void);

volatile uint32_t smp_tlb_seq;

static void ap_delay(void)
{
    for (volatile uint32_t i = 0; i < 2000000U; i++) {
        __asm__ volatile ("pause");
    }
}

uint32_t smp_register_ap(uint32_t apic_id)
{
    for (uint32_t i = 1; i < SMP_MAX_CPUS; i++) {
        if (!per_cpu[i].online) {
            per_cpu[i].apic_id = apic_id;
            per_cpu[i].online = true;
            atomic_inc(&smp_cpu_count);
            return i;
        }
    }
    return 0U;
}

void ap_entry(void)
{
    uint32_t slot = smp_register_ap(apic_id_from_cpuid());

    if (slot == 0U) {
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* AP 现在有自己的 GDT/TSS，并加载共享 IDT；中断仍关闭。 */
    gdt_init_ap(slot, *(volatile uint32_t *)(uintptr_t)AP_DATA_PA);
    idt_load_current();
    per_cpu[slot].ready = true;
    atomic_inc(&smp_ap_ready);

    /* AP 自己的 LAPIC timer：向量 0x30 由共享 IDT 分发给 smp_timer_tick。 */
    if (apic_prepare_ap()) {
        (void)apic_start_timer_ap(0x30U, 1000000U);
        __asm__ volatile ("sti");
    }

    /* AP 的 idle 循环：中断已打开，LAPIC timer 和 IPI 都会把本核拉回调度器。
     * 每轮 hlt 醒来后再显式查一次本 CPU 的 runqueue（sched_ap_tick 在
     * current==0 时会 sched_next + proc_switch_to），不依赖中断路径的时序。 */
    this_cpu()->idle_active = true;
    for (;;) {
        __asm__ volatile ("hlt");
        sched_ap_tick();
    }
}

/* BSP 校准 LAPIC 频率后写入这里，AP 在自己的定时器中断里取用并重装计数 */
static volatile uint32_t ap_timer_count;

void smp_ap_timer_reload(uint32_t count)
{
    if (count >= 1000U) ap_timer_count = count;
}

void smp_timer_tick(void)
{
    per_cpu_t *c = this_cpu();
    uint32_t want = ap_timer_count;

    /* 第一次拿到校准值时重装 AP 定时器，使 AP tick 与 BSP 100Hz 对齐 */
    if (want != 0U) {
        ap_timer_count = 0U;
        (void)apic_start_timer_ap(0x30U, want);
    }

    c->ticks++;
    apic_eoi();

    if (smp_cpu_id() != 0U) {
        sched_ap_tick();
    }
}

void smp_ipi_tick(void)
{
    per_cpu_t *c = this_cpu();
    uint32_t cr3;

    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    (void)cr3;
    c->ipi++;
    c->tlb_seen = smp_tlb_seq;
    apic_eoi();

    /* IPI 也用作"有新活"的唤醒：让 AP 回到调度器看一眼自己的 runqueue */
    sched_ap_tick();
}

uint32_t smp_tlb_shootdown(void)
{
    uint32_t seq = ++smp_tlb_seq;
    uint32_t acked = 0;

    for (uint32_t i = 1; i < SMP_MAX_CPUS; i++) {
        if (!per_cpu[i].online) continue;
        per_cpu[i].tlb_seen = 0;
        (void)apic_send_ipi(per_cpu[i].apic_id, 0x00000031U);
    }

    for (uint32_t i = 1; i < SMP_MAX_CPUS; i++) {
        if (!per_cpu[i].online) continue;
        for (volatile uint32_t n = 0; n < 4000000U; n++) {
            if (per_cpu[i].tlb_seen == seq) break;
            __asm__ volatile ("pause");
        }
        if (per_cpu[i].tlb_seen == seq) acked++;
    }
    return acked;
}

void smp_start_aps(void)
{
    uint32_t bsp = apic_id_from_cpuid();
    uint32_t discovered = madt_cpu_count();
    uint32_t blob_size = (uint32_t)(ap_trampoline_blob_end - ap_trampoline_blob_start);

    if (discovered <= 1U) return;
    if (blob_size == 0U || blob_size > 0x1000U) {
        kprintln("smp: AP trampoline size invalid");
        return;
    }
    if (!apic_prepare()) {
        kprintln("smp: LAPIC not ready, AP startup skipped");
        return;
    }

    memset((void *)(uintptr_t)AP_TRAMPOLINE_PA, 0, 0x1000U);
    memcpy((void *)(uintptr_t)AP_TRAMPOLINE_PA,
           ap_trampoline_blob_start, blob_size);

    for (uint32_t i = 0; i < discovered; i++) {
        uint32_t id = madt_cpu_apic_id(i);
        void *stack;
        uint32_t before;

        if (id == bsp || id == 0xFFFFFFFFU || !madt_cpu_enabled(i)) continue;

        stack = pmm_alloc_pages(AP_STACK_PAGES);
        if (stack == 0) {
            kprintln("smp: no AP stack");
            break;
        }

        *(volatile uint32_t *)(uintptr_t)(AP_DATA_PA + 0U) =
            (uint32_t)(uintptr_t)stack + AP_STACK_BYTES;
        *(volatile uint32_t *)(uintptr_t)(AP_DATA_PA + 4U) =
            (uint32_t)(uintptr_t)page_directory;
        *(volatile uint32_t *)(uintptr_t)(AP_DATA_PA + 8U) =
            (uint32_t)(uintptr_t)ap_entry;

        if (vmm_kernel_set_exec(AP_TRAMPOLINE_PA) != 0) {
            kprintln("smp: cannot make trampoline executable");
            break;
        }

        before = smp_cpu_count;
        (void)apic_send_ipi(id, 0x00004500U);   /* INIT assert */
        ap_delay();
        (void)apic_send_ipi(id, 0x00008500U);   /* INIT deassert */
        ap_delay();
        (void)apic_send_ipi(id, 0x00004608U);   /* SIPI vector 0x08 */
        ap_delay();

        for (volatile uint32_t n = 0; n < 2000000U; n++) {
            if (smp_cpu_count > before) break;
            __asm__ volatile ("pause");
        }
    }

    for (volatile uint32_t n = 0; n < 2000000U; n++) {
        if (smp_ap_ready + 1U >= smp_cpu_count) break;
        __asm__ volatile ("pause");
    }

    for (volatile uint32_t n = 0; n < 4000000U; n++) {
        if (per_cpu[1].ticks != 0U) break;
        __asm__ volatile ("pause");
    }

    if (per_cpu[1].online) {
        (void)smp_tlb_shootdown();
    }

    kprintf("smp: %u/%u cpus online, %u AP GDT/TSS/IDT ready, AP ticks=%u ipi=%u tlb=%u\n",
            (unsigned int)smp_cpu_count, (unsigned int)discovered,
            (unsigned int)smp_ap_ready, (unsigned int)per_cpu[1].ticks,
            (unsigned int)per_cpu[1].ipi, (unsigned int)per_cpu[1].tlb_seen);
}
