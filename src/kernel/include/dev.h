#ifndef DEV_H_MODULE_H
#define DEV_H_MODULE_H

/* Module interface header (M3). Cross-module code includes this, not internals. */

#include "dev/device.h"
#include "dev/pci.h"
#include "dev/acpi.h"
#include "dev/rtc.h"
#include "dev/block/ata.h"
#include "dev/block/block.h"
#include "dev/intc/apic.h"
#include "dev/intc/ioapic.h"
#include "dev/intc/madt.h"
#include "dev/char/console.h"
#include "dev/char/fb.h"
#include "dev/net/e1000.h"
#include "dev/hwcap.h"

#endif
