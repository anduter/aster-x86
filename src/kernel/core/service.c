/* =============================================================================
 * core/service.c - 命名服务注册表
 *
 * 只把 "名字 -> Port" 映射起来，真正的服务循环仍在用户态。
 * 这是 init/supervisor 的服务发现最小地基。
 * ============================================================================= */
#include "service.h"
#include "error.h"
#include "../ipc/port.h"
#include "../lib/string.h"
#include "../core/spinlock.h"

typedef struct {
    bool     used;
    char     name[SERVICE_NAME];
    uint32_t port;
} service_t;

static service_t services[SERVICE_MAX];
static spinlock_t service_lock = SPINLOCK_INIT;

int service_register(const char *name, uint32_t port)
{
    uint32_t flags;
    int free_slot = -1;
    int r = E_FULL;

    if (name == 0 || name[0] == '\0' || strlen(name) >= SERVICE_NAME) return E_INVAL;
    flags = spin_lock_irqsave(&service_lock);
    for (uint32_t i = 0; i < SERVICE_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0) {
            r = E_EXIST;
            goto out;
        }
        if (!services[i].used && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) goto out;
    services[free_slot].used = true;
    strncpy(services[free_slot].name, name, SERVICE_NAME - 1U);
    services[free_slot].name[SERVICE_NAME - 1U] = '\0';
    services[free_slot].port = port;
    r = E_OK;
out:
    spin_unlock_irqrestore(&service_lock, flags);
    return r;
}

int service_connect(const char *name, uint32_t *client)
{
    uint32_t flags;
    uint32_t port = 0;
    int found = 0;

    if (name == 0 || client == 0) return E_INVAL;
    flags = spin_lock_irqsave(&service_lock);
    for (uint32_t i = 0; i < SERVICE_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0) {
            port = services[i].port;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&service_lock, flags);

    if (!found) return SERVICE_E_NOENT;
    return port_connect(port, client);
}

int service_exists(const char *name)
{
    uint32_t flags;
    int found = 0;

    if (name == 0) return 0;
    flags = spin_lock_irqsave(&service_lock);
    for (uint32_t i = 0; i < SERVICE_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0) {
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&service_lock, flags);
    return found;
}

void service_unregister(uint32_t port)
{
    uint32_t flags = spin_lock_irqsave(&service_lock);

    for (uint32_t i = 0; i < SERVICE_MAX; i++) {
        if (services[i].used && services[i].port == port) {
            services[i].used = false;
        }
    }
    spin_unlock_irqrestore(&service_lock, flags);
}
