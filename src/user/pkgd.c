/* =============================================================================
 * pkgd.c - ring3 组件服务守护进程（P4：pkgd 服务化）
 *
 * 定位：把安装/卸载/列表/校验的**流程与策略**放在 ring3，内核只保留
 * "验签/落盘原语"（SYS_PKG 的 6..17 号 op，见 include/pkg.h）。
 *
 *   客户端（SYS_PKG 0..3，ring3 shell 的 pkg CLI / pkgtest）
 *        -> 内核分发器（svc_lookup("pkgd") + IPC 消息端口）
 *        -> 本进程：解析请求、读包文件、驱动原语、通知 supervisor、回滚
 *        -> 内核原语：pkg_verify / staging 写文件 / 原子 rename / DB / exec
 *
 * 服务注册（两份，过渡期）：
 *   1. SYS_SVC_REGISTER(86)：name=pkgd，给内核分发器/ svc_lookup 客户端用，
 *      svcwatch 按 restart=always 重启（entry=pkgd 仍是程序表里的启动线程）；
 *   2. native SERVICE_REGISTER：旧 service_connect("pkgd") 客户端兼容。
 *
 * 故障隔离：被杀后内核/shell 不受影响；svcwatch 顺着 manifest 重启本进程；
 * 客户端（内核分发器）重新 svc_lookup 拿新 port；ring3 不可用时 auto 模式
 * 回退内核 ring0 实现（见 pkg/pkgd.c 的 pkgd_cli_*）。
 * ============================================================================= */
#include "ulib.h"

/* ---- 与内核的 IPC 请求/回复帧（payload，16B IPC 头之外） ---- */
#define PKGD_IPC_MAGIC    0x504B4732U   /* "PKG2" 请求 */
#define PKGD_REPLY_MAGIC  0x504B4733U   /* "PKG3" 回复 */

/* 客户端命令 op（与 include/pkg.h 的 PKG_OP_* 一致） */
#define PKGD_OP_LIST      0U
#define PKGD_OP_INSTALL   1U
#define PKGD_OP_REMOVE    2U
#define PKGD_OP_VERIFY    3U

/* 内核原语 op（与 include/pkg.h 的 PKG_OP_SESSION_* 一致） */
#define PKG_OP_SESSION_BEGIN  6U
#define PKG_OP_SESSION_INFO   7U
#define PKG_OP_SESSION_STAGE  8U
#define PKG_OP_SESSION_COMMIT 9U
#define PKG_OP_SESSION_START  10U
#define PKG_OP_SESSION_ABORT  11U
#define PKG_OP_REMOVE_BEGIN   12U
#define PKG_OP_REMOVE_INFO    13U
#define PKG_OP_REMOVE_COMMIT  14U
#define PKG_OP_REMOVE_ABORT   15U
#define PKG_OP_DB_READ        16U
#define PKG_OP_VERIFY_RAW     17U

/* 组件服务返回码（与 include/pkg.h 的 PKG_E_* 一致） */
#define PKGD_OK            0
#define PKGD_E_FORMAT    (-1)
#define PKGD_E_SIGNATURE (-2)
#define PKGD_E_HASH      (-3)
#define PKGD_E_REQUIRES  (-4)
#define PKGD_E_POLICY    (-5)
#define PKGD_E_EXIST     (-6)
#define PKGD_E_NOENT     (-7)
#define PKGD_E_IO        (-8)
#define PKGD_E_BUSY      (-9)

#define PKGD_PATH_MAX   192U
#define PKGD_DETAIL_MAX 96U
#define PKGD_MAX_PKG    65536U

#define PKGD_HDR_SIZE   16U

static int pkgd_ipc_port = -1;

