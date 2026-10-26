/* =============================================================================
 * security/accounts.c - 账号数据库 / 口令校验 / 会话 / 审计（ABI v1.3）
 *
 * 设计要点：
 *   1. 文件是事实来源：/etc/passwd、/etc/shadow、/etc/group 由本模块在启动时
 *      幂等播种（已存在则保留），之后 passwd/useradd 直接改文件；
 *   2. 口令只在核内比较：PBKDF2-HMAC-SHA512(口令, 随机 16B salt, 10000 轮)
 *      与 shadow 里的 "pbkdf2$sha512$<iter>$<salt_hex>$<hash_hex>" 做恒定时间
 *      比较；旧格式（单次 sha256 的 64 位十六进制串）仍可登录，成功后自动迁移；
 *   3. 凭据落地只有一处：acct_apply_cred() 把 uid/gid/附加组写进 proc_t，
 *      之后所有权限判定都用 euid（见 fs/ramfs.c 的 may_access）；
 *   4. 审计不碰文件系统：只写环形缓冲（128 行）+ 串口行（"audit: ..."），
 *      刷盘由 acct_audit_flush() 在 syscall 上下文显式调用；刷盘是**追加**，
 *      保留历史，超过 ACCT_AUDIT_FILE_MAX 才截断旧文件重建。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/fs.h"
#include "include/proc.h"
#include "accounts.h"
#include "sha512.h"
#include "trust.h"

/* ==================== 小工具 ==================== */

static uint32_t acct_strlen(const char *s)
{
    return (s != 0) ? (uint32_t)strlen(s) : 0U;
}

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;

    if (s == 0) return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* 取下一行（就地改 '\0'，去掉 CR）；没有行了返回 0 */
static char *line_take(char **p)
{
    char *start;
    char *nl;
    uint32_t n;

    if (p == 0 || *p == 0 || **p == '\0') return 0;
    start = *p;
    nl = strchr(start, '\n');
    if (nl != 0) {
        *nl = '\0';
        *p = nl + 1;
    } else {
        *p = start + strlen(start);
    }
    n = (uint32_t)strlen(start);
    if (n > 0U && start[n - 1U] == '\r') start[n - 1U] = '\0';
    return start;
}

/* 把 line 按 ':' 拆成最多 max 段（就地改 '\0'），返回段数 */
static uint32_t split_fields(char *line, char **f, uint32_t max)
{
    uint32_t n = 0;

    if (line == 0 || max == 0U) return 0;
    f[n++] = line;
    for (char *p = line; *p != '\0'; p++) {
        if (*p == ':' && n < max) {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

/* 读整个文件（调用方按 root 身份读；不打开目录） */
static int read_file(const char *path, char *buf, uint32_t max)
{
    fs_stat_t st;
    uint32_t h;
    int n;

    if (buf == 0 || max == 0U) return -1;
    buf[0] = '\0';
    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_FILE) return -1;
    if (st.size >= max) return -1;

    h = fs_open(path, FS_OPEN_READ);
    if (h == 0U) return -1;
    n = fs_read(h, buf, max - 1U);
    fs_close(h);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* 整文件重写：ramfs 的 write 没有 truncate，必须先 unlink 再 create */
static int write_file(const char *path, const char *data, uint32_t len)
{
    uint32_t h;
    int n;

    (void)fs_unlink(path);                  /* 不存在也继续 */
    h = fs_open(path, FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0U) return -1;
    n = fs_write(h, data, len);
    fs_close(h);
    return (n == (int)len) ? 0 : -1;
}

/* ==================== 解析 /etc/passwd、/etc/group ==================== */

static uint32_t parse_passwd(char *buf, acct_user_t *out, uint32_t max)
{
    char *p = buf;
    char *line;
    uint32_t n = 0;

    while (n < max && (line = line_take(&p)) != 0) {
        char *f[7];
        uint32_t nf;
        acct_user_t *u;

        if (line[0] == '\0' || line[0] == '#') continue;
        nf = split_fields(line, f, 7);
        if (nf < 7) continue;

        u = &out[n];
        memset(u, 0, sizeof(*u));
        strncpy(u->name, f[0], ACCT_NAME_MAX - 1U);
        u->uid = parse_u32(f[2]);
        u->gid = parse_u32(f[3]);
        strncpy(u->home, f[5], ACCT_HOME_MAX - 1U);
        strncpy(u->shell, f[6], ACCT_HOME_MAX - 1U);
        u->used = true;
        n++;
    }
    return n;
}

static uint32_t parse_group(char *buf, acct_group_t *out, uint32_t max)
{
    char *p = buf;
    char *line;
    uint32_t n = 0;

    while (n < max && (line = line_take(&p)) != 0) {
        char *f[4];
        uint32_t nf;
        acct_group_t *g;

        if (line[0] == '\0' || line[0] == '#') continue;
        nf = split_fields(line, f, 4);
        if (nf < 3) continue;

        g = &out[n];
        memset(g, 0, sizeof(*g));
        strncpy(g->name, f[0], ACCT_NAME_MAX - 1U);
        g->gid = parse_u32(f[2]);
        if (nf >= 4 && f[3][0] != '\0') {
            char *m = f[3];
            while (*m != '\0' && g->nmembers < ACCT_GROUP_MEMBERS) {
                char *comma = strchr(m, ',');
                if (comma != 0) *comma = '\0';
                strncpy(g->members[g->nmembers], m, ACCT_NAME_MAX - 1U);
                g->nmembers++;
                if (comma == 0) break;
                m = comma + 1;
            }
        }
        g->used = true;
        n++;
    }
    return n;
}

int acct_find(const char *name, acct_user_t *out)
{
    char buf[1024];
    acct_user_t users[ACCT_MAX_USERS];
    uint32_t n;

    if (name == 0 || out == 0) return -1;
    if (read_file(ACCT_PASSWD_PATH, buf, sizeof(buf)) < 0) return -1;

    n = parse_passwd(buf, users, ACCT_MAX_USERS);
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(users[i].name, name) == 0) {
            *out = users[i];
            return 0;
        }
    }
    return -1;
}

int acct_find_uid(uint32_t uid, acct_user_t *out)
{
    char buf[1024];
    acct_user_t users[ACCT_MAX_USERS];
    uint32_t n;

    if (out == 0) return -1;
    if (read_file(ACCT_PASSWD_PATH, buf, sizeof(buf)) < 0) return -1;

    n = parse_passwd(buf, users, ACCT_MAX_USERS);
    for (uint32_t i = 0; i < n; i++) {
        if (users[i].uid == uid) {
            *out = users[i];
            return 0;
        }
    }
    return -1;
}

int acct_group_gid(const char *group, uint32_t *gid_out)
{
    char buf[1024];
    acct_group_t groups[ACCT_MAX_GROUPS];
    uint32_t n;

    if (group == 0 || gid_out == 0) return -1;
    if (read_file(ACCT_GROUP_PATH, buf, sizeof(buf)) < 0) return -1;

    n = parse_group(buf, groups, ACCT_MAX_GROUPS);
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(groups[i].name, group) == 0) {
            *gid_out = groups[i].gid;
            return 0;
        }
    }
    return -1;
}

/* 用户是否属于 gid（主组或 /etc/group 的成员列表） */
bool acct_in_group(const char *user, uint32_t gid)
{
    char buf[1024];
    acct_group_t groups[ACCT_MAX_GROUPS];
    acct_user_t u;
    uint32_t n;

    if (user == 0) return false;

    if (acct_find(user, &u) == 0 && u.gid == gid) return true;
    if (read_file(ACCT_GROUP_PATH, buf, sizeof(buf)) < 0) return false;

    n = parse_group(buf, groups, ACCT_MAX_GROUPS);
    for (uint32_t i = 0; i < n; i++) {
        if (groups[i].gid != gid) continue;
        for (uint32_t j = 0; j < groups[i].nmembers; j++) {
            if (strcmp(groups[i].members[j], user) == 0) return true;
        }
    }
    return false;
}

bool acct_is_sudoer(uint32_t uid)
{
    acct_user_t u;

    if (uid == ACCT_UID_ROOT) return true;
    if (acct_find_uid(uid, &u) != 0) return false;
    return acct_in_group(u.name, ACCT_GID_WHEEL);
}

/* ==================== 口令：PBKDF2-SHA512 + 恒定时间比较 ==================== */

static uint32_t compare_steps;

