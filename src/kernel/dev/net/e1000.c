/* =============================================================================
 * dev/net/e1000.c - Intel 82540EM 网卡驱动（中断 + NAPI 式收包 / TX 回收 / 统计）
 *
 * 相对 0.71.0 的改动（三条主线，对应 Linux e1000 的思路）：
 *   1. 中断 + NAPI：探测 PCI 0x3C 的 Interrupt Line，irq_register() 挂处理函数；
 *      硬中断只读 ICR 清中断、置 napi_scheduled、屏蔽 RX 中断源，然后返回；
 *      e1000_napi_poll(budget) 按预算收描述符并重新武装 RDT/IMS。
 *      为兼容既有轮询调用者，e1000_rx_poll() 保留并内部走 NAPI。
 *   2. TX 完成回收 + watchdog：发送不再原地自旋等完成，描述符带独立缓冲；
 *      轮询点读 DD 位回收（tx_head/tx_tail），长时间 TDH!=TDT 触发软复位。
 *   3. ethtool 式统计：TX/RX 包/字节/错误/丢包 + 中断/poll/预算/复位计数。
 *
 * 时序约束（重要）：e1000_init() 在 bootseq 的 devices 阶段，早于
 * interrupt_init()（idt_init + pic_remap + 注册 irq0）。此时若直接
 * irq_unmask()，中断会被随后的 pic_remap 全部屏蔽，甚至可能在 IDT 装好之前
 * 就投递进来。因此这里只注册、把"武装"延迟到中断基础设施就绪之后：
 * 以 irq_count(0) != 0（PIT 已经真正进过一次中断）作为就绪判据，
 * 在第一次 NAPI 轮询时完成 irq_unmask()/irq_migrate_to_ioapic()。
 * 若 IRQ 不可用则保持纯轮询，且绝不 panic。
 * ============================================================================= */
#include "e1000.h"
#include "../pci.h"
#include "../intc/apic.h"
#include "../intc/ioapic.h"
#include "include/mm.h"
#include "include/kernel.h"
#include "../../proc/trap/interrupt.h"
#include "../../core/error.h"
#include "../../lib/string.h"
#include "../../lib/kprintf.h"

/* ---- 寄存器偏移 ---- */
#define E1000_CTRL   0x0000U
#define E1000_STATUS 0x0008U
#define E1000_ICR    0x00C0U
#define E1000_IMS    0x00D0U
#define E1000_IMC    0x00D8U
#define E1000_RCTL   0x0100U
#define E1000_TCTL   0x0400U
#define E1000_TIPG   0x0410U
#define E1000_RDBAL  0x2800U
#define E1000_RDBAH  0x2804U
#define E1000_RDLEN  0x2808U
#define E1000_RDH    0x2810U
#define E1000_RDT    0x2818U
#define E1000_RDTR   0x2820U
#define E1000_TDBAL  0x3800U
#define E1000_TDBAH  0x3804U
#define E1000_TDLEN  0x3808U
#define E1000_TDH    0x3810U
#define E1000_TDT    0x3818U
#define E1000_RAL    0x5400U
#define E1000_RAH    0x5404U

#define E1000_CTRL_RST     0x04000000U   /* bit 26 */
#define E1000_CTRL_SLU     0x00000040U   /* bit 6  */
#define E1000_STATUS_LU    0x00000002U

#define E1000_TCTL_EN      0x00000002U
#define E1000_TCTL_PSP     0x00000008U
#define E1000_TCTL_CT      0x00000100U   /* collision threshold = 0x10 */
#define E1000_TCTL_COLD    0x00040000U   /* collision distance = 0x40 */

/* ICR / IMS / IMC 位（82540EM 数据手册 13.4.4/13.4.5/13.4.6） */
#define E1000_ICR_TXDW     0x00000001U
#define E1000_ICR_LSC      0x00000004U
#define E1000_ICR_RXSEQ    0x00000008U
#define E1000_ICR_RXDMT0   0x00000010U
#define E1000_ICR_RXO      0x00000040U
#define E1000_ICR_RXT0     0x00000080U

/* RX 相关中断源：链路变化 + 序列错误 + 描述符低水位 + 溢出 + 收包定时器。
 * 本驱动不使用 TXDW：TX 完成回收由轮询点做，避免每次发送都进中断。 */
#define E1000_RX_INT_MASK  (E1000_ICR_LSC | E1000_ICR_RXSEQ | \
                            E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0)

