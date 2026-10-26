/* =============================================================================
 * pkg/pkg_verify.c - .pkg 容器解析与验签（格式见 include/pkg.h）
 *
 * 纯函数、不碰文件系统：安装/校验/宿主侧工具（tools/pkg-verify.ps1）共用同一
 * 套规则。分三层：
 *   1. 结构：magic/版本/各段偏移必须在容器内、文件表连续、名字合法；
 *   2. 签名：SHA-256(container[0, sig_off)) + RSA-2048 vs 内嵌 pkg_modulus；
 *   3. 清单 + 依赖：字段齐全、type/capabilities 合法、requires 里的服务/
 *      ABI 版本满足。
 * 逐文件的 SHA-256 也在这里校验：签名保护"包没被改过"，哈希保护"数据区里
 * 的字节就是清单声明的那个文件"。
 * ============================================================================= */
#include "include/pkg.h"
#include "include/kernel.h"
#include "include/security.h"
#include "include/svc.h"
#include "include/fs.h"
#include "include/abi.h"

/* -------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 文件表项按小端逐字段读，不把容器里的字节直接当结构体指针用：
 * 数据区起点只保证 16 字节对齐，避免对未对齐地址做结构体访问。 */
static void ent_at(const uint8_t *pkg, uint32_t filetab_off, uint32_t idx,
                   pkg_file_ent_t *out)
{
    const uint8_t *p = pkg + filetab_off + idx * PKG_FILE_ENT_SIZE;

    memset(out, 0, sizeof(*out));
    memcpy(out->name, p, PKG_NAME_MAX);
    out->name[PKG_NAME_MAX - 1U] = '\0';
    out->offset = rd32(p + 32);
    out->size = rd32(p + 36);
    out->mode = rd32(p + 40);
    memcpy(out->sha256, p + 44, 32);
}