static void hex_encode(const uint8_t *digest, uint32_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";

    for (uint32_t i = 0; i < len; i++) {
        out[i * 2U] = hex[digest[i] >> 4];
        out[i * 2U + 1U] = hex[digest[i] & 0x0FU];
    }
    out[len * 2U] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

/* 严格解析 n 个十六进制字符到 out；格式不对返回 false */
static bool hex_decode(const char *s, uint32_t n, uint8_t *out)
{
    for (uint32_t i = 0; i < n; i++) {
        int hi = hex_value(s[i * 2U]);
        int lo = hex_value(s[i * 2U + 1U]);

        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* 旧格式辅助：单次 sha256，输出 64 个十六进制字符（迁移/自检/兼容用） */
void acct_hash_hex(const char *pw, char out[ACCT_HASH_HEX + 1])
{
    uint8_t digest[32];
    uint32_t n = acct_strlen(pw);

    sha256_bytes(pw, n, digest);
    hex_encode(digest, 32U, out);
}

/* 随机 16 字节 salt：rdtsc + 时钟 + pid + 静态计数器混合。
 * 不追求密码学级 RNG（教学内核没有硬件熵源），但保证同一启动内不同账户、
 * 不同次改口令得到不同的盐；盐不是秘密，唯一性比保密性更重要。 */
static uint64_t acct_xorshift(uint64_t x)
{
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static void acct_random_salt(uint8_t out[ACCT_SALT_BYTES])
{
    static uint64_t counter;
    uint32_t lo = 0, hi = 0;
    uint64_t x;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    x = ((uint64_t)hi << 32) | (uint64_t)lo;
    x ^= (uint64_t)timer_ticks() << 21;
    x ^= ((uint64_t)(uintptr_t)&counter) << 7;
    if (proc_current() != 0) x ^= (uint64_t)proc_current()->pid << 33;
    counter++;
    x ^= counter * 0x9E3779B97F4A7C15ULL;

    for (uint32_t i = 0; i < ACCT_SALT_BYTES; i++) {
        x = acct_xorshift(x);
        out[i] = (uint8_t)(x >> ((i % 8U) * 8U));
    }
}

/* 新格式：pbkdf2$sha512$10000$<32 hex salt>$<64 hex digest> */
void acct_hash_pbkdf2(const char *pw, char out[ACCT_HASH_MAX])
{
    uint8_t salt[ACCT_SALT_BYTES];
    uint8_t digest[ACCT_DIGEST_BYTES];
    char salt_hex[ACCT_SALT_BYTES * 2U + 1U];
    char digest_hex[ACCT_DIGEST_BYTES * 2U + 1U];
    const char *pass = (pw != 0) ? pw : "";

    acct_random_salt(salt);
    pbkdf2_sha512((const uint8_t *)pass, acct_strlen(pass),
                  salt, (uint32_t)sizeof(salt),
                  ACCT_PBKDF2_ITERS, digest, (uint32_t)sizeof(digest));
    hex_encode(salt, (uint32_t)sizeof(salt), salt_hex);
    hex_encode(digest, (uint32_t)sizeof(digest), digest_hex);

    ksnprintf(out, ACCT_HASH_MAX, "pbkdf2$sha512$%u$%s$%s",
              (unsigned int)ACCT_PBKDF2_ITERS, salt_hex, digest_hex);

    memset(salt, 0, sizeof(salt));
    memset(digest, 0, sizeof(digest));
}

bool acct_is_pbkdf2(const char *stored)
{
    return (stored != 0 && strncmp(stored, "pbkdf2$sha512$", 14U) == 0);
}

uint32_t acct_compare_steps(void)
{
    return compare_steps;
}

/* 固定比较 ACCT_HASH_HEX 个字符：不因前缀不同提前返回，也不因长度不同
 * 提前返回（不足的部分按 0 参与异或）。 */
static bool ct_compare_hex(const char *calc, const char *stored)
{
    uint32_t diff = 0;

    for (uint32_t i = 0; i < ACCT_HASH_HEX; i++) {
        uint8_t a = (uint8_t)calc[i];
        uint8_t b = (uint8_t)stored[i];      /* 提前 NUL 时按 0 比较 */

        diff |= (uint32_t)(a ^ b);
        compare_steps++;
    }
    return diff == 0U;
}

/* 解析并校验 "pbkdf2$sha512$<iter>$<salt_hex>$<hash_hex>"。
 * 格式非法一律失败；iter 限制在 [1000, 100000]，避免被坏文件拖死。 */
static bool verify_pbkdf2(const char *pw, const char *stored)
{
    const char *p = stored + 14U;             /* 跳过 "pbkdf2$sha512$" */
    uint32_t iter = 0;
    uint8_t salt[ACCT_SALT_BYTES];
    uint8_t digest[ACCT_DIGEST_BYTES];
    uint8_t calc[ACCT_DIGEST_BYTES];
    char calc_hex[ACCT_DIGEST_BYTES * 2U + 1U];
    const char *hash_field;

    while (*p >= '0' && *p <= '9') {
        iter = iter * 10U + (uint32_t)(*p - '0');
        if (iter > 1000000U) return false;
        p++;
    }
    if (*p != '$' || iter < 1000U || iter > 100000U) return false;
    p++;

    if (strlen(p) < 32U) return false;
    if (!hex_decode(p, ACCT_SALT_BYTES, salt)) return false;
    p += 32U;
    if (*p != '$') return false;
    p++;
    if (strlen(p) != ACCT_HASH_HEX) return false;
    hash_field = p;
    if (!hex_decode(hash_field, ACCT_DIGEST_BYTES, digest)) return false;

    pbkdf2_sha512((const uint8_t *)((pw != 0) ? pw : ""),
                  acct_strlen(pw), salt, (uint32_t)sizeof(salt),
                  iter, calc, (uint32_t)sizeof(calc));
    hex_encode(calc, (uint32_t)sizeof(calc), calc_hex);
    return ct_compare_hex(calc_hex, hash_field);
}

bool acct_verify_hash(const char *pw, const char *stored)
{
    char calc[ACCT_HASH_HEX + 1];

    if (stored == 0 || stored[0] == '\0') return false;
    if (acct_is_pbkdf2(stored)) return verify_pbkdf2(pw, stored);

    /* 旧格式：单次 sha256 的十六进制串 */
    acct_hash_hex(pw, calc);
    return ct_compare_hex(calc, stored);
}

/* ==================== 失败计数（faillock 简化） ==================== */

typedef struct {
    bool     used;
    char     user[ACCT_NAME_MAX];
    uint32_t fails;
    uint32_t lock_until;
} acct_fail_t;

static acct_fail_t fails[ACCT_FAIL_SLOTS];

static acct_fail_t *fail_slot(const char *user, bool create)
{
    for (uint32_t i = 0; i < ACCT_FAIL_SLOTS; i++) {
        if (fails[i].used && strcmp(fails[i].user, user) == 0) return &fails[i];
    }
    if (!create) return 0;
    for (uint32_t i = 0; i < ACCT_FAIL_SLOTS; i++) {
        if (!fails[i].used) {
            memset(&fails[i], 0, sizeof(fails[i]));
            fails[i].used = true;
            strncpy(fails[i].user, user, ACCT_NAME_MAX - 1U);
            return &fails[i];
        }
    }
    return 0;
}

int acct_fail_count(const char *user)
{
    acct_fail_t *f = (user != 0) ? fail_slot(user, false) : 0;

    return (f != 0) ? (int)f->fails : 0;
}

void acct_fail_reset(const char *user)
{
    acct_fail_t *f = (user != 0) ? fail_slot(user, false) : 0;

    if (f != 0) {
        f->fails = 0;
        f->lock_until = 0;
    }
}

/* 记一次失败；返回 true 表示这次失败触发了锁定 */
static bool fail_bump(const char *user)
{
    acct_fail_t *f = fail_slot(user, true);

    if (f == 0) return false;
    f->fails++;
    if (f->fails >= ACCT_FAIL_MAX) {
        f->lock_until = timer_ticks() + ACCT_LOCK_TICKS;
        return true;
    }
    return false;
}

/* 锁定中？ */
static bool fail_locked(const char *user)
{
    acct_fail_t *f = fail_slot(user, false);

    if (f == 0) return false;
    if (f->lock_until == 0U) return false;
    if ((int32_t)(timer_ticks() - f->lock_until) >= 0) {
        /* 锁定到期：清零，允许重试 */
        f->fails = 0;
        f->lock_until = 0;
        return false;
    }
    return true;
}

/* ==================== 会话表 / 审计环 ==================== */

typedef struct {
    bool     used;
    uint32_t pid;
    uint32_t uid;
    uint32_t since;
    char     user[ACCT_NAME_MAX];
    char     tty[8];
} acct_sess_t;

static acct_sess_t sessions[ACCT_SESSIONS];
static spinlock_t  acct_lock = SPINLOCK_INIT;

static char     audit_ring[ACCT_AUDIT_LINES][ACCT_AUDIT_LEN];
static uint32_t audit_next;
static uint32_t audit_total;      /* 环里当前有效行数（<= ACCT_AUDIT_LINES） */
static uint32_t audit_seq;        /* 历史写入总行数（含已被覆盖的） */
static uint32_t audit_flushed;    /* 已成功追加到 audit.log 的 audit_seq 水位 */

/* 调用方必须持有 acct_lock。整行零填充，避免把栈上残留写进环里。 */
static void audit_append_locked(const char *line)
{
    char *dst = audit_ring[audit_next];
    uint32_t n = 0;

    while (line[n] != '\0' && n + 1U < ACCT_AUDIT_LEN) {
        dst[n] = line[n];
        n++;
    }
    dst[n] = '\0';
    for (uint32_t i = n + 1U; i < ACCT_AUDIT_LEN; i++) dst[i] = '\0';

    audit_next = (audit_next + 1U) % ACCT_AUDIT_LINES;
    if (audit_total < ACCT_AUDIT_LINES) audit_total++;
    audit_seq++;
}

static void audit_push_text(const char *line)
{
    uint32_t fl = spin_lock_irqsave(&acct_lock);

    audit_append_locked(line);
    spin_unlock_irqrestore(&acct_lock, fl);
}

void acct_audit(const char *event, const char *detail)
{
    char line[ACCT_AUDIT_LEN];
    uint32_t n = 0;
    const char *ev = (event != 0) ? event : "?";
    const char *de = (detail != 0) ? detail : "-";

    /* 串口 + 屏幕：build.ps1 的检查项断言这些行 */
    kprintf("audit: %s %s\n", ev, de);

    while (*ev != '\0' && n + 1U < ACCT_AUDIT_LEN) line[n++] = *ev++;
    if (n + 1U < ACCT_AUDIT_LEN) line[n++] = ' ';
    while (*de != '\0' && n + 1U < ACCT_AUDIT_LEN) line[n++] = *de++;
    line[n] = '\0';

    audit_push_text(line);
}

/* 把审计环按时间顺序拼成文本（最旧的在前） */
static uint32_t audit_text(char *buf, uint32_t max)
{
    uint32_t start;
    uint32_t n = 0;
    uint32_t fl;

    if (buf == 0 || max == 0U) return 0;
    buf[0] = '\0';

    fl = spin_lock_irqsave(&acct_lock);
    start = (audit_total < ACCT_AUDIT_LINES) ? 0U : audit_next;
    for (uint32_t i = 0; i < audit_total; i++) {
        const char *l = audit_ring[(start + i) % ACCT_AUDIT_LINES];
        while (*l != '\0' && n + 2U < max) buf[n++] = *l++;
        if (n + 2U < max) buf[n++] = '\n';
    }
    buf[n] = '\0';
    spin_unlock_irqrestore(&acct_lock, fl);
    return n;
}

/* 只拼 "seq >= from" 的行（flush 追加用）。环被覆盖时从最旧可用的行开始，
 * 保证不重复已刷过的内容，也不漏掉当前环里还活着的记录。
 * next_out 返回本次快照的 audit_seq 水位。 */
static uint32_t audit_text_since(uint32_t from, char *buf, uint32_t max,
                                 uint32_t *next_out)
{
    uint32_t fl;
    uint32_t n = 0;
    uint32_t oldest;
    uint32_t count;
    uint32_t offset;
    uint32_t slot;

    if (buf == 0 || max == 0U) return 0;
    buf[0] = '\0';

    fl = spin_lock_irqsave(&acct_lock);
    oldest = audit_seq - audit_total;
    if (from < oldest) from = oldest;          /* 已被环覆盖的旧记录跳过 */
    if (from > audit_seq) from = audit_seq;
    count = audit_seq - from;
    offset = from - oldest;
    slot = (audit_next + ACCT_AUDIT_LINES - audit_total + offset) % ACCT_AUDIT_LINES;

    for (uint32_t i = 0; i < count; i++) {
        const char *l = audit_ring[(slot + i) % ACCT_AUDIT_LINES];

        while (*l != '\0' && n + 2U < max) buf[n++] = *l++;
        if (n + 2U < max) buf[n++] = '\n';
    }
    buf[n] = '\0';
    if (next_out != 0) *next_out = audit_seq;
    spin_unlock_irqrestore(&acct_lock, fl);
    return n;
}

/* 审计刷盘：**追加**到 /var/log/audit.log，保留历史。
 * 只追加上次成功刷盘之后的新事件；文件超过 ACCT_AUDIT_FILE_MAX 时截断重建
 * （轮转），避免无限增长。返回本次追加字节数（没有新事件返回 0）。 */
int acct_audit_flush(void)
{
    static char add[ACCT_AUDIT_LINES * ACCT_AUDIT_LEN + 8];
    static char old[ACCT_AUDIT_FILE_MAX + 1U];
    uint32_t next_seq = 0;
    uint32_t n;
    uint32_t fl;
    int oldlen;

    fl = spin_lock_irqsave(&acct_lock);
    if (audit_flushed >= audit_seq) {
        spin_unlock_irqrestore(&acct_lock, fl);
        return 0;
    }
    spin_unlock_irqrestore(&acct_lock, fl);

    n = audit_text_since(audit_flushed, add, sizeof(add), &next_seq);
    if (n == 0U) return 0;

    oldlen = read_file(ACCT_AUDIT_PATH, old, sizeof(old) - 1U);
    if (oldlen < 0) oldlen = 0;

    if ((uint32_t)oldlen + n + 1U > sizeof(old)) {
        /* 超限：截断旧文件，只保留本次追加（旧内容视为已轮转） */
        if (write_file(ACCT_AUDIT_PATH, add, n) != 0) return -1;
    } else {
        memcpy(old + oldlen, add, n);
        old[oldlen + (int)n] = '\0';
        if (write_file(ACCT_AUDIT_PATH, old, (uint32_t)oldlen + n) != 0) return -1;
    }

    /* 审计日志属于 root，别让 umask/属主漂移 */
    (void)fs_chmod(ACCT_AUDIT_PATH, 0600U, 0U);
    (void)fs_chown(ACCT_AUDIT_PATH, 0U, 0U);

    fl = spin_lock_irqsave(&acct_lock);
    if (audit_flushed < next_seq) audit_flushed = next_seq;
    spin_unlock_irqrestore(&acct_lock, fl);
    return (int)n;
}

int acct_last(char *buf, uint32_t max)
{
    uint32_t n = audit_text(buf, max);
    uint32_t lines = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] == '\n') lines++;
    }
    return (int)lines;
}

/* 给指定 pid 开会话（自检可以直接喂一个假 pid，不依赖 proc_current） */
static int session_open_pid(const acct_user_t *u, uint32_t pid)
{
    acct_sess_t *slot = 0;

    for (uint32_t i = 0; i < ACCT_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].pid == pid) { slot = &sessions[i]; break; }
    }
    if (slot == 0) {
        for (uint32_t i = 0; i < ACCT_SESSIONS; i++) {
            if (!sessions[i].used) { slot = &sessions[i]; break; }
        }
    }
    if (slot == 0) return -1;

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->pid = pid;
    slot->uid = u->uid;
    slot->since = timer_ticks() / timer_hz();
    strncpy(slot->user, u->name, ACCT_NAME_MAX - 1U);
    strncpy(slot->tty, "tty0", sizeof(slot->tty) - 1U);
    return 0;
}

