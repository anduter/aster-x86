#ifndef CORE_SERVICE_H
#define CORE_SERVICE_H

#include "../types.h"

#define SERVICE_MAX   8U
#define SERVICE_NAME  32U
#define SERVICE_E_NOENT (-2)

int service_register(const char *name, uint32_t port);
int service_connect(const char *name, uint32_t *client);
int service_exists(const char *name);
void service_unregister(uint32_t port);

#endif
