/* =============================================================================
 * demoapp.c - 程序层 SDK 示例（P7）
 *
 * 演示"新服务/app 只需 manifest + 入口"：
 *   - demo_server_loop()：ring3 演示服务，创建端口 → SYS_SVC_REGISTER
 *     （name=echosvc, restart=never）→ 收一帧回一帧 → 心跳。
 *   - demoapp_main()：SDK 客户端，svc_connect("echosvc") → svc_rpc("ping")
 *     → 收到 "pong" → 打印 `sdk: demo ok`；失败打印 SVC_ERR_* 的友好错误。
 *   - demoapp_selftest()：-Test 用，覆盖 connect/rpc/noent/reconnect 与 demo，
 *     输出 docs/claims/sdk.md 里的 `sdk: ...` 自检行。
 *
 * 帧约定见 src/kernel/include/libsvc.h §1：
 *   请求 payload = u32 ticket | u32 req_id | req[]
 *   回复 payload = u32 req_id | reply[]
 *
 * 构建（build.ps1）：
 *   demoapp.o -DDEMOAPP_EMBEDDED 链接进 user_shell.elf（shell 内置 `svc-demo`）
 *   demoapp.elf                  独立 ELF（同一份源码，正常 main）
 * ============================================================================= */
#include "ulib.h"
#include "demoapp.h"
#include "../kernel/include/libsvc.h"

#define DEMO_SVC_NAME "echosvc"

/* 演示服务的 manifest：换一个名字/入口就是"新服务"，客户端代码不用动。
 * restart=never：演示进程退出后由 svcwatch 标 stopped，不自动拉起。 */
static const char demo_manifest[] =
    "name=" DEMO_SVC_NAME "\n"
    "version=1.0.0\n"
    "type=service\n"
    "entry=" DEMO_SVC_NAME "\n"
    "restart=never\n"
    "restart_limit=0\n"
    "uid=0\n";

/* demoapp 会被链接进 shell（没有 shell 内部的 putstr），这里用 printf 输出。 */
static void demo_puts(const char *s)
{
    printf("%s", s);
}

