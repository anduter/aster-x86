/*
 * ng WP1-3: GDT + TSS + IST。
 *
 * GDT 布局（选择子）：
 *   0x00 null
 *   0x08 内核代码（L=1, DPL=0）
 *   0x10 内核数据（DPL=0）
 *   0x18 用户数据（DPL=3）  <- SYSRET 的 SS = STAR[63:48] + 8
 *   0x20 用户代码（L=1, DPL=3）<- SYSRET 的 CS = STAR[63:48] + 16
 *   0x28 TSS（64 位 available TSS，占 0x28/0x30 两个 8 字节槽）
 *
 * SYSRET 的硬约束：CS = 基址 + 16、SS = 基址 + 8，所以“用户数据”必须紧跟在
 * “用户代码”前面（本表用 STAR 高 16 位 = 0x13，得到 SS=0x1b、CS=0x23）。
 *
 * TSS 提供：
 *   - RSP0：ring3 -> ring0 的异常/中断入口栈；
 *   - IST1：double fault 专用栈（IDT[8].IST=1）。
 *   - I/O map base 指到 TSS 末尾（无位图）=> ring3 执行 in/out 会 #GP。
 *
 * 本文件只装 GDT/TSS；IDT 与异常语义在 trap.c。
 */
#include "gdt.h"

#include "io.h"
#include "serial.h"

/* 64 位 TSS，布局由架构固定，必须 packed。 */
struct tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss64) == 104, "TSS64 must be 104 bytes");

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

/* 7 个 8 字节槽：TSS 描述符要 16 字节，占第 5、6 槽。 */
static u64 gdt[7] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static struct gdt_ptr gdtr;

/* IST1：double fault 独立栈；RSP0：从 ring3 进内核的栈。 */
static u8 ist1_stack[8192] __attribute__((aligned(16)));
static u8 rsp0_stack[16384] __attribute__((aligned(16)));

extern void gdt_load(struct gdt_ptr *gdtr);

/* 描述符低/高 8 字节（TSS 用），见 Intel SDM 3.5.3 / 3.5.5。 */
static u64 tss_desc_low(u64 base, u32 limit)
{
    u64 d = 0;

    d |= (u64)(limit & 0xFFFFu);
    d |= (base & 0xFFFFFFULL) << 16;
    d |= (u64)0x89 << 40; /* P=1, DPL=0, type=9 (64-bit available TSS) */
    d |= (u64)((limit >> 16) & 0xFu) << 48;
    d |= ((base >> 24) & 0xFFULL) << 56;
    return d;
}

void gdt_init(void)
{
    u64 base;
    u32 limit;
    u32 i;

    for (i = 0; i < 7; i++) {
        gdt[i] = 0;
    }

    /* 与 entry.S 的引导 GDT 相同的编码；用户段多了 DPL=3。 */
    gdt[1] = 0x00AF9A000000FFFFULL; /* kernel code, L=1 */
    gdt[2] = 0x00CF92000000FFFFULL; /* kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL; /* user data  (DPL=3, access 0xF2) */
    gdt[4] = 0x00AFFA000000FFFFULL; /* user code  (DPL=3, L=1, access 0xFA) */

    for (i = 0; i < (u32)sizeof(tss); i++) {
        ((u8 *)&tss)[i] = 0;
    }
    tss.rsp0 = (u64)(usize)rsp0_stack + sizeof(rsp0_stack);
    tss.ist[0] = (u64)(usize)ist1_stack + sizeof(ist1_stack);
    tss.iomap_base = (u16)sizeof(tss); /* 位图在 TSS 之外 => 无 I/O 权限 */

    base = (u64)(usize)&tss;
    limit = (u32)(sizeof(tss) - 1);
    gdt[5] = tss_desc_low(base, limit);
    gdt[6] = base >> 32;

    gdtr.limit = (u16)(sizeof(gdt) - 1);
    gdtr.base = (u64)(usize)&gdt[0];

    gdt_load(&gdtr);

    serial_puts("ng: gdt 7 entries (kcode/kdata/udata/ucode/tss), tr=");
    serial_puthex((u64)read_tr());
    serial_puts(" gdt=");
    serial_puthex((u64)(usize)&gdt[0]);
    serial_puts(" tss=");
    serial_puthex((u64)(usize)&tss);
    serial_puts("\r\n");
}

u64 gdt_tss_rsp0(void)
{
    return tss.rsp0;
}

void gdt_set_rsp0(u64 top)
{
    if (top == 0) {
        return;
    }
    tss.rsp0 = top;
}

u64 gdt_ist1_top(void)
{
    return tss.ist[0];
}
