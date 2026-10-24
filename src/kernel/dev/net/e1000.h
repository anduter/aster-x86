#ifndef CORE_E1000_H
#define CORE_E1000_H

#include "../../types.h"

/* Intel 82540EM（QEMU 默认网卡）驱动：
 * 探测 -> 映射 BAR0 -> 读 MAC -> 查链路 -> 初始化 TX/RX 环 ->
 * 中断注册（延迟武装）+ NAPI 式收包 -> TX 完成回收/watchdog -> 统计。
 *
 * 中断不可用（PCI 0x3C 为 0/0xFF 或 IOAPIC/PIC 路由失败）时自动回退纯轮询，
 * 所有 API 在两种模式下语义一致。 */

#define E1000_NAPI_BUDGET 8U

int  e1000_init(void);
int  e1000_present(void);
int  e1000_link_up(void);
void e1000_mac(uint8_t mac[6]);

/* 发一帧广播测试帧（不含协议栈），成功返回 0 */
int  e1000_send_test(void);

/* 发送任意以太网帧（不含 CRC），成功返回 0。
 * 发送是异步的：描述符交给硬件即返回，完成回收由 e1000_tx_reclaim() 做。 */
int  e1000_send_frame(const void *frame, uint32_t len);

/* 兼容旧轮询接口：收到一帧返回长度，没有帧返回 0。
 * 内部走 e1000_napi_poll()（预算 E1000_NAPI_BUDGET）。 */
int  e1000_rx_poll(void *buf, uint32_t max_len);

/* NAPI 式软轮询：按 budget 处理 RX 描述符、回收 TX、检查 watchdog，
 * 结束时重新武装 RDT 与 RX 中断。返回本次处理的 RX 描述符数。 */
int  e1000_napi_poll(uint32_t budget);

/* 回收已完成的 TX 描述符（DD 置位），返回本次释放的描述符数。 */
uint32_t e1000_tx_reclaim(void);

/* ethtool 式统计（driver 内部计数，不新增 syscall）。 */
typedef struct {
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_errors;
    uint32_t tx_reclaim;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_dropped;
    uint32_t rx_errors;
    uint32_t interrupts;
    uint32_t polls;
    uint32_t napi_budget_hits;
    uint32_t resets;
} e1000_stats_t;

void e1000_stats_get(e1000_stats_t *out);
/* 打印一行：net: e1000 stats rx=.. tx=.. irq=.. polls=.. drop=.. reset=.. */
void e1000_stats_print(void);

/* 当前 IRQ 线（-1 = 无/未用）与模式：0=纯轮询，1=PIC，2=IOAPIC。 */
int  e1000_irq_line(void);
int  e1000_irq_mode(void);

#endif