#define TX_DESC_COUNT 8U
#define TX_DESC_SIZE  16U
#define TX_BUF_SIZE   2048U              /* 每描述符独立缓冲（无 skb，用固定池） */
#define RX_DESC_COUNT 8U
#define RX_DESC_SIZE  16U
#define RX_BUF_SIZE   2048U
#define MAX_PCI_DEVS  16U

/* NAPI 预算：一次软轮询最多处理多少个 RX 描述符，与软件收包队列深度一致。 */
#define E1000_NAPI_BUDGET   8U
#define RX_FIFO_FRAMES      8U

/* TX watchdog：在途描述符连续 >200 tick（100Hz 下 2s）没有推进就软复位。 */
#define E1000_TX_WATCHDOG_TICKS 200U

#define E1000_DESC_STATUS_DD 0x01U

/* watchdog 复位策略：
 *  - 只在本轮 NAPI 处理里"标记"需要复位，退出 e1000_lock 后由
 *    e1000_reset_service() 做一次；
 *  - CTRL.RST 自清等待有界（20k 次 MMIO 读，毫秒级），远小于 NMI 看门狗
 *    约 1s 的无进展判定，避免在持锁 + 关中断路径里长时间不返回；
 *  - 复位后 5s（500 tick）冷却，防止硬件真坏时复位风暴。 */
#define E1000_CTRL_RST_POLL       20000U
#define E1000_TX_RESET_COOLDOWN   500U

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

static volatile uint32_t *bar0;
static uint8_t  mac[6];
static int      present;
static int      link;

/* TX：tx_head = 最老的未回收描述符，tx_tail = 下一个要交给硬件的槽位。
 * 发送后不原地等完成，由 e1000_tx_reclaim() 按 DD 位回收。 */
static e1000_tx_desc_t *tx_ring;
static uint8_t *tx_buffers;
static uint32_t tx_ring_phys;
static uint32_t tx_bufs_phys;
static uint32_t tx_head;
static uint32_t tx_tail;
static uint32_t tx_stall_since;          /* 在途帧开始积压的 tick（0=没有积压） */
static volatile int tx_reset_pending;    /* 1 = 已判定需要 watchdog 复位，待锁外执行 */
static uint32_t tx_reset_cooldown;       /* 复位后到该 tick 之前不再触发 */

/* RX */
static e1000_rx_desc_t *rx_ring;
static uint8_t *rx_buffers;
static uint32_t rx_ring_phys;
static uint32_t rx_bufs_phys;
static uint32_t rx_next;

/* NAPI 软队列：e1000_napi_poll() 按预算把描述符搬进这里，
 * e1000_rx_poll() 从队列取一帧交给协议栈，语义与旧轮询接口一致。 */
static struct {
    uint8_t  data[RX_FIFO_FRAMES][RX_BUF_SIZE];
    uint16_t len[RX_FIFO_FRAMES];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} rx_fifo;

/* IRQ / NAPI 状态 */
static int           irq_line = -1;
static int           irq_registered;
static int           irq_armed;
static int           irq_ioapic;
static int           irq_first_logged;
static volatile int  napi_scheduled;

static e1000_stats_t stats;
static int            reclaim_logged;
static spinlock_t     e1000_lock = SPINLOCK_INIT;

static uint32_t reg_read(uint32_t off) { return bar0[off / 4U]; }
static void     reg_write(uint32_t off, uint32_t v) { bar0[off / 4U] = v; }

static uint32_t e1000_tx_reclaim_locked(void);

static int pci_find_e1000(pci_dev_t *out)
{
    for (uint32_t i = 0; i < pci_count(); i++) {
        const pci_dev_t *d = pci_at(i);

        if (d == 0) continue;
        if (d->vendor == 0x8086U &&
            (d->device == 0x100EU || d->device == 0x100FU || d->device == 0x10D3U)) {
            if (out != 0) *out = *d;
            return 1;
        }
    }
    return 0;
}

/* 读 PCI 配置空间的 BAR0（偏移 0x10） */
static uint32_t pci_bar0(const pci_dev_t *d)
{
    uint32_t addr = 0x80000000U
                  | ((uint32_t)d->bus << 16)
                  | ((uint32_t)d->dev << 11)
                  | ((uint32_t)d->func << 8)
                  | 0x10U;

    __asm__ volatile("outl %0, %1" :: "a"(addr), "d"((unsigned short)0xCF8));
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "d"((unsigned short)0xCFC));
    return val & 0xFFFFFFF0U;
}

