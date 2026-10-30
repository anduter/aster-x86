/* =============================================================================
 * ulib.c - 用户态最小 C 库
 *
 * 提供 printf / puts / malloc / 字符串函数，迁移期建立在 int 0x81 之上：
 *   - 输出走 SYS_WRITE（fd=1）；
 *   - malloc 走 SYS_BRK 扩展用户堆（0x40080000 起，向上增长）。
 * ============================================================================= */
#include "ulib.h"
#include "../kernel/include/libsvc.h"   /* P7 程序层 SDK（实现见本文件下半部分） */

int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

/* 内核返回 (OS_ABI_VERSION << 16) | OS_ABI_FEATURES；负数表示门不可用 */
int abi_version(void)
{
    return syscall3(SYS_ABI_VERSION, 0, 0, 0);
}

/* ---- IPC（ABI 1.5 预留 80..84） ---- */

static void ipc_hdr_put(unsigned char *f, unsigned int type,
                        unsigned int flags, unsigned int plen)
{
    struct ipc_msg_hdr *h = (struct ipc_msg_hdr *)f;

    h->magic = IPC_MAGIC;
    h->type = type;
    h->flags = (unsigned short)flags;
    h->ncaps = 0;
    h->len = plen;
}

int port_create(int flags)
{
    return syscall3(SYS_PORT_CREATE, flags, 0, 0);
}

int port_send_raw(int port, const void *msg, int len)
{
    return syscall3(SYS_PORT_SEND, port, (int)msg, len);
}

int port_send(int port, unsigned int type, unsigned int flags,
              const void *payload, int len)
{
    unsigned char frame[IPC_HDR_SIZE + IPC_PAYLOAD_MAX];

    if (len < 0 || len > IPC_PAYLOAD_MAX) return -1;
    if (len > 0 && payload == 0) return -1;
    ipc_hdr_put(frame, type, flags, (unsigned int)len);
    memcpy(frame + IPC_HDR_SIZE, payload, (usize)len);
    return port_send_raw(port, frame, IPC_HDR_SIZE + len);
}

int port_recv(int port, void *buf, int maxlen)
{
    return syscall3(SYS_PORT_RECV, port, (int)buf, maxlen);
}

int port_reply_raw(int ticket, const void *msg, int len)
{
    return syscall3(SYS_PORT_REPLY, ticket, (int)msg, len);
}

int port_reply(int ticket, unsigned int type, const void *payload, int len)
{
    unsigned char frame[IPC_HDR_SIZE + IPC_PAYLOAD_MAX];

    if (len < 0 || len > IPC_PAYLOAD_MAX) return -1;
    if (len > 0 && payload == 0) return -1;
    ipc_hdr_put(frame, type, 0, (unsigned int)len);
    memcpy(frame + IPC_HDR_SIZE, payload, (usize)len);
    return port_reply_raw(ticket, frame, IPC_HDR_SIZE + len);
}

int cap_grant(int port, int type, int id, int rights)
{
    int r;

    /* abi.h 注释只列 ebx/ecx/edx；rights 走 esi（0 = 默认 RW）。
     * 内核中断现场保存了 esi，见 svc/svc_syscall.c 的 SYS_CAP_GRANT。 */
    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(SYS_CAP_GRANT), "b"(port), "c"(type), "d"(id),
                        "S"(rights)
                      : "memory");
    return r;
}

int port_ticket(int port)
{
    return (syscall3(SYS_GETPID, 0, 0, 0) << 16) | (port & 0xFFFF);
}

/* ---- 服务注册表客户端（ABI 1.5：86..88 / 93） ---- */

int svc_register(const char *manifest, int port)
{
    return syscall3(SYS_SVC_REGISTER, (int)(usize)manifest, port, 0);
}

int svc_lookup(const char *name)
{
    if (name == 0) return -1;
    return syscall3(SYS_SVC_LOOKUP, (int)(usize)name, (int)strlen(name), 0);
}

