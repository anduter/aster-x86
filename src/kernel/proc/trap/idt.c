#include "interrupt.h"
#include "../../types.h"
#include "../../panic.h"
#include "../proc.h"
#include "../../user/user_mode.h"
#include "../../core/ksym.h"
#include "../smp.h"
#include "../../mm/vmm.h"
#include "../../lib/kprintf.h"

/* ---- IDT 表项（x86 中断门，8 字节） ---- */
typedef struct {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

/* 异常钩子：注册了就用注册的，没注册就 panic */
static exception_handler_t exc_hooks[32];

/* 汇编里导出的入口地址表（见 intr/isr.asm） */
extern uint32_t isr_stub_table[32];
extern uint32_t irq_stub_table[16];
extern void isr128(void);
extern void isr129(void);
extern void isr_ap_timer(void);
extern void isr_ap_ipi(void);
extern void isr_spurious(void);

static const char *const exc_name[32] = {
    "divide error",              /* 0  */
    "debug",                     /* 1  */
    "non-maskable interrupt",    /* 2  */
    "breakpoint",                /* 3  */
    "overflow",                  /* 4  */
    "bound range exceeded",      /* 5  */
    "invalid opcode",            /* 6  */
    "device not available",      /* 7  */
    "double fault",              /* 8  */
    "coprocessor segment overrun", /* 9  */
    "invalid TSS",               /* 10 */
    "segment not present",       /* 11 */
    "stack-segment fault",       /* 12 */
    "general protection fault",  /* 13 */
    "page fault",                /* 14 */
    "reserved",                  /* 15 */
    "x87 floating-point error",  /* 16 */
    "alignment check",           /* 17 */
    "machine check",             /* 18 */
    "SIMD floating-point error", /* 19 */
    "virtualization exception",  /* 20 */
    "reserved", "reserved", "reserved", "reserved",   /* 21~24 */
    "reserved", "reserved", "reserved", "reserved",   /* 25~28 */
    "reserved", "reserved", "reserved"                /* 29~31 */
};

static void idt_set_gate(uint32_t n, uint32_t handler, uint16_t sel, uint8_t flags)
{
    idt[n].base_lo = (uint16_t)(handler & 0xFFFFU);
    idt[n].base_hi = (uint16_t)((handler >> 16) & 0xFFFFU);
    idt[n].sel     = sel;
    idt[n].always0 = 0;
    idt[n].flags   = flags;
}

void idt_init(void)
{
    for (uint32_t i = 0; i < 256U; i++) {
        idt_set_gate(i, 0, 0, 0);
    }
    for (uint32_t i = 0; i < 32U; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);   /* DPL0 中断门 */
    }
    for (uint32_t i = 0; i < 16U; i++) {
        idt_set_gate(32U + i, irq_stub_table[i], 0x08, 0x8E);
    }

    /* 0x80：Native ABI 系统调用门；0x81：旧原型的过渡门。
     * 0xEE = 存在 | DPL3 | 32 位中断门，允许 ring3 执行。 */
    idt_set_gate(0x80U, (uint32_t)(uintptr_t)isr128, 0x08, 0xEE);
    idt_set_gate(0x81U, (uint32_t)(uintptr_t)isr129, 0x08, 0xEE);
    idt_set_gate(0x30U, (uint32_t)(uintptr_t)isr_ap_timer, 0x08, 0x8E);
    idt_set_gate(0x31U, (uint32_t)(uintptr_t)isr_ap_ipi, 0x08, 0x8E);
    idt_set_gate(0xFFU, (uint32_t)(uintptr_t)isr_spurious, 0x08, 0x8E);

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base  = (uint32_t)(uintptr_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

void idt_load_current(void)
{
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

int exception_register(unsigned int vec, exception_handler_t handler)
{
    if (vec >= 32U || handler == 0) return -1;
    if (exc_hooks[vec] != 0) return -2;
    exc_hooks[vec] = handler;
    return 0;
}

int exception_unregister(unsigned int vec)
{
    if (vec >= 32U) return -1;
    if (exc_hooks[vec] == 0) return -2;
    exc_hooks[vec] = 0;
    return 0;
}

/* 用户态异常：只终止当前线程（不返回） */
__attribute__((noreturn))
static void kill_current(interrupt_frame_t *f)
{
    uint32_t cr2 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    kprintf("user: exception %u at eip=%p err=%x cr2=%p, killing thread\n",
            f->vec, (void *)(uintptr_t)f->eip, f->err, (void *)(uintptr_t)cr2);
    proc_exit((int)(128U + f->vec));
}

/* 内核态异常：打印栈回溯（符号化），杀掉当前线程 */
static void dump_trace(uint32_t ebp)
{
    for (int i = 0; i < 8; i++) {
        uint32_t ret, next, off = 0;
        const char *sym;

        if (ebp < 0x100000U) break;
        ret = *(volatile uint32_t *)(uintptr_t)(ebp + 4);
        next = *(volatile uint32_t *)(uintptr_t)ebp;

        sym = ksym_lookup(ret, &off);
        if (sym != 0) {
            kprintf("  #%d %s+0x%x (ret=%p)\n", i + 1, sym, off, (void *)(uintptr_t)ret);
        } else {
            kprintf("  #%d ret=%p\n", i + 1, (void *)(uintptr_t)ret);
        }

        if (next <= ebp) break;
        ebp = next;
    }
}

__attribute__((noreturn))
static void kernel_oops(interrupt_frame_t *f)
{
    uint32_t ebp = 0;
    uint32_t cr2 = 0;
    uint32_t off = 0;
    const char *sym;

    __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    sym = ksym_lookup(f->eip, &off);
    if (sym != 0) {
        kprintf("oops: %s+0x%x eip=%p vec=%u err=%x cr2=%p\n",
                sym, off, (void *)(uintptr_t)f->eip, f->vec, f->err,
                (void *)(uintptr_t)cr2);
    } else {
        kprintf("oops: exception %u (%s) at eip=%p err=%x cr2=%p\n",
                f->vec, (f->vec < 32U) ? exc_name[f->vec] : "unknown",
                (void *)(uintptr_t)f->eip, f->err, (void *)(uintptr_t)cr2);
    }
    kprintf("oops: stack trace:\n");
    dump_trace(ebp);

    proc_exit(139);
}

/* 异常分发：
 *   1. 有钩子 -> 按返回值处理（HANDLED 直接返回，KILL 杀线程，PANIC 继续）；
 *   2. 用户态异常 -> 先尝试栈增长，否则杀线程；
 *   3. 内核态异常 -> oops（回溯 + 杀线程）；没有当前线程才 panic。 */
void isr_dispatch(interrupt_frame_t *f)
{
    unsigned int vec = f->vec;

    if (vec == 0x30U) {
        smp_timer_tick();
        return;
    }
    if (vec == 0x31U) {
        smp_ipi_tick();
        return;
    }
    if (vec == 0xFFU) {
        return;                         /* LAPIC spurious，不 EOI */
    }

    /* 系统调用走独立分发：0x80 = Native ABI，0x81 = 旧原型过渡门 */
    if (vec == 128U) {
        native_syscall_dispatch(f);
        return;
    }
    if (vec == 129U) {
        syscall_dispatch(f);
        return;
    }

    if (vec < 32U && exc_hooks[vec] != 0) {
        int action = exc_hooks[vec](f);
        if (action == EXC_HANDLED) {
            return;
        }
        if (action == EXC_KILL) {
            kill_current(f);
        }
        /* EXC_PANIC 落到下面 */
    }

    /* 来自 ring3 的异常：先处理写时复制，再看栈增长，否则杀掉当前线程 */
    if ((f->cs & 3U) == 3U) {
        if (vec == 14U) {
            uint32_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if ((f->err & 2U) != 0U && vmm_cow_fault(cr2)) {
                return;                         /* 复制/恢复可写后重试指令 */
            }
            if (user_try_grow_stack(cr2)) return;   /* 补页后重试指令 */
        }
        kill_current(f);
    }

    /* 内核态异常：oops 而不是停机 */
    if (proc_current() != 0) {
        kernel_oops(f);
    }

    const char *name = (vec < 32U) ? exc_name[vec] : "unknown";

    if (vec == 14U) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        panic("CPU exception %u: %s\n"
              "  err=0x%08x eip=0x%08x cs=0x%08x eflags=0x%08x\n"
              "  eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n"
              "  esi=0x%08x edi=0x%08x ebp=0x%08x esp=0x%08x\n"
              "  ds=0x%08x es=0x%08x fs=0x%08x gs=0x%08x\n"
              "  cr2=0x%08x",
              vec, name, f->err, f->eip, f->cs, f->eflags,
              f->eax, f->ebx, f->ecx, f->edx,
              f->esi, f->edi, f->ebp, f->esp_dummy,
              f->ds, f->es, f->fs, f->gs, cr2);
    }

    panic("CPU exception %u: %s\n"
          "  err=0x%08x eip=0x%08x cs=0x%08x eflags=0x%08x\n"
          "  eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n"
          "  esi=0x%08x edi=0x%08x ebp=0x%08x esp=0x%08x\n"
          "  ds=0x%08x es=0x%08x fs=0x%08x gs=0x%08x",
          vec, name, f->err, f->eip, f->cs, f->eflags,
          f->eax, f->ebx, f->ecx, f->edx,
          f->esi, f->edi, f->ebp, f->esp_dummy,
          f->ds, f->es, f->fs, f->gs);
}