static uint32_t pci_cfg_addr(const pci_dev_t *d, uint32_t off)
{
    return 0x80000000U
         | ((uint32_t)d->bus << 16)
         | ((uint32_t)d->dev << 11)
         | ((uint32_t)d->func << 8)
         | (off & 0xFCU);
}

static uint32_t pci_read32(const pci_dev_t *d, uint32_t off)
{
    uint32_t val;

    __asm__ volatile("outl %0, %1" :: "a"(pci_cfg_addr(d, off)), "d"((unsigned short)0xCF8));
    __asm__ volatile("inl %1, %0" : "=a"(val) : "d"((unsigned short)0xCFC));
    return val;
}

static void pci_write32(const pci_dev_t *d, uint32_t off, uint32_t val)
{
    __asm__ volatile("outl %0, %1" :: "a"(pci_cfg_addr(d, off)), "d"((unsigned short)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(val), "d"((unsigned short)0xCFC));
}

/* 打开 PCI 命令寄存器里的"内存空间 + 总线主控"。
 * 没有 bus master，网卡读不到描述符、也写不回接收缓冲：
 * 表现就是 TX 的 TDH 照常推进（描述符被取走），但帧根本没上线。 */
static uint32_t pci_enable_dma(const pci_dev_t *d)
{
    uint32_t cmd = pci_read32(d, 0x04U);

    cmd |= 0x0006U;                 /* bit1 = MEMORY, bit2 = BUS MASTER */
    pci_write32(d, 0x04U, cmd);
    return pci_read32(d, 0x04U);
}

/* ==================== 环初始化（init 与 watchdog 复位共用） ==================== */
static void e1000_ring_init(void)
{
    uint32_t i;

    /* ---- TX：8 个描述符，每个指向自己的固定缓冲 ---- */
    memset(tx_ring, 0, 4096);
    for (i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr = tx_bufs_phys + i * TX_BUF_SIZE;
        tx_ring[i].status = 0;
    }
    tx_head = 0;
    tx_tail = 0;
    tx_stall_since = 0;

    reg_write(E1000_TDBAL, tx_ring_phys);
    reg_write(E1000_TDBAH, 0);
    reg_write(E1000_TDLEN, TX_DESC_COUNT * TX_DESC_SIZE);
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);

    /* ---- RX：8 个描述符 + 8 个 2KB DMA 缓冲 ---- */
    memset(rx_ring, 0, 4096);
    for (i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr = rx_bufs_phys + i * RX_BUF_SIZE;
        rx_ring[i].status = 0;
    }
    rx_next = 0;
    rx_fifo.head = 0;
    rx_fifo.tail = 0;
    rx_fifo.count = 0;

    reg_write(E1000_RCTL, 0);
    reg_write(E1000_RDBAL, rx_ring_phys);
    reg_write(E1000_RDBAH, 0);
    reg_write(E1000_RDLEN, RX_DESC_COUNT * RX_DESC_SIZE);
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, RX_DESC_COUNT - 1U);
    reg_write(E1000_RDTR, 0);              /* 不用收包定时器，中断靠 RXT0/RXDMT0 */
    reg_write(E1000_RCTL, (1U << 1) | (1U << 2) | (1U << 3) |
                          (1U << 4) | (1U << 15));

    /* 环就绪后最后再打开发送单元（顺序对某些固件版本有影响） */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_SLU);
    reg_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                          E1000_TCTL_CT | E1000_TCTL_COLD);
    reg_write(E1000_TIPG, 0x0060200AU);

    /* 中断源默认全关：等 e1000_irq_arm() 真正武装后再打开 RX 中断 */
    reg_write(E1000_IMC, 0xFFFFFFFFU);
}

/* watchdog 软复位：重新初始化环和寄存器，保留 DMA 页与 IRQ 注册。
 * 调用者持有 e1000_lock；等待循环有界（毫秒级），不得长时间阻塞。 */
