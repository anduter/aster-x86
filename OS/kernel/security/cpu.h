#ifndef CORE_CPU_H
#define CORE_CPU_H

#include "../include/types.h"

/* 检测并启用可选的 CPU 安全特性（SMEP/SMAP）。
 * 必须在分页开启之后调用；不支持的 CPU 只打印状态，不置 CR4 位。 */
void cpu_security_init(void);

int cpu_smep_enabled(void);
int cpu_smap_enabled(void);

/* 供汇编 uaccess_copy 检查：只有 SMAP 真正启用时才执行 stac/clac。 */
extern volatile int smap_enabled;

/* 内核直接访问用户内存（信号帧等）时成对使用；未启用 SMAP 时是空操作。 */
void user_access_begin(void);
void user_access_end(void);

#endif
