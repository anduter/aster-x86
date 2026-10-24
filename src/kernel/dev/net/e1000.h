#ifndef CORE_E1000_H
#define CORE_E1000_H

#include "../../types.h"

/* Intel 82540EM（QEMU 默认网卡）基础驱动：
 * 探测 -> 映射 BAR0 -> 读 MAC -> 查链路 -> 初始化 TX 环 -> 发一帧测试。
 * 不做 RX，也没有协议栈。 */
int  e1000_init(void);
int  e1000_present(void);
int  e1000_link_up(void);
void e1000_mac(uint8_t mac[6]);

/* 发一帧广播测试帧（不含协议栈），成功返回 0 */
int  e1000_send_test(void);

/* 发送任意以太网帧（不含 CRC），成功返回 0。 */
int  e1000_send_frame(const void *frame, uint32_t len);
/* 轮询 RX 环；收到一帧返回长度，没有帧返回 0。 */
int  e1000_rx_poll(void *buf, uint32_t max_len);

#endif
