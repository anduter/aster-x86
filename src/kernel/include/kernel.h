#ifndef KERNEL_H_MODULE_H
#define KERNEL_H_MODULE_H

/* Module interface header (M0). Cross-module code includes this, not internals. */

#include "types.h"
#include "arch/x86/io.h"
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

/* 内核参数（core/bootargs.c）：编译期注入的 boot cmdline。
 * 测试模式（-Test 构建）默认带 autologin=root；生产构建为空串 -> 必须登录。 */
const char *boot_cmdline(void);
bool        boot_cmdline_has(const char *token);
bool        boot_cmdline_autologin_root(void);
uint32_t    boot_test_build(void);

#endif
