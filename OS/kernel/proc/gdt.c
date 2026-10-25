/* =============================================================================
 * gdt.c - 每 CPU GDT + TSS
 *
 * 段布局（每个 CPU 一份）：
 *   0x08 内核代码 DPL0   0x10 内核数据 DPL0
 *   0x18 用户代码 DPL3   0x20 用户数据 DPL3
 *   0x28 本 CPU TSS
 *
 * AP 先加载自己的 GDT/TSS，再加载共享 IDT；当前 AP 仍关中断 idle。
 * ============================================================================= */
#include "gdt.h"
#include "include/proc.h"
#include "include/kernel.h"

typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdtr_t;

/* 32 位 TSS（104 字节，不含 IO 位图） */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

#define GDT_ENTRIES 6U

static gdt_entry_t gdt[SMP_MAX_CPUS][GDT_ENTRIES];
static gdtr_t      gdtr[SMP_MAX_CPUS];
static tss_t       tss[SMP_MAX_CPUS];

extern void gdt_flush(void *gdtr_ptr);
extern void tss_flush(void);

static void set_entry(gdt_entry_t *e, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t gran)
{
    e->base_lo  = (uint16_t)(base & 0xFFFFU);
    e->base_mid = (uint8_t)((base >> 16) & 0xFFU);
    e->base_hi  = (uint8_t)((base >> 24) & 0xFFU);
    e->limit_lo = (uint16_t)(limit & 0xFFFFU);
    e->gran     = (uint8_t)(((limit >> 16) & 0x0FU) | (gran & 0xF0U));
    e->access   = access;
}

static void build_cpu(uint32_t cpu)
{
    gdt_entry_t *g;

    if (cpu >= SMP_MAX_CPUS) return;
    g = gdt[cpu];

    memset(&tss[cpu], 0, sizeof(tss[cpu]));
    tss[cpu].ss0 = KERNEL_DS;
    tss[cpu].iomap_base = (uint16_t)sizeof(tss[cpu]);

    set_entry(&g[0], 0, 0, 0, 0);                              /* null */
    set_entry(&g[1], 0, 0xFFFFFU, 0x9AU, 0xCFU);               /* kernel code */
    set_entry(&g[2], 0, 0xFFFFFU, 0x92U, 0xCFU);               /* kernel data */
    set_entry(&g[3], 0, 0xFFFFFU, 0xFAU, 0xCFU);               /* user code */
    set_entry(&g[4], 0, 0xFFFFFU, 0xF2U, 0xCFU);               /* user data */
    set_entry(&g[5], (uint32_t)(uintptr_t)&tss[cpu],
              (uint32_t)sizeof(tss_t) - 1U, 0x89U, 0x00U);      /* TSS */

    gdtr[cpu].limit = (uint16_t)(sizeof(gdt[cpu]) - 1U);
    gdtr[cpu].base  = (uint32_t)(uintptr_t)&g[0];
}

void gdt_init(void)
{
    build_cpu(0);
    gdt_flush(&gdtr[0]);
    tss_flush();
}

void gdt_init_ap(uint32_t cpu, uint32_t stack_top)
{
    if (cpu == 0U || cpu >= SMP_MAX_CPUS) return;

    build_cpu(cpu);
    tss[cpu].esp0 = stack_top;
    gdt_flush(&gdtr[cpu]);
    tss_flush();
}

void tss_set_esp0(uint32_t esp0)
{
    uint32_t cpu = smp_cpu_id();

    if (cpu < SMP_MAX_CPUS) tss[cpu].esp0 = esp0;
}
