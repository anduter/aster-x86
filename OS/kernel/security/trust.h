#ifndef CORE_TRUST_H
#define CORE_TRUST_H

#include "../include/types.h"

/* 验证 RSA-2048 / SHA-256 PKCS#1 v1.5 签名。
 * signature / modulus 都是 256 字节大端。成功返回 1。 */
int trust_verify_image(const uint8_t *image, uint32_t size,
                       const uint8_t *signature, const uint8_t *modulus);

/* 用构建时内嵌的 shell 公钥/签名验证一个 shell 镜像。 */
int trust_verify_shell(const uint8_t *image, uint32_t size);

/* 启动自检：验证内嵌 shell ELF，并确认篡改后的副本被拒绝。 */
void trust_selftest(void);

#endif
