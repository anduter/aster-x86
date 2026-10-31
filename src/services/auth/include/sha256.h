/*
 * ng WP2-5: SHA-256（L1，自实现，不依赖内核/OpenSSL/libc）。
 *
 * 只依赖 types.h（L0 公共类型），内核与 host 单测编译同一份源码。
 * 用途：
 *   - auth：口令 + salt 迭代哈希（>= 1000 轮）；
 *   - pkg ：清单字段 + 包文件完整性校验。
 *
 * 说明：
 *   - 纯 C、无动态内存；流式 update 支持任意长度（u64 位长）；
 *   - 大端输出（FIPS 180-4），hex 一律小写；
 *   - 本文件放在 auth 的 include 目录是 L1 内部的共享原语，pkg 同层引用，
 *     不存在 auth 登录语义对 pkg 的依赖。
 */
#ifndef SHA256_H
#define SHA256_H

#include "types.h"

#define SHA256_DIGEST_BYTES 32u
#define SHA256_BLOCK_BYTES 64u
#define SHA256_HEX_BYTES 64u
#define SHA256_HEX_MAX 65u /* 含 NUL */

struct sha256_ctx {
    u32 state[8];
    u64 bitlen;             /* 已处理的消息位数 */
    u8 buf[SHA256_BLOCK_BYTES];
    u32 buflen;             /* buf 里待处理字节数 */
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, u64 len);
void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_BYTES]);
void sha256(const void *data, u64 len, u8 out[SHA256_DIGEST_BYTES]);

/* 32 字节摘要 -> 64 字节小写 hex + NUL。 */
void sha256_hex(const u8 digest[SHA256_DIGEST_BYTES],
                char out[SHA256_HEX_MAX]);

/* 64 个 hex 字符（大小写均可）-> 32 字节；非法返回 0，成功 1。 */
int sha256_hex_parse(const char *hex, u8 out[SHA256_DIGEST_BYTES]);
int sha256_hex_valid(const char *hex);

#endif /* SHA256_H */