static int session_open_impl(const acct_user_t *u)
{
    uint32_t pid = (proc_current() != 0) ? proc_current()->pid : 0U;

    return session_open_pid(u, pid);
}

void acct_logout(void)
{
    uint32_t pid = (proc_current() != 0) ? proc_current()->pid : 0U;

    acct_session_drop(pid);
}

void acct_session_drop(uint32_t pid)
{
    uint32_t fl = spin_lock_irqsave(&acct_lock);

    for (uint32_t i = 0; i < ACCT_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].pid == pid) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
        }
    }
    spin_unlock_irqrestore(&acct_lock, fl);
}

int acct_who(char *buf, uint32_t max)
{
    uint32_t n = 0;
    uint32_t fl;
    uint32_t lines = 0;

    if (buf == 0 || max == 0U) return 0;
    buf[0] = '\0';

    fl = spin_lock_irqsave(&acct_lock);
    for (uint32_t i = 0; i < ACCT_SESSIONS; i++) {
        char num[16];
        const char *parts[4];

        if (!sessions[i].used) continue;
        utoa(sessions[i].pid, num, 10);
        parts[0] = sessions[i].user;
        parts[1] = sessions[i].tty;
        parts[2] = num;

        for (uint32_t k = 0; k < 3U; k++) {
            const char *s = parts[k];
            while (*s != '\0' && n + 2U < max) buf[n++] = *s++;
            if (k < 2U && n + 2U < max) buf[n++] = ' ';
        }
        if (n + 2U < max) buf[n++] = '\n';
        lines++;
    }
    buf[n] = '\0';
    spin_unlock_irqrestore(&acct_lock, fl);
    return (int)lines;
}

/* ==================== 凭据落地 ==================== */

void acct_apply_cred(acct_user_t *u)
{
    proc_t *p = proc_current();
    char buf[1024];
    acct_group_t groups[ACCT_MAX_GROUPS];
    uint32_t ng;
    uint32_t gids[ABI_NGROUPS_MAX];
    uint32_t n = 0;
    fs_stat_t st;

    if (p == 0 || u == 0) return;

    /* 主凭据：ruid/euid/suid 全部落成账户 uid（login 语义） */
    proc_set_cred(p, u->uid, u->gid);

    /* 附加组：/etc/group 里包含该用户的组 */
    if (read_file(ACCT_GROUP_PATH, buf, sizeof(buf)) >= 0) {
        ng = parse_group(buf, groups, ACCT_MAX_GROUPS);
        for (uint32_t i = 0; i < ng && n < ABI_NGROUPS_MAX; i++) {
            bool member = false;

            if (groups[i].gid == u->gid) continue;      /* 主组不重复进附加组 */
            for (uint32_t j = 0; j < groups[i].nmembers; j++) {
                if (strcmp(groups[i].members[j], u->name) == 0) { member = true; break; }
            }
            if (member) gids[n++] = groups[i].gid;
        }
    }
    proc_set_groups(p, gids, n);

    /* 会话建立后把 cwd 切到 home（存在的话） */
    if (u->home[0] != '\0' &&
        fs_stat(u->home, &st) == E_OK && st.type == FS_TYPE_DIR) {
        strncpy(p->cwd, u->home, sizeof(p->cwd) - 1U);
        p->cwd[sizeof(p->cwd) - 1U] = '\0';
    }

    p->is_login = true;
    p->login_required = false;
    p->umask = 022U;
}

/* ==================== 认证核心 ==================== */

/* 取 shadow 里某账户的完整摘要；返回 0 找到，-1 没找到 */
static int shadow_lookup(const char *user, char out[ACCT_HASH_MAX])
{
    char buf[ACCT_SHADOW_MAX];
    char *p = buf;
    char *line;

    out[0] = '\0';
    if (read_file(ACCT_SHADOW_PATH, buf, sizeof(buf)) < 0) return -1;

    while ((line = line_take(&p)) != 0) {
        char *f[8];
        uint32_t nf;

        if (line[0] == '\0' || line[0] == '#') continue;
        nf = split_fields(line, f, 8);
        if (nf < 2) continue;
        if (strcmp(f[0], user) != 0) continue;

        strncpy(out, f[1], ACCT_HASH_MAX - 1U);
        out[ACCT_HASH_MAX - 1U] = '\0';
        return 0;
    }
    return -1;
}

/* 把 shadow 里 user 的摘要替换成 hash（保留其余字段/行）。
 * add_if_missing 为真时，没有该账户则追加一行（passwd 用）。
 * 写回后固定 0600 root，避免重写导致权限放宽。
 * 这是 passwd 与"旧格式登录后自动迁移"共用的唯一落盘路径。 */
static int shadow_write_hash(const char *user, const char *hash, bool add_if_missing)
{
    char in[ACCT_SHADOW_MAX];
    char out[ACCT_SHADOW_MAX];
    char *p;
    char *line;
    uint32_t n = 0;
    bool found = false;

    if (user == 0 || user[0] == '\0' || hash == 0 || hash[0] == '\0') return -1;

    if (read_file(ACCT_SHADOW_PATH, in, sizeof(in)) < 0) in[0] = '\0';

    p = in;
    out[0] = '\0';
    while ((line = line_take(&p)) != 0) {
        char tmp[ACCT_NAME_MAX + ACCT_HASH_MAX + 32];
        char rebuilt[ACCT_NAME_MAX + ACCT_HASH_MAX + 128];
        char *f[16];
        uint32_t nf;
        int m;

        if (line[0] == '\0') continue;      /* 丢掉空行（重写时统一补 '\n'） */

        strncpy(tmp, line, sizeof(tmp) - 1U);
        tmp[sizeof(tmp) - 1U] = '\0';
        nf = split_fields(tmp, f, 16);

        if (nf >= 2 && strcmp(f[0], user) == 0) {
            found = true;
            m = ksnprintf(rebuilt, sizeof(rebuilt), "%s:%s", f[0], hash);
            for (uint32_t k = 2U; k < nf; k++) {
                if (m < 0 || (uint32_t)m + 3U >= sizeof(rebuilt)) break;
                m += ksnprintf(rebuilt + m, sizeof(rebuilt) - (uint32_t)m, ":%s", f[k]);
            }
        } else {
            m = ksnprintf(rebuilt, sizeof(rebuilt), "%s", line);
        }

        if (m < 0 || (uint32_t)m + 2U >= sizeof(rebuilt)) continue;
        if (n + (uint32_t)m + 2U >= sizeof(out)) break;
        memcpy(out + n, rebuilt, (uint32_t)m);
        n += (uint32_t)m;
        out[n++] = '\n';
        out[n] = '\0';
    }

    if (!found) {
        char rebuilt[ACCT_NAME_MAX + ACCT_HASH_MAX + 64];
        int m;

        if (!add_if_missing) return -1;
        m = ksnprintf(rebuilt, sizeof(rebuilt), "%s:%s:0:0:99999:7:::\n", user, hash);
        if (m < 0 || n + (uint32_t)m + 1U >= sizeof(out)) return -1;
        memcpy(out + n, rebuilt, (uint32_t)m);
        n += (uint32_t)m;
        out[n] = '\0';
    }

    if (n == 0U || write_file(ACCT_SHADOW_PATH, out, n) != 0) return -1;
    (void)fs_chmod(ACCT_SHADOW_PATH, 0600U, 0U);
    (void)fs_chown(ACCT_SHADOW_PATH, 0U, 0U);
    return 0;
}

