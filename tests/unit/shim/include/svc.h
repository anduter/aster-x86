/* =============================================================================
 * tests/unit/shim/include/svc.h - host 单元测试用最小 svc 接口
 *
 * service.c 只依赖 port_connect 的外部符号；端口层在 host 侧用桩替代。
 * ========================================================================== */
#ifndef SVC_H_MODULE_H
#define SVC_H_MODULE_H

#include "../../../../src/kernel/include/types.h"

int port_connect(uint32_t port, uint32_t *client);

#endif /* SVC_H_MODULE_H */
