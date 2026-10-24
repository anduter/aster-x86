#ifndef KERNEL_H_MODULE_H
#define KERNEL_H_MODULE_H

/* Module interface header (M0). Cross-module code includes this, not internals. */

#include "types.h"
#include "io.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "core/error.h"
#include "core/atomic.h"
#include "core/spinlock.h"
#include "core/panic.h"
#include "core/ksym.h"
#include "core/splash.h"
#include "core/version.h"

/* boot sequence entry (core/bootseq.c) */
void kernel_boot(void);

#endif
