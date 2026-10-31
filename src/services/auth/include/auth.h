/*
 * ng WP2-5: auth 服务接口（L1，src/services/auth）。
 *
 * 用户记录：name / uid / salt[16] / SHA-256 迭代哈希[32] / iterations。
 * 口令校验只走 auth_login()（内核 syscall 31 的 L1 实现）；hash 是公开的，
 * 仅用于测试与 /etc/passwd 导出。
 */
#ifndef AUTH_H
#define AUTH_H

#include "types.h"
#include "vfs.h"

#define AUTH_MAX_USERS 8u
#define AUTH_NAME_MAX 32u
#define AUTH_PASS_MAX 64u
#define AUTH_SALT_BYTES 16u
#define AUTH_HASH_BYTES 32u
#define AUTH_ITERATIONS 4096u
#define AUTH_MAX_FAILS 5u

/* 与 VFS/Linux errno 同值（负值）。 */
#define AUTH_OK 0
#define AUTH_ENOENT (-2)
#define AUTH_EACCES (-13)
#define AUTH_ENOSPC (-28)
#define AUTH_EINVAL (-22)

struct auth_user {
    u32 used;
    u32 uid;
    u32 iterations;
    u32 failed;
    u32 locked;
    u64 logins;
    char name[AUTH_NAME_MAX];
    u8 salt[AUTH_SALT_BYTES];
    u8 hash[AUTH_HASH_BYTES];
};

struct auth_stats {
    u32 users;
    u32 iterations;
    u64 logins;
    u64 failures;
    u64 unknown;
    u64 locked_rejects;
    u64 locks;
    u64 hash_calls;
};

/*
 * 清空用户表 -> 种入 root(0)/alice(1000) -> 写 /etc/passwd（ramfs）。
 * c 必须是已挂载 ramfs 的 VFS 上下文（内核用 fs_sys_ctx()，host 单测自建）。
 * 返回 AUTH_OK 或负 errno；失败后 auth_login() 返回 EINVAL。
 */
int auth_init(struct vfs_ctx *c);

/*
 * 校验口令：成功返回 uid（root 是 0），未知用户 -ENOENT，口令错/已锁定
 * -EACCES，参数非法/未初始化 -EINVAL。连续失败 AUTH_MAX_FAILS 次锁定该用户。
 */
int auth_login(const char *name, const char *pass);

/* 单次迭代哈希（测试与 pkg 共用 SHA-256 原语；iterations 必须 >= 1）。 */
int auth_hash_password(const char *pass, const u8 salt[AUTH_SALT_BYTES],
                       u32 iterations, u8 out[AUTH_HASH_BYTES]);

u32 auth_user_count(void);
const struct auth_user *auth_user_at(u32 index);
const struct auth_stats *auth_stat(void);

#endif /* AUTH_H */
