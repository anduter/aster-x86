/* =============================================================================
 * pkg/pkgd.c - _pkgd：组件安装服务（ring0 内建，接口与 ring3 服务一致）
 *
 * 职责：
 *   install  验签 -> 查 requires -> 写 staging -> 逐文件校验 -> 原子 rename
 *            -> 追加组件 DB -> 通知 supervisor 启动（service）
 *   verify   已安装组件的完整性（DB 行 + 目录 + 每个文件哈希）
 *   list     列出组件 DB
 *   remove   停掉运行中的 service -> 删组件目录 -> 删 DB 行
 *
 * 目录约定：
 *   system:/System/Packages/<name>/<version>/<file>   系统组件
 *   <home>/Apps/<name>/<version>/<file>               用户应用
 *   system:/System/.staging/<name>/                   staging（失败回滚时删掉）
 *   system:/System/packages.db, <home>/Apps/apps.db   组件 DB（文本）
 *
 * 权限：只有 root 能装/删 service；用户只能装 type=app 到自己的 ~/Apps，
 * 且 manifest 的 account= 必须就是自己。组件 DB 与目录布局是文本+普通文件，
 * 方便 fsck/审计，也让"安装了什么"一眼可见。
 * ============================================================================= */
#include "include/pkg.h"
#include "include/kernel.h"
#include "include/fs.h"
#include "include/security.h"
#include "include/svc.h"
#include "include/proc.h"
#include "include/mm.h"
#include "include/userland.h"

#define PKG_SYS_DIR     "system:/System"
#define PKG_SYS_PKGS    "system:/System/Packages"
#define PKG_SYS_STAGE   "system:/System/.staging"
#define PKG_SYS_DB      "system:/System/packages.db"
#define PKG_DB_MAX      2048U
#define PKG_SUP_NAME    "supervisor"
#define PKG_RUNTIME_ELF "system:/bin/shell"

static int pkgd_port = -1;

/* ---- P0：_pkgd 启动的组件进程（service ELF）----
 * pid 用来在卸载时等它退出/兜底 SIGKILL；port 由组件自己通过
 * SYS_PKG(PKG_OP_COMPONENT_PORT) 报告，注销时不用猜。 */
typedef struct {
    bool     used;
    char     name[PKG_NAME_MAX];
    int      pid;
    uint32_t port;
} pkg_run_t;

static pkg_run_t pkg_runs[PKG_COMPONENT_MAX];

/* -------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
static int suffix_join(char *out, uint32_t max, const char *dir, const char *leaf)
{
    int n = ksnprintf(out, max, "%s/%s", dir, leaf);

    return (n > 0 && (uint32_t)n < max) ? 0 : -1;
}

/* 读出整文件；返回字节数，文件不存在返回 0，出错返回 -1。 */
static int read_all(const char *path, uint8_t *buf, uint32_t max)
{
    fs_stat_t st;
    uint32_t h;
    int n;

    if (fs_stat(path, &st) != E_OK) return 0;
    if (st.type != FS_TYPE_FILE) return -1;
    if (st.size > max) return -1;

    h = fs_open(path, FS_OPEN_READ);
    if (h == 0) return -1;
    n = fs_read(h, buf, st.size);
    fs_close(h);
    return (n < 0) ? -1 : n;
}

/* 覆盖写（先删再建，避免旧文件尾部残留）。 */
static int write_replace(const char *path, const void *buf, uint32_t len)
{
    uint32_t h;
    int n;

    (void)fs_unlink(path);
    h = fs_open(path, FS_OPEN_WRITE | FS_OPEN_CREAT);
    if (h == 0) return -1;
    n = fs_write(h, buf, len);
    fs_close(h);
    return (n == (int)len) ? 0 : -1;
}

/* mkdir -p：逐级创建，已存在不算错。 */
static void ensure_dir(const char *path)
{
    char buf[FS_PATH_MAX];
    fs_stat_t st;
    uint32_t len = (uint32_t)strlen(path);

    if (len == 0U || len >= sizeof(buf)) return;
    memcpy(buf, path, len + 1U);

    for (uint32_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (strcmp(buf, "system:") != 0 && buf[0] != '\0') {
            if (fs_stat(buf, &st) != E_OK) (void)fs_mkdir(buf);
        }
        buf[i] = '/';
    }
    if (fs_stat(path, &st) != E_OK) (void)fs_mkdir(path);
}

