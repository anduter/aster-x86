/* =============================================================================
 * core/pci.c - PCI 配置空间枚举
 *
 * 通过 0xCF8/0xCFC 访问配置空间，扫描所有总线/设备/功能，
 * 记录厂商号、设备号、类代码。只做枚举与展示，不分配资源。
 * ============================================================================= */
#include "pci.h"
#include "arch/x86/io.h"
#include "include/kernel.h"

#define PCI_MAX_DEVS 16

static pci_dev_t devs[PCI_MAX_DEVS];
static uint32_t  dev_count;

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000U
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFCU);

    outl(0xCF8U, addr);
    return inl(0xCFCU);
}

void pci_init(void)
{
    dev_count = 0;

    for (uint32_t bus = 0; bus < 256U; bus++) {
        for (uint32_t dev = 0; dev < 32U; dev++) {
            for (uint32_t func = 0; func < 8U; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x00U);

                if (id == 0xFFFFFFFFU) {
                    if (func == 0U) break;      /* 设备不存在 */
                    continue;
                }

                if (dev_count < PCI_MAX_DEVS) {
                    uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x08U);
                    pci_dev_t *d = &devs[dev_count++];

                    d->bus = (uint8_t)bus;
                    d->dev = (uint8_t)dev;
                    d->func = (uint8_t)func;
                    d->vendor = (uint16_t)(id & 0xFFFFU);
                    d->device = (uint16_t)(id >> 16);
                    d->class_code = (uint8_t)(cls >> 24);
                    d->subclass = (uint8_t)((cls >> 16) & 0xFFU);
                    d->prog_if = (uint8_t)((cls >> 8) & 0xFFU);
                }

                /* 多功能设备：header type bit7 置位才继续扫 function */
                if (func == 0U) {
                    uint32_t ht = pci_read32((uint8_t)bus, (uint8_t)dev, 0U, 0x0CU);
                    if (((ht >> 16) & 0x80U) == 0U) break;
                }
            }
        }
    }

    kprintf("pci: %u device(s) found\n", (unsigned int)dev_count);
    for (uint32_t i = 0; i < dev_count; i++) {
        const pci_dev_t *d = &devs[i];
        kprintf("  pci %x:%x.%x %04x:%04x class=%02x%02x\n",
                (unsigned int)d->bus, (unsigned int)d->dev, (unsigned int)d->func,
                (unsigned int)d->vendor, (unsigned int)d->device,
                (unsigned int)d->class_code, (unsigned int)d->subclass);
    }
}

uint32_t pci_count(void) { return dev_count; }

const pci_dev_t *pci_at(uint32_t index)
{
    return (index < dev_count) ? &devs[index] : 0;
}
