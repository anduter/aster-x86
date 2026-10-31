/*
 * ng WP1-2: 端口 IO / 控制寄存器 / MSR / CPUID / TLB 的小封装。
 *
 * 全部是 static inline，只有 include 它的 .c 才生成代码。
 * 约定：CR0/CR3/CR4 与 MSR 一律按 64 位读写（长模式下高半部分也必须保留）。
 */
#ifndef NG_IO_H
#define NG_IO_H

#include "types.h"

/* ---- 端口 IO（16550 串口用）---- */
static inline void outb(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ---- 控制寄存器 ---- */
static inline u64 read_cr0(void)
{
    u64 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(u64 v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline u64 read_cr2(void)
{
    u64 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline u64 read_cr3(void)
{
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(u64 v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline u64 read_cr4(void)
{
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(u64 v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* ---- MSR（EFER 等）---- */
static inline u64 rdmsr(u32 msr)
{
    u32 lo;
    u32 hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | (u64)lo;
}

static inline void wrmsr(u32 msr, u64 value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32))
                     : "memory");
}

/* ---- CPUID ---- */
static inline void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

/* ---- TLB ---- */
static inline void invlpg(u64 va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/*
 * SMAP 访问用户内存的窗口：内核（ring0）访问 U/S=1 的页面前必须 stac，
 * 访问完立刻 clac。约定见 docs/design/paging-ng.md 与 ADR-014。
 * （stac/clac 只在 CPUID 报 SMAP 时可用；SMAP=off 时执行它们仍合法。）
 */
static inline void stac(void)
{
    __asm__ volatile("stac" : : : "memory");
}

static inline void clac(void)
{
    __asm__ volatile("clac" : : : "memory");
}

/* ---- 停 CPU / 关中断 ---- */
static inline void cpu_hlt(void)
{
    __asm__ volatile("hlt");
}

static inline void cpu_cli(void)
{
    __asm__ volatile("cli" : : : "memory");
}

/* ---- 任务寄存器 / 段寄存器（TSS 装载后读回证据）---- */
static inline u16 read_tr(void)
{
    u16 v;
    __asm__ volatile("str %0" : "=r"(v));
    return v;
}

#endif /* NG_IO_H */
