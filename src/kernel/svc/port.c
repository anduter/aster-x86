/* =============================================================================
 * svc/port.c - 最小服务监听端点
 *
 * Port 只保存 pending 的 server endpoint。connect 创建一对 channel，
 * 把 server 端挂到 port，client 端返回给调用者；accept 取出 server 端。
 * 没有阻塞 accept、命名空间和服务注册表，这些留给 supervisor。
 * ============================================================================= */
#include "port.h"
#include "include/svc.h"
#include "include/kernel.h"
#include "include/proc.h"

#define PORT_WAIT_KEY(id) (0x504F0000U | ((id) & 0xFFFFU))

typedef struct {
    bool     used;
    uint32_t refs;
    uint32_t pending[PORT_PENDING_MAX];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} port_t;

static port_t ports[PORT_MAX];
static spinlock_t port_lock = SPINLOCK_INIT;

int port_create(void)
{
    uint32_t flags;
    int id = -1;

    flags = spin_lock_irqsave(&port_lock);
    for (uint32_t i = 0; i < PORT_MAX; i++) {
        if (!ports[i].used) {
            memset(&ports[i], 0, sizeof(ports[i]));
            ports[i].used = true;
            ports[i].refs = 1;
            id = (int)i;
            break;
        }
    }
    spin_unlock_irqrestore(&port_lock, flags);
    return id;
}

int port_connect(uint32_t port, uint32_t *client)
{
    uint32_t flags;
    uint32_t a, b;

    if (port >= PORT_MAX || client == 0) return -1;
    if (chan_create_pair(&a, &b) != 0) return -1;

    flags = spin_lock_irqsave(&port_lock);
    if (!ports[port].used || ports[port].count >= PORT_PENDING_MAX) {
        spin_unlock_irqrestore(&port_lock, flags);
        chan_release(a);
        chan_release(b);
        return PORT_E_AGAIN;
    }
    ports[port].pending[ports[port].tail] = b;
    ports[port].tail = (ports[port].tail + 1U) % PORT_PENDING_MAX;
    ports[port].count++;
    spin_unlock_irqrestore(&port_lock, flags);

    proc_wakeup(PORT_WAIT_KEY(port));
    *client = a;
    return 0;
}

int port_accept(uint32_t port, uint32_t *server)
{
    uint32_t flags;

    if (port >= PORT_MAX || server == 0) return -1;
    flags = spin_lock_irqsave(&port_lock);
    if (!ports[port].used || ports[port].count == 0U) {
        spin_unlock_irqrestore(&port_lock, flags);
        return PORT_E_AGAIN;
    }
    *server = ports[port].pending[ports[port].head];
    ports[port].head = (ports[port].head + 1U) % PORT_PENDING_MAX;
    ports[port].count--;
    spin_unlock_irqrestore(&port_lock, flags);
    return 0;
}

int port_accept_block(uint32_t port, uint32_t *server)
{
    int r;

    if (port >= PORT_MAX) return -1;
    for (;;) {
        r = port_accept(port, server);
        if (r != PORT_E_AGAIN) return r;
        proc_block(PORT_WAIT_KEY(port));
    }
}

void port_release(uint32_t port)
{
    uint32_t flags;

    if (port >= PORT_MAX) return;
    flags = spin_lock_irqsave(&port_lock);
    if (ports[port].used) {
        if (ports[port].refs > 1U) {
            ports[port].refs--;
        } else {
            while (ports[port].count != 0U) {
                uint32_t s = ports[port].pending[ports[port].head];
                ports[port].head = (ports[port].head + 1U) % PORT_PENDING_MAX;
                ports[port].count--;
                chan_release(s);
            }
            ports[port].used = false;
            ports[port].refs = 0;
        }
    }
    spin_unlock_irqrestore(&port_lock, flags);
}

void port_retain(uint32_t port)
{
    uint32_t flags;

    if (port >= PORT_MAX) return;
    flags = spin_lock_irqsave(&port_lock);
    if (ports[port].used && ports[port].refs < 0xFFFFFFFFU) {
        ports[port].refs++;
    }
    spin_unlock_irqrestore(&port_lock, flags);
}
