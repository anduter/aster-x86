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

#define PKG_SYS_DIR     "system:/System"
#define PKG_SYS_PKGS    "system:/System/Packages"
#define PKG_SYS_STAGE   "system:/System/.staging"
#define PKG_SYS_DB      "system:/System/packages.db"
#define PKG_DB_MAX      2048U
#define PKG_SUP_NAME    "supervisor"

static int pkgd_port = -1;

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

int pkg_install(const uint8_t *pkg, uint32_t len, uint32_t euid,
                char *msg, uint32_t max)
{
    pkg_manifest_t m;
    pkg_header_t h;
    acct_user_t user;
    char root[FS_PATH_MAX];
    char stage[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    int r;

    r = pkg_verify(pkg, len, &m);
    if (r != PKG_OK) {
        set_msg(msg, max, pkg_status_text(r));
        return r;
    }
    r = pkg_read_header(pkg, len, &h);
    if (r != PKG_OK) { set_msg(msg, max, pkg_status_text(r)); return r; }

    /* ---- 策略：service 只有 root + account=root；app 只能进自己家目录 ---- */
    if (m.type == PKG_TYPE_SERVICE) {
        if (euid != 0U) { set_msg(msg, max, "service requires root"); return PKG_E_POLICY; }
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
        char line[320];
        int n = ksnprintf(line, sizeof(line),
                          "name=%s version=%s type=%s entry=%s euid=%u root=%s files=%u",
                          m.name, m.version,
                          (m.type == PKG_TYPE_SERVICE) ? "service" : "app",
                          m.entry, (unsigned int)euid, root,
                          (unsigned int)h.file_count);
        if (n <= 0 || (uint32_t)n >= sizeof(line) || db_append(euid, line) != 0) {
            (void)remove_tree(root);
            set_msg(msg, max, "db write failed");
            return PKG_E_IO;
        }
    }

    /* ---- 通知 supervisor 启动（安装后立即可用） ---- */
    if (m.type == PKG_TYPE_SERVICE) {
        char cmd[48];
        char reply[32];

        (void)ksnprintf(cmd, sizeof(cmd), "start:%s", m.entry);
        if (sup_request(cmd, reply, sizeof(reply)) > 0) {
            kprintf("pkg: supervisor start %s %s\n", m.entry, reply);
        } else {
            kprintf("pkg: supervisor start %s failed\n", m.entry);
        }
    }

    kprintf("pkg: installed %s-%s -> %s (%u file(s))\n",
            m.name, m.version, root, (unsigned int)h.file_count);
    set_msg(msg, max, "ok");
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * 卸载
 * ------------------------------------------------------------------------- */
int pkg_remove(const char *name, uint32_t euid, char *msg, uint32_t max)
{
    char line[320];
    char type[16];
    char root[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char entry[PKG_ENTRY_MAX];

    if (name == 0) {
        set_msg(msg, max, "bad name");
        return PKG_E_FORMAT;
    }
    /* 在调用者自己的 DB 里找：root 看系统组件，普通用户只看自己的应用 */
    if (db_find(name, euid, line, sizeof(line)) != 0) {
        set_msg(msg, max, "not installed");
        return PKG_E_NOENT;
    }
    if (db_field(line, "type", type, sizeof(type)) != 0 ||
        db_field(line, "root", root, sizeof(root)) != 0) {
        set_msg(msg, max, "db corrupt");
        return PKG_E_IO;
    }
    if (euid != 0U && strcmp(type, "app") != 0) {
        set_msg(msg, max, "remove requires root");
        return PKG_E_POLICY;
    }

    if (strcmp(type, "service") == 0) {
        char cmd[48];
        char reply[32];

        if (db_field(line, "entry", entry, sizeof(entry)) == 0) {
            (void)ksnprintf(cmd, sizeof(cmd), "stop:%s", entry);
            if (sup_request(cmd, reply, sizeof(reply)) > 0) {
                kprintf("pkg: supervisor stop %s %s\n", entry, reply);
            } else {
                kprintf("pkg: supervisor stop %s failed\n", entry);
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
                char line[320];
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
    char line[320];
    char version[PKG_VERSION_MAX];
    char root[FS_PATH_MAX];
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
    set_msg(msg, max, "ok");
    return PKG_OK;
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
            const pkg_blob_t *blob = pkg_catalog_find(cmd + 8);
            char detail[40];

            if (blob == 0) {
                reply_fmt(reply, sizeof(reply), PKG_E_NOENT, "no such package");
            } else {
                int r = pkg_install(blob->data, blob->len, euid, detail, sizeof(detail));
                reply_fmt(reply, sizeof(reply), r, detail);
            }
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
