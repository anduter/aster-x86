#ifndef PROC_H_MODULE_H
#define PROC_H_MODULE_H

/* Module interface header (M1). Cross-module code includes this, not internals. */

#include "proc/proc.h"
#include "proc/smp.h"
#include "proc/percpu.h"
#include "proc/gdt.h"
#include "proc/trap/fault.h"
#include "proc/trap/interrupt.h"
#include "proc/trap/native.h"
#include "proc/trap/syscall.h"
#include "proc/trap/uaccess.h"

#endif