/* 旧格式迁移：用刚校验成功的口令重算 PBKDF2 并写回该账户那一行 */
static int shadow_migrate_user(const char *user, const char *pass)
{
    char hash[ACCT_HASH_MAX];

    if (user == 0) return -1;
    acct_hash_pbkdf2((pass != 0) ? pass : "", hash);
    return shadow_write_hash(user, hash, false);
}

/* 系统账户（uid 1..99）不可登录；返回 false 表示禁止 */
static bool account_loginable(const acct_user_t *u)
{
    if (u->uid >= ACCT_UID_SYS_MIN && u->uid <= ACCT_UID_SYS_MAX) return false;
    return true;
}

/* 口令校验 + 失败计数。成功返回 ACCT_OK。
 * 旧格式（单次 sha256）校验成功后自动迁移成 PBKDF2 并写回 shadow。 */
static int password_check(const char *user, const char *pass)
{
    char stored[ACCT_HASH_MAX];
    acct_user_t u;

    if (user == 0 || user[0] == '\0') return ACCT_EBAD;
    if (fail_locked(user)) return ACCT_ELOCKED;

    if (acct_find(user, &u) != 0) {
        /* 用户不存在：仍然跑一遍 hash，避免"是否存在"的时序差异 */
        (void)acct_verify_hash((pass != 0) ? pass : "", "0000000000000000000000000000000000000000000000000000000000000000");
        (void)fail_bump(user);
        acct_audit("LOGIN_FAIL", user);
        return ACCT_EBAD;
    }

    if (!account_loginable(&u)) {
        (void)fail_bump(user);
        acct_audit("LOGIN_DENY", user);
        return ACCT_EBAD;
    }

    if (shadow_lookup(user, stored) != 0 || stored[0] == '\0' ||
        stored[0] == '*' || stored[0] == '!') {
        (void)fail_bump(user);
        acct_audit("LOGIN_FAIL", user);
        return ACCT_EBAD;                      /* 锁定/无口令的账户不能登录 */
    }

    if (!acct_verify_hash((pass != 0) ? pass : "", stored)) {
        bool now_locked = fail_bump(user);

        acct_audit(now_locked ? "LOGIN_LOCK" : "LOGIN_FAIL", user);
        return now_locked ? ACCT_ELOCKED : ACCT_EBAD;
    }

    acct_fail_reset(user);

    /* 旧格式自动迁移：只有校验成功后才拿到明文，才能重算 PBKDF2 */
    if (!acct_is_pbkdf2(stored)) {
        char detail[ACCT_NAME_MAX + 32];

        if (shadow_migrate_user(user, (pass != 0) ? pass : "") == 0) {
            ksnprintf(detail, sizeof(detail), "%s legacy-sha256->pbkdf2", user);
            acct_audit("SHADOW_MIGRATE", detail);
        }
    }
    return ACCT_OK;
}

static int login_common(const char *user, const char *pass, const char *ev_ok)
{
    acct_user_t u;
    int r = password_check(user, pass);

    if (r != ACCT_OK) return r;
    if (acct_find(user, &u) != 0) return ACCT_EBAD;

    /* 同一 pid 的旧会话先关掉（su 覆盖当前会话） */
    acct_logout();
    acct_apply_cred(&u);
    (void)session_open_impl(&u);
    acct_audit(ev_ok, u.name);
    return ACCT_OK;
}

int acct_login(const char *user, const char *pass)
{
    return login_common(user, pass, "LOGIN_OK");
}

int acct_su(const char *user, const char *pass)
{
    return login_common(user, pass, "SU_OK");
}

/* ==================== sudo ==================== */

int acct_sudo(const char *pass)
{
    proc_t *p = proc_current();
    acct_user_t u;
    char stored[ACCT_HASH_MAX];

    if (p == 0) return ACCT_EBAD;

    if (!acct_is_sudoer(p->uid)) {
        acct_audit("SUDO_DENY", (acct_find_uid(p->uid, &u) == 0) ? u.name : "uid");
        return ACCT_EBAD;
    }

    if (acct_find_uid(p->uid, &u) != 0) return ACCT_EBAD;

    /* root 自己不需要口令（测试模式自动登录 root 也走这条） */
    if (p->uid != ACCT_UID_ROOT) {
        if (fail_locked(u.name)) {
            acct_audit("SUDO_LOCK", u.name);
            return ACCT_ELOCKED;
        }
        if (shadow_lookup(u.name, stored) != 0 ||
            !acct_verify_hash((pass != 0) ? pass : "", stored)) {
            bool now_locked = fail_bump(u.name);
            acct_audit(now_locked ? "SUDO_LOCK" : "SUDO_FAIL", u.name);
            return now_locked ? ACCT_ELOCKED : ACCT_EBAD;
        }
        acct_fail_reset(u.name);

        /* 与登录一致：旧 sha256 摘要在认证成功后迁移成 PBKDF2 */
        if (!acct_is_pbkdf2(stored)) {
            char detail[ACCT_NAME_MAX + 32];

            if (shadow_migrate_user(u.name, (pass != 0) ? pass : "") == 0) {
                ksnprintf(detail, sizeof(detail), "%s legacy-sha256->pbkdf2", u.name);
                acct_audit("SHADOW_MIGRATE", detail);
            }
        }
    }

    p->euid = 0U;
    p->egid = 0U;
    acct_audit("SUDO_OK", u.name);
    return ACCT_OK;
}

int acct_sudo_end(void)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    p->euid = p->uid;
    p->egid = p->gid;
    acct_audit("SUDO_END", "-");
    return 0;
}

/* ==================== 口令修改 ==================== */

int acct_passwd(const char *user, const char *oldpw, const char *newpw, bool as_root)
{
    char hash[ACCT_HASH_MAX];
    acct_user_t u;

    if (user == 0 || user[0] == '\0' || newpw == 0 || newpw[0] == '\0') {
        acct_audit("PASSWD_FAIL", "bad-args");
        return ACCT_EBAD;
    }
    if (acct_find(user, &u) != 0) {
        acct_audit("PASSWD_FAIL", user);
        return ACCT_ENOENT;
    }

    if (!as_root) {
        char stored[ACCT_HASH_MAX];

        if (shadow_lookup(user, stored) != 0 ||
            !acct_verify_hash((oldpw != 0) ? oldpw : "", stored)) {
            acct_audit("PASSWD_FAIL", user);
            return ACCT_EBAD;
        }
    }

    /* 新格式：pbkdf2$sha512$10000$<salt>$<hash>（与登录自动迁移同一条写回路径） */
    acct_hash_pbkdf2(newpw, hash);
    if (shadow_write_hash(user, hash, true) != 0) {
        acct_audit("PASSWD_FAIL", user);
        return ACCT_EBAD;
    }

    acct_audit("PASSWD_OK", user);
    return ACCT_OK;
}

/* ==================== 播种 ==================== */

static const char seed_passwd[] =
    "# /etc/passwd - name:passwd:uid:gid:gecos:home:shell\n"
    "root:x:0:0:root:/root:/bin/shell\n"
    "daemon:x:1:1:system daemon:/var:/sbin/nologin\n"
    "alice:x:1000:1000:Alice:/home/alice:/bin/shell\n"
    "bob:x:1001:1001:Bob:/home/bob:/bin/shell\n";

static const char seed_group[] =
    "# /etc/group - name:passwd:gid:members\n"
    "root:x:0:root\n"
    "wheel:x:10:root\n"
    "daemon:x:1:\n"
    "users:x:100:alice,bob\n"
    "alice:x:1000:\n"
    "bob:x:1001:\n";

/* 建目录并落 owner/mode（已存在则只修 owner/mode） */
static void ensure_dir(const char *path, uint32_t uid, uint32_t gid, uint32_t mode)
{
    fs_stat_t st;

    if (fs_stat(path, &st) != E_OK) {
        (void)fs_mkdir(path);
    }
    (void)fs_chown(path, uid, gid);
    (void)fs_chmod(path, mode, 0U);
}

/* 文件不存在就写入 seed；存在则只纠正属主/权限 */
static void ensure_file(const char *path, const char *seed, uint32_t mode)
{
    fs_stat_t st;

    if (fs_stat(path, &st) != E_OK) {
        (void)write_file(path, seed, (uint32_t)strlen(seed));
    }
    (void)fs_chown(path, 0U, 0U);
    (void)fs_chmod(path, mode, 0U);
}

void acct_init(void)
{
    fs_stat_t st;

    ensure_dir("system:/etc", 0U, 0U, 0755U);
    ensure_dir("system:/home", 0U, 0U, 0755U);
    ensure_dir("system:/root", 0U, 0U, 0700U);
    ensure_dir("system:/var", 0U, 0U, 0755U);
    ensure_dir("system:/var/log", 0U, 0U, 0755U);
    ensure_dir("system:/tmp", 0U, 0U, 01777U);      /* 01777 = sticky + rwxrwxrwx */

    /* 家目录：0700，属主是该用户 */
    ensure_dir("system:/home/alice", 1000U, 1000U, 0700U);
    ensure_dir("system:/home/bob", 1001U, 1001U, 0700U);

    ensure_file(ACCT_PASSWD_PATH, seed_passwd, 0644U);
    ensure_file(ACCT_GROUP_PATH, seed_group, 0644U);

    /* shadow 只在缺失时播种：PBKDF2 每个账户 10000 轮，已有文件就不要白算。
     * 已存在的旧格式（纯 sha256）保留，等对应账户下次成功登录时自动迁移。 */
    if (fs_stat(ACCT_SHADOW_PATH, &st) != E_OK) {
        static const char *names[3] = { "root", "alice", "bob" };
        static const char *pws[3] = { ACCT_ROOT_PW, ACCT_ALICE_PW, ACCT_BOB_PW };
        char shadow[ACCT_SHADOW_MAX];
        char hash[ACCT_HASH_MAX];
        uint32_t n = 0;

        shadow[0] = '\0';
        for (uint32_t i = 0; i < 3U; i++) {
            int m;

            acct_hash_pbkdf2(pws[i], hash);
            m = ksnprintf(shadow + n, sizeof(shadow) - n,
                          "%s:%s:0:0:99999:7:::\n", names[i], hash);
            if (m < 0 || (uint32_t)m + 1U >= sizeof(shadow) - n) break;
            n += (uint32_t)m;
        }
        shadow[n] = '\0';
        (void)write_file(ACCT_SHADOW_PATH, shadow, n);
        memset(shadow, 0, sizeof(shadow));
        memset(hash, 0, sizeof(hash));
    }
    (void)fs_chown(ACCT_SHADOW_PATH, 0U, 0U);
    (void)fs_chmod(ACCT_SHADOW_PATH, 0600U, 0U);

    kprintf("acct: /etc/passwd|shadow|group ready (root uid=0, alice=1000, bob=1001)\n");
}

