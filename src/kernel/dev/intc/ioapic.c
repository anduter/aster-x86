/* =============================================================================
 * core/ioapic.c - IOAPIC 驱动（MMIO 寄存器 + 重定向表）
 *
 * 关键点：
 *   - MMIO 基址来自 ACPI MADT（通常 0xFEC00000）；
 *   - IOREGSEL(0x00) 选寄存器，IOWIN(0x10) 读写；
 *   - 重定向表项 0x10 + 2*n：低 32 位含 vector/delivery/trigger/mask，
 *     高 32 位 24..31 是目标 LAPIC ID。
 *
 * 策略：初始化并打印信息；路由函数供 irq.c 逐条迁移传统 IRQ 使用，
 * IRQ->GSI 的映射来自 MADT Interrupt Source Override（PIT 是 IRQ0->GSI2）。
 * 迁移失败的单条 IRQ 保留 8259 PIC 回退。
 * ============================================================================= */
#include "ioapic.h"
#include "madt.h"
#include "include/mm.h"
#include "../../core/spinlock.h"
#include "../../core/error.h"
#include "../../lib/kprintf.h"

#define IOAPIC_REGSEL  0x00U
#define IOAPIC_WIN     0x10U

#define IOAPIC_ID      0x00U
#define IOAPIC_VER     0x01U
#define IOAPIC_REDTBL  0x10U

static volatile uint32_t *ioapic;
static uint32_t ioapic_entries;
static bool     ioapic_ok;
static spinlock_t ioapic_lock = SPINLOCK_INIT;

static uint32_t io_read(uint32_t reg)
{
    ioapic[IOAPIC_REGSEL / 4U] = reg;
    return ioapic[IOAPIC_WIN / 4U];
}

static void io_write(uint32_t reg, uint32_t val)
{
    ioapic[IOAPIC_REGSEL / 4U] = reg;
    ioapic[IOAPIC_WIN / 4U] = val;
}

int ioapic_init(void)
{
    uint32_t base = madt_ioapic_base();
    uint32_t ver;
    uint32_t flags;

    ioapic_ok = false;
    if (base == 0U) {
        kprintln("ioapic: no MADT IOAPIC");
        return E_NOENT;
    }

    if (vmm_map_mmio(base, 0x1000U) != 0) {
        kprintln("ioapic: cannot map MMIO");
        return E_NOSPC;
    }
    ioapic = (volatile uint32_t *)(uintptr_t)base;

    flags = spin_lock_irqsave(&ioapic_lock);
    ver = io_read(IOAPIC_VER);
    ioapic_entries = ((ver >> 16) & 0xFFU) + 1U;

    /* 自检：把最后一条 GSI 设成“掩码 + 固定向量”，读回验证后保持掩码。
     * 不碰正在使用的 GSI，避免影响 PIC 路径。 */
    if (ioapic_entries > 0U) {
        uint32_t gsi = ioapic_entries - 1U;
        uint32_t reg = IOAPIC_REDTBL + gsi * 2U;
        uint32_t old_low = io_read(reg);
        uint32_t old_high = io_read(reg + 1U);

        io_write(reg + 1U, 0U);                    /* 目标 BSP */
        io_write(reg, 0x00010000U | 0xFFU);        /* masked, vector 0xFF */
        if ((io_read(reg) & 0x00010000U) == 0U) {
            spin_unlock_irqrestore(&ioapic_lock, flags);
            kprintln("ioapic: self-test FAILED (mask bit not sticky)");
            return E_INVAL;
        }
        io_write(reg + 1U, old_high);
        io_write(reg, old_low);
    }

    ioapic_ok = true;
    kprintf("ioapic: base=0x%x id=%u version=0x%02x entries=%u (PIC retained)\n",
            (unsigned int)base, (unsigned int)((io_read(IOAPIC_ID) >> 24) & 0xFU),
            (unsigned int)(ver & 0xFFU), (unsigned int)ioapic_entries);
    spin_unlock_irqrestore(&ioapic_lock, flags);
    return E_OK;
}

int ioapic_present(void) { return ioapic_ok ? 1 : 0; }
uint32_t ioapic_gsi_count(void) { return ioapic_entries; }

int ioapic_route(uint32_t gsi, uint32_t apic_id, uint8_t vector,
                 bool masked, bool level, bool active_low)
{
    uint32_t reg;
    uint32_t low = 0;
    uint32_t flags;

    if (!ioapic_ok || gsi >= ioapic_entries) return E_INVAL;

    low = (uint32_t)vector;                    /* vector 0..7 */
    low |= (0U << 8);                          /* delivery: fixed */
    low |= (0U << 11);                         /* dest mode: physical */
    if (active_low) low |= (1U << 13);         /* polarity: active low */
    if (level) low |= (1U << 15);              /* trigger: level */
    if (masked) low |= (1U << 16);

    reg = IOAPIC_REDTBL + gsi * 2U;

    flags = spin_lock_irqsave(&ioapic_lock);
    io_write(reg + 1U, (apic_id & 0xFFU) << 24);
    io_write(reg, low);
    spin_unlock_irqrestore(&ioapic_lock, flags);
    return E_OK;
}

int ioapic_mask(uint32_t gsi, bool masked)
{
    uint32_t reg;
    uint32_t low;
    uint32_t flags;

    if (!ioapic_ok || gsi >= ioapic_entries) return E_INVAL;
    reg = IOAPIC_REDTBL + gsi * 2U;

    flags = spin_lock_irqsave(&ioapic_lock);
    low = io_read(reg);
    if (masked) low |= (1U << 16);
    else        low &= ~(1U << 16);
    io_write(reg, low);
    spin_unlock_irqrestore(&ioapic_lock, flags);
    return E_OK;
}

int ioapic_eoi(uint8_t vector)
{
    uint32_t flags;

    if (!ioapic_ok) return E_NOENT;

    flags = spin_lock_irqsave(&ioapic_lock);
    io_write(0x40U, (uint32_t)vector);         /* IOAPIC EOI: 写回中断向量 */
    spin_unlock_irqrestore(&ioapic_lock, flags);
    return E_OK;
}

uint32_t ioapic_read_entry_low(uint32_t gsi)
{
    uint32_t flags;
    uint32_t v;

    if (!ioapic_ok || gsi >= ioapic_entries) return 0U;

    flags = spin_lock_irqsave(&ioapic_lock);
    v = io_read(IOAPIC_REDTBL + gsi * 2U);
    spin_unlock_irqrestore(&ioapic_lock, flags);
    return v;
}