int svc_heartbeat(int port)
{
    return syscall3(SYS_SVC_HEARTBEAT, port, 0, 0);
}

int svc_log_write(const char *tag, const char *msg)
{
    if (tag == 0 || msg == 0) return -1;
    return syscall3(SYS_LOG_WRITE, (int)(usize)tag, (int)(usize)msg,
                    (int)strlen(msg));
}

/* 每进程一个客户端端口（ABI 没有 PORT_CLOSE，见 docs/claims/ipc.md §7 #7）。
 * svc_call / svc_rpc / 程序层 SDK 共用，避免一个进程吃掉多个端口。 */
static int svc_cli_port(void)
{
    static int cli_port;
    static int cli_pid;
    int pid = syscall3(SYS_GETPID, 0, 0, 0);

    if (cli_port <= 0 || cli_pid != pid) {
        cli_port = port_create(0);
        cli_pid = pid;
        if (cli_port <= 0) {
            cli_port = 0;
            return -1;
        }
    }
    return cli_port;
}

int svc_call(int port, unsigned int type, const void *req, int reqlen,
             void *reply, int maxlen)
{
    /* 当前 ABI 没有 SYS_PORT_CLOSE，所以每进程缓存一个客户端端口。端口带上
     * 创建者的 pid：fork 后子进程不会复用父进程的端口。
     * 与 SDK 的 svc_rpc 共用同一个客户端端口（svc_cli_port）。 */
    unsigned char *frame;
    unsigned char *rbuf;
    struct ipc_msg_hdr *h;
    struct ipc_msg_hdr *rh;
    int total, sent, got, copy, cli;

    if (port <= 0 || reqlen < 0 || reqlen > IPC_PAYLOAD_MAX - 4) return -1;
    if (reqlen > 0 && req == 0) return -1;
    if (maxlen < 0 || (maxlen > 0 && reply == 0)) return -1;

    cli = svc_cli_port();
    if (cli <= 0) return -1;

    total = 4 + reqlen;
    frame = (unsigned char *)malloc((usize)(IPC_HDR_SIZE + total));
    rbuf = (unsigned char *)malloc((usize)(IPC_HDR_SIZE + (maxlen > 0 ? maxlen : 1)));
    if (frame == 0 || rbuf == 0) {
        free(frame);
        free(rbuf);
        return -1;
    }

    {
        int ticket = port_ticket(cli);

        memcpy(frame + IPC_HDR_SIZE, &ticket, 4);
    }
    if (reqlen > 0) memcpy(frame + IPC_HDR_SIZE + 4, req, (usize)reqlen);

    h = (struct ipc_msg_hdr *)frame;
    h->magic = IPC_MAGIC;
    h->type = type;
    h->flags = (unsigned short)IPC_F_NEED_REPLY;
    h->ncaps = 0;
    h->len = (unsigned int)total;

    sent = port_send_raw(port, frame, IPC_HDR_SIZE + total);
    free(frame);
    if (sent != 0) return sent;         /* -1 / -EAGAIN / -EINTR 原样上报 */

    got = port_recv(cli, rbuf, IPC_HDR_SIZE + (maxlen > 0 ? maxlen : 1));
    if (got < 0) {
        free(rbuf);
        return got;
    }
    if (got < IPC_HDR_SIZE) {
        free(rbuf);
        return -1;
    }
    rh = (struct ipc_msg_hdr *)rbuf;
    if (rh->magic != IPC_MAGIC) {
        free(rbuf);
        return -1;
    }
    copy = (int)rh->len;
    if (copy > got - IPC_HDR_SIZE) copy = got - IPC_HDR_SIZE;   /* 截断的帧 */
    if (copy > maxlen) copy = maxlen;
    if (copy > 0) memcpy(reply, rbuf + IPC_HDR_SIZE, (usize)copy);
    free(rbuf);
    return copy;
}

