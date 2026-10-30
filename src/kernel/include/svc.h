#ifndef SVC_H_MODULE_H
#define SVC_H_MODULE_H

/* Module interface header (M6). Cross-module code includes this, not internals. */

#include "svc/service.h"
#include "svc/supervisor.h"
#include "svc/channel.h"
#include "svc/port.h"
#include "svc/ipc.h"
#include "svc/registry.h"

/* selftests */
void service_lifecycle_selftest(void);

#endif
