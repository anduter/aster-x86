#ifndef SECURITY_ACCOUNTS_H
#define SECURITY_ACCOUNTS_H

/* =============================================================================
 * security/accounts.h - Linux 式账号数据库（P1 身份 / P3 提权与审计）
 *
 * 这是账号系统的内核侧实现：账户数据库（/etc/passwd、/etc/shadow、
 * /etc/group）、口令校验（sha256 + 恒定时间比较）、会话表（who/last）、
 * 登录失败锁定（faillock 简化）与审计环。
 *
 * 与用户态的分工（"PAM-lite"）：
 *   - 用户态 login/getty 前端负责提示、读键盘、失败延迟、以及 exec shell；
 *   - 口令的比较与凭据的落地在核内（SYS_LOGIN），用户态拿不到 shadow 明文，
 *     普通用户也读不到 /etc/shadow（0600 root）。
 *
 * /etc/shadow 的行格式（第三个字段起用 ':' 分隔，和 Linux 同构，
 * 但只解释前两段，其余留给以后）：
 *     name:<64 位十六进制 sha256>:0:0:99999:7:::
 * 命令写文件（useradd/passwd）由用户态/核内各自完成，格式必须与这里一致。
 * ========================================================================== */
#include "../include/types.h"

#define ACCT_NAME_MAX      32
#define ACCT_HOME_MAX      64
#define ACCT_HASH_HEX      64
#define ACCT_MAX_USERS     12     /* 同时能解析的账户数 */
#define ACCT_MAX_GROUPS    12     /* /etc/group 条目数 */
#define ACCT_GROUP_MEMBERS 8
#define ACCT_SESSIONS      8
#define ACCT_AUDIT_LINES   24
#define ACCT_AUDIT_LEN     96
#define ACCT_FAIL_SLOTS    8
#define ACCT_FAIL_MAX      5      /* 连续失败 5 次锁定（faillock 简化） */
#define ACCT_LOCK_TICKS    1500U  /* 锁定 15s（100Hz） */

/* 默认口令（教学镜像；生产部署请用 passwd 改掉） */
#define ACCT_ROOT_PW       "root"
#define ACCT_ALICE_PW      "alice"
#define ACCT_BOB_PW        "bob"

/* /etc/passwd 里的标准 uid/gid 约定 */
#define ACCT_UID_ROOT      0U
#define ACCT_UID_SYS_MIN   1U     /* 1..99：系统服务，不可登录 */
#define ACCT_UID_SYS_MAX   99U
#define ACCT_UID_USER_MIN  1000U  /* 1000+：普通用户 */
#define ACCT_GID_WHEEL     10U    /* wheel：sudo 组 */

/* 文件路径（都在 system: 卷上） */
#define ACCT_PASSWD_PATH   "system:/etc/passwd"
#define ACCT_SHADOW_PATH   "system:/etc/shadow"
#define ACCT_GROUP_PATH    "system:/etc/group"
#define ACCT_AUDIT_PATH    "system:/var/log/audit.log"

/* acct_login / acct_su 的返回码 */
#define ACCT_OK            0
#define ACCT_EBAD         (-1)    /* 口令错/用户不存在（不区分，避免枚举） */
#define ACCT_ELOCKED      (-2)    /* 连续失败已锁定 */
#define ACCT_ENOENT       (-3)    /* 账户/文件不存在 */

typedef struct {
    char     name[ACCT_NAME_MAX];
    char     home[ACCT_HOME_MAX];
    char     shell[ACCT_HOME_MAX];
    uint32_t uid;
    uint32_t gid;
    bool     used;
} acct_user_t;

typedef struct {
    char     name[ACCT_NAME_MAX];
    uint32_t gid;
    char     members[ACCT_GROUP_MEMBERS][ACCT_NAME_MAX];
    uint32_t nmembers;
    bool     used;
} acct_group_t;

/* ---- 初始化 / 自检 ---- */
/* 建 /etc、/home、/root、/var/log 并写入默认账户（已存在则保留，幂等）。
 * 必须在文件系统可用之后、spawn 程序之前调用。 */
void acct_init(void);
/* 启动自检：把多用户矩阵的结论打成 acct: 开头的行（build.ps1 断言这些行）。 */
void acct_selftest(void);

/* ---- 账户查询 ---- */
int  acct_find(const char *name, acct_user_t *out);         /* 0 找到 / -1 */
int  acct_find_uid(uint32_t uid, acct_user_t *out);         /* 0 找到 / -1 */
int  acct_group_gid(const char *group, uint32_t *gid_out);  /* 0 找到 / -1 */
bool acct_in_group(const char *user, uint32_t gid);         /* 成员列表 + 主组 */
/* 当前 uid 是否 sudoer（wheel 组）：root 与 wheel 成员 */
bool acct_is_sudoer(uint32_t uid);

/* ---- 口令 ---- */
/* 把口令算成 64 位十六进制 sha256（out 必须 >= 65 字节） */
void acct_hash_hex(const char *pw, char out[ACCT_HASH_HEX + 1]);
/* 恒定时间比较：固定比较 64 个字符，返回值只反映"是否相等"。
 * steps 会累加到 acct_compare_steps()，自检用它证明两条路径迭代次数相同。 */
bool acct_verify_hash(const char *pw, const char *stored_hex);
uint32_t acct_compare_steps(void);

/* ---- 登录 / 会话 ---- */
/* 校验口令并（成功时）把凭据落到当前进程 + 开会话 + 写审计。
 * 失败会累加失败计数，达到 ACCT_FAIL_MAX 后锁定 ACCT_LOCK_TICKS。 */
int  acct_login(const char *user, const char *pass);
/* su：同 acct_login，但保留"是 su 提权"的审计语义 */
int  acct_su(const char *user, const char *pass);
/* sudo：校验调用者口令 + wheel 成员，成功把 euid 提到 0（保留 ruid） */
int  acct_sudo(const char *pass);
/* sudo 结束：euid/egid 回到 ruid/rgid */
int  acct_sudo_end(void);
/* 当前进程退出会话（logout/shell 退出） */
void acct_logout(void);
/* 会话表：写 "user tty pid home" 行，返回行数 */
int  acct_who(char *buf, uint32_t max);
/* 审计环：写 "event detail" 行，返回行数 */
int  acct_last(char *buf, uint32_t max);

/* ---- 口令修改 ---- */
/* 修改 user 的口令。as_root 为真时不校验旧口令（root 给任何人改）。
 * 普通用户改自己的口令必须提供旧口令。成功返回 ACCT_OK。 */
int acct_passwd(const char *user, const char *oldpw, const char *newpw, bool as_root);

/* ---- 审计 ---- */
/* 写一条审计：环形缓冲 + 串口行 "audit: <event> <detail>"。
 * 可以在中断/持有 fs 锁之外任意调用；本函数不碰文件系统。 */
void acct_audit(const char *event, const char *detail);
/* 把审计环刷进 ACCT_AUDIT_PATH（整文件重写）。返回写入字节数或负错误。 */
int  acct_audit_flush(void);

/* ---- 失败计数（诊断/自检） ---- */
int  acct_fail_count(const char *user);
void acct_fail_reset(const char *user);

/* ---- 用户态可见的降权辅助 ---- */
/* 按账户把 uid/gid/附加组落到进程上（login/su 成功后由 acct_login 调用） */
void acct_apply_cred(acct_user_t *u);

#endif
