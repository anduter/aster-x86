/*
 * ng WP1-3: IDT / 异常分发 / 受控探测 / panic。
 */
#ifndef NG_TRAP_H
#define NG_TRAP_H

/* 装 GDT/TSS 之后调用：填 256 项 IDT、lidt、打印 TSS/IST 证据。返回失败项数。 */
int trap_init(void);

/* 受控探测：#PF（写 .rodata）、#UD（ud2）、#GP（坏段选择子）；返回失败项数。 */
int trap_selftest(void);

#endif /* NG_TRAP_H */
