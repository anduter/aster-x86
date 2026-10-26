#ifndef SECURITY_SHA512_H
#define SECURITY_SHA512_H

/* =============================================================================
 * security/sha512.h - SHA-512 与 PBKDF2-HMAC-SHA512（账号口令用）
 *
 * 只被 security/accounts.c 使用：shadow 的存储格式从单次 sha256 升级为
 * PBKDF2-HMAC-SHA512（随机 salt、固定迭代次数、32 字节输出）。实现与
 * RSA 验签用的 sha256（security/trust.c）完全独立，互不影响启动验签。
 * ========================================================================== */
#include "../include/types.h"

/* 标准 SHA-512：把 data 的摘要写进 out[64]。 */
void sha512_bytes(const void *data, uint32_t len, uint8_t out[64]);

/* PBKDF2-HMAC-SHA512（RFC 8018）。
 *   password/pwlen : 口令字节（不含结尾 NUL）
 *   salt/saltlen   : 随机盐
 *   iterations     : 迭代次数（>= 1）
 *   out/outlen     : 派生输出；本实现按单块输出，outlen 上限 64 字节。
 * 账号系统固定 outlen=32、iterations=10000。 */
void pbkdf2_sha512(const uint8_t *password, uint32_t pwlen,
                   const uint8_t *salt, uint32_t saltlen,
                   uint32_t iterations, uint8_t *out, uint32_t outlen);

#endif /* SECURITY_SHA512_H */
