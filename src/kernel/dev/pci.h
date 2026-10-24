#ifndef CORE_PCI_H
#define CORE_PCI_H

#include "../types.h"

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
} pci_dev_t;

/* 扫描 PCI 总线（bus 0..255，device 0..31，function 0..7），最多保存 16 个设备 */
void pci_init(void);
uint32_t pci_count(void);
const pci_dev_t *pci_at(uint32_t index);

#endif
