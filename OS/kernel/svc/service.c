/* =============================================================================
 * core/service.c - 命名服务注册表
 *
 * 只把 "名字 -> Port" 映射起来，真正的服务循环仍在用户态。
 * 这是 init/supervisor 的服务发现最小地基。
 *
 * manifest 版本只做入口校验并记录在表项里：不匹配的注册者被拒绝，
 * 已注册的旧调用方（version=0）按 v1 处理，行为不变。
 * ============================================================================= */
#include "service.h"
#include "include/kernel.h"
#include "include/svc.h"

typedef struct {
    bool     used;
    char     name[SERVICE_NAME];
    uint32_t port;
    uint32_t manifest_version;   /* 实际生效的 manifest 版本（旧接口写入 v1） */
} service_t;

static service_t services[SERVICE_MAX];
static spinlock_t service_lock = SPINLOCK_INIT;

int service_manifest_version_ok(uint32_t version)
{
    /* 0 = 旧格式，按 v1 解释；其余必须精确匹配当前版本。 */
    if (version == 0U || version == SERVICE_MANIFEST_VERSION) return E_OK;
    return E_INVAL;
}

int service_manifest_check(const service_manifest_t *m)
{
    if (m == 0 || m->name == 0 || m->name[0] == '\0') return E_INVAL;
    if (m->flags != 0U) return E_INVAL;
    if (service_manifest_version_ok(m->version) != E_OK) {
        kprintf("svc: manifest '%s' version %u rejected (expected %u)\n",
                m->name, (unsigned int)m->version,
                (unsigned int)SERVICE_MANIFEST_VERSION);
        return E_INVAL;
    }
    return E_OK;
}

static int service_register_versioned(const char *name, uint32_t port,
                                      uint32_t manifest_version)
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
    services[free_slot].manifest_version = manifest_version;
    r = E_OK;
out:
    spin_unlock_irqrestore(&service_lock, flags);
    return r;
}

int service_register(const char *name, uint32_t port)
{
    /* 旧接口 = v1 manifest；把 0 归一化成当前版本再记录。 */
    return service_register_versioned(name, port, SERVICE_MANIFEST_VERSION);
}

int service_register_manifest(const service_manifest_t *m)
{
    if (service_manifest_check(m) != E_OK) return E_INVAL;
    return service_register_versioned(m->name, m->port, SERVICE_MANIFEST_VERSION);
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