int svc_call_name(const char *name, unsigned int type, const void *req,
                  int reqlen, void *reply, int maxlen)
{
    int port;
    int rc;

    if (name == 0) return -1;
    port = svc_lookup(name);
    if (port < 0) return -1;
    rc = svc_call(port, type, req, reqlen, reply, maxlen);
    if (rc != -1) return rc;

    /* -1 通常是“服务重启、旧端口已回收”：重新 lookup 一次再试。 */
    port = svc_lookup(name);
    if (port < 0) return -1;
    return svc_call(port, type, req, reqlen, reply, maxlen);
}

/* =============================================================================
 * 程序层 SDK（P7，契约见 src/kernel/include/libsvc.h / docs/sdk.md）
 *
 *   name -> port 进程内缓存（SVC_CACHE_MAX 条） + 连接对象 + req_id 帧。
 *   没有时钟，timeout_ms 折算成 10ms 睡眠切片与 IPC 单次超时的重试次数。
 * ========================================================================== */

struct svc_cache_ent {
    char name[SVC_NAME_MAX];
    int port;
};

static struct svc_cache_ent svc_cache[SVC_CACHE_MAX];
static unsigned int svc_cache_next;
static int svc_err;

static void svc_set_err(int err)
{
    svc_err = err;
}

static int svc_name_ok(const char *name)
{
    usize n;

    if (name == 0) return 0;
    n = strlen(name);
    return (n > 0U && n < (usize)SVC_NAME_MAX) ? 1 : 0;
}

static void svc_sleep_ms(int ms)
{
    if (ms > 0) (void)syscall3(SYS_SLEEP, ms, 0, 0);
}

static struct svc_cache_ent *svc_cache_find(const char *name)
{
    int i;

    for (i = 0; i < SVC_CACHE_MAX; i++) {
        if (svc_cache[i].port > 0 && strcmp(svc_cache[i].name, name) == 0) {
            return &svc_cache[i];
        }
    }
    return 0;
}

static void svc_cache_store(const char *name, int port)
{
    struct svc_cache_ent *e = svc_cache_find(name);
    usize n;

    if (e == 0) {
        int i;

        for (i = 0; i < SVC_CACHE_MAX; i++) {
            if (svc_cache[i].port <= 0) { e = &svc_cache[i]; break; }
        }
        if (e == 0) {
            e = &svc_cache[svc_cache_next % SVC_CACHE_MAX];
            svc_cache_next++;
        }
    }
    n = strlen(name);
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, n);
    e->name[n] = '\0';
    e->port = port;
}

int svc_cache_invalidate(const char *name)
{
    int i, n = 0;

    if (name == 0) return 0;
    for (i = 0; i < SVC_CACHE_MAX; i++) {
        if (svc_cache[i].port > 0 && strcmp(svc_cache[i].name, name) == 0) {
            svc_cache[i].port = 0;
            svc_cache[i].name[0] = '\0';
            n++;
        }
    }
    return n;
}

int svc_lookup_cached(const char *name)
{
    struct svc_cache_ent *e;
    int port;

    svc_set_err(SVC_OK);
    if (!svc_name_ok(name)) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    e = svc_cache_find(name);
    if (e != 0) return e->port;

    port = svc_lookup(name);
    if (port <= 0) {
        svc_cache_invalidate(name);
        svc_set_err(SVC_ERR_NOENT);
        return SVC_ERR_NOENT;
    }
    svc_cache_store(name, port);
    return port;
}

struct svc_conn *svc_connect(const char *name, int timeout_ms)
{
    struct svc_conn *c;
    int budget, port;

    svc_set_err(SVC_OK);
    if (!svc_name_ok(name)) {
        svc_set_err(SVC_ERR_IO);
        return 0;
    }

