#ifndef IPC_CHANNEL_H
#define IPC_CHANNEL_H

#include "../types.h"

#define CHAN_MAX       32U
#define CHAN_MSG_MAX   4U
#define CHAN_MSG_LEN   64U
#define CHAN_E_AGAIN   (-6)

/* 创建一对已连接的 channel endpoint；返回两个对象 id。 */
int chan_create_pair(uint32_t *a, uint32_t *b);
/* 发送到 peer 的队列；满返回 CHAN_E_AGAIN。 */
int chan_send(uint32_t id, const void *buf, uint32_t len);
/* 从自己的队列接收；空返回 CHAN_E_AGAIN。 */
int chan_recv(uint32_t id, void *buf, uint32_t max_len);
/* 阻塞 recv：队列空时阻塞当前进程，send 唤醒；返回长度或错误。 */
int chan_recv_block(uint32_t id, void *buf, uint32_t max_len);
/* native handle 关闭时释放一个引用。 */
void chan_release(uint32_t id);
/* handle 复制时增加一个引用。 */
void chan_retain(uint32_t id);

#endif
