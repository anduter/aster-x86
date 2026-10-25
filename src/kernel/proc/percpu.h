#ifndef CORE_PERCPU_H
#define CORE_PERCPU_H

#include "../include/types.h"

#define SMP_MAX_CPUS 4U

/* 每 CPU 状态：SMP 的第一块地基。 */
struct process;

typedef struct {
    uint32_t id;          /* 逻辑 CPU 号 0..SMP_MAX_CPUS-1 */
    uint32_t apic_id;     /* LAPIC ID */
    bool     online;
    uint32_t ticks;       /* 本 CPU 的时钟 tick */
    uint32_t ipi;         /* 本 CPU 收到的固定投递 IPI 数 */
    uint32_t tlb_seen;    /* 最近一次 TLB shootdown 序号 ack */
    uint32_t preempt_off; /* >0 表示禁止抢占（临界区） */
    bool     ready;       /* AP 已装好 per-CPU GDT/TSS 和共享 IDT */
    struct process *current; /* 本 CPU 当前进程 */
    void    *bootstrap_esp;  /* 本 CPU 第一次调度前的占位上下文 */
    uint32_t runq_mask;      /* 本 CPU 的 READY 进程位图 */
    bool     idle_active;    /* 本 CPU 正停在 idle 的 hlt 循环里（可被 timer 拉起） */
} per_cpu_t;

extern per_cpu_t per_cpu[SMP_MAX_CPUS];
extern uint32_t  smp_cpu_count;
extern uint32_t  smp_ap_ready;

/* 当前 CPU 的编号：SMP 之前恒为 0；接 AP 后用 GS/LAPIC ID 取 */
uint32_t     smp_cpu_id(void);
per_cpu_t   *this_cpu(void);

/* BSP 初始化（在中断打开前调用） */
void percpu_init_bsp(uint32_t apic_id, uint32_t discovered);

/* 关闭/恢复抢占计数（为将来每 CPU 调度预留） */
void preempt_disable(void);
void preempt_enable(void);

#endif
