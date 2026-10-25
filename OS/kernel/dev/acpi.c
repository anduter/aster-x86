/* =============================================================================
 * core/acpi.c - ACPI 表扫描 + S5 关机
 *
 * 1. 在 EBDA / BIOS ROM 区找 RSDP（"RSD PTR "，20 字节校验和）；
 * 2. RSDT -> FACP(FADT)，取 PM1a 控制寄存器与 SMI_CMD；
 * 3. 关机：向 PM1a_CNT 写 (SLP_TYP << 10) | SLP_EN。
 *    QEMU/PIIX4 上 S5 的 SLP_TYP 是 0，这里按 0 处理。
 * ============================================================================= */
#include "acpi.h"
#include "arch/x86/io.h"
#include "include/kernel.h"

static uint32_t pm1a_cnt;      /* PM1a 控制寄存器 I/O 端口 */
static uint32_t smi_cmd;
static uint8_t  acpi_enable_val;
static int      acpi_ok;

static int sig_eq(const uint8_t *p, const char *sig)
{
    for (int i = 0; i < 4; i++) {
        if (p[i] != (uint8_t)sig[i]) return 0;
    }
    return 1;
}

static uint8_t checksum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return (uint8_t)(sum == 0U);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t *find_rsdp(void)
{
    /* EBDA：0x40E 处是段地址 */
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)(uintptr_t)0x40EU) << 4;

    if (ebda >= 0x400U && ebda < 0xA0000U) {
        for (uint32_t a = ebda; a < 0xA0000U; a += 16U) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;
            if (sig_eq(p, "RSD ") && p[4] == 'P' && p[5] == 'T' && p[6] == 'R' &&
                p[7] == ' ' && checksum_ok(p, 20U)) {
                return p;
            }
        }
    }

    /* BIOS ROM 区：0xE0000 .. 0xFFFFF */
    for (uint32_t a = 0xE0000U; a < 0x100000U; a += 16U) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        if (sig_eq(p, "RSD ") && p[4] == 'P' && p[5] == 'T' && p[6] == 'R' &&
            p[7] == ' ' && checksum_ok(p, 20U)) {
            return p;
        }
    }
    return 0;
}

/* 按签名在 RSDT 里找一张表；找不到返回 0（MADT 等子系统复用） */
const uint8_t *acpi_find_table(const char *sig)
{
    const uint8_t *rsdp = find_rsdp();
    const uint8_t *rsdt;
    uint32_t entries, length;

    if (rsdp == 0) return 0;

    rsdt = (const uint8_t *)(uintptr_t)rd32(rsdp + 16);
    if (rsdt == 0 || !sig_eq(rsdt, "RSDT")) return 0;

    length = rd32(rsdt + 4);
    if (length < 36U || length > 4096U) return 0;
    entries = (length - 36U) / 4U;

    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *t = (const uint8_t *)(uintptr_t)rd32(rsdt + 36U + i * 4U);

        if (t != 0 && sig_eq(t, sig)) return t;
    }
    return 0;
}

int acpi_init(void)
{
    const uint8_t *rsdp = find_rsdp();
    const uint8_t *rsdt;    uint32_t entries, length;

    if (rsdp == 0) {
        kprintln("acpi: RSDP not found");
        return 0;
    }

    rsdt = (const uint8_t *)(uintptr_t)rd32(rsdp + 16);
    if (rsdt == 0 || !sig_eq(rsdt, "RSDT")) {
        kprintln("acpi: RSDT invalid");
        return 0;
    }

    length = rd32(rsdt + 4);
    if (length < 36U || length > 4096U) {
        kprintln("acpi: RSDT length suspicious");
        return 0;
    }
    entries = (length - 36U) / 4U;

    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *t = (const uint8_t *)(uintptr_t)rd32(rsdt + 36U + i * 4U);

        if (t == 0 || !sig_eq(t, "FACP")) continue;

        smi_cmd = rd32(t + 0x30U);
        acpi_enable_val = t[0x34U];
        pm1a_cnt = rd32(t + 0x40U);

        if (pm1a_cnt == 0U) {
            kprintln("acpi: FADT has no PM1a_CNT");
            return 0;
        }

        /* 如果 ACPI 还没启用（SCI_EN=0），通过 SMI_CMD 请求固件启用 */
        if (smi_cmd != 0U && acpi_enable_val != 0U &&
            (inw((unsigned short)pm1a_cnt) & 0x01U) == 0U) {
            outb((unsigned short)smi_cmd, acpi_enable_val);
            for (volatile uint32_t d = 0; d < 1000000U; d++) {
                if ((inw((unsigned short)pm1a_cnt) & 0x01U) != 0U) break;
            }
        }

        acpi_ok = 1;
        kprintf("acpi: PM1a_CNT=0x%x SMI_CMD=0x%x SCI_EN=%u\n",
                (unsigned int)pm1a_cnt, (unsigned int)smi_cmd,
                (unsigned int)(inw((unsigned short)pm1a_cnt) & 0x01U));
        return 1;
    }

    kprintln("acpi: FADT not found");
    return 0;
}

int acpi_available(void)
{
    return acpi_ok;
}

void acpi_poweroff(void)
{
    if (!acpi_ok) {
        kprintln("acpi: no poweroff register, halting instead");
        for (;;) __asm__ volatile("cli; hlt");
    }

    kprintln("acpi: powering off (S5)");

    /* S5：SLP_TYP=0，SLP_EN=bit13 */
    outw((unsigned short)pm1a_cnt, (unsigned short)((0U << 10) | (1U << 13)));

    /* 正常情况这里不会返回；留着兜底 */
    for (;;) __asm__ volatile("cli; hlt");
}
