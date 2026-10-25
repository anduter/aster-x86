/* =============================================================================
 * apps/echosvc.c - 可被 supervisor 自动拉起的 echo 服务
 * ============================================================================= */
#include "echosvc.h"
#include "include/svc.h"
#include "include/svc.h"
#include "include/svc.h"
#include "include/kernel.h"
#include "include/kernel.h"
#include "include/proc.h"

static int svc_port = -1;
static volatile int svc_stop;

void echosvc_request_stop(void)
{
    svc_stop = 1;
    if (svc_port >= 0) {
        uint32_t client = 0;
        /* 用一个 dummy 连接唤醒阻塞在 accept 的服务线程。 */
        (void)port_connect((uint32_t)svc_port, &client);
        if (client != 0U) chan_release(client);
    }
}

void echosvc_entry(void)
{
    int port = port_create();

    if (port < 0) {
        kprintln("echosvc: no port");
        return;
    }
    svc_port = port;
    svc_stop = 0;
    if (service_register("echo", (uint32_t)port) != E_OK) {
        kprintln("echosvc: echo already registered");
        return;
    }
    kprintf("echosvc: online port=%d\n", port);

    for (;;) {
        uint32_t server = 0;
        uint8_t msg[CHAN_MSG_LEN];
        int n;

        if (port_accept_block((uint32_t)port, &server) != 0) continue;
        if (svc_stop) {
            chan_release(server);
            break;
        }
        n = chan_recv_block(server, msg, sizeof(msg));
        if (n > 0) (void)chan_send(server, msg, (uint32_t)n);
        chan_release(server);
    }

    service_unregister((uint32_t)port);
    port_release((uint32_t)port);
    svc_port = -1;
    kprintln("echosvc: stopped");
    proc_exit(0);
}
