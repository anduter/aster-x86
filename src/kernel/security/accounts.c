/* =============================================================================
 * security/accounts.c - 账号数据库 / 口令校验 / 会话 / 审计（ABI v1.3）
 *
 * 设计要点：
 *   1. 文件是事实来源：/etc/passwd、/etc/shadow、/etc/group 由本模块在启动时
 *      幂等播种（已存在则保留），之后 passwd/useradd 直接改文件；
 *   2. 口令只在核内比较：sha256(口令) 与 shadow 里的 64 位十六进制串做
 *      恒定时间比较（固定 64 次迭代，不因前缀匹配提前返回）；
 *   3. 凭据落地只有一处：acct_apply_cred() 把 uid/gid/附加组写进 proc_t，
 *      之后所有权限判定都用 euid（见 fs/ramfs.c 的 may_access）；
 *   4. 审计不碰文件系统：只写环形缓冲 + 串口行（"audit: ..."），刷盘由
 *      acct_audit_flush() 在 syscall 上下文显式调用，避免 fs 锁重入。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"
#include "include/fs.h"
#include "include/proc.h"
#include "accounts.h"
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

/* ==================== 口令：sha256 + 恒定时间比较 ==================== */

static uint32_t compare_steps;

void acct_hash_hex(const char *pw, char out[ACCT_HASH_HEX + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    uint32_t n = acct_strlen(pw);

    sha256_bytes(pw, n, digest);
    for (uint32_t i = 0; i < 32U; i++) {
        out[i * 2U] = hex[digest[i] >> 4];
        out[i * 2U + 1U] = hex[digest[i] & 0x0FU];
    }
    out[ACCT_HASH_HEX] = '\0';
}

uint32_t acct_compare_steps(void)
{
    return compare_steps;
}

/* 固定比较 ACCT_HASH_HEX 个字符：不因前缀不同提前返回，也不因长度不同
 * 提前返回（不足的部分按 0 参与异或）。 */
bool acct_verify_hash(const char *pw, const char *stored_hex)
{
    char calc[ACCT_HASH_HEX + 1];
    uint32_t diff = 0;

    if (stored_hex == 0) return false;

    acct_hash_hex(pw, calc);
    for (uint32_t i = 0; i < ACCT_HASH_HEX; i++) {
        uint8_t a = (uint8_t)calc[i];
        uint8_t b = (uint8_t)stored_hex[i];      /* 提前 NUL 时按 0 比较 */

        diff |= (uint32_t)(a ^ b);
        compare_steps++;
    }
    return diff == 0U;
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
static uint32_t audit_total;

void acct_audit(const char *event, const char *detail)
{
    char line[ACCT_AUDIT_LEN];
    uint32_t n = 0;
    uint32_t fl;
    const char *ev = (event != 0) ? event : "?";
    const char *de = (detail != 0) ? detail : "-";

    /* 串口 + 屏幕：build.ps1 的检查项断言这些行 */
    kprintf("audit: %s %s\n", ev, de);

    fl = spin_lock_irqsave(&acct_lock);
    {
        while (*ev != '\0' && n + 1U < ACCT_AUDIT_LEN) line[n++] = *ev++;
        if (n + 1U < ACCT_AUDIT_LEN) line[n++] = ' ';
        while (*de != '\0' && n + 1U < ACCT_AUDIT_LEN) line[n++] = *de++;
        line[n] = '\0';
    }
    memcpy(audit_ring[audit_next], line, ACCT_AUDIT_LEN);
    audit_next = (audit_next + 1U) % ACCT_AUDIT_LINES;
    if (audit_total < ACCT_AUDIT_LINES) audit_total++;
    spin_unlock_irqrestore(&acct_lock, fl);
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

int acct_audit_flush(void)
{
    static char buf[ACCT_AUDIT_LINES * ACCT_AUDIT_LEN + 8];
    uint32_t n = audit_text(buf, sizeof(buf));

    if (n == 0U) return 0;
    return (write_file(ACCT_AUDIT_PATH, buf, n) == 0) ? (int)n : -1;
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

static int session_open_impl(const acct_user_t *u)
{
    uint32_t pid = (proc_current() != 0) ? proc_current()->pid : 0U;
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

void acct_logout(void)
{
    uint32_t pid = (proc_current() != 0) ? proc_current()->pid : 0U;
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

/* 取 shadow 里某账户的摘要；返回 0 找到，-1 没找到 */
static int shadow_lookup(const char *user, char out[ACCT_HASH_HEX + 1])
{
    char buf[1024];
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

        strncpy(out, f[1], ACCT_HASH_HEX);
        out[ACCT_HASH_HEX] = '\0';
        return 0;
    }
    return -1;
}

/* 系统账户（uid 1..99）不可登录；返回 false 表示禁止 */
static bool account_loginable(const acct_user_t *u)
{
    if (u->uid >= ACCT_UID_SYS_MIN && u->uid <= ACCT_UID_SYS_MAX) return false;
    return true;
}

/* 口令校验 + 失败计数。成功返回 ACCT_OK。 */
static int password_check(const char *user, const char *pass)
{
    char stored[ACCT_HASH_HEX + 1];
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
    char stored[ACCT_HASH_HEX + 1];

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
    struct { char data[1024]; } in;
    struct { char data[1024]; } out;
    char hash[ACCT_HASH_HEX + 1];
    char *p;
    char *line;
    uint32_t n = 0;
    bool found = false;
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
        char stored[ACCT_HASH_HEX + 1];

        if (shadow_lookup(user, stored) != 0 ||
            !acct_verify_hash((oldpw != 0) ? oldpw : "", stored)) {
            acct_audit("PASSWD_FAIL", user);
            return ACCT_EBAD;
        }
    }

    if (read_file(ACCT_SHADOW_PATH, in.data, sizeof(in.data)) < 0) return ACCT_ENOENT;

    acct_hash_hex(newpw, hash);
    p = in.data;
    out.data[0] = '\0';
    while ((line = line_take(&p)) != 0) {
        char *f[8];
        uint32_t nf = split_fields(line, f, 8);
        char rebuilt[ACCT_NAME_MAX + ACCT_HASH_HEX + 8];
        uint32_t m = 0;

        if (nf < 2) continue;
        if (strcmp(f[0], user) == 0) {
            found = true;
            for (const char *s = f[0]; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
            rebuilt[m++] = ':';
            for (const char *s = hash; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
            rebuilt[m++] = ':';
            rebuilt[m++] = '0';
            rebuilt[m++] = '\0';
        } else {
            for (const char *s = f[0]; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
            rebuilt[m++] = ':';
            for (const char *s = f[1]; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
            rebuilt[m++] = '\0';
        }

        if (n + (uint32_t)strlen(rebuilt) + 2U >= sizeof(out.data)) break;
        memcpy(out.data + n, rebuilt, strlen(rebuilt));
        n += (uint32_t)strlen(rebuilt);
        out.data[n++] = '\n';
        out.data[n] = '\0';
    }

    if (!found) {
        /* shadow 里没有这个账户（比如 useradd 刚建）：追加一行 */
        char rebuilt[ACCT_NAME_MAX + ACCT_HASH_HEX + 8];
        uint32_t m = 0;

        for (const char *s = user; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
        rebuilt[m++] = ':';
        for (const char *s = hash; *s != '\0' && m + 3U < sizeof(rebuilt); s++) rebuilt[m++] = *s;
        rebuilt[m++] = '\n';
        if (n + m + 2U < sizeof(out.data)) {
            memcpy(out.data + n, rebuilt, m);
            n += m;
            out.data[n] = '\0';
        }
    }

    if (out.data[0] == '\0' || write_file(ACCT_SHADOW_PATH, out.data, (uint32_t)strlen(out.data)) != 0) {
        acct_audit("PASSWD_FAIL", user);
        return ACCT_EBAD;
    }

    /* shadow 必须是 0600 root（重写后重新落权限，防止 umask/属主漂移） */
    (void)fs_chmod(ACCT_SHADOW_PATH, 0600U, 0U);
    (void)fs_chown(ACCT_SHADOW_PATH, 0U, 0U);

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
    char shadow[512];
    char hash_root[ACCT_HASH_HEX + 1];
    char hash_alice[ACCT_HASH_HEX + 1];
    char hash_bob[ACCT_HASH_HEX + 1];
    uint32_t n = 0;

    ensure_dir("system:/etc", 0U, 0U, 0755U);
    ensure_dir("system:/home", 0U, 0U, 0755U);
    ensure_dir("system:/root", 0U, 0U, 0700U);
    ensure_dir("system:/var", 0U, 0U, 0755U);
    ensure_dir("system:/var/log", 0U, 0U, 0755U);
    ensure_dir("system:/tmp", 0U, 0U, 01777U);      /* 01777 = sticky + rwxrwxrwx */

    /* 家目录：0700，属主是该用户 */
    ensure_dir("system:/home/alice", 1000U, 1000U, 0700U);
    ensure_dir("system:/home/bob", 1001U, 1001U, 0700U);

    acct_hash_hex(ACCT_ROOT_PW, hash_root);
    acct_hash_hex(ACCT_ALICE_PW, hash_alice);
    acct_hash_hex(ACCT_BOB_PW, hash_bob);

    /* shadow 内容按 "name:hash:0:0:99999:7:::" 生成（口令在核内算） */
    {
        static const char *names[3] = { "root", "alice", "bob" };
        const char *hashes[3];
        hashes[0] = hash_root;
        hashes[1] = hash_alice;
        hashes[2] = hash_bob;

        shadow[0] = '\0';
        for (uint32_t i = 0; i < 3U; i++) {
            const char *s;

            for (s = names[i]; *s != '\0' && n + 2U < sizeof(shadow); s++) shadow[n++] = *s;
            if (n + 2U < sizeof(shadow)) shadow[n++] = ':';
            for (s = hashes[i]; *s != '\0' && n + 2U < sizeof(shadow); s++) shadow[n++] = *s;
            for (s = ":0:0:99999:7:::\n"; *s != '\0' && n + 2U < sizeof(shadow); s++) {
                shadow[n++] = *s;
            }
        }
        shadow[n] = '\0';
    }

    ensure_file(ACCT_PASSWD_PATH, seed_passwd, 0644U);
    ensure_file(ACCT_GROUP_PATH, seed_group, 0644U);
    ensure_file(ACCT_SHADOW_PATH, shadow, 0600U);

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

    /* 7) 口令：正确口令通过、错口令拒绝，且两条路径的迭代次数相同 */
    {
        char stored[ACCT_HASH_HEX + 1];
        char buf[1024];
        char *p = buf;
        char *line;
        bool found = false;

        stored[0] = '\0';
        if (read_file(ACCT_SHADOW_PATH, buf, sizeof(buf)) >= 0) {
            while ((line = line_take(&p)) != 0) {
                char *f[8];
                if (split_fields(line, f, 8) < 2) continue;
                if (strcmp(f[0], "root") != 0) continue;
                strncpy(stored, f[1], ACCT_HASH_HEX);
                stored[ACCT_HASH_HEX] = '\0';
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

    kprintf("acct: selftest %s\n", ok ? "PASS" : "FAIL");
}
