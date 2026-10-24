/* =============================================================================
 * apps/echosvc.c - 可被 supervisor 自动拉起的 echo 服务
 * ============================================================================= */
#include "echosvc.h"
#include "../ipc/port.h"
#include "../ipc/channel.h"
#include "../core/service.h"
#include "../core/error.h"
#include "../lib/kprintf.h"

void echosvc_entry(void)
{
    int port = port_create();

    if (port < 0) {
        kprintln("echosvc: no port");
        return;
    }
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
        n = chan_recv_block(server, msg, sizeof(msg));
        if (n > 0) (void)chan_send(server, msg, (uint32_t)n);
        chan_release(server);
    }
}
