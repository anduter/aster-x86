#ifndef SVC_H_MODULE_H
#define SVC_H_MODULE_H

/* Module interface header (M6). Cross-module code includes this, not internals. */

#include "svc/service.h"
#include "svc/supervisor.h"
#include "svc/channel.h"
#include "svc/port.h"

/* selftests */
void service_lifecycle_selftest(void);

#endif