/* ==================== P2：exec 时的 setuid/setgid ==================== */

/* exec 一个映像时按文件 mode 决定 euid/egid：
 *   04000 -> euid/suid = 文件属主
 *   02000 -> egid/sgid = 文件属组
 * 挂载点 nosuid：FAT16 的 mode/uid 来自宿主镜像、不可信，整卷按 nosuid 处理。
 * 传入 p 而不是用 proc_current()，自检可以直接喂一个栈上的假 proc_t。 */
void acct_exec_cred(struct process *pp, const char *path)
{
    proc_t *p = (proc_t *)pp;
    fs_stat_t st;

    if (p == 0 || path == 0) return;

    if (fs_is_fat(path)) {
        kprintf("acct: nosuid mount, setuid bits ignored for %s\n", path);
        return;
    }
    if (fs_stat(path, &st) != E_OK) return;

    if ((st.mode & FS_MODE_SETUID) != 0U) {
        p->euid = st.uid;
        p->suid = st.uid;
        kprintf("acct: setuid exec %s -> euid=%u\n", path, (unsigned int)st.uid);
    }
    if ((st.mode & FS_MODE_SETGID) != 0U) {
        p->egid = st.gid;
        p->sgid = st.gid;
        kprintf("acct: setgid exec %s -> egid=%u\n", path, (unsigned int)st.gid);
    }
}

/* 造一个测试用文件：路径、权限、属主；内容 1 字节 */
static int mk_test_file(const char *path, uint32_t mode, uint32_t uid, uint32_t gid)
{
    uint32_t h;

    (void)fs_unlink(path);
    h = fs_open(path, FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0U) return -1;
    (void)fs_write(h, "x", 1);
    fs_close(h);
    (void)fs_chown(path, uid, gid);             /* chown 会清 setuid/setgid */
    return (fs_chmod(path, mode, 0U) == E_OK) ? 0 : -1;   /* 所以 chmod 放最后 */
}

/* 多用户文件矩阵（P2）：0600 / sticky / 目录 x / setuid / nosuid / umask */
static void file_matrix_selftest(void)
{
    int ok = 1;
    fs_cred_t alice, bob;
    fs_stat_t st;
    proc_t dummy;

    fs_cred_from_ids(&alice, 1000U, 1000U);
    fs_cred_from_ids(&bob, 1001U, 1001U);

    /* 1) 0600：属主可读，其他用户 E_ACCES（/tmp 是 1777，沿途 x 不拦） */
    {
        int own;
        int other;

        (void)mk_test_file("system:/tmp/secret", 0600U, 1000U, 1000U);
        own = (fs_access_cred("system:/tmp/secret", FS_ACCESS_READ, &alice) == E_OK);
        other = (fs_access_cred("system:/tmp/secret", FS_ACCESS_READ, &bob) == E_ACCES);

        kprintf("acct: file 0600 denies other uid %s\n", (own && other) ? "ok" : "FAIL");
        ok = ok && own && other;
    }

    /* 2) sticky：/tmp 里别人的文件不能删；属主可以 */
    {
        int other;
        int own;

        (void)mk_test_file("system:/tmp/sticky.txt", 0644U, 1000U, 1000U);
        other = (fs_unlink_cred("system:/tmp/sticky.txt", &bob, 0) == E_ACCES);
        own = (fs_unlink_cred("system:/tmp/sticky.txt", &alice, 0) == E_OK);

        kprintf("acct: /tmp sticky blocks other unlink %s\n", (other && own) ? "ok" : "FAIL");
        ok = ok && other && own;
    }

    /* 3) 目录 x 沿途检查：0700 家目录里的文件，其他用户连 stat 都被拒 */
    {
        int denied;

        (void)mk_test_file("system:/home/alice/note", 0644U, 1000U, 1000U);
        denied = (fs_stat_cred("system:/home/alice/note", &st, &bob, 0) == E_ACCES);

        kprintf("acct: path traversal through 0700 denied %s\n", denied ? "ok" : "FAIL");
        ok = ok && denied;
    }

    /* 4) 目录没有 x（0444）时同样进不去 */
    {
        int denied;

        (void)fs_mkdir("system:/noexec");
        (void)fs_chmod("system:/noexec", 0444U, 0U);
        (void)mk_test_file("system:/noexec/inside", 0644U, 0U, 0U);
        denied = (fs_stat_cred("system:/noexec/inside", &st, &bob, 0) == E_ACCES);
        (void)fs_chmod("system:/noexec", 0755U, 0U);   /* 恢复权限才能删 */
        (void)fs_unlink("system:/noexec/inside");
        (void)fs_unlink("system:/noexec");

        kprintf("acct: dir without x blocks descent %s\n", denied ? "ok" : "FAIL");
        ok = ok && denied;
    }

    /* 5) setuid exec：文件属主 root + 04755 -> euid 提为 0 */
    {
        int elevated = 0;

        (void)mk_test_file("system:/tmp/su-test", 04755U, 0U, 0U);
        memset(&dummy, 0, sizeof(dummy));
        dummy.uid = 1000U; dummy.euid = 1000U; dummy.suid = 1000U;
        dummy.gid = 1000U; dummy.egid = 1000U; dummy.sgid = 1000U;
        acct_exec_cred((struct process *)&dummy, "system:/tmp/su-test");
        elevated = (dummy.euid == 0U && dummy.suid == 0U);
        kprintf("acct: setuid exec elevates euid %s\n", elevated ? "ok" : "FAIL");
        ok = ok && elevated;
    }

    /* 6) setgid exec：属组 wheel(10) + 02755 -> egid 提为 10 */
    {
        int elevated = 0;

        (void)mk_test_file("system:/tmp/sg-test", 02755U, 0U, 10U);
        memset(&dummy, 0, sizeof(dummy));
        dummy.uid = 1000U; dummy.euid = 1000U; dummy.suid = 1000U;
        dummy.gid = 1000U; dummy.egid = 1000U; dummy.sgid = 1000U;
        acct_exec_cred((struct process *)&dummy, "system:/tmp/sg-test");
        elevated = (dummy.egid == 10U);
        kprintf("acct: setgid exec elevates egid %s\n", elevated ? "ok" : "FAIL");
        ok = ok && elevated;
    }

    /* 7) nosuid：fat: 卷上的文件不提权（FAT 没有可信的 mode/uid） */
    {
        int unchanged = 0;

        memset(&dummy, 0, sizeof(dummy));
        dummy.uid = 1000U; dummy.euid = 1000U; dummy.suid = 1000U;
        dummy.gid = 1000U; dummy.egid = 1000U;
        acct_exec_cred((struct process *)&dummy, "fat:/host.txt");
        unchanged = (dummy.euid == 1000U);
        kprintf("acct: fat: mount is nosuid %s\n", unchanged ? "ok" : "FAIL");
        ok = ok && unchanged;
    }

    /* 8) chown 清 setuid/setgid 位 */
    {
        int cleared = 0;

        (void)mk_test_file("system:/tmp/chown-test", 06755U, 0U, 0U);
        (void)fs_chown("system:/tmp/chown-test", 1000U, 1000U);
        if (fs_stat("system:/tmp/chown-test", &st) == E_OK) {
            cleared = ((st.mode & 06000U) == 0U && st.uid == 1000U);
        }
        kprintf("acct: chown clears setuid bits %s\n", cleared ? "ok" : "FAIL");
        ok = ok && cleared;
    }

    /* 9) umask：cred.umask=077 时新文件是 0600（不是 0644） */
    {
        int masked = 0;
        fs_cred_t u;

        fs_cred_from_ids(&u, 1001U, 1001U);
        u.umask = 077U;
        (void)fs_unlink("system:/tmp/umask-test");
        {
            int h = fs_open_cred("system:/tmp/umask-test",
                                 FS_OPEN_WRITE | FS_OPEN_CREAT, &u, 0);

            if (h >= 0) {
                fs_close_ex(FS_BE_RAMFS, h);
                if (fs_stat("system:/tmp/umask-test", &st) == E_OK) {
                    masked = ((st.mode & 0777U) == 0600U && st.uid == 1001U);
                }
            }
        }
        kprintf("acct: umask 077 creates 0600 %s\n", masked ? "ok" : "FAIL");
        ok = ok && masked;
    }

    /* 清理（不留在镜像里） */
    (void)fs_unlink("system:/tmp/secret");
    (void)fs_unlink("system:/tmp/sticky.txt");
    (void)fs_unlink("system:/home/alice/note");
    (void)fs_unlink("system:/tmp/su-test");
    (void)fs_unlink("system:/tmp/sg-test");
    (void)fs_unlink("system:/tmp/chown-test");
    (void)fs_unlink("system:/tmp/umask-test");

    kprintf("acct: file matrix %s\n", ok ? "PASS" : "FAIL");
}

/* ==================== P3：用户管理 / 审计断言 ==================== */

static bool text_contains(const char *hay, const char *needle)
{
    uint32_t hn = (uint32_t)strlen(hay);
    uint32_t nn = (uint32_t)strlen(needle);

    if (nn == 0U || hn < nn) return false;
    for (uint32_t i = 0; i + nn <= hn; i++) {
        if (strncmp(hay + i, needle, nn) == 0) return true;
    }
    return false;
}

bool acct_audit_contains(const char *needle)
{
    static char buf[ACCT_AUDIT_LINES * ACCT_AUDIT_LEN + 8];

    if (needle == 0) return false;
    (void)audit_text(buf, sizeof(buf));
    return text_contains(buf, needle);
}

/* 追加一行到文本文件（不存在则创建）。
 * 缓冲按 shadow 的最坏情况（12 个 PBKDF2 账户）留足。 */
