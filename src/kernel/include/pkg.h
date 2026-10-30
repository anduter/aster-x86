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

/* 组件 DB 一行最多多少字节（P0 起多出 program=/elf=/elfsha= 三个字段）。 */
#define PKG_DB_LINE_MAX     512u

/* 组件 ELF entry（P0）：
 * 安装时把服务的可执行映像放到 <root>/bin/<name>，启动时从组件目录读回来，
 * 先比对 DB 里记录的 SHA-256、再验证 APKG 包签名（或 shell 运行时签名），
 * 成功后映射成 ring3 程序 exec。失败由安装流程回滚（删目录 + 删 DB 行）。
 * 文件名/哈希在组件 DB 里是 elf=/elfsha= 字段。 */
#define PKG_ELF_DIR         "bin"
#define PKG_ELF_HASH_HEX    64u
#define PKG_COMPONENT_MAX   4u

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

/* 从"内建包目录里的文件名"或文件系统里的 .pkg 路径安装。
 * 路径形式（system:/tmp/x.pkg）优先按文件读；否则先查内建包目录。
 * 这是 SYS_PKG / _pkgd "install <file>" 的统一入口。 */
int pkg_install_path(const char *path, uint32_t euid, char *msg, uint32_t max);

/* 卸载：运行中的 service 先停；删组件目录 + DB 行。 */
int pkg_remove(const char *name, uint32_t euid, char *msg, uint32_t max);

/* 列出已安装组件（多行文本，按 euid 选择 DB）。返回组件数。 */
uint32_t pkg_list(uint32_t euid, char *out, uint32_t max);

/* 校验已安装组件：DB 行存在 + 组件目录在 + 每个文件的 SHA-256 与内建
 * 包目录里的原始包一致。P0 起还会校验 bin/ 下组件 ELF 的 SHA-256。 */
int pkg_verify_installed(const char *name, uint32_t euid, char *msg, uint32_t max);

/* ---- P0：组件 ELF entry ---- */
/* 启动已安装 service 的 ELF：读 <root>/bin/<name>，比对 DB 的 elfsha，
 * 再验 APKG 包签名（内建包）或 shell 运行时签名；成功后新建进程、映射
 * 成 ring3 并 exec（argv[0]="svc:<name>"）。返回 PKG_OK / 负错误码。 */
int pkg_component_start(const char *name, uint32_t euid,
                        char *msg, uint32_t max);

/* 停止 _pkgd 启动的组件进程：先发 "stop" 让它自己注销服务/退出，
 * 超时才 SIGKILL；最后兜底 service_unregister。没有运行中的组件也返回 PKG_OK。 */
int pkg_component_stop(const char *name);

/* 只校验（不启动）：<root>/bin/<name> 存在、SHA-256 与 DB 一致、签名有效、
 * 同名服务在线。selftest 用。 */
int pkg_component_verify_elf(const char *name, uint32_t euid,
                             char *msg, uint32_t max);

/* 组件进程通过 SYS_PKG 报告/注销自己的服务 port：
 * 只有 _pkgd 启动的那个 pid 才能调用（调用者身份取 proc_current()）。 */
int pkg_component_report_port(uint32_t port);
int pkg_component_unregister_port(uint32_t port);

/* ---- SYS_PKG (77)：ring3 组件 CLI 的内核入口 ----
 *
 * 编码（完整说明见 docs/claims/pkg.md）：
 *   ebx=op, ecx=a, edx=b；返回 0 成功 / -1 失败；
 *   LIST 返回组件条数（>=0），其余 op 的回复文本写进 b 指向的用户缓冲。
 *
 *   PKG_OP_LIST          0  a=user buf, b=max            -> 条数
 *   PKG_OP_INSTALL       1  a=包名/路径, b=reply buf     -> 0 / -1
 *   PKG_OP_REMOVE        2  a=组件名,   b=reply buf      -> 0 / -1
 *   PKG_OP_VERIFY        3  a=组件名,   b=reply buf      -> 0 / -1
 *   PKG_OP_COMPONENT_PORT 4 a=port object id             -> 0 / -1（仅组件进程）
 *   PKG_OP_COMPONENT_UNREG 5 a=port object id            -> 0 / -1（仅组件进程）
 */
#define PKG_OP_LIST              0u
#define PKG_OP_INSTALL           1u
#define PKG_OP_REMOVE            2u
#define PKG_OP_VERIFY            3u
#define PKG_OP_COMPONENT_PORT    4u
#define PKG_OP_COMPONENT_UNREG   5u

