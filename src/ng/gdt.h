/*
 * ng WP1-3: GDT + TSS + IST。
 */
#ifndef NG_GDT_H
#define NG_GDT_H

#include "types.h"

/* 建 GDT、装 TSS（含 RSP0/IST1）、ltr；之后 CS/DS 等段寄存器已刷新。 */
void gdt_init(void);

/* TSS.RSP0（ring3 -> ring0 的栈顶）与 TSS.IST1（#DF 栈顶），给 trap 打证据。 */
u64 gdt_tss_rsp0(void);
u64 gdt_ist1_top(void);

/*
 * WP1-7：per-thread 内核栈。调度器在切换线程时把当前线程的内核栈顶写进
 * TSS.RSP0（ring3 -> ring0 的异常/中断入口），syscall 入口用同一份值
 * （syscall.c 的 syscall_kernel_rsp0，两者总是同步更新）。
 */
void gdt_set_rsp0(u64 top);

#endif /* NG_GDT_H */
