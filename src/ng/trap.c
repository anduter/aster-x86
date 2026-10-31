/*
 * ng WP1-3: IDT / 异常分发 / 可恢复的受控探测 / panic。
 *
 * IDT 256 项全部填满：0..31 是独立 stub（有/无 error code 分开），32..255 指向
 * 一个默认 stub（本 WP 不接 IRQ，正常不会进入）。0x8（#DF）用 IST1 独立栈。
 *
 * 可恢复异常的做法：probe_ro_write()/probe_ud2()/probe_gp() 是故意触发异常的
 * 汇编小函数；handler 在 probe_active 期间核对“向量 + 故障 RIP（#PF 还要核对
 * CR2/error）”，命中就把 frame->rip 改成 *_after 标签，iretq 之后从下一条指令
 * 继续执行。未命中一律 panic —— 恢复逻辑只对受控探测开放。
 */
#include "trap.h"

#include "gdt.h"
#include "io.h"
#include "paging.h"
#include "serial.h"

/* 64 位中断门描述符（16 字节）。 */
struct idt_gate {
    u16 off_lo;
    u16 sel;
    u8 ist;
    u8 type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate) == 16, "IDT gate must be 16 bytes");

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

/*
 * 异常入口压栈后的帧，必须与 isr.S 的 push 顺序严格一致。
 * rsp/ss 只在异常来自更低特权级（CPL 切换）时有效。
 */