static void e1000_reset_locked(void)
{
    uint32_t spin;
    uint32_t dropped;
    uint32_t lost_rx;

    /* 先尽量回收已完成帧，剩下的才算真正丢弃 */
    (void)e1000_tx_reclaim_locked();
    dropped = (tx_tail + TX_DESC_COUNT - tx_head) % TX_DESC_COUNT;
    lost_rx = rx_fifo.count;

    stats.resets++;
    stats.tx_errors += dropped;
    stats.rx_dropped += lost_rx;

    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_RST);
    /* CTRL.RST 在 QEMU 上立即自清：最多 20k 次读。旧实现 2M 次读在
     * 持锁 + IF=0 的 NAPI 路径里可能超过 NMI 看门狗 1s 判定。 */
    for (spin = 0; spin < E1000_CTRL_RST_POLL; spin++) {
        if ((reg_read(E1000_CTRL) & E1000_CTRL_RST) == 0U) break;
    }

    napi_scheduled = 0;
    tx_reset_pending = 0;
    e1000_ring_init();
    tx_stall_since = 0;
    tx_reset_cooldown = timer_ticks() + E1000_TX_RESET_COOLDOWN;
    link = (reg_read(E1000_STATUS) & E1000_STATUS_LU) != 0U;
    if (irq_armed) reg_write(E1000_IMS, E1000_RX_INT_MASK);

    kprintf("net: e1000 tx watchdog reset done (dropped_tx=%u dropped_rx=%u reset=%u)\n",
            (unsigned int)dropped, (unsigned int)lost_rx, (unsigned int)stats.resets);
}

/* 锁外执行一次挂起的复位：NAPI/发送路径只标记，真正复位在这里做。 */
static void e1000_reset_service(void)
{
    uint32_t flags;

    if (!present) return;
    flags = spin_lock_irqsave(&e1000_lock);
    if (tx_reset_pending) e1000_reset_locked();
    spin_unlock_irqrestore(&e1000_lock, flags);
}

/* ==================== IRQ：注册与延迟武装 ==================== */
static void e1000_irq_handler(void *arg)
{
    uint32_t icr;

    (void)arg;
    if (!present || bar0 == 0) return;

    icr = reg_read(E1000_ICR);              /* 读 ICR 即清中断原因 */
    stats.interrupts++;

    /* 硬中断只做三件事：清中断、记 NAPI 待调度、屏蔽 RX 中断源。
     * 收包/回收/重新武装都留给 e1000_napi_poll()。 */
    reg_write(E1000_IMC, E1000_RX_INT_MASK);
    napi_scheduled = 1;

    if (!irq_first_logged) {
        irq_first_logged = 1;
        kprintf("net: e1000 irq: first interrupt icr=%08x, napi scheduled\n",
                (unsigned int)icr);
    }
}

/* 中断基础设施是否已经就绪：interrupt_init() 先注册 irq0(timer) 再开中断，
 * 所以 PIT 至少进过一次中断就说明 IDT/PIC 重映射都完成了。 */
static int e1000_irq_ready(void)
{
    return irq_count(0U) != 0U;
}

/* 首次真正打开中断。调用者需持有 e1000_lock。 */
static void e1000_irq_arm_locked(void)
{
    int routed = 0;

    if (!irq_registered || irq_armed) return;
    if (irq_line < 0) return;
    if (!e1000_irq_ready()) return;

    if (ioapic_present() && apic_base() != 0U) {
        if (irq_migrate_to_ioapic((unsigned int)irq_line, apic_id_from_cpuid()) == E_OK) {
            routed = 1;
        }
    }
    if (!routed) {
        if (irq_unmask((unsigned int)irq_line) != E_OK) {
            (void)irq_unregister((unsigned int)irq_line);
            irq_registered = 0;
            kprintln("net: e1000 irq arm failed, fallback polling");
            return;
        }
    }

    irq_armed = 1;
    irq_ioapic = routed;
    reg_write(E1000_IMS, E1000_RX_INT_MASK);
    kprintf("net: e1000 irq armed (%s line=%d)\n",
            routed ? "ioapic" : "pic", irq_line);
}