    /* 连一次就拿最新端口：先让缓存失效，避免连到已被回收的旧句柄。 */
    svc_cache_invalidate(name);
    budget = (timeout_ms > 0) ? timeout_ms : 0;
    for (;;) {
        port = svc_lookup(name);
        if (port > 0) break;
        if (budget <= 0) {
            svc_set_err(SVC_ERR_NOENT);
            return 0;
        }
        if (budget > 10) {
            svc_sleep_ms(10);
            budget -= 10;
        } else {
            svc_sleep_ms(budget);
            budget = 0;
        }
    }

    c = (struct svc_conn *)malloc(sizeof(*c));
    if (c == 0) {
        svc_set_err(SVC_ERR_IO);
        return 0;
    }
    memset(c, 0, sizeof(*c));
    c->port = port;
    c->pid = syscall3(SYS_GETPID, 0, 0, 0);
    c->timeout_ms = (timeout_ms > 0) ? timeout_ms : SVC_RPC_TIMEOUT_MS;
    c->last_err = SVC_OK;
    c->req_id = 1;
    strcpy(c->name, name);
    svc_cache_store(name, port);
    return c;
}

int svc_close(struct svc_conn *conn)
{
    svc_set_err(SVC_OK);
    if (conn == 0) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    free(conn);
    return SVC_OK;
}

