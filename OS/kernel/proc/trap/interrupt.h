#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "../../include/types.h"

/* 时钟中断频率（Hz） */
#define TIMER_HZ 100

/* 8259 的 IRQ 条数 */
#define IRQ_MAX  16

/* 中断发生时由汇编入口压栈的完整寄存器现场。
 * 字段顺序必须与 isr.asm 里的压栈顺序完全一致。 */
typedef struct interrupt_frame {
    unsigned int gs, fs, es, ds;                       /* 段寄存器（后压的先出） */
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pushad 的现场 */
    unsigned int vec, err;                             /* 中断向量号 / 错误码 */
    unsigned int eip, cs, eflags, useresp, ss;         /* CPU 自动压入的部分 */
} interrupt_frame_t;

/* ---- 汇编入口 ---- */
void isr_dispatch(interrupt_frame_t *f);
void irq_dispatch(interrupt_frame_t *f);
/* 系统调用分发（isr_dispatch 按向量转发）：
 *   0x80 = Native ABI；0x81 = 旧原型过渡门。 */
void native_syscall_dispatch(interrupt_frame_t *f);
void syscall_dispatch(interrupt_frame_t *f);

/* ---- 初始化 ---- */
void idt_init(void);
/* 在 AP 上加载同一份 IDT（只读共享）。 */
void idt_load_current(void);
void interrupt_init(void);

/* PS/2 鼠标（IRQ12）：初始化、探测、读取坐标/按键 */
int  mouse_init(void);
int  mouse_present(void);
void mouse_get(int *x, int *y, int *buttons);

/* 系统 tick 数（100Hz），NMI 看门狗用它判断“有没有进展” */
unsigned int timer_ticks(void);

/* ---- 异常钩子 ----
 * 处理函数返回动作：
 *   EXC_HANDLED 已修复，iret 后重新执行故障指令
 *   EXC_KILL    终止当前线程（用户态异常默认行为）
 *   EXC_PANIC   交给 panic 打印现场并停机
 * 没有注册处理函数的向量按默认策略：用户态杀线程，内核态 panic。 */
#define EXC_HANDLED 0
#define EXC_KILL    1
#define EXC_PANIC   2

typedef int (*exception_handler_t)(interrupt_frame_t *f);
int exception_register(unsigned int vec, exception_handler_t handler);
int exception_unregister(unsigned int vec);

/* 安全访问用户内存：缺页时回到安全点返回错误，而不是 panic 或杀线程。
 * 必须在 interrupt_init() 之后使用（#PF 钩子在那里注册）。 */
void uaccess_init(void);
int copy_from_user(void *dst, const void *src, uint32_t len);
int copy_to_user(void *dst, const void *src, uint32_t len);

/* ---- IRQ 注册与屏蔽 ----
 * 驱动或内核模块用 irq_register 挂自己的处理函数，不再改 irq.c。 */
typedef void (*irq_handler_t)(void *arg);

int irq_register(unsigned int irq, irq_handler_t handler, void *arg, const char *name);
int irq_unregister(unsigned int irq);
int irq_mask(unsigned int irq);
int irq_unmask(unsigned int irq);

/* 把 IRQ 从 8259 迁到 IOAPIC（目标 CPU 的 LAPIC），成功返回 0 */
int irq_migrate_to_ioapic(unsigned int irq, unsigned int apic_id);
int irq_is_ioapic_routed(unsigned int irq);

/* 统计与命名（供 shell / 日志） */
unsigned int irq_count(unsigned int irq);
const char  *irq_name(unsigned int irq);
unsigned int irq_spurious_count(void);

/* ---- 时钟 ---- */
unsigned int timer_ticks(void);
unsigned int timer_hz(void);

/* ---- 键盘 ---- */
int keyboard_getchar(void);

#endif
