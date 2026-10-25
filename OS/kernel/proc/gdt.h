#ifndef USER_GDT_H
#define USER_GDT_H

#include "../include/types.h"

/* 段选择子 */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B      /* 0x18 | RPL3 */
#define USER_DS   0x23      /* 0x20 | RPL3 */
#define TSS_SEL   0x28

/* 建立带用户段和 TSS 的 GDT，并加载 TR */
void gdt_init(void);

/* AP 建立自己的 GDT/TSS 并加载；stack_top 作为 TSS.esp0。 */
void gdt_init_ap(uint32_t cpu, uint32_t stack_top);

/* 设置 TSS.esp0：从 ring3 进 ring0 时 CPU 用它切换内核栈 */
void tss_set_esp0(uint32_t esp0);

/* 进入 ring3：entry=用户入口，stack_top=用户栈顶（不返回） */
void user_enter(uint32_t entry, uint32_t stack_top) __attribute__((noreturn));

/* 同上，但把 eax 设成 retval（fork 子进程返回 0 用） */
void user_enter_ret(uint32_t entry, uint32_t stack_top, uint32_t retval)
    __attribute__((noreturn));

/* 带寄存器现场：regs = { ebx, ebp, esi, edi }，fork 子进程恢复父进程现场用 */
void user_enter_regs(uint32_t entry, uint32_t stack_top, uint32_t retval,
                     const uint32_t *regs) __attribute__((noreturn));

#endif