/* ==================== TX：回收 + watchdog ==================== */
static uint32_t e1000_tx_reclaim_locked(void)
{
    uint32_t freed = 0;
    uint32_t pending;
    uint32_t hw_done;
    uint32_t i = 0;

    if (!present || tx_ring == 0) return 0;

    pending = (tx_tail + TX_DESC_COUNT - tx_head) % TX_DESC_COUNT;
    if (pending == 0U) return 0;

    /* 以 DD 为完成判据；部分实现不回写 DD，则用 TDH 兜底：
     * TDH 已经越过的描述符说明硬件取走了（与旧驱动"TDH==TDT 视为完成"
     * 的判据一致）。 */
    hw_done = (reg_read(E1000_TDH) + TX_DESC_COUNT - tx_head) % TX_DESC_COUNT;
    if (hw_done > pending) hw_done = pending;

    while (tx_head != tx_tail && i < pending) {
        e1000_tx_desc_t *d = &tx_ring[tx_head];

        if ((d->status & E1000_DESC_STATUS_DD) == 0U && hw_done <= i) break;
        d->status = 0;
        tx_head = (tx_head + 1U) % TX_DESC_COUNT;
        freed++;
        i++;
    }

    if (freed != 0U) {
        stats.tx_reclaim += freed;
        if (!reclaim_logged) {
            reclaim_logged = 1;
            kprintf("net: e1000 tx reclaim ok (freed=%u)\n", (unsigned int)freed);
        }
    }
    if (tx_head == tx_tail) tx_stall_since = 0;
    return freed;
}

/* 在途帧连续超过阈值（200 tick = 2s）没有推进就标记软复位。
 * 这里只标记并打印 (deferred)，复位在退出 e1000_lock 后由
 * e1000_reset_service() 执行，避免在收包路径里持锁做长时间硬件操作。
 * tick 尚未启动（boot 期）时不做判断。 */
static void e1000_tx_watchdog_locked(void)
{
    uint32_t ticks = timer_ticks();
    uint32_t pending = (tx_tail + TX_DESC_COUNT - tx_head) % TX_DESC_COUNT;

    if (pending == 0U) {
        tx_stall_since = 0;
        return;
    }
    if (ticks == 0U) return;                 /* boot 期 timer_ticks() 不前进 */
    if (tx_reset_pending) return;
    if (tx_reset_cooldown != 0U && ticks < tx_reset_cooldown) return;
    if (tx_stall_since == 0U) {              /* 本轮积压的起点 */
        tx_stall_since = ticks;
        return;
    }
    if ((ticks - tx_stall_since) <= E1000_TX_WATCHDOG_TICKS) return;

    tx_reset_pending = 1;
    kprintln("net: e1000 tx watchdog reset (deferred)");
}

/* ==================== NAPI 式收包 ==================== */
static int e1000_napi_poll_locked(uint32_t budget)
{
    uint32_t done = 0;

    if (!present || rx_ring == 0) return 0;

    e1000_irq_arm_locked();
    stats.polls++;

    if (tx_tail != tx_head) {
        (void)e1000_tx_reclaim_locked();
        e1000_tx_watchdog_locked();
    } else {
        tx_stall_since = 0;
    }

    if (budget == 0U) budget = 1U;

    while (done < budget) {
        e1000_rx_desc_t *d = &rx_ring[rx_next];
        uint32_t len;

        if ((d->status & E1000_DESC_STATUS_DD) == 0U) break;

        if (d->errors != 0U) stats.rx_errors++;
        len = d->length;
        if (len > RX_BUF_SIZE) {
            len = RX_BUF_SIZE;
            stats.rx_errors++;
        }

        if (rx_fifo.count == RX_FIFO_FRAMES) {
            stats.rx_dropped++;              /* 软队列满：丢新帧，但照样归还描述符 */
        } else {
            uint32_t tail = rx_fifo.tail;

            memcpy(rx_fifo.data[tail], (const void *)(uintptr_t)d->addr, len);
            rx_fifo.len[tail] = (uint16_t)len;
            rx_fifo.tail = (tail + 1U) % RX_FIFO_FRAMES;
            rx_fifo.count++;
            stats.rx_packets++;
            stats.rx_bytes += len;
        }

        d->status = 0;
        reg_write(E1000_RDT, rx_next);       /* 把描述符还给硬件 */
        rx_next = (rx_next + 1U) % RX_DESC_COUNT;
        done++;
    }

    /* 预算用尽但环里还有帧：记账，下一次轮询继续。 */
    if (done == budget && (rx_ring[rx_next].status & E1000_DESC_STATUS_DD) != 0U) {
        stats.napi_budget_hits++;
    }

    /* 处理完重新武装 RDT（上面已逐个归还）并重新打开 RX 中断。 */
    if (irq_armed && napi_scheduled) {
        napi_scheduled = 0;
        reg_write(E1000_ICR, 0xFFFFFFFFU);   /* 清掉期间累积的原因 */
        reg_write(E1000_IMS, E1000_RX_INT_MASK);
    }
    return (int)done;
}