struct trap_frame {
    u64 rax;
    u64 rcx;
    u64 rdx;
    u64 rbx;
    u64 rbp;
    u64 rsi;
    u64 rdi;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 vector;
    u64 error;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

/* ---- 汇编侧符号 ----------------------------------------------------------- */
extern u64 isr_stub_table[32];
extern char isr_default[];
extern void probe_ro_write(u64 addr);
extern char probe_ro_write_after[];
extern void probe_ud2(void);
extern char probe_ud2_after[];
extern void probe_gp(void);
extern char probe_gp_load[];
extern char probe_gp_after[];
extern char __rodata_start[];

/* isr.S 的入口用它决定能不能执行 clac（SMAP=off 时 CLAC 会 #UD）。 */
int ng_clac_enabled;

static struct idt_gate idt[256] __attribute__((aligned(16)));
static struct idt_ptr idtr;

static volatile int probe_active;
static u64 probe_ro_addr;

static volatile int trap_pf_ok;
static volatile int trap_ud_ok;
static volatile int trap_gp_ok;
static volatile u64 trap_pf_cr2;
static volatile u64 trap_pf_error;
static volatile u64 trap_gp_error;

static int trap_fails;

/* ------------------------------------------------------------------------- */
/* IDT                                                                        */
/* ------------------------------------------------------------------------- */

static void idt_set(int vector, u64 handler, int ist, int dpl)
{
    idt[vector].off_lo = (u16)(handler & 0xFFFFu);
    idt[vector].sel = 0x08; /* 内核代码段 */
    idt[vector].ist = (u8)(ist & 7);
    idt[vector].type = (u8)(0x8E | ((dpl & 3) << 5)); /* P=1, interrupt gate */
    idt[vector].off_mid = (u16)((handler >> 16) & 0xFFFFu);
    idt[vector].off_hi = (u32)(handler >> 32);
    idt[vector].zero = 0;
}

int trap_init(void)
{
    int i;

    for (i = 0; i < 256; i++) {
        idt_set(i, (u64)(usize)isr_default, 0, 0);
    }
    for (i = 0; i < 32; i++) {
        idt_set(i, isr_stub_table[i], 0, 0);
    }
    /* double fault 走 IST1：即使内核栈坏了也能进 handler。 */
    idt_set(8, isr_stub_table[8], 1, 0);

    idtr.limit = (u16)(sizeof(idt) - 1);
    idtr.base = (u64)(usize)&idt[0];
    ng_clac_enabled = pg_smap_on();

    __asm__ volatile("lidt %0" : : "m"(idtr));

    serial_puts("ng: idt 256 entries, 32 exception stubs base=");
    serial_puthex(idtr.base);
    serial_puts("\r\n");
    serial_puts("ng: tss rsp0=");
    serial_puthex(gdt_tss_rsp0());
    serial_puts(" ist1=");
    serial_puthex(gdt_ist1_top());
    serial_puts(" idt8.ist=");
    serial_putdec((u64)idt[8].ist);
    serial_puts("\r\n");

    if (idt[8].ist != 1) {
        trap_fails++;
        serial_puts("ng: CHECK FAIL idt[8].ist\r\n");
    }
    if (gdt_ist1_top() == 0 || gdt_tss_rsp0() == 0) {
        trap_fails++;
        serial_puts("ng: CHECK FAIL tss stacks\r\n");
    }
    return trap_fails;
}

/* ------------------------------------------------------------------------- */
/* panic / 分发                                                               */
/* ------------------------------------------------------------------------- */

static void panic(struct trap_frame *f, u64 cr2)
{
    cpu_cli();
    serial_puts("PANIC: vec=");
    serial_puthex(f->vector);
    serial_puts(" err=");
    serial_puthex(f->error);
    serial_puts(" rip=");
    serial_puthex(f->rip);
    serial_puts(" cr2=");
    serial_puthex(cr2);
    serial_puts("\r\n");

    for (;;) {
        cpu_hlt();
    }
}

void trap_dispatch(struct trap_frame *f)
{
    u64 cr2 = read_cr2();

    if (ng_clac_enabled) {
        clac();
    }

    if (f->vector == 14) {
        trap_pf_cr2 = cr2;
        trap_pf_error = f->error;
        serial_puts("ng: #pf cr2=");
        serial_puthex(cr2);
        serial_puts(" error=");
        serial_puthex(f->error);
        serial_puts(" rip=");
        serial_puthex(f->rip);
        serial_puts("\r\n");

        if (probe_active && f->rip == (u64)(usize)probe_ro_write &&
            cr2 == probe_ro_addr) {
            f->rip = (u64)(usize)probe_ro_write_after;
            trap_pf_ok = 1;
            return;
        }
    } else if (f->vector == 6) {
        if (probe_active && f->rip == (u64)(usize)probe_ud2) {
            f->rip = (u64)(usize)probe_ud2_after;
            trap_ud_ok = 1;
            return;
        }
    } else if (f->vector == 13) {
        trap_gp_error = f->error;
        if (probe_active && f->rip == (u64)(usize)probe_gp_load) {
            f->rip = (u64)(usize)probe_gp_after;
            trap_gp_ok = 1;
            return;
        }
    }

    panic(f, cr2);
}

/* ------------------------------------------------------------------------- */
/* 受控探测自检                                                               */
/* ------------------------------------------------------------------------- */

int trap_selftest(void)
{
    int fails = 0;

    probe_active = 1;
    probe_ro_addr = (u64)(usize)__rodata_start;
    probe_ro_write(probe_ro_addr);
    probe_active = 0;

    if (trap_pf_ok && trap_pf_cr2 == probe_ro_addr && trap_pf_error == 0x3) {
        serial_puts("ng: #pf ro write handled ok\r\n");
    } else {
        fails++;
        serial_puts("ng: #pf ro write FAIL\r\n");
    }

    probe_active = 1;
    probe_ud2();
    probe_active = 0;

    if (trap_ud_ok) {
        serial_puts("ng: #ud handled ok\r\n");
    } else {
        fails++;
        serial_puts("ng: #ud FAIL\r\n");
    }

    probe_active = 1;
    probe_gp();
    probe_active = 0;

    if (trap_gp_ok) {
        serial_puts("ng: #gp handled ok\r\n");
    } else {
        fails++;
        serial_puts("ng: #gp FAIL\r\n");
    }

    serial_puts("ng: trap recovery pf=");
    serial_putdec((u64)trap_pf_ok);
    serial_puts(" ud=");
    serial_putdec((u64)trap_ud_ok);
    serial_puts(" gp=");
    serial_putdec((u64)trap_gp_ok);
    serial_puts(" gp_err=");
    serial_puthex(trap_gp_error);
    serial_puts("\r\n");

    return fails;
}