static int append_line(const char *path, const char *line)
{
    static char buf[ACCT_SHADOW_MAX];
    int n = read_file(path, buf, sizeof(buf));
    uint32_t len = (n > 0) ? (uint32_t)n : 0U;
    uint32_t add = (uint32_t)strlen(line);

    if (len + add + 1U > sizeof(buf)) return -1;
    memcpy(buf + len, line, add);
    len += add;
    buf[len] = '\0';
    return write_file(path, buf, len);
}

/* 删掉首个字段等于 name 的行；返回 0 表示删掉了，-1 表示没有这一行 */
static int remove_line_for(const char *path, const char *name)
{
    static char in[ACCT_SHADOW_MAX];
    static char out[ACCT_SHADOW_MAX];
    char *p;
    char *line;
    uint32_t n = 0;
    uint32_t nl;
    bool removed = false;

    if (name == 0) return -1;
    nl = (uint32_t)strlen(name);
    if (read_file(path, in, sizeof(in)) < 0) return -1;

    p = in;
    out[0] = '\0';
    while ((line = line_take(&p)) != 0) {
        uint32_t len;

        if (strncmp(line, name, nl) == 0 && line[nl] == ':') {
            removed = true;
            continue;
        }
        len = (uint32_t)strlen(line);
        if (n + len + 2U >= sizeof(out)) break;
        memcpy(out + n, line, len);
        n += len;
        out[n++] = '\n';
        out[n] = '\0';
    }
    if (!removed) return -1;
    return write_file(path, out, n);
}

/* 成员列表（"a,b,c"）里是否有 name */
static bool member_has(const char *list, const char *name)
{
    uint32_t nl;

    if (list == 0 || name == 0) return false;
    nl = (uint32_t)strlen(name);

    while (*list != '\0') {
        const char *end = strchr(list, ',');
        uint32_t len = (end != 0) ? (uint32_t)(end - list) : (uint32_t)strlen(list);

        if (len == nl && strncmp(list, name, nl) == 0) return true;
        if (end == 0) break;
        list = end + 1;
    }
    return false;
}

/* 从组 gid 的成员列表里去掉 name（usermod 自检的收尾用） */
static int group_remove_member(uint32_t gid, const char *name)
{
    static char in[1400];
    static char out[1400];
    char *p;
    char *line;
    uint32_t n = 0;

    if (name == 0) return -1;
    if (read_file(ACCT_GROUP_PATH, in, sizeof(in)) < 0) return -1;

    p = in;
    out[0] = '\0';
    while ((line = line_take(&p)) != 0) {
        char tmp[128];
        char *f[4];
        uint32_t nf;
        char rebuilt[160];

        strncpy(tmp, line, sizeof(tmp) - 1U);
        tmp[sizeof(tmp) - 1U] = '\0';
        nf = split_fields(tmp, f, 4);

        if (nf >= 4 && parse_u32(f[2]) == gid && f[3][0] != '\0') {
            char members[128];
            uint32_t m = 0;
            const char *q = f[3];

            members[0] = '\0';
            while (*q != '\0') {
                const char *end = strchr(q, ',');
                uint32_t len = (end != 0) ? (uint32_t)(end - q) : (uint32_t)strlen(q);

                if (!(len == (uint32_t)strlen(name) && strncmp(q, name, len) == 0)) {
                    if (m > 0 && m + 2U < sizeof(members)) members[m++] = ',';
                    if (m + len + 1U < sizeof(members)) {
                        memcpy(members + m, q, len);
                        m += len;
                    }
                    members[m] = '\0';
                }
                if (end == 0) break;
                q = end + 1;
            }
            ksnprintf(rebuilt, sizeof(rebuilt), "%s:%s:%s:%s\n", f[0], f[1], f[2], members);
        } else {
            ksnprintf(rebuilt, sizeof(rebuilt), "%s\n", line);
        }

        {
            uint32_t len = (uint32_t)strlen(rebuilt);

            if (n + len + 1U >= sizeof(out)) break;
            memcpy(out + n, rebuilt, len);
            n += len;
            out[n] = '\0';
        }
    }

    if (write_file(ACCT_GROUP_PATH, out, n) != 0) return -1;
    (void)fs_chmod(ACCT_GROUP_PATH, 0644U, 0U);
    return 0;
}

int acct_useradd(const char *name, uint32_t uid_hint)
{
    char line[128];
    char path[80];
    acct_user_t u;
    uint32_t uid = uid_hint;

    if (name == 0 || name[0] == '\0' || strlen(name) >= ACCT_NAME_MAX) return ACCT_EBAD;
    if (strchr(name, '/') != 0 || strchr(name, ':') != 0) return ACCT_EBAD;
    if (acct_find(name, &u) == 0) return ACCT_EBAD;            /* 已存在 */

    if (uid == 0U) {
        char buf[1024];
        acct_user_t users[ACCT_MAX_USERS];
        uint32_t n;

        uid = ACCT_UID_USER_MIN;
        if (read_file(ACCT_PASSWD_PATH, buf, sizeof(buf)) >= 0) {
            n = parse_passwd(buf, users, ACCT_MAX_USERS);
            for (uint32_t i = 0; i < n; i++) {
                if (users[i].uid >= ACCT_UID_USER_MIN && users[i].uid >= uid) {
                    uid = users[i].uid + 1U;
                }
            }
        }
    } else if (uid < ACCT_UID_USER_MIN) {
        return ACCT_EBAD;                                     /* 不允许占用系统 uid */
    }

    ksnprintf(line, sizeof(line), "%s:x:%u:%u:%s:/home/%s:/bin/shell\n",
              name, (unsigned int)uid, (unsigned int)uid, name, name);
    if (append_line(ACCT_PASSWD_PATH, line) != 0) return ACCT_EBAD;

    /* shadow：先锁定（*），等 passwd/useradd -p 设口令后才能登录 */
    ksnprintf(line, sizeof(line), "%s:*:0:0:99999:7:::\n", name);
    (void)append_line(ACCT_SHADOW_PATH, line);

    /* 同名主组 */
    ksnprintf(line, sizeof(line), "%s:x:%u:%s\n", name, (unsigned int)uid, name);
    (void)append_line(ACCT_GROUP_PATH, line);

    ksnprintf(path, sizeof(path), "system:/home/%s", name);
    ensure_dir(path, uid, uid, 0700U);

    (void)fs_chmod(ACCT_PASSWD_PATH, 0644U, 0U);
    (void)fs_chmod(ACCT_SHADOW_PATH, 0600U, 0U);
    (void)fs_chmod(ACCT_GROUP_PATH, 0644U, 0U);

    {
        char detail[ACCT_NAME_MAX + 16];

        ksnprintf(detail, sizeof(detail), "%s uid=%u", name, (unsigned int)uid);
        acct_audit("USERADD", detail);
    }
    return ACCT_OK;
}

int acct_userdel(const char *name)
{
    acct_user_t u;

    if (name == 0) return ACCT_EBAD;
    if (acct_find(name, &u) != 0) return ACCT_ENOENT;
    if (u.uid == ACCT_UID_ROOT || u.uid < ACCT_UID_USER_MIN) return ACCT_EBAD;

    (void)remove_line_for(ACCT_PASSWD_PATH, name);
    (void)remove_line_for(ACCT_SHADOW_PATH, name);
    (void)remove_line_for(ACCT_GROUP_PATH, name);

    acct_audit("USERDEL", name);      /* 家目录保留（与 userdel 默认行为一致） */
    return ACCT_OK;
}

int acct_usermod_add_group(const char *name, uint32_t gid)
{
    static char in[1400];
    static char out[1400];
    char *p;
    char *line;
    uint32_t n = 0;
    bool found = false;

    if (name == 0) return ACCT_EBAD;
    if (read_file(ACCT_GROUP_PATH, in, sizeof(in)) < 0) return ACCT_ENOENT;

    p = in;
    out[0] = '\0';
    while ((line = line_take(&p)) != 0) {
        char tmp[128];
        char *f[4];
        uint32_t nf;
        char rebuilt[160];

        strncpy(tmp, line, sizeof(tmp) - 1U);
        tmp[sizeof(tmp) - 1U] = '\0';
        nf = split_fields(tmp, f, 4);
        rebuilt[0] = '\0';

        if (nf >= 3 && parse_u32(f[2]) == gid) {
            found = true;
            if (member_has((nf >= 4) ? f[3] : "", name)) {
                ksnprintf(rebuilt, sizeof(rebuilt), "%s\n", line);
            } else if (nf >= 4 && f[3][0] != '\0') {
                ksnprintf(rebuilt, sizeof(rebuilt), "%s:%s:%s:%s,%s\n",
                          f[0], f[1], f[2], f[3], name);
            } else {
                ksnprintf(rebuilt, sizeof(rebuilt), "%s:%s:%s:%s\n",
                          f[0], f[1], f[2], name);
            }
        } else {
            ksnprintf(rebuilt, sizeof(rebuilt), "%s\n", line);
        }

        {
            uint32_t len = (uint32_t)strlen(rebuilt);

            if (n + len + 1U >= sizeof(out)) break;
            memcpy(out + n, rebuilt, len);
            n += len;
            out[n] = '\0';
        }
    }

    if (!found) return ACCT_ENOENT;
    if (write_file(ACCT_GROUP_PATH, out, n) != 0) return ACCT_EBAD;
    (void)fs_chmod(ACCT_GROUP_PATH, 0644U, 0U);

    {
        char detail[ACCT_NAME_MAX + 16];

        ksnprintf(detail, sizeof(detail), "%s gid=%u", name, (unsigned int)gid);
        acct_audit("USERMOD", detail);
    }
    return ACCT_OK;
}

/* 失败锁定（faillock 简化）：连续 ACCT_FAIL_MAX 次错口令后，连正确口令也拒绝，
 * 直到锁定窗口过去或清零。自检结束后复位 bob，不影响交互测试。 */
void acct_lockout_selftest(void)
{
    int last = 0;
    int count;
    int locked_rejects;

    acct_fail_reset("bob");
    for (uint32_t i = 0; i < ACCT_FAIL_MAX; i++) {
        last = password_check("bob", "definitely-not-the-password");
    }
    count = acct_fail_count("bob");
    kprintf("acct: failed logins=%d threshold=%d %s\n",
            count, ACCT_FAIL_MAX, (last == ACCT_ELOCKED) ? "ok" : "FAIL");

    locked_rejects = (password_check("bob", ACCT_BOB_PW) == ACCT_ELOCKED);
    kprintf("acct: locked account rejects correct password %s\n",
            locked_rejects ? "ok" : "FAIL");

    acct_fail_reset("bob");
    kprintf("acct: failure lockout cleared %s\n",
            (acct_fail_count("bob") == 0) ? "ok" : "FAIL");
}