int e1000_napi_poll(uint32_t budget)
{
    uint32_t flags;
    int done;
    int need_reset;

    if (!present) return 0;
    flags = spin_lock_irqsave(&e1000_lock);
    done = e1000_napi_poll_locked(budget);
    need_reset = tx_reset_pending;
    spin_unlock_irqrestore(&e1000_lock, flags);

    /* watchdog 只标记；退出锁后再做一次有界复位（见 e1000_reset_locked） */
    if (need_reset) e1000_reset_service();
    return done;
}

int e1000_init(void)
{
    pci_dev_t d;
    uint32_t bar;

    present = 0;
    link = 0;

    if (!pci_find_e1000(&d)) {
        kprintln("e1000: not found");
        return E_NOENT;
    }

    {
        uint32_t cmd = pci_enable_dma(&d);

        kprintf("e1000: PCI CMD=%x (memory=%u busmaster=%u)\n",
                (unsigned int)cmd,
                (unsigned int)((cmd >> 1) & 1U), (unsigned int)((cmd >> 2) & 1U));
    }

    bar = pci_bar0(&d);
    if (bar == 0U) {
        kprintln("e1000: BAR0 is zero");
        return E_INVAL;
    }

    if (vmm_map_mmio(bar, 0x20000U) != 0) {
        kprintln("e1000: cannot map BAR0");
        return E_NOSPC;
    }
    bar0 = (volatile uint32_t *)(uintptr_t)bar;

    /* 先复位设备：BIOS/固件可能留下了半初始化的状态。
     * 等待有界（与 watchdog 复位共用同一上限），boot 期不会卡住启动。 */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | E1000_CTRL_RST);
    for (volatile uint32_t spin = 0; spin < E1000_CTRL_RST_POLL; spin++) {
        if ((reg_read(E1000_CTRL) & E1000_CTRL_RST) == 0U) break;
    }

    /* MAC 在 RAL/RAH（复位后由 EEPROM 自动装入） */
    {
        uint32_t ral = reg_read(E1000_RAL);
        uint32_t rah = reg_read(E1000_RAH);

        mac[0] = (uint8_t)(ral & 0xFF);
        mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        mac[4] = (uint8_t)(rah & 0xFF);
        mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    }

    /* DMA 页：TX 描述符页 + 8 个独立发送缓冲（回收的安全前提）+ RX 环 + 收包缓冲 */
    {
        void *tring = pmm_alloc_pages(1);
        void *tbufs = pmm_alloc_pages((TX_DESC_COUNT * TX_BUF_SIZE) / PAGE_SIZE);
        void *rring = pmm_alloc_pages(1);
        void *rbufs = pmm_alloc_pages((RX_DESC_COUNT * RX_BUF_SIZE) / PAGE_SIZE);

        if (tring == 0 || tbufs == 0 || rring == 0 || rbufs == 0) {
            kprintln("e1000: no DMA pages");
            if (tring != 0) pmm_free_pages(tring, 1);
            if (tbufs != 0) pmm_free_pages(tbufs, (TX_DESC_COUNT * TX_BUF_SIZE) / PAGE_SIZE);
            if (rring != 0) pmm_free_pages(rring, 1);
            if (rbufs != 0) pmm_free_pages(rbufs, (RX_DESC_COUNT * RX_BUF_SIZE) / PAGE_SIZE);
            return E_NOSPC;
        }

        tx_ring = (e1000_tx_desc_t *)tring;
        tx_ring_phys = (uint32_t)(uintptr_t)tring;
        tx_buffers = (uint8_t *)tbufs;
        tx_bufs_phys = (uint32_t)(uintptr_t)tbufs;
        rx_ring = (e1000_rx_desc_t *)rring;
        rx_ring_phys = (uint32_t)(uintptr_t)rring;
        rx_buffers = (uint8_t *)rbufs;
        rx_bufs_phys = (uint32_t)(uintptr_t)rbufs;
    }

    memset(tx_buffers, 0, TX_DESC_COUNT * TX_BUF_SIZE);
    memset(rx_buffers, 0, RX_DESC_COUNT * RX_BUF_SIZE);
    memset(&stats, 0, sizeof(stats));
    tx_reset_pending = 0;
    tx_reset_cooldown = 0;
    tx_stall_since = 0;
    e1000_ring_init();

    link = (reg_read(E1000_STATUS) & E1000_STATUS_LU) != 0U;

    present = 1;
    kprintf("e1000: MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            link ? "up" : "down");
    kprintf("net: e1000 napi budget=%u rx_ring=%u tx_ring=%u\n",
            (unsigned int)E1000_NAPI_BUDGET, (unsigned int)RX_DESC_COUNT,
            (unsigned int)TX_DESC_COUNT);

    /* PCI 配置空间 0x3C = Interrupt Line（SeaBIOS 已按 PIRQ 路由写入，QEMU 通常 11）。
     * 这里只注册处理函数；真正 unmask/路由延后到中断基础设施就绪之后。 */
    irq_line = (int)(pci_read32(&d, 0x3CU) & 0xFFU);
    irq_registered = 0;
    irq_armed = 0;
    irq_ioapic = 0;
    irq_first_logged = 0;
    napi_scheduled = 0;

    /* 0=无路由，1/4/12 在 interrupt_init() 里分别归 keyboard/serial/mouse，
     * 2 是从片级联：这些线一律不抢，保持纯轮询。 */
    if (irq_line <= 0 || irq_line >= 0xFF || irq_line >= (int)IRQ_MAX ||
        irq_line == 1 || irq_line == 2 || irq_line == 4 || irq_line == 12) {
        kprintln("net: e1000 irq unavailable, fallback polling");
        irq_line = -1;
    } else if (irq_register((unsigned int)irq_line, e1000_irq_handler, 0, "e1000") != E_OK) {
        kprintln("net: e1000 irq unavailable, fallback polling");
        irq_line = -1;
    } else {
        irq_registered = 1;
        kprintf("net: e1000 irq line=%d registered (arm deferred until irq0 fires)\n",
                irq_line);
    }
    return E_OK;
}

