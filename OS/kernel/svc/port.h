#ifndef IPC_PORT_H
#define IPC_PORT_H

#include "../include/types.h"

#define PORT_MAX          16U
#define PORT_PENDING_MAX  4U
#define PORT_E_AGAIN      (-6)

int  port_create(void);
int  port_connect(uint32_t port, uint32_t *client);
int  port_accept(uint32_t port, uint32_t *server);
/* 阻塞 accept：没有 pending 连接时阻塞，connect 唤醒。 */
int  port_accept_block(uint32_t port, uint32_t *server);
void port_release(uint32_t port);
void port_retain(uint32_t port);

#endif
