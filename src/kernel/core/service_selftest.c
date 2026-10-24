/* =============================================================================
 * core/service_selftest.c - 服务/IPC 生命周期的泄漏自检
 *
 * 为什么需要：port 只有 4 个槽、channel 只有 8 个槽。connect/accept/send/recv
 * 任何一处漏掉 release，都会在若干轮之后把槽耗尽，表现为"服务突然连不上"。
 * 功能测试（native: service ok）只跑一轮，抓不到这种泄漏，所以这里循环多轮，
 * 每轮结束把两端都释放掉；跑完还能再建一个新连接，就说明没有耗尽。
 * ============================================================================= */
#include "service.h"
#include "error.h"
#include "../ipc/port.h"
#include "../ipc/channel.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"

#define SELFTEST_NAME   "selftest"
#define SELFTEST_CYCLES 200U

static const char selftest_msg[] = "ping";

void service_lifecycle_selftest(void)
{
    int port;
    uint32_t i;

    port = port_create();
    if (port < 0) {
        kprintln("service: lifecycle FAILED (no port)");
        return;
    }
    if (service_register(SELFTEST_NAME, (uint32_t)port) != E_OK) {
        kprintln("service: lifecycle FAILED (register)");
        port_release((uint32_t)port);
        return;
    }

    for (i = 0; i < SELFTEST_CYCLES; i++) {
        uint32_t client = 0;
        uint32_t server = 0;
        char buf[16];
        int n;

        if (service_connect(SELFTEST_NAME, &client) != 0) {
            kprintf("service: lifecycle FAILED at cycle %u (connect)\n", i);
            break;
        }
        if (port_accept((uint32_t)port, &server) != 0) {
            kprintf("service: lifecycle FAILED at cycle %u (accept)\n", i);
            chan_release(client);
            break;
        }
        if (chan_send(server, selftest_msg, sizeof(selftest_msg) - 1U) <= 0) {
            kprintf("service: lifecycle FAILED at cycle %u (send)\n", i);
            chan_release(client);
            chan_release(server);
            break;
        }
        n = chan_recv(client, buf, sizeof(buf));
        if (n != (int)(sizeof(selftest_msg) - 1U) ||
            memcmp(buf, selftest_msg, sizeof(selftest_msg) - 1U) != 0) {
            kprintf("service: lifecycle FAILED at cycle %u (recv=%d)\n", i, n);
            chan_release(client);
            chan_release(server);
            break;
        }
        chan_release(client);
        chan_release(server);
    }

    if (i == SELFTEST_CYCLES) {
        /* 槽真的都还回来了吗？再建一个 port、再走一轮 connect/accept 确认没耗尽 */
        int p2 = port_create();
        uint32_t client = 0;
        uint32_t server = 0;

        if (p2 >= 0) port_release((uint32_t)p2);

        if (service_connect(SELFTEST_NAME, &client) == 0) {
            if (port_accept((uint32_t)port, &server) == 0) {
                chan_release(server);
            }
            chan_release(client);
        } else {
            kprintf("service: lifecycle FAILED (slots exhausted)\n");
            i = 0;
        }
    }

    service_unregister((uint32_t)port);
    port_release((uint32_t)port);

    if (i == SELFTEST_CYCLES) {
        kprintf("service: lifecycle ok (%u cycles)\n", SELFTEST_CYCLES);
    }
}