int e1000_present(void) { return present; }
int e1000_link_up(void) { return link; }
int e1000_irq_line(void) { return irq_line; }

/* 0 = 纯轮询；1 = 8259 PIC；2 = IOAPIC -> BSP LAPIC */
int e1000_irq_mode(void)
{
    if (!irq_armed) return 0;
    return irq_ioapic ? 2 : 1;
}

void e1000_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

/* 发一帧以太网帧（不含 CRC）。发送是异步的：描述符交给硬件后立即返回，
 * 完成与缓冲回收由 e1000_tx_reclaim() 在轮询点处理。 */
int e1000_send_frame(const void *frame, uint32_t len)
{
    e1000_tx_desc_t *d;
    uint32_t n = len;
    uint32_t slot;
    uint32_t flags;

    if (!present || tx_ring == 0 || tx_buffers == 0 || frame == 0) return E_NOENT;
    if (n < 60U) n = 60U;
    if (n > TX_BUF_SIZE) return E_INVAL;

    /* 已判定需要复位：先把复位做完（退出锁、有界），不要把新帧发进将死的环 */
    if (tx_reset_pending) e1000_reset_service();

    flags = spin_lock_irqsave(&e1000_lock);

    /* 环满：先回收一轮；仍满就丢弃并计数（不再原地自旋）。 */
    if (((tx_tail + 1U) % TX_DESC_COUNT) == tx_head) {
        (void)e1000_tx_reclaim_locked();
    }
    if (((tx_tail + 1U) % TX_DESC_COUNT) == tx_head) {
        stats.tx_errors++;
        spin_unlock_irqrestore(&e1000_lock, flags);
        return E_NOSPC;
    }

    slot = tx_tail;
    memset(tx_buffers + slot * TX_BUF_SIZE, 0, n);
    memcpy(tx_buffers + slot * TX_BUF_SIZE, frame, len);

    d = &tx_ring[slot];
    d->addr = tx_bufs_phys + slot * TX_BUF_SIZE;
    d->length = (uint16_t)n;
    d->cso = 0;
    d->cmd = 0x0BU;                     /* EOP | IFCS | RS：写完回写 DD */
    d->status = 0;
    d->css = 0;
    d->special = 0;

    tx_tail = (tx_tail + 1U) % TX_DESC_COUNT;
    mb();                               /* 描述符先可见，再敲门铃 TDT */
    reg_write(E1000_TDT, tx_tail);

    stats.tx_packets++;
    stats.tx_bytes += n;

    spin_unlock_irqrestore(&e1000_lock, flags);
    return E_OK;
}