/* -------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
    p[2] = (unsigned char)((v >> 16) & 0xFFU);
    p[3] = (unsigned char)((v >> 24) & 0xFFU);
}

static int sys_pkg(int op, int a, int b)
{
    return syscall3(SYS_PKG, op, a, b);
}

static int pkgd_getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

static const char *status_text(int status)
{
    switch (status) {
    case PKGD_OK:           return "ok";
    case PKGD_E_FORMAT:     return "format";
    case PKGD_E_SIGNATURE:  return "signature";
    case PKGD_E_HASH:       return "hash";
    case PKGD_E_REQUIRES:   return "requires";
    case PKGD_E_POLICY:     return "policy";
    case PKGD_E_EXIST:      return "exist";
    case PKGD_E_NOENT:      return "noent";
    case PKGD_E_IO:         return "io";
    case PKGD_E_BUSY:       return "busy";
    default:                return "error";
    }
}

static void set_detail(char *detail, unsigned int max, const char *text)
{
    unsigned int i = 0;

    if (detail == 0 || max == 0U) return;
    if (text == 0) text = "";
    while (text[i] != '\0' && i + 1U < max) {
        detail[i] = text[i];
        i++;
    }
    detail[i] = '\0';
}

static void detail_status(char *detail, unsigned int max, int status)
{
    set_detail(detail, max, status_text(status));
}

/* 把 "key=value" 里的值取出来（值不含空格）。返回 0 成功。 */
static int info_field(const char *line, const char *key, char *out,
                      unsigned int max)
{
    unsigned int klen = strlen(key);
    const char *p = line;

    if (out == 0 || max == 0U) return -1;
    while (p != 0 && *p != '\0') {
        while (*p == ' ') p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1U;
            unsigned int n = 0;

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

static int append_str(char *dst, unsigned int cap, unsigned int *used,
                      const char *src)
{
    unsigned int n = strlen(src);

    if (dst == 0 || used == 0 || src == 0) return -1;
    if (*used + n + 1U > cap) return -1;
    memcpy(dst + *used, src, n);
    *used += n;
    dst[*used] = '\0';
    return 0;
}

static int notify_supervisor(const char *cmd)
{
    int ch;
    char reply[16];
    int n;

    if (cmd == 0 || cmd[0] == '\0') return -1;
    ch = nservice_connect("supervisor");
    if (ch <= 0) {
        printf("pkgd: supervisor unreachable (%s)\n", cmd);
        return -1;
    }
    n = nchan_send(ch, cmd, (int)strlen(cmd));
    if (n > 0) {
        (void)nchan_recv_block(ch, reply, sizeof(reply) - 1U);
    }
    (void)nhandle_close(ch);
    return (n > 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * 列表：内核只提供 DB 文本原语，过滤/格式化在 ring3
 * ------------------------------------------------------------------------- */
static int pkgd_do_list(char *out, unsigned int max, unsigned int *count_out)
{
    static char db[2048];
    int n;
    unsigned int i = 0;
    unsigned int used = 0;
    unsigned int count = 0;

    if (out == 0 || max == 0U) return PKGD_E_FORMAT;
    out[0] = '\0';
    n = sys_pkg(PKG_OP_DB_READ, (int)db, (int)(sizeof(db) - 1U));
    if (n < 0) return PKGD_E_IO;

    while (i < (unsigned int)n) {
        unsigned int start = i;
        unsigned int end;
        char line[512];
        char name[32];
        char version[16];
        char type[16];
        unsigned int ln;

        while (i < (unsigned int)n && db[i] != '\n') i++;
        end = i;
        if (i < (unsigned int)n) i++;
        while (end > start && db[end - 1U] == '\r') end--;
        if (end == start) continue;

        ln = end - start;
        if (ln >= sizeof(line)) ln = sizeof(line) - 1U;
        memcpy(line, db + start, ln);
        line[ln] = '\0';

        if (info_field(line, "name", name, sizeof(name)) != 0) continue;
        if (info_field(line, "version", version, sizeof(version)) != 0) continue;
        if (info_field(line, "type", type, sizeof(type)) != 0) continue;

        if (append_str(out, max, &used, name) == 0 &&
            append_str(out, max, &used, " ") == 0 &&
            append_str(out, max, &used, version) == 0 &&
            append_str(out, max, &used, " ") == 0 &&
            append_str(out, max, &used, type) == 0 &&
            append_str(out, max, &used, "\n") == 0) {
            count++;
        } else {
            break;
        }
    }
    if (count_out != 0) *count_out = count;
    return PKGD_OK;
}

/* -------------------------------------------------------------------------
 * 安装：读包文件 -> 内核验签/策略 -> 逐文件落盘 -> rename+DB -> 通知+启动
 * ------------------------------------------------------------------------- */
static int pkgd_read_pkg(const char *path, unsigned char **out,
                         unsigned int *out_len)
{
    sys_stat_t st;
    unsigned char *buf;
    int h;
    int n;

    if (path == 0 || out == 0 || out_len == 0) return -1;
    if (fs_stat(path, &st) != 0 || st.type != 1U || st.size == 0U ||
        st.size > PKGD_MAX_PKG) {
        return -1;
    }
    buf = (unsigned char *)malloc((usize)st.size);
    if (buf == 0) return -1;
    h = fs_open(path, ULIB_FS_READ);
    if (h <= 0) { free(buf); return -1; }
    n = fs_read(h, buf, (int)st.size);
    (void)fs_close(h);
    if (n != (int)st.size) { free(buf); return -1; }

    *out = buf;
    *out_len = st.size;
    return 0;
}

static int pkgd_do_install(const char *path, char *detail, unsigned int dmax)
{
    unsigned char *pkg = 0;
    unsigned int len = 0;
    char info[192];
    char name[32];
    char version[16];
    char type[16];
    char program[64];
    char files_s[16];
    unsigned int files = 0;
    unsigned int i;
    int r;

    if (path == 0 || path[0] == '\0') {
        set_detail(detail, dmax, "bad package");
        return PKGD_E_FORMAT;
    }

    /* 1) ring3 自己读包文件（真实 .pkg 路径）；内建包目录的名字交给内核 */
    if (pkgd_read_pkg(path, &pkg, &len) != 0) {
        pkg = 0;
        len = 0;
    }

    /* 2) 内核原语：验签（APKG 结构/哈希/RSA）+ 策略 + staging */
    r = (len != 0U) ? sys_pkg(PKG_OP_SESSION_BEGIN, (int)pkg, (int)len)
                    : sys_pkg(PKG_OP_SESSION_BEGIN, (int)path, 0);
    if (r != PKGD_OK) {
        if (pkg != 0) free(pkg);
        detail_status(detail, dmax, r);
        return r;
    }
    if (pkg != 0) free(pkg);

    /* 3) 取本次安装会话的 manifest 摘要 */
    info[0] = '\0';
    if (sys_pkg(PKG_OP_SESSION_INFO, (int)info, (int)sizeof(info)) != 0 ||
        info_field(info, "name", name, sizeof(name)) != 0 ||
        info_field(info, "type", type, sizeof(type)) != 0 ||
        info_field(info, "files", files_s, sizeof(files_s)) != 0) {
        (void)sys_pkg(PKG_OP_SESSION_ABORT, 0, 0);
        set_detail(detail, dmax, "session info");
        return PKGD_E_IO;
    }
    if (info_field(info, "version", version, sizeof(version)) != 0) {
        version[0] = '\0';
    }
    if (info_field(info, "program", program, sizeof(program)) != 0) {
        program[0] = '\0';
    }
    for (i = 0; files_s[i] >= '0' && files_s[i] <= '9'; i++) {
        files = files * 10U + (unsigned int)(files_s[i] - '0');
    }

    /* 4) 逐文件落盘（内核原语写 staging + 复验哈希；ring3 负责顺序/回滚） */
    for (i = 0; i < files; i++) {
        (void)svc_heartbeat(pkgd_ipc_port);
        r = sys_pkg(PKG_OP_SESSION_STAGE, (int)i, 0);
        if (r != PKGD_OK) {
            (void)sys_pkg(PKG_OP_SESSION_ABORT, 0, 0);
            detail_status(detail, dmax, r);
            return r;
        }
    }

    /* 5) 原子 rename + 组件 DB（内核落盘原语） */
    r = sys_pkg(PKG_OP_SESSION_COMMIT, 0, 0);
    if (r != PKGD_OK) {
        (void)sys_pkg(PKG_OP_SESSION_ABORT, 0, 0);
        detail_status(detail, dmax, r);
        return r;
    }

    /* 6) ring3 通知 supervisor（legacy echo 回退），再让内核 exec 组件 ELF */
    if (strcmp(type, "service") == 0 && program[0] != '\0') {
        char cmd[80];
        unsigned int used = 0;

        cmd[0] = '\0';
        (void)append_str(cmd, sizeof(cmd), &used, "start:");
        (void)append_str(cmd, sizeof(cmd), &used, program);
        (void)notify_supervisor(cmd);
    }

    (void)svc_heartbeat(pkgd_ipc_port);
    r = sys_pkg(PKG_OP_SESSION_START, 0, 0);
    if (r != PKGD_OK) {
        detail_status(detail, dmax, r);
        return r;
    }

    printf("pkgd: ring3 installed %s-%s (%u file(s))\n", name, version, files);
    set_detail(detail, dmax, "ok");
    return PKGD_OK;
}

/* -------------------------------------------------------------------------
 * 卸载：策略/查 DB（内核） -> 通知 supervisor -> 停组件 + 删目录/DB
 * ------------------------------------------------------------------------- */
static int pkgd_do_remove(const char *name, char *detail, unsigned int dmax)
{
    char info[256];
    char type[16];
    char program[64];
    int r;

    if (name == 0 || name[0] == '\0') {
        set_detail(detail, dmax, "bad name");
        return PKGD_E_FORMAT;
    }
    r = sys_pkg(PKG_OP_REMOVE_BEGIN, (int)name, 0);
    if (r != PKGD_OK) {
        detail_status(detail, dmax, r);
        return r;
    }
    info[0] = '\0';
    (void)sys_pkg(PKG_OP_REMOVE_INFO, (int)info, (int)sizeof(info));
    if (info_field(info, "type", type, sizeof(type)) != 0) {
        type[0] = '\0';
    }
    if (info_field(info, "program", program, sizeof(program)) != 0) {
        program[0] = '\0';
    }
    if (strcmp(type, "service") == 0 && program[0] != '\0') {
        char cmd[80];
        unsigned int used = 0;

        cmd[0] = '\0';
        (void)append_str(cmd, sizeof(cmd), &used, "stop:");
        (void)append_str(cmd, sizeof(cmd), &used, program);
        (void)notify_supervisor(cmd);
    }
    (void)svc_heartbeat(pkgd_ipc_port);
    r = sys_pkg(PKG_OP_REMOVE_COMMIT, 0, 0);
    if (r != PKGD_OK) {
        (void)sys_pkg(PKG_OP_REMOVE_ABORT, 0, 0);
        detail_status(detail, dmax, r);
        return r;
    }
    printf("pkgd: ring3 removed %s\n", name);
    set_detail(detail, dmax, "ok");
    return PKGD_OK;
}

/* -------------------------------------------------------------------------
 * 请求分发 + 回复
 * ------------------------------------------------------------------------- */
static void send_reply(int ticket, unsigned int req_id, int status,
                       unsigned int count, const char *text,
                       unsigned int textlen)
{
    unsigned char payload[20U + 256U];

    if (textlen > 256U) textlen = 256U;
    wr32(payload + 0, PKGD_REPLY_MAGIC);
    wr32(payload + 4, req_id);
    wr32(payload + 8, (unsigned int)status);
    wr32(payload + 12, count);
    wr32(payload + 16, textlen);
    if (textlen != 0U && text != 0) {
        memcpy(payload + 20, text, textlen);
    }
    (void)port_reply(ticket, 2U, payload, (int)(20U + textlen));
}

static void handle_request(const unsigned char *frame, int frame_len)
{
    const unsigned char *payload;
    unsigned int plen;
    unsigned int magic, req_id, op, arglen;
    int ticket;
    char arg[PKGD_PATH_MAX + 1];
    char detail[PKGD_DETAIL_MAX];
    char list[1024];
    unsigned int count = 0;
    int status;

    if (frame == 0 || frame_len < (int)(PKGD_HDR_SIZE + 24U)) return;
    if (rd32(frame) != IPC_MAGIC) return;
    plen = rd32(frame + 12);
    if (plen < 24U || PKGD_HDR_SIZE + plen > (unsigned int)frame_len) return;
    payload = frame + PKGD_HDR_SIZE;

    magic = rd32(payload + 0);
    if (magic != PKGD_IPC_MAGIC) return;
    ticket = (int)rd32(payload + 4);
    req_id = rd32(payload + 8);
    op = rd32(payload + 12);
    arglen = rd32(payload + 20);
    if (arglen > PKGD_PATH_MAX || 24U + arglen > plen) return;
    memcpy(arg, payload + 24, arglen);
    arg[arglen] = '\0';

    detail[0] = '\0';
    list[0] = '\0';
    switch (op) {
    case PKGD_OP_LIST:
        status = pkgd_do_list(list, sizeof(list), &count);
        send_reply(ticket, req_id, status, count,
                   (status == PKGD_OK) ? list : status_text(status),
                   (status == PKGD_OK) ? (unsigned int)strlen(list)
                                       : (unsigned int)strlen(status_text(status)));
        return;
    case PKGD_OP_INSTALL:
        status = pkgd_do_install(arg, detail, sizeof(detail));
        break;
    case PKGD_OP_REMOVE:
        status = pkgd_do_remove(arg, detail, sizeof(detail));
        break;
    case PKGD_OP_VERIFY:
        status = sys_pkg(PKG_OP_VERIFY_RAW, (int)arg, 0);
        if (status == PKGD_OK) set_detail(detail, sizeof(detail), "ok");
        else detail_status(detail, sizeof(detail), status);
        break;
    default:
        status = PKGD_E_FORMAT;
        set_detail(detail, sizeof(detail), "usage: install|remove|list|verify");
        break;
    }
    printf("pkgd: %s -> %s\n", arg, detail);
    send_reply(ticket, req_id, status, 0, detail, (unsigned int)strlen(detail));
}

void _start(void)
{
    static const char manifest[] =
        "name=pkgd\n"
        "version=1.0.0\n"
        "type=service\n"
        "entry=pkgd\n"
        "requires=fsd\n"
        "capabilities=\n"
        "restart=always\n"
        "restart_limit=3\n"
        "uid=0\n";
    unsigned char frame[IPC_HDR_SIZE + IPC_PAYLOAD_MAX];

    pkgd_ipc_port = port_create(0);
    if (pkgd_ipc_port < 0) {
        printf("pkgd: ring3 port_create failed\n");
        syscall3(SYS_EXIT, 1, 0, 0);
        return;
    }
    if (svc_register(manifest, pkgd_ipc_port) != 0) {
        printf("pkgd: ring3 register FAILED (port=%d)\n", pkgd_ipc_port);
        syscall3(SYS_EXIT, 1, 0, 0);
        return;
    }

    /* 旧注册表兼容：service_connect("pkgd") 仍能看到服务（新版改走 IPC）。 */
    {
        int legacy = nport_create();

        if (legacy > 0) {
            if (nservice_register("pkgd", legacy) == 0) {
                printf("pkgd: service online port=%d\n", legacy);
            }
            /* 重启时旧条目可能还在：失败不影响新版 IPC 路径 */
        }
    }

    printf("pkgd: ring3 online (pid=%d port=%d)\n",
           pkgd_getpid(), pkgd_ipc_port);

    for (;;) {
        int n = port_recv(pkgd_ipc_port, frame, (int)sizeof(frame));

        if (n > 0) handle_request(frame, n);
        /* 心跳：SVCH 超时 5s，300ms 一次足够；长安装过程中也会补发。 */
        (void)svc_heartbeat(pkgd_ipc_port);
    }
}