/* 递归删除（不存在视为成功）。目录必须为空才能 unlink，所以先删子项。 */static int remove_tree(const char *path)
{
    fs_stat_t st;
    char child[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (fs_stat(path, &st) != E_OK) return 0;

    if (st.type == FS_TYPE_DIR) {
        for (;;) {
            fs_stat_t cst;

            if (fs_listdir(path, 0U, name, sizeof(name), &cst) != E_OK) break;
            if (suffix_join(child, sizeof(child), path, name) != 0) break;
            (void)remove_tree(child);
        }
    }
    return (fs_unlink(path) == E_OK) ? 0 : -1;
}

/* 以指定属主建目录（用户应用要落在自己的 ~/Apps 下、归属自己） */
static void ensure_dir_owner(const char *path, uint32_t uid, uint32_t gid)
{
    char buf[FS_PATH_MAX];
    fs_stat_t st;
    uint32_t len = (uint32_t)strlen(path);

    if (len == 0U || len >= sizeof(buf)) return;
    memcpy(buf, path, len + 1U);

    for (uint32_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (strcmp(buf, "system:") != 0 && buf[0] != '\0') {
            if (fs_stat(buf, &st) != E_OK) (void)fs_mkdir_as(buf, uid, gid);
        }
        buf[i] = '/';
    }
    if (fs_stat(path, &st) != E_OK) (void)fs_mkdir_as(path, uid, gid);
}

/* 把整棵子树改成指定属主（root 才能 chown）；用户应用安装后调用 */
static void chown_tree(const char *path, uint32_t uid, uint32_t gid)
{
    fs_stat_t st;
    char child[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (fs_stat(path, &st) != E_OK) return;
    (void)fs_chown(path, uid, gid);
    if (st.type != FS_TYPE_DIR) return;

    for (uint32_t i = 0; ; i++) {
        fs_stat_t cst;

        if (fs_listdir(path, i, name, sizeof(name), &cst) != E_OK) break;
        if (suffix_join(child, sizeof(child), path, name) != 0) break;
        chown_tree(child, uid, gid);
    }
}

/* -------------------------------------------------------------------------
 * P0：组件 ELF 的落盘与完整性校验
 * ------------------------------------------------------------------------- */
static void hex_encode(const uint8_t *in, uint32_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (uint32_t i = 0; i < n; i++) {
        out[i * 2U] = digits[in[i] >> 4];
        out[i * 2U + 1U] = digits[in[i] & 0x0FU];
    }
    out[n * 2U] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 严格解析 64 个十六进制字符（SHA-256）。 */
static int hex_decode32(const char *in, uint8_t out[32])
{
    if (in == 0) return -1;
    for (uint32_t i = 0; i < 32U; i++) {
        int hi = hex_nibble(in[i * 2U]);
        int lo = hex_nibble(in[i * 2U + 1U]);

        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (in[64] == '\0') ? 0 : -1;
}

/* 读整个文件到新分配的缓冲；返回 0 成功，负错误码失败。 */
static int read_file_alloc(const char *path, uint8_t **out, uint32_t *out_len)
{
    fs_stat_t st;
    uint8_t *buf;
    int n;

    if (path == 0 || out == 0 || out_len == 0) return PKG_E_FORMAT;
    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_FILE) return PKG_E_NOENT;
    if (st.size == 0U || st.size > FS_FILE_MAX) return PKG_E_FORMAT;
    buf = (uint8_t *)malloc(st.size);
    if (buf == 0) return PKG_E_IO;
    n = read_all(path, buf, st.size);
    if (n != (int)st.size) { free(buf); return PKG_E_IO; }
    *out = buf;
    *out_len = st.size;
    return PKG_OK;
}

/* 在 staging 里找出可执行映像：优先用包内自带的 ELF（平铺文件名），
 * 没有就退回构建期签名过的 system:/bin/shell 运行时装（echosvc 的
 * 过渡机制；build.ps1 的包清单暂时只带 .conf）。两种来源都要是合法 ELF。
 * 找到后**复制**到 <stage>/bin/<name>，并算出 SHA-256。 */
static int prepare_component_elf(const pkg_manifest_t *m, const char *stage,
                                 char *elf_rel, uint32_t rel_max,
                                 uint8_t sha[32])
{
    uint8_t *buf = 0;
    uint32_t len = 0;
    char bin_dir[FS_PATH_MAX];
    char dst[FS_PATH_MAX];
    int got = 0;

    if (m == 0 || stage == 0 || elf_rel == 0) return PKG_E_FORMAT;
    if (ksnprintf(elf_rel, rel_max, "%s/%s", PKG_ELF_DIR, m->name) <= 0 ||
        (uint32_t)strlen(elf_rel) >= rel_max) {
        return PKG_E_FORMAT;
    }

    /* 1) 包内自带的 ELF：安装时已经通过 APKG 验签 + 逐文件哈希，这里只找出来。 */
    for (uint32_t i = 0; ; i++) {
        char name[FS_NAME_MAX];
        fs_stat_t st;
        char src[FS_PATH_MAX];

        if (fs_listdir(stage, i, name, sizeof(name), &st) != E_OK) break;
        if (st.type != FS_TYPE_FILE || st.size < 52U) continue;
        if (suffix_join(src, sizeof(src), stage, name) != 0) continue;
        if (read_file_alloc(src, &buf, &len) != PKG_OK) continue;
        if (len >= 52U && buf[0] == 0x7FU && buf[1] == 'E' &&
            buf[2] == 'L' && buf[3] == 'F') {
            got = 1;
            break;
        }
        free(buf);
        buf = 0;
    }

    /* 2) 包内没有 ELF：用签名 shell 运行时装（受信任的组件运行环境） */
    if (got == 0) {
        if (read_file_alloc(PKG_RUNTIME_ELF, &buf, &len) != PKG_OK) {
            return PKG_E_NOENT;
        }
        if (len < 52U || buf[0] != 0x7FU || buf[1] != 'E' ||
            buf[2] != 'L' || buf[3] != 'F' ||
            !trust_verify_shell(buf, len)) {
            free(buf);
            return PKG_E_SIGNATURE;
        }
        got = 1;
    }

    sha256_bytes(buf, len, sha);

    /* 复制到 staging 的 bin/（staging -> 正式目录的 rename 是原子的）。 */
    if (suffix_join(bin_dir, sizeof(bin_dir), stage, PKG_ELF_DIR) != 0 ||
        suffix_join(dst, sizeof(dst), bin_dir, m->name) != 0) {
        free(buf);
        return PKG_E_IO;
    }
    ensure_dir(stage);
    ensure_dir(bin_dir);
    if (write_replace(dst, buf, len) != 0) {
        free(buf);
        return PKG_E_IO;
    }
    free(buf);
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * 组件 DB（文本，一行一个组件）
 * ------------------------------------------------------------------------- */
static int db_path(uint32_t euid, char *out, uint32_t max)
{
    acct_user_t u;

    if (euid == 0U) {
        int n = ksnprintf(out, max, "%s", PKG_SYS_DB);
        return (n > 0 && (uint32_t)n < max) ? 0 : -1;
    }
    if (acct_find_uid(euid, &u) != 0) return -1;
    return ksnprintf(out, max, "system:%s/Apps/apps.db", u.home) > 0 ? 0 : -1;
}

/* 在 DB 里找 name=<name> 的行；找到返回 0 并复制整行。 */
static int db_find(const char *name, uint32_t euid, char *out, uint32_t max)
{
    char path[FS_PATH_MAX];
    uint8_t *text;
    int len;
    int found = -1;

    if (db_path(euid, path, sizeof(path)) != 0) return -1;
    text = (uint8_t *)malloc(PKG_DB_MAX);
    if (text == 0) return -1;
    len = read_all(path, text, PKG_DB_MAX - 1U);
    if (len > 0) {
        uint32_t i = 0;

        text[len] = '\0';
        while (i < (uint32_t)len) {
            uint32_t start = i;
            uint32_t end;

            while (i < (uint32_t)len && text[i] != '\n') i++;
            end = i;
            if (i < (uint32_t)len) i++;
            while (end > start && (text[end - 1U] == '\r')) end--;

            if (end - start > 5U && memcmp(text + start, "name=", 5) == 0 &&
                (uint32_t)(end - start) >= 5U + strlen(name) &&
                memcmp(text + start + 5, name, strlen(name)) == 0 &&
                (text[start + 5U + strlen(name)] == ' ' ||
                 (start + 5U + strlen(name)) == end)) {
                if (out != 0 && max > 0U) {
                    uint32_t n = end - start;
                    if (n >= max) n = max - 1U;
                    memcpy(out, text + start, n);
                    out[n] = '\0';
                }
                found = 0;
                break;
            }
        }
    }
    free(text);
    return found;
}

/* 追加一行；文件不存在时创建。 */
static int db_append(uint32_t euid, const char *line)
{
    char path[FS_PATH_MAX];
    uint8_t *text;
    int len;
    int rc;

    if (db_path(euid, path, sizeof(path)) != 0) return -1;
    text = (uint8_t *)malloc(PKG_DB_MAX);
    if (text == 0) return -1;

    len = read_all(path, text, PKG_DB_MAX - 1U);
    if (len < 0) { free(text); return -1; }
    if (len > 0 && text[len - 1] != '\n' && (uint32_t)len + 1U < PKG_DB_MAX) {
        text[len++] = '\n';
    }
    {
        uint32_t need = (uint32_t)len + (uint32_t)strlen(line) + 1U;

        if (need > PKG_DB_MAX) { free(text); return -1; }
        memcpy(text + len, line, strlen(line));
        len += (int)strlen(line);
        text[len++] = '\n';
    }
    ensure_dir(PKG_SYS_DIR);
    rc = write_replace(path, text, (uint32_t)len);
    free(text);
    return rc;
}

/* 删掉 name=<name> 的那一行（其余行原样保留）。 */
static int db_remove_line(const char *name, uint32_t euid)
{
    char path[FS_PATH_MAX];
    uint8_t *text;
    uint8_t *out;
    int len;
    int olen = 0;
    int rc = -1;

    if (db_path(euid, path, sizeof(path)) != 0) return -1;
    text = (uint8_t *)malloc(PKG_DB_MAX);
    out = (uint8_t *)malloc(PKG_DB_MAX);
    if (text == 0 || out == 0) { free(text); free(out); return -1; }

    len = read_all(path, text, PKG_DB_MAX - 1U);
    if (len < 0) { free(text); free(out); return -1; }

    for (uint32_t i = 0; i < (uint32_t)len; ) {
        uint32_t start = i;
        uint32_t end;

        while (i < (uint32_t)len && text[i] != '\n') i++;
        end = i;
        if (i < (uint32_t)len) i++;

        if (end - start > 5U && memcmp(text + start, "name=", 5) == 0 &&
            (uint32_t)(end - start) >= 5U + strlen(name) &&
            memcmp(text + start + 5, name, strlen(name)) == 0 &&
            (text[start + 5U + strlen(name)] == ' ' ||
             (start + 5U + strlen(name)) == end)) {
            continue;                       /* 跳过这一行 */
        }
        if (end > start) {
            uint32_t n = end - start;
            if ((uint32_t)olen + n + 1U >= PKG_DB_MAX) break;
            memcpy(out + olen, text + start, n);
            olen += (int)n;
            out[olen++] = '\n';
        }
    }
    if (olen == 0) {
        (void)fs_unlink(path);
        rc = 0;
    } else {
        rc = write_replace(path, out, (uint32_t)olen);
    }
    free(text);
    free(out);
    return rc;
}

/* 从 DB 行里取 key=value（值里没有空格；路径/版本/名字都符合）。 */
static int db_field(const char *line, const char *key, char *out, uint32_t max)
{
    uint32_t klen = (uint32_t)strlen(key);
    const char *p = line;

    while (p != 0 && *p != '\0') {
        while (*p == ' ') p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1U;
            uint32_t n = 0;

            while (v[n] != '\0' && v[n] != ' ') n++;
            if (n >= max) n = max - 1U;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        while (*p != '\0' && *p != ' ') p++;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * supervisor 通知（安装 service 后启动 / 卸载前停止）
 * ------------------------------------------------------------------------- */
static int sup_request(const char *cmd, char *reply, uint32_t max)
{
    uint32_t client = 0;
    int n;

    if (service_connect(PKG_SUP_NAME, &client) != 0) return -1;
    reply[0] = '\0';
    if (chan_send(client, cmd, (uint32_t)strlen(cmd)) <= 0) {
        chan_release(client);
        return -1;
    }
    n = chan_recv_block(client, reply, max);
    chan_release(client);
    if (n <= 0) return -1;
    reply[(uint32_t)n < max ? (uint32_t)n : (max - 1U)] = '\0';
    return n;
}

/* -------------------------------------------------------------------------
 * 组件路径
 * ------------------------------------------------------------------------- */
int pkg_component_root(const pkg_manifest_t *m, uint32_t euid,
                       char *out, uint32_t max)
{
    acct_user_t u;
    int n;

    if (m == 0 || out == 0 || max == 0U) return PKG_E_FORMAT;

    if (m->type == PKG_TYPE_SERVICE) {
        n = ksnprintf(out, max, "%s/%s/%s", PKG_SYS_PKGS, m->name, m->version);
        return (n > 0 && (uint32_t)n < max) ? PKG_OK : PKG_E_FORMAT;
    }
    if (acct_find_uid(euid, &u) != 0) return PKG_E_POLICY;
    n = ksnprintf(out, max, "system:%s/Apps/%s/%s", u.home, m->name, m->version);
    return (n > 0 && (uint32_t)n < max) ? PKG_OK : PKG_E_FORMAT;
}

/* 组件根目录的上一级（<...>/<name>） */
static int component_parent(const char *root, char *out, uint32_t max)
{
    uint32_t n = (uint32_t)strlen(root);

    while (n > 0U && root[n - 1U] != '/') n--;
    if (n == 0U || n > max) return -1;
    memcpy(out, root, n - 1U);
    out[n - 1U] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------
 * 安装
 * ------------------------------------------------------------------------- */
static void set_msg(char *msg, uint32_t max, const char *text)
{
    if (msg == 0 || max == 0U) return;
    strncpy(msg, text, max - 1U);
    msg[max - 1U] = '\0';
}

static int pkg_install_impl(const uint8_t *pkg, uint32_t len, uint32_t euid,
                     char *msg, uint32_t max)
{
    pkg_manifest_t m;
    pkg_header_t h;
    acct_user_t user;
    char root[FS_PATH_MAX];
    char stage[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    char elf_rel[PKG_ENTRY_MAX];
    char program[PKG_ENTRY_MAX];
    char detail[48];
    uint8_t elf_sha[32];
    int have_elf = 0;
    int r;

    elf_rel[0] = '\0';
    program[0] = '\0';
    detail[0] = '\0';

    r = pkg_verify(pkg, len, &m);
    if (r != PKG_OK) {
        set_msg(msg, max, pkg_status_text(r));
        return r;
    }
    r = pkg_read_header(pkg, len, &h);
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }

    /* ---- 策略：service 只有 root（= CAP_PKG_ADMIN）+ account=root；
     * app 只能进自己家目录 ---- */
    if (m.type == PKG_TYPE_SERVICE) {
        if (!pkg_euid_is_admin(euid)) {
            set_msg(msg, max, "service requires root/CAP_PKG_ADMIN");
            return PKG_E_POLICY;
        }
        if (strcmp(m.account, "root") != 0) {
            set_msg(msg, max, "service account must be root");
            return PKG_E_POLICY;
        }
        /* CAP_SYS_ADMIN / CAP_PKG_ADMIN 是"签名更新器"保留位（SIP-lite 的放行
         * 条件），普通组件包不能申请：否则装一个包就等于拿到改系统目录的权限。 */
        if ((m.caps & (PKG_CAP_SYS_ADMIN | PKG_CAP_PKG_ADMIN)) != 0U) {
            set_msg(msg, max, "reserved capability");
            return PKG_E_POLICY;
        }
    } else {
        if (acct_find_uid(euid, &user) != 0) {
            set_msg(msg, max, "unknown user");
            return PKG_E_POLICY;
        }
        if (strcmp(user.name, m.account) != 0) {
            set_msg(msg, max, "account mismatch");
            return PKG_E_POLICY;
        }
    }

    r = pkg_component_root(&m, euid, root, sizeof(root));
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }

    if (db_find(m.name, euid, 0, 0) == 0) {
        set_msg(msg, max, "already installed");
        return PKG_E_EXIST;
    }

    /* ---- staging ---- */
    ensure_dir(PKG_SYS_DIR);
    ensure_dir(PKG_SYS_PKGS);
    ensure_dir(PKG_SYS_STAGE);
    if (ksnprintf(stage, sizeof(stage), "%s/%s", PKG_SYS_STAGE, m.name) <= 0) {
        set_msg(msg, max, "path too long");
        return PKG_E_IO;
    }
    (void)remove_tree(stage);               /* 上次中断的残留 */
    ensure_dir(stage);

    /* ---- 逐文件落盘 + 哈希复验 ---- */
    for (uint32_t i = 0; i < h.file_count; i++) {
        pkg_file_ent_t e;
        uint8_t *data;
        uint8_t got[32];
        uint32_t hh;
        int n;

        pkg_file_ent(pkg, &h, i, &e);
        data = (uint8_t *)malloc(e.size + 1U);
        if (data == 0) { (void)remove_tree(stage); set_msg(msg, max, "no memory"); return PKG_E_IO; }
        memcpy(data, pkg + h.data_off + e.offset, e.size);

        sha256_bytes(data, e.size, got);
        if (memcmp(got, e.sha256, 32) != 0) {
            free(data);
            (void)remove_tree(stage);
            set_msg(msg, max, "hash mismatch");
            return PKG_E_HASH;
        }
        if (suffix_join(path, sizeof(path), stage, e.name) != 0) {
            free(data);
            (void)remove_tree(stage);
            set_msg(msg, max, "path too long");
            return PKG_E_IO;
        }
        hh = fs_open(path, FS_OPEN_WRITE | FS_OPEN_CREAT);
        if (hh == 0) {
            free(data);
            (void)remove_tree(stage);
            set_msg(msg, max, "create failed");
            return PKG_E_IO;
        }
        n = fs_write(hh, data, e.size);
        fs_close(hh);
        if (n != (int)e.size) {
            free(data);
            (void)remove_tree(stage);
            set_msg(msg, max, "write failed");
            return PKG_E_IO;
        }
        (void)fs_chmod(path, (e.mode != 0U) ? (e.mode & FS_MODE_ALL) : FS_MODE_FILE_DEFAULT, 0U);
        free(data);
    }

    /* ---- 组件 ELF entry（P0）：把 service 的可执行映像放进 staging 的 bin/ ----
     * 这一步在 rename 之前，失败只删 staging；rename 之后启动失败再回滚正式目录。
     * 过渡期约定：manifest.entry 如果是程序名（没有 '/'），它同时是留给
     * supervisor 的 legacy 程序名（echosvc 的内建 echo 回退）；真正的入口
     * 一律是组件目录里的 bin/<name>。 */
    if (m.type == PKG_TYPE_SERVICE) {
        if (strchr(m.entry, '/') == 0) {
            strncpy(program, m.entry, sizeof(program) - 1U);
            program[sizeof(program) - 1U] = '\0';
        }
        r = prepare_component_elf(&m, stage, elf_rel, sizeof(elf_rel), elf_sha);
        if (r != PKG_OK) {
            (void)remove_tree(stage);
            set_msg(msg, max, "no component elf");
            return r;
        }
        have_elf = 1;
    }

    /* ---- 原子 rename 到正式目录 ---- */
    if (component_parent(root, parent, sizeof(parent)) != 0) {
        (void)remove_tree(stage);
        set_msg(msg, max, "bad root");
        return PKG_E_IO;
    }
    if (m.type == PKG_TYPE_APP) ensure_dir_owner(parent, user.uid, user.gid);
    else ensure_dir(parent);
    r = fs_rename(stage, root);
    if (r != E_OK) {
        /* rename 失败时目标目录可能本来就存在（不是本次创建）：只删 staging，
         * 绝不能因为回滚把原本就在那里的内容删掉。 */
        (void)remove_tree(stage);
        set_msg(msg, max, "rename failed");
        return PKG_E_IO;
    }
    /* 用户应用：整棵子树改成用户自己（否则 root 装的 app 用户写不了） */
    if (m.type == PKG_TYPE_APP) chown_tree(root, user.uid, user.gid);

    /* ---- 组件 DB ---- */
    {
        char line[PKG_DB_LINE_MAX];
        char sha_hex[PKG_ELF_HASH_HEX + 1U];
        int n;

        if (have_elf != 0) {
            hex_encode(elf_sha, 32U, sha_hex);
            n = ksnprintf(line, sizeof(line),
                          "name=%s version=%s type=service entry=%s program=%s "
                          "euid=%u root=%s files=%u elf=%s elfsha=%s",
                          m.name, m.version, elf_rel, program,
                          (unsigned int)euid, root, (unsigned int)h.file_count,
                          elf_rel, sha_hex);
        } else {
            n = ksnprintf(line, sizeof(line),
                          "name=%s version=%s type=%s entry=%s euid=%u root=%s files=%u",
                          m.name, m.version,
                          (m.type == PKG_TYPE_SERVICE) ? "service" : "app",
                          m.entry, (unsigned int)euid, root,
                          (unsigned int)h.file_count);
        }
        if (n <= 0 || (uint32_t)n >= sizeof(line) || db_append(euid, line) != 0) {
            (void)remove_tree(root);
            set_msg(msg, max, "db write failed");
            return PKG_E_IO;
        }
    }

    /* ---- legacy：通知 supervisor 拉起内建程序（echo 回退） ----
     * P0 起组件真正的入口是上面的 ELF；program= 只是过渡期保底，等内建
     * echosvc 退役后删掉这一段即可。 */
    if (m.type == PKG_TYPE_SERVICE && program[0] != '\0') {
        char cmd[48];
        char reply[32];

        (void)ksnprintf(cmd, sizeof(cmd), "start:%s", program);
        if (sup_request(cmd, reply, sizeof(reply)) > 0) {
            kprintf("pkg: supervisor start %s %s\n", program, reply);
        } else {
            kprintf("pkg: supervisor start %s failed\n", program);
        }
    }

    /* ---- 启动组件 ELF：读 bin/<name>、校验哈希+签名、ring3 exec ----
     * 这一步失败要把刚装好的目录和 DB 行一起回滚，不能留下"装上了但跑不起来"的组件。 */
    if (have_elf != 0) {
        r = pkg_component_start(m.name, euid, detail, sizeof(detail));
        if (r != PKG_OK) {
            kprintf("pkg: elf entry FAILED (%s: %s)\n", m.name,
                    pkg_status_text(r));
            (void)pkg_component_stop(m.name);
            (void)remove_tree(root);
            (void)db_remove_line(m.name, euid);
            set_msg(msg, max, "component elf start failed");
            return r;
        }
        kprintf("pkg: elf entry ok (%s entry=%s)\n", m.name, elf_rel);
    }

    kprintf("pkg: installed %s-%s -> %s (%u file(s))\n",
            m.name, m.version, root, (unsigned int)h.file_count);
    set_msg(msg, max, "ok");
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * 卸载
 * ------------------------------------------------------------------------- */
static int pkg_remove_impl(const char *name, uint32_t euid, char *msg, uint32_t max)
{
    char line[PKG_DB_LINE_MAX];
    char type[16];
    char root[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char entry[PKG_ENTRY_MAX];
    char program[PKG_ENTRY_MAX];

    if (name == 0) {
        set_msg(msg, max, "bad name");
        return PKG_E_FORMAT;
    }
    /* 在调用者自己的 DB 里找：root 看系统组件，普通用户只看自己的应用。
     * 普通用户点名系统组件时给 POLICY（而不是含糊的 noent），提示需要
     * root/CAP_PKG_ADMIN。 */
    if (db_find(name, euid, line, sizeof(line)) != 0) {
        if (!pkg_euid_is_admin(euid) &&
            db_find(name, 0U, line, sizeof(line)) == 0) {
            set_msg(msg, max, "system component requires root/CAP_PKG_ADMIN");
            return PKG_E_POLICY;
        }
        set_msg(msg, max, "not installed");
        return PKG_E_NOENT;
    }
    if (db_field(line, "type", type, sizeof(type)) != 0 ||
        db_field(line, "root", root, sizeof(root)) != 0) {
        set_msg(msg, max, "db corrupt");
        return PKG_E_IO;
    }
    if (!pkg_euid_is_admin(euid) && strcmp(type, "app") != 0) {
        set_msg(msg, max, "remove requires root/CAP_PKG_ADMIN");
        return PKG_E_POLICY;
    }

    /* 先停组件自己的 ELF 进程（它可能占着同名服务/端口），再走 supervisor
     * 的 legacy stop。_pkgd 启动的组件按 pid 记录，stop 会等它真正退出。 */
    (void)pkg_component_stop(name);

    entry[0] = '\0';
    program[0] = '\0';
    if (strcmp(type, "service") == 0) {
        char cmd[48];
        char reply[32];
        const char *target = 0;

        if (db_field(line, "program", program, sizeof(program)) != 0) {
            program[0] = '\0';
        }
        if (db_field(line, "entry", entry, sizeof(entry)) != 0) {
            entry[0] = '\0';
        }
        /* P0 的 entry 是 bin/<name>（不是程序名）；legacy 停服务要用 program=。
         * 老 DB 行没有 program=，退回 entry。 */
        target = (program[0] != '\0') ? program : entry;
        if (target[0] != '\0' && strchr(target, '/') == 0) {
            (void)ksnprintf(cmd, sizeof(cmd), "stop:%s", target);
            if (sup_request(cmd, reply, sizeof(reply)) > 0) {
                kprintf("pkg: supervisor stop %s %s\n", target, reply);
            } else {
                kprintf("pkg: supervisor stop %s failed\n", target);
            }
        }
    }

    if (remove_tree(root) != 0) {
        set_msg(msg, max, "remove tree failed");
        return PKG_E_IO;
    }
    if (component_parent(root, parent, sizeof(parent)) == 0) {
        (void)fs_unlink(parent);     /* 非空/不存在都忽略 */
    }
    if (db_remove_line(name, euid) != 0) {
        set_msg(msg, max, "db update failed");
        return PKG_E_IO;
    }
    kprintf("pkg: removed %s\n", name);
    set_msg(msg, max, "ok");
    return PKG_OK;
}

/* ---- 公开入口：整个安装/卸载过程处于"签名更新器作用域" ----
 * SIP-lite 会拒绝写 system:/System|bin|drivers，只有 updater scope 内的线程被
 * 放行；scope 绑定进入时的 pid，所以不会顺带放开别的线程。 */
int pkg_install(const uint8_t *pkg, uint32_t len, uint32_t euid,
                char *msg, uint32_t max)
{
    int r;

    sip_updater_enter();
    r = pkg_install_impl(pkg, len, euid, msg, max);
    sip_updater_exit();
    return r;
}

int pkg_remove(const char *name, uint32_t euid, char *msg, uint32_t max)
{
    int r;

    sip_updater_enter();
    r = pkg_remove_impl(name, euid, msg, max);
    sip_updater_exit();
    return r;
}
/* -------------------------------------------------------------------------
 * 列表 / 已安装校验
 * ------------------------------------------------------------------------- */
uint32_t pkg_list(uint32_t euid, char *out, uint32_t max)
{
    char path[FS_PATH_MAX];
    uint8_t *text;
    int len;
    uint32_t count = 0;
    uint32_t used = 0;

    if (out == 0 || max == 0U) return 0;
    out[0] = '\0';
    if (db_path(euid, path, sizeof(path)) != 0) return 0;

    text = (uint8_t *)malloc(PKG_DB_MAX);
    if (text == 0) return 0;
    len = read_all(path, text, PKG_DB_MAX - 1U);
    if (len > 0) {
        uint32_t i = 0;

        text[len] = '\0';
        while (i < (uint32_t)len) {
            uint32_t start = i;
            uint32_t end;
            char name[PKG_NAME_MAX], version[PKG_VERSION_MAX], type[16];

            while (i < (uint32_t)len && text[i] != '\n') i++;
            end = i;
            if (i < (uint32_t)len) i++;
            if (end == start) continue;

            {
                char line[PKG_DB_LINE_MAX];
                uint32_t n = end - start;
                if (n >= sizeof(line)) n = sizeof(line) - 1U;
                memcpy(line, text + start, n);
                line[n] = '\0';
                if (db_field(line, "name", name, sizeof(name)) != 0) continue;
                if (db_field(line, "version", version, sizeof(version)) != 0) continue;
                if (db_field(line, "type", type, sizeof(type)) != 0) continue;

                if (used + 48U < max) {
                    int w = ksnprintf(out + used, max - used, "%s %s %s\n",
                                      name, version, type);
                    if (w > 0) used += (uint32_t)w;
                }
                count++;
            }
        }
    }
    free(text);
    return count;
}

int pkg_verify_installed(const char *name, uint32_t euid, char *msg, uint32_t max)
{
    char line[PKG_DB_LINE_MAX];
    char version[PKG_VERSION_MAX];
    char root[FS_PATH_MAX];
    char elf_rel[PKG_ENTRY_MAX];
    char elf_hex[PKG_ELF_HASH_HEX + 1U];
    char blobname[64];
    const pkg_blob_t *blob;
    pkg_manifest_t m;
    pkg_header_t h;
    int r;

    if (name == 0) { set_msg(msg, max, "bad name"); return PKG_E_FORMAT; }
    if (db_find(name, euid, line, sizeof(line)) != 0) {
        set_msg(msg, max, "not installed");
        return PKG_E_NOENT;
    }
    if (db_field(line, "version", version, sizeof(version)) != 0 ||
        db_field(line, "root", root, sizeof(root)) != 0) {
        set_msg(msg, max, "db corrupt");
        return PKG_E_IO;
    }
    (void)ksnprintf(blobname, sizeof(blobname), "%s-%s.pkg", name, version);
    blob = pkg_catalog_find(blobname);
    if (blob == 0) { set_msg(msg, max, "no reference pkg"); return PKG_E_NOENT; }

    r = pkg_verify(blob->data, blob->len, &m);
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }
    r = pkg_read_header(blob->data, blob->len, &h);
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }

    for (uint32_t i = 0; i < h.file_count; i++) {
        pkg_file_ent_t e;
        char path[FS_PATH_MAX];
        uint8_t *data;
        uint8_t got[32];
        int n;

        pkg_file_ent(blob->data, &h, i, &e);
        if (suffix_join(path, sizeof(path), root, e.name) != 0) {
            set_msg(msg, max, "path too long");
            return PKG_E_IO;
        }
        data = (uint8_t *)malloc(e.size + 1U);
        if (data == 0) { set_msg(msg, max, "no memory"); return PKG_E_IO; }
        n = read_all(path, data, e.size);
        if (n != (int)e.size) {
            free(data);
            set_msg(msg, max, "file missing");
            return PKG_E_IO;
        }
        sha256_bytes(data, e.size, got);
        free(data);
        if (memcmp(got, e.sha256, 32) != 0) {
            set_msg(msg, max, "file hash mismatch");
            return PKG_E_HASH;
        }
    }

    /* P0：组件 ELF 也要对得上 DB 里记录的 SHA-256（bin/<name> 不是包文件，
     * 不在这轮 package file 表里）。 */
    if (db_field(line, "elf", elf_rel, sizeof(elf_rel)) == 0 &&
        db_field(line, "elfsha", elf_hex, sizeof(elf_hex)) == 0) {
        char path[FS_PATH_MAX];
        uint8_t want[32];
        uint8_t *data = 0;
        uint32_t len = 0;
        uint8_t got[32];

        if (hex_decode32(elf_hex, want) != 0 ||
            suffix_join(path, sizeof(path), root, elf_rel) != 0) {
            set_msg(msg, max, "db corrupt");
            return PKG_E_IO;
        }
        r = read_file_alloc(path, &data, &len);
        if (r != PKG_OK) { set_msg(msg, max, "elf missing"); return r; }
        sha256_bytes(data, len, got);
        free(data);
        if (memcmp(got, want, 32) != 0) {
            set_msg(msg, max, "elf hash mismatch");
            return PKG_E_HASH;
        }
    }

    set_msg(msg, max, "ok");
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * P0：组件 ELF entry 的启动 / 校验 / 停止
 * ------------------------------------------------------------------------- */
static pkg_run_t *run_find(const char *name)
{
    if (name == 0) return 0;
    for (uint32_t i = 0; i < PKG_COMPONENT_MAX; i++) {
        if (pkg_runs[i].used && strcmp(pkg_runs[i].name, name) == 0) {
            return &pkg_runs[i];
        }
    }
    return 0;
}

static pkg_run_t *run_alloc(const char *name, int pid)
{
    pkg_run_t *r = run_find(name);

    if (r == 0) {
        for (uint32_t i = 0; i < PKG_COMPONENT_MAX; i++) {
            if (!pkg_runs[i].used) { r = &pkg_runs[i]; break; }
        }
    }
    if (r == 0) return 0;
    memset(r, 0, sizeof(*r));
    r->used = true;
    strncpy(r->name, name, sizeof(r->name) - 1U);
    r->name[sizeof(r->name) - 1U] = '\0';
    r->pid = pid;
    return r;
}

/* 校验 <root>/bin/<name>：DB 的 elfsha + 签名 + 同名服务在线。
 * 成功时把组件根目录和 ELF 相对路径返回给调用方（启动要用）。 */
static int component_elf_check(const char *name, uint32_t euid,
                               char *root_out, uint32_t root_max,
                               char *elf_out, uint32_t elf_max)
{
    char line[PKG_DB_LINE_MAX];
    char type[16];
    char version[PKG_VERSION_MAX];
    char root[FS_PATH_MAX];
    char elf_rel[PKG_ENTRY_MAX];
    char elf_hex[PKG_ELF_HASH_HEX + 1U];
    char path[FS_PATH_MAX];
    char blobname[64];
    const pkg_blob_t *blob;
    uint8_t want[32];
    uint8_t got[32];
    uint8_t *data = 0;
    uint32_t len = 0;
    int r;

    if (name == 0) return PKG_E_FORMAT;
    if (db_find(name, euid, line, sizeof(line)) != 0) return PKG_E_NOENT;
    if (db_field(line, "type", type, sizeof(type)) != 0 ||
        strcmp(type, "service") != 0) {
        return PKG_E_FORMAT;
    }
    if (db_field(line, "version", version, sizeof(version)) != 0 ||
        db_field(line, "root", root, sizeof(root)) != 0 ||
        db_field(line, "elf", elf_rel, sizeof(elf_rel)) != 0 ||
        db_field(line, "elfsha", elf_hex, sizeof(elf_hex)) != 0) {
        return PKG_E_FORMAT;               /* 没有 ELF entry 的旧组件 */
    }
    if (hex_decode32(elf_hex, want) != 0 ||
        suffix_join(path, sizeof(path), root, elf_rel) != 0) {
        return PKG_E_FORMAT;
    }

    /* 1) 文件哈希：DB 记录的是安装时算出的 SHA-256。 */
    r = read_file_alloc(path, &data, &len);
    if (r != PKG_OK) return r;
    sha256_bytes(data, len, got);
    if (memcmp(got, want, 32) != 0) {
        free(data);
        return PKG_E_HASH;
    }

    /* 2) 签名：内建包 -> 重验 APKG（签名覆盖包内全部字节）；
     *    外部/过渡组件 -> 验 shell 运行时 RSA 签名（同一个信任根）。 */
    (void)ksnprintf(blobname, sizeof(blobname), "%s-%s.pkg", name, version);
    blob = pkg_catalog_find(blobname);
    if (blob != 0) {
        r = pkg_verify(blob->data, blob->len, 0);
        if (r != PKG_OK) { free(data); return r; }
    } else {
        int ok = trust_verify_shell(data, len);

        if (!ok) { free(data); return PKG_E_SIGNATURE; }
    }
    free(data);

    if (root_out != 0 && root_max > 0U) {
        strncpy(root_out, root, root_max - 1U);
        root_out[root_max - 1U] = '\0';
    }
    if (elf_out != 0 && elf_max > 0U) {
        strncpy(elf_out, elf_rel, elf_max - 1U);
        elf_out[elf_max - 1U] = '\0';
    }
    return PKG_OK;
}

int pkg_component_verify_elf(const char *name, uint32_t euid,
                             char *msg, uint32_t max)
{
    char root[FS_PATH_MAX];
    char elf_rel[PKG_ENTRY_MAX];
    int r = component_elf_check(name, euid, root, sizeof(root),
                                elf_rel, sizeof(elf_rel));

    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }
    if (!service_exists(name)) { set_msg(msg, max, "service offline"); return PKG_E_NOENT; }
    set_msg(msg, max, "ok");
    return PKG_OK;
}

int pkg_component_start(const char *name, uint32_t euid,
                        char *msg, uint32_t max)
{
    char root[FS_PATH_MAX];
    char elf_rel[PKG_ENTRY_MAX];
    char path[FS_PATH_MAX];
    char args[PKG_ENTRY_MAX + 8U];
    pkg_run_t *run;
    int pid;
    int r;

    r = component_elf_check(name, euid, root, sizeof(root),
                            elf_rel, sizeof(elf_rel));
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }
    if (suffix_join(path, sizeof(path), root, elf_rel) != 0) {
        set_msg(msg, max, "path too long");
        return PKG_E_IO;
    }

    run = run_find(name);
    if (run != 0) {
        /* 已在跑：确认进程还活着且服务在线，就不重复拉起。 */
        if (proc_find((uint32_t)run->pid) != 0 && service_exists(name)) {
            set_msg(msg, max, "ok");
            return PKG_OK;
        }
        (void)pkg_component_stop(name);
    }

    (void)ksnprintf(args, sizeof(args), "svc:%s", name);
    pid = user_component_exec(path, args);
    if (pid <= 0) { set_msg(msg, max, "ring3 exec failed"); return PKG_E_IO; }

    /* 先把 pid 记进运行表，组件启动后第一次 SYS_PKG(COMPONENT_PORT) 就能
     * 找到自己；服务起不来的话下面统一回滚。 */
    run = run_alloc(name, pid);
    if (run == 0) {
        proc_t *p = proc_find((uint32_t)pid);

        if (p != 0) proc_send_signal(p, SIGKILL);
        set_msg(msg, max, "component table full");
        return PKG_E_IO;
    }

    /* 等组件注册服务（服务名 = 组件名）。最多 3 秒。 */
    for (uint32_t i = 0; i < 300U; i++) {
        if (service_exists(name)) break;
        if (proc_find((uint32_t)pid) == 0) break;
        proc_sleep_ticks(1U);
    }
    if (!service_exists(name)) {
        proc_t *p = proc_find((uint32_t)pid);

        if (p != 0) proc_send_signal(p, SIGKILL);
        memset(run, 0, sizeof(*run));
        set_msg(msg, max, "component service offline");
        return PKG_E_IO;
    }

    kprintf("pkg: component %s running pid=%d service=%s\n",
            name, pid, name);
    set_msg(msg, max, "ok");
    return PKG_OK;
}

int pkg_component_stop(const char *name)
{
    pkg_run_t *run = run_find(name);
    uint32_t client = 0;
    int exited = 0;

    if (run == 0) return PKG_OK;

    /* 1) 优雅停止：让服务自己注销 port 并退出。 */
    if (service_connect(name, &client) == 0) {
        (void)chan_send(client, "stop", 4U);
        chan_release(client);
    }

    for (uint32_t i = 0; i < 200U; i++) {
        proc_t *p = proc_find((uint32_t)run->pid);

        if (p == 0) { exited = 1; break; }
        if (p->state == PROC_ZOMBIE) {
            (void)proc_wait_options(run->pid, 0, PROC_WAIT_WNOHANG);
            exited = 1;
            break;
        }
        proc_sleep_ticks(1U);
    }

    /* 2) 超时兜底：SIGKILL（服务不再响应时不能让卸载卡死）。 */
    if (!exited) {
        proc_t *p = proc_find((uint32_t)run->pid);

        if (p != 0) proc_send_signal(p, SIGKILL);
        for (uint32_t i = 0; i < 100U; i++) {
            p = proc_find((uint32_t)run->pid);
            if (p == 0 || p->state == PROC_ZOMBIE) {
                (void)proc_wait_options(run->pid, 0, PROC_WAIT_WNOHANG);
                exited = 1;
                break;
            }
            proc_sleep_ticks(1U);
        }
    }

    /* 3) 兜底注销：正常路径组件已经自己注销；异常路径用记录的 port。 */
    if (run->port != 0U) {
        service_unregister(run->port);
    }
    kprintf("pkg: component %s stopped (pid=%d %s)\n", name, run->pid,
            exited ? "ok" : "killed");
    memset(run, 0, sizeof(*run));
    return PKG_OK;
}

/* SYS_PKG(PKG_OP_COMPONENT_PORT)：组件进程报告自己的 service port。
 * 只接受 _pkgd 启动的那个 pid，别的进程冒充不了。 */
int pkg_component_report_port(uint32_t port)
{
    proc_t *p = proc_current();

    if (p == 0 || port == 0U) return -1;
    for (uint32_t i = 0; i < PKG_COMPONENT_MAX; i++) {
        if (pkg_runs[i].used && pkg_runs[i].pid == (int)p->pid) {
            pkg_runs[i].port = port;
            return 0;
        }
    }
    return -1;
}

/* SYS_PKG(PKG_OP_COMPONENT_UNREG)：组件退出前注销服务，避免注册表里
 * 留下指向已释放 port 的僵尸条目。 */
int pkg_component_unregister_port(uint32_t port)
{
    proc_t *p = proc_current();

    if (p == 0) return -1;
    for (uint32_t i = 0; i < PKG_COMPONENT_MAX; i++) {
        if (pkg_runs[i].used && pkg_runs[i].pid == (int)p->pid) {
            service_unregister(port);
            pkg_runs[i].port = 0U;
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * 安装来源：内建包目录 / 文件系统里的 .pkg
 * ------------------------------------------------------------------------- */
int pkg_install_path(const char *path, uint32_t euid, char *msg, uint32_t max)
{
    const pkg_blob_t *blob;
    fs_stat_t st;
    uint8_t *buf;
    uint32_t h;
    int n;
    int r;

    if (path == 0 || path[0] == '\0') {
        set_msg(msg, max, "bad package");
        return PKG_E_FORMAT;
    }
    blob = pkg_catalog_find(path);
    if (blob != 0) return pkg_install(blob->data, blob->len, euid, msg, max);

    if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_FILE) {
        set_msg(msg, max, "no such package");
        return PKG_E_NOENT;
    }
    if (st.size < PKG_HEADER_SIZE + PKG_SIG_SIZE || st.size > FS_FILE_MAX) {
        set_msg(msg, max, "package too big/small");
        return PKG_E_FORMAT;
    }
    buf = (uint8_t *)malloc(st.size);
    if (buf == 0) { set_msg(msg, max, "no memory"); return PKG_E_IO; }
    h = fs_open(path, FS_OPEN_READ);
    if (h == 0) { free(buf); set_msg(msg, max, "open failed"); return PKG_E_IO; }
    n = fs_read(h, buf, st.size);
    fs_close(h);
    if (n != (int)st.size) {
        free(buf);
        set_msg(msg, max, "short read");
        return PKG_E_IO;
    }
    r = pkg_install(buf, st.size, euid, msg, max);
    free(buf);
    return r;
}

/* -------------------------------------------------------------------------
 * _pkgd 服务
 * ------------------------------------------------------------------------- */
void pkgd_init(void)
{
    pkgd_port = port_create();
    if (pkgd_port < 0) {
        kprintln("pkgd: no port");
        return;
    }
    if (service_register(PKGD_SERVICE_NAME, (uint32_t)pkgd_port) != E_OK) {
        kprintln("pkgd: register failed");
        port_release((uint32_t)pkgd_port);
        pkgd_port = -1;
        return;
    }
    kprintf("pkgd: service online port=%d\n", pkgd_port);
}

int pkgd_request(const char *cmd, char *reply, uint32_t max)
{
    uint32_t client = 0;
    int n;

    if (cmd == 0 || reply == 0 || max == 0U) return -1;
    if (service_connect(PKGD_SERVICE_NAME, &client) != 0) return -1;
    if (chan_send(client, cmd, (uint32_t)strlen(cmd)) <= 0) {
        chan_release(client);
        return -1;
    }
    reply[0] = '\0';
    n = chan_recv_block(client, reply, max);
    chan_release(client);
    if (n <= 0) return -1;
    reply[(uint32_t)n < max ? (uint32_t)n : (max - 1U)] = '\0';
    return n;
}

/* 回复统一截断到 CHAN_MSG_LEN 以内（channel 消息上限 64 字节） */
static void reply_fmt(char *reply, uint32_t max, int status, const char *detail)
{
    int n;

    if (status == PKG_OK) n = ksnprintf(reply, max, "ok %s", detail);
    else n = ksnprintf(reply, max, "err %d %s", status, detail);
    if (n < 0) reply[0] = '\0';
    reply[max - 1U] = '\0';
}

void pkgd_entry(void)
{
    if (pkgd_port < 0) return;

    for (;;) {
        uint32_t server = 0;
        char cmd[CHAN_MSG_LEN + 1];
        char reply[CHAN_MSG_LEN];
        uint32_t euid = (proc_current() != 0) ? proc_euid_of(proc_current()) : 0U;
        int n;

        if (port_accept_block((uint32_t)pkgd_port, &server) != 0) continue;
        n = chan_recv_block(server, cmd, CHAN_MSG_LEN);
        if (n <= 0) {
            chan_release(server);
            continue;
        }
        cmd[(uint32_t)n] = '\0';

        if (strncmp(cmd, "install ", 8) == 0) {
            char detail[40];
            int r = pkg_install_path(cmd + 8, euid, detail, sizeof(detail));

            reply_fmt(reply, sizeof(reply), r, detail);
        } else if (strncmp(cmd, "remove ", 7) == 0) {
            char detail[40];
            int r = pkg_remove(cmd + 7, euid, detail, sizeof(detail));
            reply_fmt(reply, sizeof(reply), r, detail);
        } else if (strcmp(cmd, "list") == 0) {
            char list[48];
            uint32_t c = pkg_list(euid, list, sizeof(list));
            reply_fmt(reply, sizeof(reply), PKG_OK,
                      (c == 0U) ? "empty" : list);
        } else if (strncmp(cmd, "verify ", 7) == 0) {
            char detail[40];
            int r = pkg_verify_installed(cmd + 7, euid, detail, sizeof(detail));
            reply_fmt(reply, sizeof(reply), r, detail);
        } else {
            reply_fmt(reply, sizeof(reply), PKG_E_FORMAT, "usage: install|remove|list|verify");
        }

        kprintf("pkgd: %s -> %s\n", cmd, reply);
        (void)chan_send(server, reply, (uint32_t)strlen(reply));
        chan_release(server);
    }
}