int svc_reconnect(struct svc_conn *conn)
{
    int port;

    svc_set_err(SVC_OK);
    if (conn == 0 || conn->name[0] == '\0') {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    svc_cache_invalidate(conn->name);
    port = svc_lookup(conn->name);
    if (port <= 0) {
        conn->last_err = SVC_ERR_NOENT;
        svc_set_err(SVC_ERR_NOENT);
        return SVC_ERR_NOENT;
    }
    conn->port = port;
    conn->pid = syscall3(SYS_GETPID, 0, 0, 0);
    conn->req_id = 1;
    conn->last_err = SVC_OK;
    svc_cache_store(conn->name, port);
    return SVC_OK;
}

static unsigned int svc_get32(const unsigned char *p)
{
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static void svc_put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
    p[2] = (unsigned char)((v >> 16) & 0xFFU);
    p[3] = (unsigned char)((v >> 24) & 0xFFU);
}

/* timeout_ms 折算成“最多试几次”：SEND 单次内核超时 200ms，RECV 300ms。
 * 上限 4 次，避免失控的长等待；每次失败本身已经消耗了对应的内核超时。 */
static int svc_tries(int timeout_ms, int per_ms)
{
    int t = 1 + (timeout_ms > 0 ? timeout_ms / per_ms : 0);

    if (t > 4) t = 4;
    if (t < 1) t = 1;
    return t;
}

int svc_rpc(struct svc_conn *conn, const void *req, int len,
            void *reply, int maxlen)
{
    /* 每进程一份收发缓冲（bump 堆不回收，不能每次 malloc）。非可重入。 */
    static unsigned char send_buf[IPC_HDR_SIZE + SVC_FRAME_MAX];
    static unsigned char recv_buf[IPC_HDR_SIZE + SVC_FRAME_MAX];
    unsigned char *payload;
    struct ipc_msg_hdr *h;
    int cli, me, frame_len, sent, got, tries, t, copy, mine, repaired;

    svc_set_err(SVC_OK);
    if (conn == 0 || conn->port <= 0) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    if (len < 0 || len > SVC_REQ_DATA_MAX) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    if (len > 0 && req == 0) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }
    if (maxlen < 0 || maxlen > SVC_REPLY_DATA_MAX ||
        (maxlen > 0 && reply == 0)) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }

    me = syscall3(SYS_GETPID, 0, 0, 0);
    if (conn->pid != me) {
        /* fork 后子进程不能复用父进程的端口/ticket：先重连。 */
        if (svc_reconnect(conn) != SVC_OK) return svc_err;
    }
    cli = svc_cli_port();
    if (cli <= 0) {
        svc_set_err(SVC_ERR_IO);
        return SVC_ERR_IO;
    }

    frame_len = IPC_HDR_SIZE + SVC_REQ_HDR_SIZE + len;
    h = (struct ipc_msg_hdr *)send_buf;
    h->magic = IPC_MAGIC;
    h->type = (unsigned int)SVC_TYPE_RPC;
    h->flags = (unsigned short)IPC_F_NEED_REPLY;
    h->ncaps = 0;
    h->len = (unsigned int)(SVC_REQ_HDR_SIZE + len);
    payload = send_buf + IPC_HDR_SIZE;
    svc_put32(payload, (unsigned int)port_ticket(cli));
    svc_put32(payload + 4, conn->req_id);
    if (len > 0) memcpy(payload + SVC_REQ_HDR_SIZE, req, (usize)len);

    /* ---- 发送（队列满/被信号打断按预算重试；端口失效重连一次） ---- */
    repaired = 0;
    tries = svc_tries(conn->timeout_ms, 200);
    sent = -1;
    for (t = 0; t < tries; t++) {
        sent = port_send_raw(conn->port, send_buf, frame_len);
        if (sent == 0) break;
        if (sent == -IPC_EAGAIN || sent == -IPC_EINTR) continue;
        /* 其它负值 = 端口句柄失效（服务重启/回收）：重新 lookup 再发。 */
        if (repaired) break;
        repaired = 1;
        if (svc_reconnect(conn) != SVC_OK) return svc_err;
        svc_put32(payload + 4, conn->req_id);
        t--;
    }
    if (sent != 0) {
        int e = (sent == -IPC_EAGAIN || sent == -IPC_EINTR)
                    ? SVC_ERR_TIMEOUT : SVC_ERR_IO;

        conn->last_err = e;
        svc_set_err(e);
        return e;
    }

    /* ---- 收回复（丢弃 req_id 不匹配的迟到回复；端口失效重连重发） ---- */
    tries = svc_tries(conn->timeout_ms, 300);
    for (t = 0; t < tries; t++) {
        got = port_recv(cli, recv_buf, (int)sizeof(recv_buf));
        if (got == -IPC_EAGAIN || got == -IPC_EINTR) continue;
        if (got < 0) {
            if (repaired) break;
            repaired = 1;
            if (svc_reconnect(conn) != SVC_OK) return svc_err;
            svc_put32(payload + 4, conn->req_id);
            if (port_send_raw(conn->port, send_buf, frame_len) != 0) break;
            t--;
            continue;
        }
        if (got < IPC_HDR_SIZE) continue;
        h = (struct ipc_msg_hdr *)recv_buf;
        if (h->magic != IPC_MAGIC) continue;
        if (h->len < SVC_REPLY_HDR_SIZE) continue;
        copy = (int)h->len - SVC_REPLY_HDR_SIZE;
        if (copy > got - IPC_HDR_SIZE - SVC_REPLY_HDR_SIZE) {
            copy = got - IPC_HDR_SIZE - SVC_REPLY_HDR_SIZE;
        }
        if (copy < 0) copy = 0;
        mine = (int)svc_get32(recv_buf + IPC_HDR_SIZE);
        if (mine != (int)conn->req_id) continue;    /* 迟到/错配：丢弃 */
        if (copy > maxlen) copy = maxlen;
        if (copy > 0) memcpy(reply, recv_buf + IPC_HDR_SIZE + SVC_REPLY_HDR_SIZE,
                             (usize)copy);
        conn->last_err = SVC_OK;
        return copy;
    }

    conn->last_err = SVC_ERR_TIMEOUT;
    svc_set_err(SVC_ERR_TIMEOUT);
    return SVC_ERR_TIMEOUT;
}

int svc_last_error(void)
{
    return svc_err;
}

const char *svc_strerror(int err)
{
    switch (err) {
    case SVC_OK:          return "ok";
    case SVC_ERR_NOENT:   return "no such service";
    case SVC_ERR_TIMEOUT: return "timed out";
    case SVC_ERR_IO:      return "io error";
    case SVC_ERR_DENIED:  return "permission denied";
    default:              return "unknown error";
    }
}