/* ---- P4：ring3 pkgd 调用的内核“验签/落盘原语”（op 6..17） ----
 * 只允许已注册的 ring3 pkgd 进程调用（内核校验 svc 注册表 pid），
 * 客户端身份由内核分发器在转发请求时记录（见 pkgd.c 的 pkgd_ctx）。
 *
 *   6  SESSION_BEGIN   a=包字节指针 或 路径串, b=字节数（0=按路径/内建包）
 *   7  SESSION_INFO    a=out, b=max   -> "name=.. version=.. type=.. files=N ..."
 *   8  SESSION_STAGE   a=文件下标     -> 写 staging + 复验哈希
 *   9  SESSION_COMMIT  a=0            -> 组件 ELF + 原子 rename + DB 行
 *   10 SESSION_START   a=0            -> exec 组件 ELF（失败自动回滚）
 *   11 SESSION_ABORT   a=0            -> 回滚（staging 或已提交目录 + DB 行）
 *   12 REMOVE_BEGIN    a=组件名       -> 策略/DB 校验，建卸载会话
 *   13 REMOVE_INFO     a=out, b=max   -> "type=.. program=.. root=.."
 *   14 REMOVE_COMMIT   a=0            -> 停组件 + 删目录 + 删 DB 行
 *   15 REMOVE_ABORT    a=0            -> 取消卸载会话
 *   16 DB_READ         a=out, b=max   -> DB 文本长度（列表由 ring3 解析）
 *   17 VERIFY_RAW      a=组件名       -> 0 / PKG_E_*（已安装完整性）
 */
#define PKG_OP_SESSION_BEGIN     6u
#define PKG_OP_SESSION_INFO      7u
#define PKG_OP_SESSION_STAGE     8u
#define PKG_OP_SESSION_COMMIT    9u
#define PKG_OP_SESSION_START     10u
#define PKG_OP_SESSION_ABORT     11u
#define PKG_OP_REMOVE_BEGIN      12u
#define PKG_OP_REMOVE_INFO       13u
#define PKG_OP_REMOVE_COMMIT     14u
#define PKG_OP_REMOVE_ABORT      15u
#define PKG_OP_DB_READ           16u
#define PKG_OP_VERIFY_RAW        17u

/* 入口模式（启动参数 pkgd=ring0|ring3|auto，缺省 auto） */
#define PKGD_MODE_AUTO           0
#define PKGD_MODE_RING0          1
#define PKGD_MODE_RING3          2

#define PKG_CLI_REPLY_MAX        128u

int pkg_syscall(uint32_t op, uint32_t a, uint32_t b);

/* 把状态码转成短文本（日志/回复用） */
const char *pkg_status_text(int status);

/* 系统组件的管理员判定：root（euid==0）等价于持有 CAP_PKG_ADMIN。
 * SIP-lite 的签名更新器作用域同样要求 root；per-process capability 位图
 * 还没落地，所以这里是唯一的判定点，CLI 与 pkg_install/pkg_remove 共用。 */
static inline int pkg_euid_is_admin(uint32_t euid)
{
    return euid == 0U;
}

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

/* ---- _pkgd：组件安装服务（ring0 内建 / ring3 守护进程，P4） ---- */
#define PKGD_SERVICE_NAME "pkgd"

/* 入口模式：ring0=内核内建实现；ring3=ring3 守护进程；auto=优先 ring3，
 * 不可用时回退 ring0。启动参数 pkgd=... 与编译期 -DPKGD_FORCE_MODE 都认。 */
int pkgd_mode(void);
const char *pkgd_mode_name(int mode);
/* 测试钩子：只在 -Test 构建的 pkgd 自检里切换模式（模拟 pkgd=ring0/ring3 启动） */
void pkgd_test_set_mode(int mode);

/* ring3 pkgd 是否在线（svc 注册表 RUNNING + 进程存活 + IPC 端口有效） */
int pkgd_ring3_online(void);
uint32_t pkgd_ring3_pid(void);

/* 内建 ring3 pkgd 的 ELF 映像（build.ps1 生成 obj/pkgd_blob.asm incbin 进来） */
extern uint8_t pkgd_user_blob_start[];
extern uint8_t pkgd_user_blob_end[];

/* CLI 路由（SYS_PKG 0..3 的实现）：ring3 在线则转发，auto 失败回退 ring0。 */
int pkgd_cli_install(const char *path, uint32_t euid,
                     char *detail, uint32_t max);
int pkgd_cli_remove(const char *name, uint32_t euid,
                    char *detail, uint32_t max);
int pkgd_cli_verify(const char *name, uint32_t euid,
                    char *detail, uint32_t max);
uint32_t pkgd_cli_list(uint32_t euid, char *out, uint32_t max);

/* 内核原语分发（op 6..17）；只接受已注册的 ring3 pkgd 调用。 */
int pkgd_primitive(uint32_t op, uint32_t a, uint32_t b);

/* ring3 服务化自检（-Test 构建，由 pkgtest 线程在 pkg 回归之后调用） */
void pkgd_selftest(void);

/* supervisor 线程在启动期自检（svc_selftest 会用"虚拟 now"驱动整张服务表）
 * 结束后调用：ring3 pkgd 必须等它结束再注册，否则会被误判心跳超时。
 * 生产构建里 supervisor 线程一启动就调用，等于无延迟。 */
void pkgd_supervisor_ready(void);

/* 建 port 并注册服务（ring0 模式在 supervisor_init 之前调用一次） */
void pkgd_init(void);
/* 服务入口：按模式启动 ring3 守护进程或 ring0 回退；作为程序 "pkgd" 运行 */
void pkgd_entry(void);
/* 客户端：发一条文本命令，收一条回复（阻塞）。返回回复长度或负错误。 */
int pkgd_request(const char *cmd, char *reply, uint32_t max);

/* ---- 自检驱动：作为程序 "pkgtest" 运行，打印 pkg: 开头的断言行 ---- */
void pkg_selftest_entry(void);

#endif /* PKG_H_MODULE_H */
