/* =============================================================================
 * core/madt.c - ACPI MADT 解析（多核发现）
 *
 * MADT（签名 "APIC"）里列出：
 *   type 0 = Processor Local APIC：ACPI id、APIC id、flags（bit0=可用）
 *   type 1 = IOAPIC：MMIO 基址、GSI 基址
 * 这是 AP 启动和 IOAPIC 路由的第一步：先知道“有几个核、它们的 APIC ID”。
 * ============================================================================= */
#include "madt.h"
#include "../acpi.h"
#include "../../lib/kprintf.h"

#define MADT_MAX_CPUS 8U
#define MADT_MAX_OVERRIDES 16U

static uint32_t cpu_apic[MADT_MAX_CPUS];
static bool     cpu_on[MADT_MAX_CPUS];
static uint32_t cpu_count;
static uint32_t ioapic_base;
static uint32_t ioapic_gsi;
static int      madt_ok;

/* Interrupt Source Override：ISA IRQ -> GSI。QEMU 的 PIT 是 IRQ0 -> GSI2，
 * 若不解析 override 直接按 IRQ 号写重定向表，timer 会静默丢中断。 */
static uint8_t  ov_irq[MADT_MAX_OVERRIDES];
static uint32_t ov_gsi[MADT_MAX_OVERRIDES];
static uint16_t ov_flags[MADT_MAX_OVERRIDES];
static uint32_t ov_count;

uint32_t madt_cpu_count(void)          { return cpu_count; }
uint32_t madt_cpu_apic_id(uint32_t i)  { return (i < cpu_count) ? cpu_apic[i] : 0xFFFFFFFFU; }
int      madt_cpu_enabled(uint32_t i)  { return (i < cpu_count) ? (cpu_on[i] ? 1 : 0) : 0; }
uint32_t madt_ioapic_base(void)        { return ioapic_base; }
uint32_t madt_ioapic_gsi_base(void)    { return ioapic_gsi; }

uint32_t madt_irq_override_count(void) { return ov_count; }

uint32_t madt_irq_gsi(uint32_t irq)
{
    for (uint32_t i = 0; i < ov_count; i++) {
        if (ov_irq[i] == (uint8_t)irq) return ov_gsi[i];
    }
    return irq;
}

uint16_t madt_irq_flags(uint32_t irq)
{
    for (uint32_t i = 0; i < ov_count; i++) {
        if (ov_irq[i] == (uint8_t)irq) return ov_flags[i];
    }
    return 0U;
}

int madt_init(void)
{
    const uint8_t *madt = acpi_find_table("APIC");
    uint32_t length;
    uint32_t off;

    cpu_count = 0;
    ioapic_base = 0;
    ioapic_gsi = 0;
    ov_count = 0;

    if (madt == 0) {
        kprintln("madt: MADT not found");
        return 0;
    }

    length = (uint32_t)madt[4] | ((uint32_t)madt[5] << 8) |
             ((uint32_t)madt[6] << 16) | ((uint32_t)madt[7] << 24);
    if (length < 44U || length > 4096U) {
        kprintln("madt: length suspicious");
        return 0;
    }

    /* MADT 头部 44 字节，之后是变长条目 */
    for (off = 44U; off + 2U <= length; ) {
        uint8_t type = madt[off];
        uint8_t len  = madt[off + 1U];

        if (len < 2U || off + len > length) break;

        if (type == 0U && len >= 8U) {                 /* Processor Local APIC */
            if (cpu_count < MADT_MAX_CPUS) {
                cpu_apic[cpu_count] = madt[off + 3U];  /* APIC ID */
                cpu_on[cpu_count] = (madt[off + 4U] & 0x01U) != 0U;
                cpu_count++;
            }
        } else if (type == 1U && len >= 12U) {         /* IOAPIC */
            uint32_t base = (uint32_t)madt[off + 4U] | ((uint32_t)madt[off + 5U] << 8) |
                            ((uint32_t)madt[off + 6U] << 16) | ((uint32_t)madt[off + 7U] << 24);
            uint32_t gsi  = (uint32_t)madt[off + 8U] | ((uint32_t)madt[off + 9U] << 8) |
                            ((uint32_t)madt[off + 10U] << 16) | ((uint32_t)madt[off + 11U] << 24);
            if (ioapic_base == 0U) {
                ioapic_base = base;
                ioapic_gsi = gsi;
            }
        } else if (type == 2U && len >= 10U) {         /* Interrupt Source Override */
            if (ov_count < MADT_MAX_OVERRIDES) {
                ov_irq[ov_count]   = madt[off + 3U];   /* source IRQ */
                ov_gsi[ov_count]   = (uint32_t)madt[off + 4U] |
                                     ((uint32_t)madt[off + 5U] << 8) |
                                     ((uint32_t)madt[off + 6U] << 16) |
                                     ((uint32_t)madt[off + 7U] << 24);
                ov_flags[ov_count] = (uint16_t)((uint16_t)madt[off + 8U] |
                                                ((uint16_t)madt[off + 9U] << 8));
                ov_count++;
            }
        }

        off += len;
    }

    madt_ok = 1;
    kprintf("madt: %u cpu(s), ioapic=0x%x gsi_base=%u\n",
            (unsigned int)cpu_count, (unsigned int)ioapic_base,
            (unsigned int)ioapic_gsi);
    for (uint32_t i = 0; i < ov_count; i++) {
        kprintf("madt: irq%u -> gsi%u flags=0x%x\n",
                (unsigned int)ov_irq[i], (unsigned int)ov_gsi[i],
                (unsigned int)ov_flags[i]);
    }
    return madt_ok;
}