static usize utoa(unsigned int v, char *buf, unsigned int base)
{
    char tmp[16];
    usize n = 0, i = 0;

    if (v == 0U) {
        buf[0] = '0';
        return 1;
    }
    while (v > 0U) {
        unsigned int d = v % base;
        tmp[n++] = (d < 10U) ? (char)('0' + d) : (char)('a' + d - 10U);
        v /= base;
    }
    while (n > 0U) buf[i++] = tmp[--n];
    return i;
}

int fd_read(int fd, void *buf, int len)
{
    return syscall3(SYS_FREAD, fd, (int)buf, len);
}

int fd_write(int fd, const void *buf, int len)
{
    return syscall3(SYS_FWRITE, fd, (int)buf, len);
}

/* ---- 路径文件 API（P4 ring3 服务用） ---- */

int fs_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (int)(usize)path, flags, 0);
}

int fs_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}

int fs_read(int fd, void *buf, int len)
{
    return syscall3(SYS_FREAD, fd, (int)(usize)buf, len);
}

int fs_write(int fd, const void *buf, int len)
{
    return syscall3(SYS_FWRITE, fd, (int)(usize)buf, len);
}

int fs_stat(const char *path, sys_stat_t *st)
{
    return syscall3(SYS_STAT, (int)(usize)path, (int)(usize)st, 0);
}

int fs_unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (int)(usize)path, 0, 0);
}

int fs_mkdir(const char *path)
{
    return syscall3(SYS_MKDIR, (int)(usize)path, 0, 0);
}

/* ---- Native ABI（int 0x80）：端点/通道 ------------------------------------
 * pkgd 通过它把 "start:/stop:<program>" 通知给 supervisor（服务层之间走
 * native 端口，不经过内建函数）。参数约定见 proc/trap/native.h。 */
static int native3(int nr, int a, int b, int c)
{
    int r;

    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

int nport_create(void)
{
    return native3(0x33, 0, 0, 0);              /* NATIVE_SYS_PORT_CREATE */
}

int nservice_register(const char *name, int port)
{
    return native3(0x38, (int)(usize)name, (int)strlen(name), port);
}

int nservice_connect(const char *name)
{
    return native3(0x39, (int)(usize)name, (int)strlen(name), 0);
}

int nchan_send(int ch, const void *buf, int len)
{
    return native3(0x31, ch, (int)(usize)buf, len);
}

int nchan_recv_block(int ch, void *buf, int max)
{
    return native3(0x36, ch, (int)(usize)buf, max);
}

int nhandle_close(int handle)
{
    return native3(0x10, handle, 0, 0);         /* NATIVE_SYS_HANDLE_CLOSE */
}

/* ---- 进程/内存（ABI v1.1，pm 会话） ---- */

int waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, pid, (int)status, options);
}

int getppid(void)
{
    return syscall3(SYS_GETPPID, 0, 0, 0);
}

int mprotect(void *addr, unsigned int len, int prot)
{
    return syscall3(SYS_MPROTECT, (int)(usize)addr, (int)len, prot);
}

int signal(int sig, unsigned int handler)
{
    return syscall3(SYS_SIGACTION, sig, (int)handler, 0);
}

/* ---- 控制台（ABI v1.2）：输入框下划线光标显示/隐藏 ---- */
int console_cursor(int visible)
{
    return syscall3(SYS_CONSOLE_CURSOR, visible, 0, 0);
}

int putchar(int c)
{
    char ch = (char)c;
    fd_write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    usize n = strlen(s);
    fd_write(1, s, (int)n);
    putchar('\n');
    return (int)n + 1;
}

/* 把 tmp 里的字符串按 width/zero 填充后追加到 out */
static int append_padded(char *out, int n, const char *tmp, int len,
                         int width, int zero)
{
    int pad = (width > len) ? (width - len) : 0;

    for (int i = 0; i < pad && n < 240; i++) out[n++] = zero ? '0' : ' ';
    for (int i = 0; i < len && n < 240; i++) out[n++] = tmp[i];
    return n;
}

