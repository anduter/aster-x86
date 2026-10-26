/* =============================================================================
 * include/pkg.h - 组件包（.pkg）与组件数据库 _pkgd
 *
 * 包格式（全部小端，单文件容器）：
 *
 *   0                header (64 字节, pkg_header_t)
 *   manifest_off     manifest 文本（key=value，每行一条）
 *   filetab_off      file_count 个 pkg_file_ent_t（每个 76 字节）
 *   data_off         文件数据区（按 file 表里的 offset/size 切分）
 *   sig_off          RSA-2048 签名（256 字节，PKCS#1 v1.5 + SHA-256）
 *
 * 签名覆盖 [0, sig_off) 的全部字节：header + manifest + 文件表 + 数据。
 * 文件表里每个文件另有自己的 SHA-256，安装时会逐字节重新校验
 * （签名保护"包没被改过"，哈希保护"写入磁盘的内容正确"）。
 *
 * 组件数据库是纯文本（system:/System/packages.db，用户应用在
 * <home>/Apps/apps.db），一行一个组件：
 *   name=<n> version=<v> type=<service|app> euid=<uid> root=<path> files=<n>
 *
 * 安装流程：验签 -> 查 requires -> 写 staging -> 逐文件校验哈希 -> 原子
 * rename 到正式目录 -> 追加 DB 行。任何一步失败都回滚（删 staging/正式目录）。
 * ============================================================================= */
#ifndef PKG_H_MODULE_H
#define PKG_H_MODULE_H

#include "types.h"

#define PKG_MAGIC0          'A'
#define PKG_MAGIC1          'P'
#define PKG_MAGIC2          'K'
#define PKG_MAGIC3          'G'
#define PKG_FORMAT_VERSION  1u

#define PKG_TYPE_SERVICE    1u
#define PKG_TYPE_APP        2u

#define PKG_HEADER_SIZE     64u
#define PKG_SIG_SIZE        256u
#define PKG_FILE_ENT_SIZE   76u
#define PKG_NAME_MAX        32u
#define PKG_VERSION_MAX     16u
#define PKG_ENTRY_MAX       64u
#define PKG_LIST_MAX        128u
#define PKG_CAPS_MAX        96u
#define PKG_ACCOUNT_MAX     32u
#define PKG_REQ_MAX         160u
#define PKG_MANIFEST_MAX    640u
#define PKG_FILE_MAX        16u

/* 包能申请的 capability（manifest 的 capabilities=，逗号分隔）。
 * app 包不允许申请任何 capability；service 包可以申请下面的位。
 * CAP_SYS_ADMIN / CAP_PKG_ADMIN 只授予签名更新器（_pkgd 自身），
 * 是 SIP-lite 的放行条件（见 docs/security-model.md）。 */
#define PKG_CAP_SVC_REGISTER (1u << 0)
#define PKG_CAP_NET          (1u << 1)
#define PKG_CAP_FS_WRITE     (1u << 2)
#define PKG_CAP_SYS_ADMIN    (1u << 3)
#define PKG_CAP_PKG_ADMIN    (1u << 4)
#define PKG_CAP_ALL          (PKG_CAP_SVC_REGISTER | PKG_CAP_NET | \
                              PKG_CAP_FS_WRITE | PKG_CAP_SYS_ADMIN | PKG_CAP_PKG_ADMIN)

/* 安装/校验结果码：0 成功，负数是具体原因（和 E_* 分开，便于打印） */
#define PKG_OK             0
#define PKG_E_FORMAT     (-1)   /* 容器/清单格式错误 */
#define PKG_E_SIGNATURE  (-2)   /* 验签失败（坏签名/被篡改） */
#define PKG_E_HASH       (-3)   /* 文件内容哈希不匹配 */
#define PKG_E_REQUIRES   (-4)   /* requires 缺失（服务/ABI） */
#define PKG_E_POLICY     (-5)   /* 权限/策略拒绝 */
#define PKG_E_EXIST      (-6)   /* 重复安装 */
#define PKG_E_NOENT      (-7)   /* 未安装/不存在 */
#define PKG_E_IO         (-8)   /* 文件系统写入/删除失败 */
#define PKG_E_BUSY       (-9)   /* 服务仍在运行/文件被占用 */