/* 轮询 RX：先看 NAPI 软队列，没有就按预算跑一次 e1000_napi_poll()。
 * 保持旧接口语义：收到一帧返回长度，没有帧返回 0。 */
int e1000_rx_poll(void *buf, uint32_t max_len)
{
    uint32_t flags;
    uint32_t len;
    int need_reset;

    if (!present || buf == 0) return 0;

    flags = spin_lock_irqsave(&e1000_lock);
    if (rx_fifo.count == 0U) {
        (void)e1000_napi_poll_locked(E1000_NAPI_BUDGET);
    }
    need_reset = tx_reset_pending;
    if (rx_fifo.count == 0U) {
        spin_unlock_irqrestore(&e1000_lock, flags);
        if (need_reset) e1000_reset_service();
        return 0;
    }

    len = rx_fifo.len[rx_fifo.head];
    if (len > max_len) len = max_len;
    memcpy(buf, rx_fifo.data[rx_fifo.head], len);
    rx_fifo.head = (rx_fifo.head + 1U) % RX_FIFO_FRAMES;
    rx_fifo.count--;
    spin_unlock_irqrestore(&e1000_lock, flags);

    if (need_reset) e1000_reset_service();
    return (int)len;
}

uint32_t e1000_tx_reclaim(void)
{
    uint32_t flags;
    uint32_t freed;

    if (!present) return 0;
    flags = spin_lock_irqsave(&e1000_lock);
    freed = e1000_tx_reclaim_locked();
    spin_unlock_irqrestore(&e1000_lock, flags);
    return freed;
}

void e1000_stats_get(e1000_stats_t *out)
{
    uint32_t flags;

    if (out == 0) return;
    flags = spin_lock_irqsave(&e1000_lock);
    *out = stats;
    spin_unlock_irqrestore(&e1000_lock, flags);
}

/* ethtool 式一行统计：net_init() 末尾打印，供测试脚本断言。 */
void e1000_stats_print(void)
{
    e1000_stats_t s;

    if (!present) return;
    e1000_stats_get(&s);
    kprintf("net: e1000 stats rx=%u tx=%u irq=%u polls=%u drop=%u reset=%u "
            "rx_bytes=%u tx_bytes=%u rx_err=%u tx_err=%u reclaim=%u budget_hits=%u\n",
            (unsigned int)s.rx_packets, (unsigned int)s.tx_packets,
            (unsigned int)s.interrupts, (unsigned int)s.polls,
            (unsigned int)s.rx_dropped, (unsigned int)s.resets,
            (unsigned int)s.rx_bytes, (unsigned int)s.tx_bytes,
            (unsigned int)s.rx_errors, (unsigned int)s.tx_errors,
            (unsigned int)s.tx_reclaim, (unsigned int)s.napi_budget_hits);
}

int e1000_send_test(void)
{
    uint8_t f[60];                      /* 以太网最小帧 */
    uint32_t len = 60;

    if (!present || tx_ring == 0) return E_NOENT;

    memset(f, 0, sizeof(f));
    for (int i = 0; i < 6; i++) f[i] = 0xFFU;        /* 广播 */
    for (int i = 0; i < 6; i++) f[6 + i] = mac[i];   /* 源 MAC */
    f[12] = 0x88U; f[13] = 0xB5U;                    /* 类型 */
    for (int i = 14; i < 20; i++) f[i] = (uint8_t)('A' + i - 14);

    if (e1000_send_frame(f, len) != E_OK) {
        kprintln("e1000: TX timeout");
        return E_INVAL;
    }

    /* 启动期没有中断/NAPI 调度，这里做一次有界自旋等 DD 回写，
     * 保持原日志断言（旧实现也接受 TDH==TDT 的完成判据）。 */
    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        if ((tx_ring[tx_head].status & E1000_DESC_STATUS_DD) != 0U) break;
        __asm__ volatile("pause");
    }
    if ((tx_ring[tx_head].status & E1000_DESC_STATUS_DD) == 0U) {
        kprintln("e1000: TX timeout");
        return E_INVAL;
    }

    (void)e1000_tx_reclaim();
    kprintf("e1000: TX test frame sent (%u bytes, DD set)\n", (unsigned int)len);
    return E_OK;
}