int printf(const char *fmt, ...)
{
    char out[256];
    int n = 0;
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);

    for (; *fmt != '\0' && n < 240; fmt++) {
        int zero = 0, width = 0;
        char tmp[40];
        int len;

        if (*fmt != '%') {
            out[n++] = *fmt;
            continue;
        }
        fmt++;

        /* 解析 "%0Nd" 里的 '0' 标志和宽度 */
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            len = 0;
            while (s[len] != '\0' && len < (int)sizeof(tmp)) len++;
            for (int i = 0; i < len && n < 240; i++) out[n++] = s[i];
            break;
        }
        case 'd': {
            int v = __builtin_va_arg(ap, int);
            unsigned int uv;
            int neg = 0;

            if (v < 0) { neg = 1; uv = (unsigned int)(-v); } else { uv = (unsigned int)v; }
            len = (int)utoa(uv, tmp, 10U);
            if (neg) {
                if (len + 1 < (int)sizeof(tmp)) {
                    for (int i = len; i > 0; i--) tmp[i] = tmp[i - 1];
                    tmp[0] = '-';
                    len++;
                }
            }
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'u': {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            len = (int)utoa(v, tmp, 10U);
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'x': {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            len = (int)utoa(v, tmp, 16U);
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'o': {
            unsigned int v = __builtin_va_arg(ap, unsigned int);
            len = (int)utoa(v, tmp, 8U);
            n = append_padded(out, n, tmp, len, width, zero);
            break;
        }
        case 'c': {
            tmp[0] = (char)__builtin_va_arg(ap, int);
            n = append_padded(out, n, tmp, 1, width, zero);
            break;
        }
        case 'p': {
            unsigned int v = (unsigned int)(usize)__builtin_va_arg(ap, void *);
            out[n++] = '0';
            out[n++] = 'x';
            len = (int)utoa(v, tmp, 16U);
            for (int i = 0; i < len && n < 240; i++) out[n++] = tmp[i];
            break;
        }
        default:
            out[n++] = '%';
            if (*fmt != '\0') out[n++] = *fmt;
            break;
        }
    }

    __builtin_va_end(ap);

    syscall3(SYS_FWRITE, 1, (int)out, n);
    return n;
}

/* ---- 堆：向上增长的 bump 分配器，free 是空操作 ---- */
static char *heap_ptr;
static char *heap_end;

void *malloc(usize n)
{
    char *p;
    int brk;

    n = (n + 7U) & ~7U;
    if (n == 0U) n = 8U;

    if (heap_ptr == 0) {
        brk = syscall3(SYS_BRK, 0, 0, 0);
        if (brk < 0) return 0;
        heap_ptr = (char *)(usize)brk;
        heap_end = heap_ptr;
    }

    if (heap_ptr + n > heap_end) {
        brk = syscall3(SYS_BRK, (int)n, 0, 0);
        if (brk < 0) return 0;
        heap_end = (char *)(usize)brk;
    }

    p = heap_ptr;
    heap_ptr += n;
    return p;
}

void free(void *p)
{
    (void)p;            /* bump 分配器不回收 */
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    while (n > 0U && *a != '\0' && *a == *b) { a++; b++; n--; }
    if (n == 0U) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
    char *p = d;
    while ((*d++ = *s++) != '\0') { }
    return p;
}

void *memcpy(void *d, const void *s, usize n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n-- > 0U) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, usize n)
{
    unsigned char *dp = (unsigned char *)d;
    while (n-- > 0U) *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, usize n)
{
    const unsigned char *ap = (const unsigned char *)a;
    const unsigned char *bp = (const unsigned char *)b;

    while (n-- > 0U) {
        if (*ap != *bp) return (int)*ap - (int)*bp;
        ap++;
        bp++;
    }
    return 0;
}