typedef struct {
    char     magic[4];
    uint32_t version;        /* PKG_FORMAT_VERSION */
    uint32_t type;           /* PKG_TYPE_SERVICE / PKG_TYPE_APP */
    uint32_t manifest_off;
    uint32_t manifest_len;
    uint32_t file_count;
    uint32_t filetab_off;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t sig_off;
    uint32_t total_len;
    uint32_t flags;
    char     signer[16];
} pkg_header_t;

typedef struct {
    char     name[PKG_NAME_MAX];
    uint32_t offset;         /* 相对 data_off */
    uint32_t size;
    uint32_t mode;           /* 安装后的权限位（0 = 默认 0644） */
    uint8_t  sha256[32];
} pkg_file_ent_t;

typedef struct {
    char     name[PKG_NAME_MAX];
    char     version[PKG_VERSION_MAX];
    char     entry[PKG_ENTRY_MAX];
    char     requires[PKG_REQ_MAX];
    char     capabilities[PKG_CAPS_MAX];
    char     account[PKG_ACCOUNT_MAX];
    char     files[PKG_LIST_MAX];
    uint32_t type;           /* PKG_TYPE_* */
    uint32_t caps;           /* capabilities 解析后的位图 */
} pkg_manifest_t;

/* 解析 + 完整校验（格式、签名、manifest、逐文件哈希）。不落盘。
 * out 可为 0（只校验）；成功后 out 里是解析出的 manifest。 */
int pkg_verify(const uint8_t *pkg, uint32_t len, pkg_manifest_t *out);

/* 只做结构校验并取出 header（pkg_verify 通过后用它定位文件表/数据区）。 */
int pkg_read_header(const uint8_t *pkg, uint32_t len, pkg_header_t *out);

/* 取第 idx 个文件表项（pkg 必须已通过 pkg_verify）。 */
void pkg_file_ent(const uint8_t *pkg, const pkg_header_t *h, uint32_t idx,
                  pkg_file_ent_t *out);

/* 已安装组件的路径：system:/System/Packages/<name>/<version>，
 * 用户应用：<home>/Apps/<name>/<version>。成功返回 0。 */
int pkg_component_root(const pkg_manifest_t *m, uint32_t euid,
                       char *out, uint32_t max);

/* 安装：验签 -> requires -> staging -> 哈希校验 -> 原子 rename -> DB。
 * euid 是调用者身份（0 = root 可装 service 到系统目录；
 * 非 root 只能装 type=app 到自己的 <home>/Apps）。
 * service 装好后通知 supervisor 启动。失败会自动回滚。 */
int pkg_install(const uint8_t *pkg, uint32_t len, uint32_t euid,
                char *msg, uint32_t max);

/* 卸载：运行中的 service 先停；删组件目录 + DB 行。 */
int pkg_remove(const char *name, uint32_t euid, char *msg, uint32_t max);

/* 列出已安装组件（多行文本，按 euid 选择 DB）。返回组件数。 */
uint32_t pkg_list(uint32_t euid, char *out, uint32_t max);

/* 校验已安装组件：DB 行存在 + 组件目录在 + 每个文件的 SHA-256 与内建
 * 包目录里的原始包一致。 */
int pkg_verify_installed(const char *name, uint32_t euid, char *msg, uint32_t max);

/* 把状态码转成短文本（日志/回复用） */
const char *pkg_status_text(int status);

/* ---- 构建期生成的包目录（obj/pkg_catalog.c） ---- */
typedef struct {
    const char *name;        /* 文件名，例如 "echosvc-1.0.0.pkg" */
    const uint8_t *data;
    uint32_t len;
} pkg_blob_t;

extern const pkg_blob_t pkg_catalog[];
extern const unsigned int pkg_catalog_count;
extern const unsigned char pkg_modulus[256];

const pkg_blob_t *pkg_catalog_find(const char *name);

/* ---- _pkgd：ring0 内建组件服务（后续可原样迁到 ring3） ---- */
#define PKGD_SERVICE_NAME "pkgd"

/* 建 port 并注册服务（在 supervisor_init 之前调用一次） */
void pkgd_init(void);
/* 服务循环：作为程序 "pkgd" 运行 */
void pkgd_entry(void);
/* 客户端：发一条文本命令，收一条回复（阻塞）。返回回复长度或负错误。 */
int pkgd_request(const char *cmd, char *reply, uint32_t max);

/* ---- 自检驱动：作为程序 "pkgtest" 运行，打印 pkg: 开头的断言行 ---- */
void pkg_selftest_entry(void);

#endif /* PKG_H_MODULE_H */