static int starts_with(const char *s, const char *prefix)
{
    uint32_t i = 0;

    if (s == 0 || prefix == 0) return 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static void copy_field(char *dst, uint32_t max, const char *src, uint32_t len)
{
    uint32_t n = (len < (max - 1U)) ? len : (max - 1U);

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 合法包名/文件名：小写字母、数字、'.'、'_'、'-'；不允许 '/'、'..' */
static int valid_component(const char *s)
{
    uint32_t i = 0;

    if (s == 0 || s[0] == '\0') return 0;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    while (s[i] != '\0') {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return 0;
        }
        i++;
        if (i >= PKG_NAME_MAX) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * manifest 解析
 * ------------------------------------------------------------------------- */
static int caps_parse(const char *list, uint32_t *out)
{
    uint32_t caps = 0;
    const char *p = list;

    if (list == 0 || list[0] == '\0') { *out = 0; return PKG_OK; }
    while (*p != '\0') {
        char item[24];
        uint32_t n = 0;

        while (*p == ' ' || *p == ',') p++;
        if (*p == '\0') break;
        while (*p != '\0' && *p != ',') {
            if (n + 1U >= sizeof(item)) return PKG_E_FORMAT;
            item[n++] = *p++;
        }
        item[n] = '\0';
        while (n > 0U && item[n - 1U] == ' ') item[--n] = '\0';

        if (strcmp(item, "CAP_SVC_REGISTER") == 0) caps |= PKG_CAP_SVC_REGISTER;
        else if (strcmp(item, "CAP_NET") == 0) caps |= PKG_CAP_NET;
        else if (strcmp(item, "CAP_FS_WRITE") == 0) caps |= PKG_CAP_FS_WRITE;
        else if (strcmp(item, "CAP_SYS_ADMIN") == 0) caps |= PKG_CAP_SYS_ADMIN;
        else if (strcmp(item, "CAP_PKG_ADMIN") == 0) caps |= PKG_CAP_PKG_ADMIN;
        else return PKG_E_FORMAT;      /* 未知 capability 一律拒绝 */
    }
    *out = caps;
    return PKG_OK;
}

static int parse_manifest(const uint8_t *text, uint32_t len, pkg_manifest_t *m)
{
    uint32_t i = 0;
    uint32_t seen = 0;
    int have_caps = 0;

    memset(m, 0, sizeof(*m));
    while (i < len) {
        uint32_t start = i;
        uint32_t eq;
        uint32_t end;

        while (i < len && text[i] != '\n') i++;
        end = i;
        if (i < len) i++;                       /* 跳过 '\n' */
        if (end > start && text[end - 1U] == '\r') end--;
        if (end == start) continue;             /* 空行 */

        eq = start;
        while (eq < end && text[eq] != '=') eq++;
        if (eq >= end) return PKG_E_FORMAT;

        {
            uint32_t klen = eq - start;
            const char *val = (const char *)(text + eq + 1U);
            uint32_t vlen = end - eq - 1U;

            if (klen == 4U && memcmp(text + start, "name", 4) == 0) {
                if (vlen == 0U || vlen >= PKG_NAME_MAX) return PKG_E_FORMAT;
                copy_field(m->name, sizeof(m->name), val, vlen);
                seen |= 1U;
            } else if (klen == 7U && memcmp(text + start, "version", 7) == 0) {
                if (vlen == 0U || vlen >= PKG_VERSION_MAX) return PKG_E_FORMAT;
                copy_field(m->version, sizeof(m->version), val, vlen);
                seen |= 2U;
            } else if (klen == 4U && memcmp(text + start, "type", 4) == 0) {
                if (vlen == 7U && memcmp(val, "service", 7) == 0) m->type = PKG_TYPE_SERVICE;
                else if (vlen == 3U && memcmp(val, "app", 3) == 0) m->type = PKG_TYPE_APP;
                else return PKG_E_FORMAT;
                seen |= 4U;
            } else if (klen == 5U && memcmp(text + start, "entry", 5) == 0) {
                if (vlen == 0U || vlen >= PKG_ENTRY_MAX) return PKG_E_FORMAT;
                copy_field(m->entry, sizeof(m->entry), val, vlen);
                seen |= 8U;
            } else if (klen == 8U && memcmp(text + start, "requires", 8) == 0) {
                if (vlen >= PKG_REQ_MAX) return PKG_E_FORMAT;
                copy_field(m->requires, sizeof(m->requires), val, vlen);
                seen |= 16U;
            } else if (klen == 12U && memcmp(text + start, "capabilities", 12) == 0) {
                if (vlen >= PKG_CAPS_MAX) return PKG_E_FORMAT;
                copy_field(m->capabilities, sizeof(m->capabilities), val, vlen);
                have_caps = 1;
                seen |= 32U;
            } else if (klen == 7U && memcmp(text + start, "account", 7) == 0) {
                if (vlen == 0U || vlen >= PKG_ACCOUNT_MAX) return PKG_E_FORMAT;
                copy_field(m->account, sizeof(m->account), val, vlen);
                seen |= 64U;
            } else if (klen == 5U && memcmp(text + start, "files", 5) == 0) {
                if (vlen >= PKG_LIST_MAX) return PKG_E_FORMAT;
                copy_field(m->files, sizeof(m->files), val, vlen);
                seen |= 128U;
            } else {
                return PKG_E_FORMAT;            /* 未知字段：拒绝而不是忽略 */
            }
        }
    }

    if (seen != 255U) return PKG_E_FORMAT;      /* 8 个必填字段缺一不可 */
    if (!have_caps) return PKG_E_FORMAT;
    if (!valid_component(m->name)) return PKG_E_FORMAT;
    if (m->type == PKG_TYPE_APP && m->capabilities[0] != '\0') return PKG_E_FORMAT;
    if (caps_parse(m->capabilities, &m->caps) != PKG_OK) return PKG_E_FORMAT;
    return PKG_OK;
}

/* 清单里的 files= 必须与文件表逐项同名同序（安装按顺序落盘） */
static int manifest_matches_table(const pkg_manifest_t *m, const uint8_t *pkg,
                                  const pkg_header_t *h)
{
    const char *p = m->files;
    uint32_t idx = 0;

    while (*p != '\0') {
        char item[PKG_NAME_MAX];
        uint32_t n = 0;
        pkg_file_ent_t e;

        while (*p == ' ' || *p == ',') p++;
        if (*p == '\0') break;
        while (*p != '\0' && *p != ',') {
            if (n + 1U >= sizeof(item)) return PKG_E_FORMAT;
            item[n++] = *p++;
        }
        item[n] = '\0';
        if (idx >= h->file_count) return PKG_E_FORMAT;

        ent_at(pkg, h->filetab_off, idx, &e);
        if (strcmp(e.name, item) != 0) return PKG_E_FORMAT;
        idx++;
    }
    return (idx == h->file_count) ? PKG_OK : PKG_E_FORMAT;
}

/* -------------------------------------------------------------------------
 * 结构校验
 * ------------------------------------------------------------------------- */
static int check_structure(const uint8_t *pkg, uint32_t len, pkg_header_t *h)
{
    uint32_t data_cursor = 0;

    if (pkg == 0 || len < PKG_HEADER_SIZE + PKG_SIG_SIZE) return PKG_E_FORMAT;
    if (pkg[0] != PKG_MAGIC0 || pkg[1] != PKG_MAGIC1 ||
        pkg[2] != PKG_MAGIC2 || pkg[3] != PKG_MAGIC3) {
        return PKG_E_FORMAT;
    }

    h->version      = rd32(pkg + 4);
    h->type         = rd32(pkg + 8);
    h->manifest_off = rd32(pkg + 12);
    h->manifest_len = rd32(pkg + 16);
    h->file_count   = rd32(pkg + 20);
    h->filetab_off  = rd32(pkg + 24);
    h->data_off     = rd32(pkg + 28);
    h->data_len     = rd32(pkg + 32);
    h->sig_off      = rd32(pkg + 36);
    h->total_len    = rd32(pkg + 40);
    h->flags        = rd32(pkg + 44);

    if (h->version != PKG_FORMAT_VERSION) return PKG_E_FORMAT;
    if (h->type != PKG_TYPE_SERVICE && h->type != PKG_TYPE_APP) return PKG_E_FORMAT;
    if (h->total_len != len) return PKG_E_FORMAT;
    if (h->flags != 0U) return PKG_E_FORMAT;

    if (h->manifest_off < PKG_HEADER_SIZE ||
        h->manifest_len == 0U || h->manifest_len >= PKG_MANIFEST_MAX ||
        h->manifest_off + h->manifest_len > h->sig_off) return PKG_E_FORMAT;
    if (h->file_count == 0U || h->file_count > PKG_FILE_MAX) return PKG_E_FORMAT;
    if (h->filetab_off < PKG_HEADER_SIZE ||
        h->filetab_off + h->file_count * PKG_FILE_ENT_SIZE > h->sig_off) return PKG_E_FORMAT;
    if (h->data_off < h->filetab_off || h->data_off > h->sig_off) return PKG_E_FORMAT;
    if (h->data_off + h->data_len != h->sig_off) return PKG_E_FORMAT;
    if (h->sig_off + PKG_SIG_SIZE != h->total_len) return PKG_E_FORMAT;

    /* 文件表：名字合法/不重复、数据区连续且覆盖整个 data_len */
    for (uint32_t i = 0; i < h->file_count; i++) {
        pkg_file_ent_t e;

        ent_at(pkg, h->filetab_off, i, &e);
        if (!valid_component(e.name)) return PKG_E_FORMAT;
        for (uint32_t j = 0; j < i; j++) {
            pkg_file_ent_t o;

            ent_at(pkg, h->filetab_off, j, &o);
            if (strcmp(o.name, e.name) == 0) return PKG_E_FORMAT;
        }
        if (e.offset != data_cursor) return PKG_E_FORMAT;
        if (e.size > FS_FILE_MAX) return PKG_E_FORMAT;
        if (e.offset + e.size > h->data_len) return PKG_E_FORMAT;
        data_cursor += e.size;
    }
    if (data_cursor != h->data_len) return PKG_E_FORMAT;
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * 依赖：service:<name> / abi:<major>.<minor>
 * ------------------------------------------------------------------------- */
static int abi_ok(const char *v)
{
    uint32_t major = 0, minor = 0;
    const char *p = v;

    if (v == 0 || v[0] == '\0') return 0;
    while (*p >= '0' && *p <= '9') { major = major * 10U + (uint32_t)(*p - '0'); p++; }
    if (*p != '.') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') { minor = minor * 10U + (uint32_t)(*p - '0'); p++; }
    if (*p != '\0') return 0;
    if (major != (uint32_t)OS_ABI_VERSION_MAJOR) return 0;
    return (minor <= (uint32_t)OS_ABI_VERSION_MINOR) ? 1 : 0;
}

static int requires_ok(const char *requires)
{
    const char *p = requires;

    while (p != 0 && *p != '\0') {
        char item[40];
        uint32_t n = 0;

        while (*p == ' ' || *p == ',') p++;
        if (*p == '\0') break;
        while (*p != '\0' && *p != ',') {
            if (n + 1U >= sizeof(item)) return PKG_E_FORMAT;
            item[n++] = *p++;
        }
        item[n] = '\0';
        while (n > 0U && item[n - 1U] == ' ') item[--n] = '\0';

        if (starts_with(item, "service:")) {
            if (!service_exists(item + 8)) return PKG_E_REQUIRES;
        } else if (starts_with(item, "abi:")) {
            if (!abi_ok(item + 4)) return PKG_E_REQUIRES;
        } else {
            return PKG_E_FORMAT;               /* 未知依赖种类：拒绝 */
        }
    }
    return PKG_OK;
}

/* -------------------------------------------------------------------------
 * 对外：pkg_verify
 * ------------------------------------------------------------------------- */
int pkg_verify(const uint8_t *pkg, uint32_t len, pkg_manifest_t *out)
{
    pkg_header_t h;
    pkg_manifest_t man;
    int r;

    r = check_structure(pkg, len, &h);
    if (r != PKG_OK) return r;

    r = parse_manifest(pkg + h.manifest_off, h.manifest_len, &man);
    if (r != PKG_OK) return r;
    if (man.type != h.type) return PKG_E_FORMAT;

    r = manifest_matches_table(&man, pkg, &h);
    if (r != PKG_OK) return r;

    /* 逐文件哈希（数据区里就是文件内容） */
    for (uint32_t i = 0; i < h.file_count; i++) {
        pkg_file_ent_t e;
        uint8_t got[32];

        ent_at(pkg, h.filetab_off, i, &e);
        sha256_bytes(pkg + h.data_off + e.offset, e.size, got);
        if (memcmp(got, e.sha256, 32) != 0) return PKG_E_HASH;
    }

    /* 签名覆盖 [0, sig_off)：header + manifest + 文件表 + 数据 */
    if (!trust_verify_image(pkg, h.sig_off, pkg + h.sig_off, pkg_modulus)) {
        return PKG_E_SIGNATURE;
    }

    r = requires_ok(man.requires);
    if (r != PKG_OK) return r;

    if (out != 0) *out = man;
    return PKG_OK;
}

int pkg_read_header(const uint8_t *pkg, uint32_t len, pkg_header_t *out)
{
    pkg_header_t h;
    int r = check_structure(pkg, len, &h);

    if (r != PKG_OK) return r;
    if (out != 0) *out = h;
    return PKG_OK;
}

void pkg_file_ent(const uint8_t *pkg, const pkg_header_t *h, uint32_t idx,
                  pkg_file_ent_t *out)
{
    if (pkg == 0 || h == 0 || out == 0 || idx >= h->file_count) return;
    ent_at(pkg, h->filetab_off, idx, out);
}

const char *pkg_status_text(int status)
{
    switch (status) {
    case PKG_OK:            return "ok";
    case PKG_E_FORMAT:      return "format";
    case PKG_E_SIGNATURE:   return "signature";
    case PKG_E_HASH:        return "hash";
    case PKG_E_REQUIRES:    return "requires";
    case PKG_E_POLICY:      return "policy";
    case PKG_E_EXIST:       return "exist";
    case PKG_E_NOENT:       return "noent";
    case PKG_E_IO:          return "io";
    case PKG_E_BUSY:        return "busy";
    default:                return "unknown";
    }
}

/* 内建包目录查找（obj/pkg_catalog.c 由 build.ps1 生成） */
const pkg_blob_t *pkg_catalog_find(const char *name)
{
    if (name == 0) return 0;
    for (unsigned int i = 0; i < pkg_catalog_count; i++) {
        if (strcmp(pkg_catalog[i].name, name) == 0) return &pkg_catalog[i];
    }
    return 0;
}
