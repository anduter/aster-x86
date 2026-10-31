/*
 * ng WP2-5: pkg 服务接口（L1，包目录/清单/完整性校验最小集）。
 *
 * 目录布局（ramfs）：
 *   /System/Packages/<name>/<version>/manifest
 *   /System/Packages/<name>/<version>/<entry>        （entry 是清单里的相对路径）
 *
 * 启动时 pkg_init() 种入示例包 hello/1.0.0（manifest + bin/hello）。
 * manifest 是 "key=value" 文本，本 WP 要求的字段：
 *   name / version / type / entry / sha256
 * 其中 sha256 是 entry 文件内容的 SHA-256（64 位小写 hex），pkg_verify()
 * 重新计算并比对；未知字段/重复字段/缺字段一律 EINVAL。
 *
 * 明确不做（按 WP2-5 边界）：签名/RSA（D1）、依赖解析/安装事务/网络仓库。
 * 因此本 WP 的 pkg_verify 只证明"清单与文件一致"，不证明"发布者可信"，
 * 差异记录在 docs/claims/ng-WP2-5.md。
 *
 * 依赖：vfs.h + types.h + sha256.h（L1 内部共享原语），不 include 内核头。
 */
#ifndef PKG_H
#define PKG_H

#include "types.h"
#include "vfs.h"

#define PKG_NAME_MAX 32u
#define PKG_VERSION_MAX 16u
#define PKG_TYPE_MAX 16u
#define PKG_ENTRY_MAX 64u
#define PKG_MANIFEST_MAX 512u
#define PKG_LIST_MAX 1024u
#define PKG_MAX_PACKAGES 16u
#define PKG_MAX_VERSIONS 8u

#define PKG_ROOT "/System/Packages"

/* 与 VFS/Linux errno 同值（负值）；EIO 表示哈希/完整性不匹配。 */
#define PKG_OK 0
#define PKG_ENOENT (-2)
#define PKG_EIO (-5)
#define PKG_EINVAL (-22)
#define PKG_ENOSPC (-28)
#define PKG_ENAMETOOLONG (-36)

/* manifest 字段位（pkg_manifest.fields）。 */
#define PKG_F_NAME (1u << 0)
#define PKG_F_VERSION (1u << 1)
#define PKG_F_TYPE (1u << 2)
#define PKG_F_ENTRY (1u << 3)
#define PKG_F_SHA256 (1u << 4)
#define PKG_F_ALL (PKG_F_NAME | PKG_F_VERSION | PKG_F_TYPE | PKG_F_ENTRY | \
                  PKG_F_SHA256)

struct pkg_manifest {
    char name[PKG_NAME_MAX];
    char version[PKG_VERSION_MAX];
    char type[PKG_TYPE_MAX];
    char entry[PKG_ENTRY_MAX];
    u8 sha256[32];
    u32 fields;
};

struct pkg_stats {
    u32 packages; /* pkg_init 种子数（本 WP 1） */
    u64 lists;
    u64 verifies;
    u64 failures;
    u64 seeds;
    u64 hash_calls;
};

/*
 * 种入 /System/Packages/hello/1.0.0（manifest + bin/hello）。
 * c 必须是已挂载 ramfs 的 VFS 上下文。返回 PKG_OK 或负 errno。
 */
int pkg_init(struct vfs_ctx *c);

/*
 * 列出所有包：每行 "name version\n"，buf 必须能容纳全部内容 + NUL。
 * 返回包数（>=0）；缓冲不足 -ENOSPC；未初始化/参数非法 -EINVAL。
 */
int pkg_list(char *buf, u32 max);

/*
 * 校验一个包：定位唯一版本目录 -> 读 manifest -> 校验字段/路径 -> 对 entry
 * 文件重算 SHA-256 并比对。返回 PKG_OK / -ENOENT / -EINVAL / -EIO。
 */
int pkg_verify(const char *name);

/* 纯文本 manifest 解析（host 单测直接调用）。返回 PKG_OK / -EINVAL。 */
int pkg_manifest_parse(const char *text, u32 len, struct pkg_manifest *out);

u32 pkg_seed_count(void);
const struct pkg_stats *pkg_stat(void);

#endif /* PKG_H */