static void demo_exit(int code)
{
    (void)syscall3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

static unsigned int demo_get32(const unsigned char *p)
{
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static void demo_put32(unsigned char *p, int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* 演示服务主循环：收到 "ping" 回 "pong"，其它原样回显。
 * 每轮刷一次心跳，避免 svcwatch 把演示服务判成 UNHEALTHY。 */
static void demo_server_loop(int port)
{
    unsigned char in[IPC_HDR_SIZE + SVC_REQ_HDR_SIZE + 128];
    unsigned char out[SVC_REPLY_HDR_SIZE + 128];

    for (;;) {
        struct ipc_msg_hdr *h;
        int n = port_recv(port, in, (int)sizeof(in));

        if (n == -IPC_EAGAIN) {
            if (svc_heartbeat(port) != 0) break;
            continue;
        }
        if (n < 0) break;                       /* 端口/注册表失效：退出 */
        if (n >= IPC_HDR_SIZE + SVC_REQ_HDR_SIZE) {
            h = (struct ipc_msg_hdr *)in;
            if (h->magic == IPC_MAGIC && h->len >= SVC_REQ_HDR_SIZE &&
                (int)h->len <= n - IPC_HDR_SIZE) {
                int ticket = (int)demo_get32(in + IPC_HDR_SIZE);
                int req_id = (int)demo_get32(in + IPC_HDR_SIZE + 4);
                int dlen = (int)h->len - SVC_REQ_HDR_SIZE;
                unsigned char *data = in + IPC_HDR_SIZE + SVC_REQ_HDR_SIZE;

                if (dlen > (int)sizeof(out) - SVC_REPLY_HDR_SIZE) {
                    dlen = (int)sizeof(out) - SVC_REPLY_HDR_SIZE;
                }
                demo_put32(out, req_id);
                if (dlen == 4 && memcmp(data, "ping", 4) == 0) {
                    memcpy(out + SVC_REPLY_HDR_SIZE, "pong", 4);
                    dlen = 4;
                } else if (dlen > 0) {
                    memcpy(out + SVC_REPLY_HDR_SIZE, data, (usize)dlen);
                }
                (void)port_reply(ticket, SVC_TYPE_RPC, out,
                                 SVC_REPLY_HDR_SIZE + dlen);
            }
        }
        if (svc_heartbeat(port) != 0) break;
    }
}

/* fork 一个演示服务并等它注册完成：0 = 服务已在线（子进程继续跑），-1 = 失败。
 * 用 pipe 同步，避免"父进程 lookup 得比子进程注册更早"的竞态。 */
static int demo_start_server(void)
{
    int fds[2];
    int pid, r;
    char status = '0';

    if (syscall3(SYS_PIPE, (int)(usize)fds, 0, 0) != 0) return -1;
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        (void)syscall3(SYS_CLOSE, fds[0], 0, 0);
        (void)syscall3(SYS_CLOSE, fds[1], 0, 0);
        return -1;
    }
    if (pid == 0) {
        int port;
        char ok = '0';

        (void)syscall3(SYS_CLOSE, fds[0], 0, 0);
        port = port_create(0);
        if (port > 0 && svc_register(demo_manifest, port) == 0) {
            ok = '1';
            (void)syscall3(SYS_FWRITE, fds[1], (int)(usize)&ok, 1);
            (void)syscall3(SYS_CLOSE, fds[1], 0, 0);
            demo_server_loop(port);
            demo_exit(0);
        }
        (void)syscall3(SYS_FWRITE, fds[1], (int)(usize)&ok, 1);
        (void)syscall3(SYS_CLOSE, fds[1], 0, 0);
        demo_exit(1);
    }

    (void)syscall3(SYS_CLOSE, fds[1], 0, 0);
    r = (int)syscall3(SYS_FREAD, fds[0], (int)(usize)&status, 1);
    (void)syscall3(SYS_CLOSE, fds[0], 0, 0);
    if (r != 1 || status != '1') {
        int st = -1;

        (void)waitpid(pid, &st, 0);
        return -1;
    }
    return 0;                                   /* 服务子进程常驻 */
}

/* 示例客户端：连 echosvc → ping → pong → 打一条 tag 日志。
 * 返回 0 成功；1 失败（已打印友好错误，含 SVC_ERR_* 文本）。 */
int demoapp_main(void)
{
    struct svc_conn *conn;
    char reply[32];
    int n, err;

    conn = svc_connect(DEMO_SVC_NAME, 100);
    if (conn == 0 && svc_last_error() == SVC_ERR_NOENT) {
        /* 没有服务就起一个本地演示服务（把 manifest + 入口一起演示）。 */
        if (demo_start_server() == 0) {
            conn = svc_connect(DEMO_SVC_NAME, SVC_CONNECT_TIMEOUT_MS);
        }
    }
    if (conn == 0) {
        err = svc_last_error();
        printf("sdk: demo error: %s (%s)\n", svc_strerror(err), DEMO_SVC_NAME);
        if (err == SVC_ERR_NOENT) {
            demo_puts("sdk: demo hint: 服务未注册；root 下本 demo 会自动起本地服务\n");
        }
        return 1;
    }

    n = svc_rpc(conn, "ping", 4, reply, (int)sizeof(reply));
    if (n != 4 || memcmp(reply, "pong", 4) != 0) {
        printf("sdk: demo FAIL (rpc=%d err=%s)\n", n,
               svc_strerror(svc_last_error()));
        (void)svc_close(conn);
        return 1;
    }

    (void)svc_log_write("sdk", "demo client ok");
    printf("sdk: demo ok (svc=%s port=%d reply=pong)\n",
           DEMO_SVC_NAME, conn->port);
    (void)svc_close(conn);
    return 0;
}

/* -Test 自检：所有 `sdk: ...` 行都在这里产出（docs/claims/sdk.md §3）。 */
int demoapp_selftest(void)
{
    struct svc_conn *conn;
    struct svc_conn *miss;
    char reply[32];
    int ok = 1;
    int n;

    /* 1) 演示 app：起服务 + ping/pong；demoapp_main 内部打印 sdk: demo ok */
    if (demoapp_main() == 0) {
        demo_puts("sdk: demo app ok\n");
    } else {
        demo_puts("sdk: demo app FAIL\n");
        ok = 0;
    }

    /* 2) SDK connect + RPC（服务已在线） */
    conn = svc_connect(DEMO_SVC_NAME, SVC_CONNECT_TIMEOUT_MS);
    if (conn != 0) {
        demo_puts("sdk: svc connect ok\n");
    } else {
        printf("sdk: svc connect FAIL (%s)\n", svc_strerror(svc_last_error()));
        ok = 0;
    }
    if (conn != 0) {
        n = svc_rpc(conn, "ping", 4, reply, (int)sizeof(reply));
        if (n == 4 && memcmp(reply, "pong", 4) == 0) {
            demo_puts("sdk: svc rpc ok\n");
        } else {
            printf("sdk: svc rpc FAIL (rpc=%d err=%s)\n", n,
                   svc_strerror(svc_last_error()));
            ok = 0;
        }
    }

    /* 3) 服务不存在：svc_connect 超时后返回 SVC_ERR_NOENT */
    miss = svc_connect("nosuchsvc", 40);
    if (miss == 0 && svc_last_error() == SVC_ERR_NOENT) {
        demo_puts("sdk: svc noent ok\n");
    } else {
        printf("sdk: svc noent FAIL (conn=%d err=%s)\n",
               (miss != 0), svc_strerror(svc_last_error()));
        if (miss != 0) (void)svc_close(miss);
        ok = 0;
    }

    /* 4) 重连：把连接上的端口弄成失效句柄 → svc_rpc 自动重查并重发。
     *    真实服务重启在单次 -Test 里不好等（restart=never），这里等价地
     *    模拟"旧端口已被回收"；再显式 svc_reconnect 校验一次。 */
    if (conn != 0) {
        conn->port = 0x7FFF;
        n = svc_rpc(conn, "ping", 4, reply, (int)sizeof(reply));
        if (n == 4 && memcmp(reply, "pong", 4) == 0 &&
            svc_reconnect(conn) == SVC_OK) {
            demo_puts("sdk: svc reconnect ok\n");
        } else {
            printf("sdk: svc reconnect FAIL (rpc=%d err=%s)\n", n,
                   svc_strerror(svc_last_error()));
            ok = 0;
        }
    }

    if (conn != 0) (void)svc_close(conn);
    if (ok) {
        demo_puts("sdk: selftest PASS\n");
        return 0;
    }
    demo_puts("sdk: selftest FAIL\n");
    return 1;
}

#ifndef DEMOAPP_EMBEDDED
/* 独立 ELF 的入口：内核 loader 跳到 ELF entry（见 user.ld / pkgd.c 的 _start
 * 约定），没有 argv；返回值作为进程退出码。 */
void _start(void)
{
    (void)syscall3(SYS_EXIT, demoapp_main(), 0, 0);
    for (;;) { }
}
#endif
