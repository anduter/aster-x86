#ifndef DEV_HWCAP_H
#define DEV_HWCAP_H

/* =============================================================================
 * dev/hwcap.h - 硬件 capability 后端（MMIO / IRQ / DMA，ABI 1.5 的 89..92）
 *
 * 契约来源：docs/design/three-layer.md §3.3/§6。P1 的 SYS_CAP_GRANT 只把
 * (type,id,rights) 记在端口上，没有真实硬件后端；本模块补上：
 *
 *   - CAP_MMIO：SYS_MMIO_MAP(90) 把已授权的物理区间映射进调用者用户地址
 *     空间（页对齐、可读/可写、NX、MMIO 缓存属性）。cap id 在本实现里就是
 *     grant 的物理基址（SYS_CAP_GRANT 的 edx）。
 *   - CAP_IRQ：SYS_CAP_GRANT 把 IRQ 绑定到调用者端口；IRQ 到达时内核投递
 *     一个 HWCAP_EV_IRQ 事件到端口，并计数。SYS_IRQ_WAIT(89) 阻塞等待事件，
 *     返回 IRQ 号；等待成功即 ack（解除 pending、重新放行 IRQ）。
 *   - CAP_DMA：SYS_DMA_ALLOC(91) 分配物理连续页（<=64KB，4KB 对齐）、映射
 *     到调用者地址空间并返回 cap id；SYS_DMA_FREE(92) 解映射+释放。
 *
 * 安全边界：
 *   - 三类 cap 的“获取”仍由 SYS_CAP_GRANT 把关（只有 root 能授，见
 *     svc/ipc.c）；本模块只允许 cap 所在端口的 owner 使用它。
 *   - MMIO 映射强制 NX；只读 grant 映射成只读 PTE。
 *   - 无 IOMMU 时只允许使用 SYS_DMA_ALLOC 返回的内核分配缓冲；设备仍可能
 *     越界 DMA，netd 必须等 vIOMMU（设计 §6）。
 * ============================================================================= */

#include "../include/types.h"

struct process;

/* 用户地址空间里的硬件 cap 映射窗口。放在 user_layout 的 0x40500000 之上、
 * 3GB 之下；vmm 的 fork 克隆只覆盖 [0x40000000,0x40500000)，因此这些映射
 * 不会跨 fork 继承（驱动进程自己持有 cap，fork 后子进程要重新申请）。
 * 16MB 足够 256 页 MMIO 或 256 个 64KB DMA 缓冲。 */
#define HWCAP_VA_BASE        0x41000000U
#define HWCAP_VA_END         0x42000000U

/* 单次 MMIO 映射上限：256 页 = 1MB（netd 的 BAR 只有 128KB）。 */
#define HWCAP_MMIO_MAX_PAGES 256U
/* 单次 DMA 分配上限 64KB（设计 §6 的受限模式）。 */
#define HWCAP_DMA_MAX        65536U
/* 传统 IRQ 条数（与 proc/trap/interrupt.h 的 IRQ_MAX 一致）。 */
#define HWCAP_IRQ_MAX        16U
/* 用户态 ack 前允许堆积的事件数；超过就掩掉 IRQ 线，避免中断风暴。 */
#define HWCAP_IRQ_STORM_LIMIT 8U
/* IRQ 事件消息类型（payload: irq, seq, dropped 三个 u32，小端）。 */
#define HWCAP_EV_IRQ         0x49525131U   /* "IRQ1" */

/* SYS_IRQ_WAIT：timeout_ticks == 0 使用默认 100 tick（1s），上限 60000。 */
#define HWCAP_IRQ_WAIT_DEFAULT 100U
#define HWCAP_IRQ_WAIT_MAX     60000U

/* SYS_MMIO_MAP(90)：base 是 grant 的物理基址；成功把映射 VA 写到 uptr。 */
int  hwcap_mmio_map(uint32_t base, uint32_t uptr, struct process *caller);
/* 内核侧版本：直接返回 VA（自检/内部用；不做用户指针拷贝）。 */
int  hwcap_mmio_map_core(uint32_t base, uint32_t *out_va, struct process *caller);
/* 解除一次 MMIO 映射（仅自检/清理用；ABI 没有 MMIO_UNMAP）。 */
int  hwcap_mmio_unmap_core(uint32_t base, struct process *caller);

/* 把已授权的 IRQ 绑定到 port（SYS_CAP_GRANT 的内核侧动作）。 */
int  hwcap_irq_bind(uint32_t port, uint32_t irq, struct process *caller);
/* SYS_IRQ_WAIT(89)：成功返回 IRQ 号（>=0），超时 -EAGAIN，信号 -EINTR。 */
int  hwcap_irq_wait(uint32_t irq, uint32_t timeout_ticks, struct process *caller);
/* IRQ 投递钩子（proc/trap/irq.c 在 irq_dispatch 里调用）：
 * 返回 1 = 已由 hwcap 处理（跳过默认 handler），0 = 未绑定。 */
int  hwcap_irq_deliver(uint32_t irq);
/* 端口释放/回收时解绑该端口上的所有 IRQ（svc/ipc.c 调用）。 */
void hwcap_port_released(uint32_t port);
/* 周期性回收：owner 退出/端口消失时解绑 IRQ、清理 MMIO/DMA 记录。
 * 由 irq.c 的时钟中断每 10 tick 调用一次（进程退出没有 cap 模块回调）。 */
void hwcap_poll(void);

/* SYS_DMA_ALLOC(91)：返回 cap id（>0），VA 写到 uptr（uptr==0 表示不关心）。 */
int  hwcap_dma_alloc(uint32_t len, uint32_t align, uint32_t uptr,
                     struct process *caller);
int  hwcap_dma_alloc_core(uint32_t len, uint32_t align, uint32_t *out_va,
                          struct process *caller);
/* SYS_DMA_FREE(92)：成功 0；坏 cap/重复释放/非 owner 返回 -1。 */
int  hwcap_dma_free(uint32_t cap, struct process *caller);

/* -Test 构建里由 supervisor 线程在 ipc_selftest() 之后调用一次。 */
void hwcap_selftest(void);

#endif /* DEV_HWCAP_H */