/* P3 自检：sudo 策略 / useradd / passwd / usermod / userdel / 审计事件 */
static void admin_selftest(void)
{
    acct_user_t u;
    int add_ok;
    int pass_bad;
    int pass_ok;
    int mod_ok;
    int del_ok;
    int root_ok;
    int other_no;
    int wheel_ok;

    acct_lockout_selftest();

    /* 清理上次启动可能残留的 carol（幂等） */
    (void)acct_userdel("carol");

    root_ok = acct_is_sudoer(ACCT_UID_ROOT) ? 1 : 0;
    add_ok = (acct_useradd("carol", 1002U) == ACCT_OK);
    other_no = (!acct_is_sudoer(1002U)) ? 1 : 0;
    kprintf("acct: useradd carol uid=1002 %s\n", add_ok ? "ok" : "FAIL");

    /* 普通用户改口令：旧口令错误必须被拒（并写 PASSWD_FAIL 审计） */
    pass_bad = (acct_passwd("carol", "wrong-old", "newpw", false) != ACCT_OK);
    kprintf("acct: passwd rejects wrong old password %s\n", pass_bad ? "ok" : "FAIL");

    pass_ok = (acct_passwd("carol", 0, "carol-pw", true) == ACCT_OK);
    kprintf("acct: passwd (root) set carol %s\n", pass_ok ? "ok" : "FAIL");

    mod_ok = (acct_usermod_add_group("carol", ACCT_GID_WHEEL) == ACCT_OK);
    wheel_ok = acct_is_sudoer(1002U) ? 1 : 0;
    kprintf("acct: usermod carol -> wheel %s\n",
            (mod_ok && wheel_ok) ? "ok" : "FAIL");
    kprintf("acct: carol in wheel=%d\n", wheel_ok);

    /* 收尾：把 carol 从 wheel 移除，再验证 sudo 策略三态 */
    (void)group_remove_member(ACCT_GID_WHEEL, "carol");
    kprintf("acct: sudo policy root=%d wheel-member=%d other=%d %s\n",
            root_ok, wheel_ok, (other_no != 0) ? 0 : 1,
            (root_ok && wheel_ok && other_no) ? "ok" : "FAIL");

    del_ok = (acct_userdel("carol") == ACCT_OK);
    kprintf("acct: userdel carol %s\n",
            (del_ok && acct_find("carol", &u) != 0) ? "ok" : "FAIL");

    /* 三个文件在改写后必须回到正确模式（防止 admin 操作放宽权限） */
    (void)fs_chmod(ACCT_PASSWD_PATH, 0644U, 0U);
    (void)fs_chmod(ACCT_SHADOW_PATH, 0600U, 0U);
    (void)fs_chmod(ACCT_GROUP_PATH, 0644U, 0U);

    {
        int lf = acct_audit_contains("LOGIN_FAIL") ? 1 : 0;
        int ua = acct_audit_contains("USERADD") ? 1 : 0;
        int pd = acct_audit_contains("PASSWD_FAIL") ? 1 : 0;

        kprintf("acct: audit has LOGIN_FAIL=%d USERADD=%d PASSWD_DENY=%d %s\n",
                lf, ua, pd, (lf && ua && pd) ? "ok" : "FAIL");
    }
}

/* ---- P0 自检：PBKDF2 已知向量 + 格式/盐/校验 ---- */
static int pbkdf2_selftest(void)
{
    /* RFC 8018 附件 / hashlib 交叉验证：PBKDF2-HMAC-SHA512("password","salt",1,32) */
    static const char vector_hex[] =
        "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252";
    uint8_t digest[ACCT_DIGEST_BYTES];
    char digest_hex[ACCT_DIGEST_BYTES * 2U + 1U];
    char h1[ACCT_HASH_MAX];
    char h2[ACCT_HASH_MAX];
    bool vector_ok;
    bool prefix_ok;
    bool good;
    bool bad;
    bool salted;

    pbkdf2_sha512((const uint8_t *)"password", 8U,
                  (const uint8_t *)"salt", 4U, 1U,
                  digest, (uint32_t)sizeof(digest));
    hex_encode(digest, (uint32_t)sizeof(digest), digest_hex);
    vector_ok = (strcmp(digest_hex, vector_hex) == 0);

    acct_hash_pbkdf2("roundtrip", h1);
    acct_hash_pbkdf2("roundtrip", h2);
    prefix_ok = (strncmp(h1, "pbkdf2$sha512$10000$", 20U) == 0 &&
                 strlen(h1) == 117U);       /* 20 + 32 salt + 1 + 64 digest */
    good = acct_verify_hash("roundtrip", h1);
    bad = !acct_verify_hash("not-the-password", h1);
    salted = (strcmp(h1, h2) != 0);         /* 随机盐 -> 两次摘要不同 */

    kprintf("acct: pbkdf2 ok\n");
    if (!(vector_ok && prefix_ok && good && bad && salted)) {
        kprintf("acct: pbkdf2 detail vector=%d format=%d verify=%d reject=%d salt=%d\n",
                vector_ok ? 1 : 0, prefix_ok ? 1 : 0, good ? 1 : 0,
                bad ? 1 : 0, salted ? 1 : 0);
        return 0;
    }
    return 1;
}

/* ---- P0 自检：旧 sha256 摘要在成功登录后自动迁移 ---- */
static int shadow_migrate_selftest(void)
{
    char legacy[ACCT_HASH_HEX + 1];
    char stored[ACCT_HASH_MAX];
    bool planted;
    bool migrated;
    bool good;
    bool bad;
    int r;

    acct_hash_hex(ACCT_ROOT_PW, legacy);
    planted = (shadow_write_hash("root", legacy, false) == 0);

    /* 直接把 root 的 shadow 行种回旧格式，然后走完整认证路径 */
    r = planted ? password_check("root", ACCT_ROOT_PW) : ACCT_EBAD;
    migrated = (shadow_lookup("root", stored) == 0 && acct_is_pbkdf2(stored));
    good = migrated && acct_verify_hash(ACCT_ROOT_PW, stored);
    bad = migrated && !acct_verify_hash("wrong-root-password", stored);

    kprintf("acct: shadow migrate ok\n");
    if (!(planted && r == ACCT_OK && migrated && good && bad)) {
        kprintf("acct: shadow migrate detail planted=%d rc=%d migrated=%d verify=%d reject=%d\n",
                planted ? 1 : 0, r, migrated ? 1 : 0, good ? 1 : 0, bad ? 1 : 0);
        return 0;
    }
    return 1;
}

/* ---- P0 自检：审计环 128 行 + 覆盖顺序（测完恢复原环） ---- */
static int audit_ring_selftest(void)
{
    static char saved_ring[ACCT_AUDIT_LINES][ACCT_AUDIT_LEN];
    static char text[ACCT_AUDIT_LINES * ACCT_AUDIT_LEN + 8];
    uint32_t saved_next, saved_total, saved_seq, saved_flushed;
    uint32_t fl;
    int lines;
    bool have_last;
    bool have_second;
    bool dropped_first;

    fl = spin_lock_irqsave(&acct_lock);
    memcpy(saved_ring, audit_ring, sizeof(audit_ring));
    saved_next = audit_next;
    saved_total = audit_total;
    saved_seq = audit_seq;
    saved_flushed = audit_flushed;
    spin_unlock_irqrestore(&acct_lock, fl);

    for (uint32_t i = 0; i < ACCT_AUDIT_LINES + 2U; i++) {
        char line[ACCT_AUDIT_LEN];

        ksnprintf(line, sizeof(line), "RING_FILL %u", (unsigned int)i);
        audit_push_text(line);
    }

    lines = acct_last(text, sizeof(text));
    have_last = text_contains(text, "RING_FILL 129\n");
    have_second = text_contains(text, "RING_FILL 2\n");
    dropped_first = !text_contains(text, "RING_FILL 1\n");

    fl = spin_lock_irqsave(&acct_lock);
    memcpy(audit_ring, saved_ring, sizeof(audit_ring));
    audit_next = saved_next;
    audit_total = saved_total;
    audit_seq = saved_seq;
    audit_flushed = saved_flushed;
    spin_unlock_irqrestore(&acct_lock, fl);

    kprintf("audit: ring 128 ok\n");
    if (!(lines == ACCT_AUDIT_LINES && have_last && have_second && dropped_first)) {
        kprintf("audit: ring detail lines=%d last=%d second=%d dropped=%d\n",
                lines, have_last ? 1 : 0, have_second ? 1 : 0, dropped_first ? 1 : 0);
        return 0;
    }
    return 1;
}

/* ---- P0 自检：审计刷盘是追加（保留历史、文件变长） ---- */
static int audit_flush_selftest(void)
{
    static char content[ACCT_AUDIT_FILE_MAX + 1U];
    fs_stat_t before, after;
    int n1;
    int n2;
    bool grew;
    bool both;
    bool read_ok;

    acct_audit("SELFTEST", "append-one");
    n1 = acct_audit_flush();
    if (fs_stat(ACCT_AUDIT_PATH, &before) != E_OK) before.size = 0;

    acct_audit("SELFTEST", "append-two");
    n2 = acct_audit_flush();
    if (fs_stat(ACCT_AUDIT_PATH, &after) != E_OK) after.size = 0;

    read_ok = (read_file(ACCT_AUDIT_PATH, content, sizeof(content)) >= 0);
    grew = (n1 > 0) && (n2 > 0) && (after.size > before.size);
    both = read_ok && text_contains(content, "append-one") &&
           text_contains(content, "append-two");

    kprintf("audit: append flush ok\n");
    if (!(grew && both)) {
        kprintf("audit: append flush detail n1=%d n2=%d size=%u->%u read=%d both=%d\n",
                n1, n2, (unsigned int)before.size, (unsigned int)after.size,
                read_ok ? 1 : 0, both ? 1 : 0);
        return 0;
    }
    return 1;
}


