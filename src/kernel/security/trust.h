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


/* ---- 账号系统用的 SHA-256（ABI v1.3，P1）----
 * 把 data 的 sha256 摘要写进 out[32]；data 为 NULL 或 len==0 时算空串的摘要。
 * /etc/shadow 存的是口令的 sha256 十六进制串；校验由 accounts.c 做恒定时间比较。 */
void sha256_bytes(const void *data, uint32_t len, uint8_t out[32]);

#endif