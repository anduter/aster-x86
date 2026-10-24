#ifndef CORE_SERVICE_H
#define CORE_SERVICE_H

#include "../types.h"

#define SERVICE_MAX   8U
#define SERVICE_NAME  32U
#define SERVICE_E_NOENT (-2)

/* =============================================================================
 * 服务 manifest v1
 *
 * 名字 -> Port 的注册表本身不解释协议；manifest 只固定"注册者声明的版本"，
 * 让不兼容的注册在入口处被拒绝，而不是跑起来后才发现。
 *
 * 版本策略（见 docs/api.md）：
 *   - SERVICE_MANIFEST_VERSION 是当前 manifest 布局版本；
 *   - version == 0 表示旧格式，按 v1 解释（旧调用方零改动）；
 *   - 其它值拒绝注册、返回 E_INVAL 并打印一行；
 *   - flags 是保留位，v1 必须为 0。
 * ========================================================================== */
#define SERVICE_MANIFEST_VERSION 1u

typedef struct service_manifest {
    uint32_t    version;   /* SERVICE_MANIFEST_VERSION；0 = 旧格式（按 v1） */
    const char *name;      /* 注册名，NUL 结尾，长度 < SERVICE_NAME */
    uint32_t    port;      /* 已创建的 port id */
    uint32_t    flags;     /* 保留，必须为 0 */
} service_manifest_t;

/* 旧接口：等价于 version=0（v1）的 manifest，保留给现有调用方。 */
int service_register(const char *name, uint32_t port);

/* 带 manifest 的注册：版本/参数校验失败返回 E_INVAL（不注册）；
 * 重名 E_EXIST、表满 E_FULL 与旧接口一致。 */
int service_register_manifest(const service_manifest_t *m);

int service_connect(const char *name, uint32_t *client);
int service_exists(const char *name);
void service_unregister(uint32_t port);

/* 纯判定：0 与 SERVICE_MANIFEST_VERSION 都接受（0 = 旧格式按 v1）。
 * 返回 E_OK / E_INVAL，不打印。 */
int service_manifest_version_ok(uint32_t version);

/* 校验 manifest：版本不匹配时打印一行并返回 E_INVAL；参数非法返回 E_INVAL。 */
int service_manifest_check(const service_manifest_t *m);

#endif