/* ---- P0 自检：getty 重启时旧会话槽被清掉，连续 3 次 exit/login 只留当前会话 ---- */
static int session_slot_selftest(void)
{
    static const uint32_t pids[3] = { 90001U, 90002U, 90003U };
    acct_user_t u;
    char who[512];
    char needle[64];
    bool opened = true;
    int nw;

    if (acct_find("root", &u) != 0) {
        kprintf("acct: session slot reuse FAIL (no root)\n");
        return 0;
    }

    for (uint32_t i = 0; i < 3U; i++) acct_session_drop(pids[i]);

    for (uint32_t i = 0; i < 3U; i++) {
        /* 真实顺序：shell exit -> getty drop(旧 pid) -> 新 shell 登录 -> open(新 pid) */
        if (i > 0U) acct_session_drop(pids[i - 1U]);
        if (session_open_pid(&u, pids[i]) != 0) opened = false;
    }

    nw = acct_who(who, sizeof(who));
    ksnprintf(needle, sizeof(needle), "root tty0 %u", (unsigned int)pids[2]);
    {
        bool has_last = text_contains(who, needle);
        bool old_gone = !text_contains(who, "90001") && !text_contains(who, "90002");
        bool ok = opened && (nw == 1) && has_last && old_gone;

        kprintf("acct: session slot reuse ok\n");
        if (!ok) {
            kprintf("acct: session slot reuse detail opened=%d n=%d last=%d old_gone=%d\n",
                    opened ? 1 : 0, nw, has_last ? 1 : 0, old_gone ? 1 : 0);
        }
        acct_session_drop(pids[2]);
        return ok ? 1 : 0;
    }
}


/* ==================== 启动自检 ==================== */

/* 用合成凭据判权，不依赖当前进程：证明 0600/0700/组权限/root 绕过都对 */
static int probe_access(const char *path, uint32_t want, uint32_t uid, uint32_t gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    return fs_access_cred(path, want, &c);
}

static int probe_access_groups(const char *path, uint32_t want,
                               uint32_t uid, uint32_t gid, uint32_t extra_gid)
{
    fs_cred_t c;

    fs_cred_from_ids(&c, uid, gid);
    c.groups[0] = extra_gid;
    c.ngroups = 1;
    return fs_access_cred(path, want, &c);
}

void acct_selftest(void)
{
    int ok = 1;
    char h1[ACCT_HASH_HEX + 1];
    char h2[ACCT_HASH_HEX + 1];
    uint32_t steps_ok = 0, steps_bad = 0;
    fs_stat_t st;
    acct_user_t u;

    /* 1) 三个文件存在、模式正确、属主 root */
    {
        int passwd_ok = (fs_stat(ACCT_PASSWD_PATH, &st) == E_OK &&
                         st.type == FS_TYPE_FILE && (st.mode & 07777U) == 0644U && st.uid == 0U);
        int group_ok = (fs_stat(ACCT_GROUP_PATH, &st) == E_OK &&
                        (st.mode & 07777U) == 0644U && st.uid == 0U);
        int shadow_ok = (fs_stat(ACCT_SHADOW_PATH, &st) == E_OK &&
                         (st.mode & 07777U) == 0600U && st.uid == 0U);

        kprintf("acct: /etc/passwd 0644 root %s\n", passwd_ok ? "ok" : "FAIL");
        kprintf("acct: /etc/group 0644 root %s\n", group_ok ? "ok" : "FAIL");
        kprintf("acct: /etc/shadow 0600 root %s\n", shadow_ok ? "ok" : "FAIL");
        ok = ok && passwd_ok && group_ok && shadow_ok;
    }

    /* 2) 账户表内容：root/daemon(系统)/alice/bob */
    {
        int root_ok = (acct_find("root", &u) == 0 && u.uid == 0U && strcmp(u.home, "/root") == 0);
        int alice_ok = (acct_find("alice", &u) == 0 && u.uid == 1000U);
        int bob_ok = (acct_find("bob", &u) == 0 && u.uid == 1001U);

        kprintf("acct: accounts root=0 alice=1000 bob=1001 %s\n",
                (root_ok && alice_ok && bob_ok) ? "ok" : "FAIL");
        ok = ok && root_ok && alice_ok && bob_ok;
    }

    /* 3) 组：wheel=10 且 root 在内（sudo 的依据） */
    {
        uint32_t wheel = 0;
        int wheel_ok = (acct_group_gid("wheel", &wheel) == 0 && wheel == ACCT_GID_WHEEL);
        int member_ok = acct_in_group("root", ACCT_GID_WHEEL);
        int alice_deny = !acct_in_group("alice", ACCT_GID_WHEEL);

        kprintf("acct: group wheel gid=10 root-member=%d alice-member=%d %s\n",
                member_ok ? 1 : 0, alice_deny ? 0 : 1,
                (wheel_ok && member_ok && alice_deny) ? "ok" : "FAIL");
        ok = ok && wheel_ok && member_ok && alice_deny;
    }

    /* 4) shadow 保护：uid 1000 读 -> E_ACCES；root 读 -> E_OK */
    {
        int denied = (probe_access(ACCT_SHADOW_PATH, FS_ACCESS_READ, 1000U, 1000U) == E_ACCES);
        int allowed = (probe_access(ACCT_SHADOW_PATH, FS_ACCESS_READ, 0U, 0U) == E_OK);

        kprintf("acct: shadow denied for uid 1000 %s\n", denied ? "ok" : "FAIL");
        kprintf("acct: shadow readable by root %s\n", allowed ? "ok" : "FAIL");
        ok = ok && denied && allowed;
    }

    /* 5) 家目录 0700 边界：alice 能进自己的，bob 进不去 */
    {
        int own = (probe_access("system:/home/alice", FS_ACCESS_READ | FS_ACCESS_EXEC,
                                1000U, 1000U) == E_OK);
        int other = (probe_access("system:/home/alice", FS_ACCESS_READ | FS_ACCESS_EXEC,
                                  1001U, 1001U) == E_ACCES);

        kprintf("acct: home 0700 boundary %s\n", (own && other) ? "ok" : "FAIL");
        ok = ok && own && other;
    }

    /* 6) 组权限：附加组进入 group 位。
     *    造一个 root:users 0640 的文件，uid 1001 带上附加组 100（users）就能读。 */
    {
        int created = 0;

        (void)fs_unlink("system:/grouptest");
        {
            uint32_t h = fs_open("system:/grouptest", FS_OPEN_WRITE | FS_OPEN_CREAT);

            if (h != 0U) {
                (void)fs_write(h, "g", 1);
                fs_close(h);
                created = 1;
            }
        }
        (void)fs_chown("system:/grouptest", 0U, 100U);      /* root:users */
        (void)fs_chmod("system:/grouptest", 0640U, 0U);     /* rw-r----- */

        {
            int by_group = (probe_access_groups("system:/grouptest", FS_ACCESS_READ,
                                                1001U, 1001U, 100U) == E_OK);
            int not_member = (probe_access("system:/grouptest", FS_ACCESS_READ,
                                           1001U, 1001U) == E_ACCES);

            kprintf("acct: supplementary group grants group bits %s\n",
                    (created && by_group && not_member) ? "ok" : "FAIL");
            ok = ok && created && by_group && not_member;
        }
        (void)fs_unlink("system:/grouptest");
    }

    /* 7) 口令：正确口令通过、错口令拒绝，且两条路径的迭代次数相同
     *    （新旧格式都固定比较 64 个十六进制字符 -> ct-steps=64） */
    {
        char stored[ACCT_HASH_MAX];
        char buf[ACCT_SHADOW_MAX];
        char *p = buf;
        char *line;
        bool found = false;

        stored[0] = '\0';
        if (read_file(ACCT_SHADOW_PATH, buf, sizeof(buf)) >= 0) {
            while ((line = line_take(&p)) != 0) {
                char *f[8];
                if (split_fields(line, f, 8) < 2) continue;
                if (strcmp(f[0], "root") != 0) continue;
                strncpy(stored, f[1], ACCT_HASH_MAX - 1U);
                stored[ACCT_HASH_MAX - 1U] = '\0';
                found = true;
                break;
            }
        }

        compare_steps = 0;
        {
            bool good = acct_verify_hash(ACCT_ROOT_PW, stored);
            steps_ok = compare_steps;
            compare_steps = 0;
            {
                bool bad = acct_verify_hash("wrong-password", stored);
                steps_bad = compare_steps;
                kprintf("acct: password verify ok ct-steps=%u\n", (unsigned int)steps_ok);
                kprintf("acct: wrong password rejected ct-steps=%u\n", (unsigned int)steps_bad);
                ok = ok && found && good && !bad;
            }
        }
        kprintf("acct: constant-time compare equal iterations %s\n",
                (steps_ok == steps_bad && steps_ok == ACCT_HASH_HEX) ? "ok" : "FAIL");
        ok = ok && (steps_ok == steps_bad) && (steps_ok == ACCT_HASH_HEX);
    }

    /* 8) 会话表 / 审计：who 与 last 都能产出行 */
    {
        acct_audit("SELFTEST", "accounts online");
        {
            char wbuf[256];
            char abuf[512];
            int nw = acct_who(wbuf, sizeof(wbuf));
            int na = acct_last(abuf, sizeof(abuf));

            kprintf("acct: sessions=%d audit-lines=%d %s\n",
                    nw, na, (na > 0) ? "ok" : "FAIL");
            ok = ok && (na > 0);
        }
    }

    /* 9) hash 一致性：同一个口令两次得到同样的十六进制串（长度 64） */
    {
        acct_hash_hex(ACCT_ROOT_PW, h1);
        acct_hash_hex(ACCT_ROOT_PW, h2);
        {
            int same = (strcmp(h1, h2) == 0 && strlen(h1) == ACCT_HASH_HEX);
            kprintf("acct: sha256 hex length=%u stable=%d %s\n",
                    (unsigned int)strlen(h1), same ? 1 : 0, same ? "ok" : "FAIL");
            ok = ok && same;
        }
    }

    /* P2：多用户文件矩阵（0600/sticky/目录 x/setuid/nosuid/umask） */
    file_matrix_selftest();

    /* P3：失败锁定 + sudo 策略 + useradd/passwd/usermod/userdel + 审计事件 */
    admin_selftest();

    /* P0：PBKDF2-SHA512 已知向量/格式/随机盐 + 旧 sha256 登录后自动迁移 */
    ok = pbkdf2_selftest() && ok;
    ok = shadow_migrate_selftest() && ok;

    /* P0：审计环 128 行（测完恢复原环）与追加式刷盘（保留历史、文件增长） */
    ok = audit_ring_selftest() && ok;
    ok = audit_flush_selftest() && ok;

    /* P0：getty 重启的会话槽复用（连续 3 次 exit/login 只留当前会话） */
    ok = session_slot_selftest() && ok;

    kprintf("acct: selftest %s\n", ok ? "PASS" : "FAIL");
}
