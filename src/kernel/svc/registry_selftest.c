/* =============================================================================
 * svc/registry_selftest.c - P3 服务注册/拓扑/心跳/重启/隔离自检
 *
 * 在 -Test 构建、调度器启动后的 supervisor 线程里跑一次（supervisor_entry）。
 * 自检尽量走真实路径：
 *   - SYS_SVC_REGISTER/LOOKUP/HEARTBEAT/LOG_WRITE 用 int 0x81 触发（内核线程的
 *     静态缓冲在 3GB 以下，uaccess 的“内核上下文例外”允许它当用户指针用，
 *     与 klog 的 SYS_DMESG 自检同一手法）；
 *   - 重启/隔离用真实的 proc_spawn 与确定的 now 驱动 svc_supervise_step()；
 *   - 自检期间暂停 svcwatch 的 tick，避免两个线程抢状态机。
 * 打印行（写入 docs/claims/svc.md）：
 *   svc: register ok / lookup ok / duplicate rejected / non-root register denied
 *   svc: dependency order ok / cycle rejected / missing requires skipped
 *   svc: heartbeat ok / heartbeat timeout -> unhealthy
 *   svc: restart on-failure ok / quarantine after limit ok
 *   svc: log write ok / selftest PASS
 * ============================================================================= */
#include "registry.h"
#include "ipc.h"
#include "include/kernel.h"
#include "include/klog.h"
#include "include/proc.h"

#define SVC_TEST_SERVICE   "testsvc"
#define SVC_TEST_SERVICE2  "testsvc2"
#define SVC_TEST_RESTART   "tsvc-restart"
#define SVC_TEST_QUAR      "tsvc-quar"

static const char svc_test_manifest[] =
    "name=" SVC_TEST_SERVICE "\n"
    "version=1.0.0\n"
    "type=service\n"
    "entry=hello\n"
    "requires=\n"
    "capabilities=IRQ:11,DMA:65536\n"
    "restart=never\n"
    "restart_limit=3\n"
    "uid=0\n";

static const char svc_test_manifest2[] =
    "name=" SVC_TEST_SERVICE2 "\n"
    "version=1.0.0\n"
    "type=service\n"
    "entry=hello\n"
    "requires=\n"
    "restart=never\n";

static const char svc_test_manifest_restart[] =
    "name=" SVC_TEST_RESTART "\n"
    "version=1.0.0\n"
    "type=service\n"
    "entry=hello\n"
    "requires=\n"
    "restart=on-failure\n"
    "restart_limit=3\n";

static const char svc_test_manifest_quar[] =
    "name=" SVC_TEST_QUAR "\n"
    "version=1.0.0\n"
    "type=service\n"
    "entry=nosuch-program\n"
    "requires=\n"
    "restart=on-failure\n"
    "restart_limit=3\n";

static const char svc_test_log_tag[] = "svclog";
static const char svc_test_log_msg[] = "svc selftest log line";

static int svc_ok(bool cond, const char *step, bool *ok)
{
    if (!cond) {
        kprintf("svc: selftest FAIL (%s)\n", step);
        *ok = false;
    }
    return cond ? 1 : 0;
}

/* int 0x81：与 ulib 的 syscall3 相同（eax=nr, ebx/ecx/edx）。 */
static int svc_sc(uint32_t nr, uint32_t a, uint32_t b, uint32_t c)
{
    int r;

    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(nr), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static void svc_test_node(svc_manifest_t *m, const char *name, const char *req)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->name, name, SVC_NAME_MAX - 1U);
    strncpy(m->entry, name, SVC_ENTRY_MAX - 1U);
    m->type = SVC_TYPE_SERVICE;
    m->restart_policy = SVC_RESTART_NEVER;
    m->restart_limit = SVC_RESTART_LIMIT_DEFAULT;
    if (req != 0 && req[0] != '\0') {
        strncpy(m->requires[0], req, SVC_NAME_MAX - 1U);
        m->req_count = 1U;
    }
}

static bool svc_test_nothing_provided(const char *name)
{
    (void)name;
    return false;       /* 合成表：依赖只能来自表内，用来测缺依赖/环 */
}

/* 期望内置拓扑序的前 5 个（设计 §5 的启动顺序示例）。 */
static bool svc_test_order_matches(char (*names)[SVC_NAME_MAX])
{
    static const char *expect[5] = { "logd", "fsd", "netd", "pkgd", "shell" };
    uint32_t i;

    for (i = 0; i < 5U; i++) {
        if (strcmp(names[i], expect[i]) != 0) return false;
    }
    return true;
}

void svc_selftest(void)
{
    proc_t *p = proc_current();
    svc_entry_t snap;
    bool ok = true;
    uint32_t saved_euid;
    int ports[6];
    int rc, i;

    if (p == 0) {
        kprintln("svc: selftest SKIP (no process context)");
        return;
    }
    for (i = 0; i < 6; i++) ports[i] = -1;
    svc_supervise_pause(true);

    /* 1) 注册：manifest 文本 -> 内核解析 -> 表项 */
    ports[0] = ipc_port_create(0U, p);
    svc_ok(ports[0] > 0, "create port", &ok);
    rc = svc_sc(SYS_SVC_REGISTER, (uint32_t)(uintptr_t)svc_test_manifest,
                (uint32_t)ports[0], 0U);
    if (svc_ok(rc == 0, "register", &ok)) {
        kprintf("svc: register ok (name=%s port=%d)\n", SVC_TEST_SERVICE,
                ports[0]);
    }

    /* 2) lookup 返回同一个 port */
    rc = svc_sc(SYS_SVC_LOOKUP, (uint32_t)(uintptr_t)SVC_TEST_SERVICE,
                (uint32_t)strlen(SVC_TEST_SERVICE), 0U);
    if (svc_ok(rc == ports[0], "lookup", &ok)) {
        kprintf("svc: lookup ok (name=%s port=%d)\n", SVC_TEST_SERVICE, rc);
    }

    /* 3) 重名注册被拒 */
    ports[1] = ipc_port_create(0U, p);
    rc = svc_sc(SYS_SVC_REGISTER, (uint32_t)(uintptr_t)svc_test_manifest,
                (uint32_t)ports[1], 0U);
    if (svc_ok(rc == -1, "duplicate", &ok)) {
        kprintln("svc: duplicate rejected");
    }

    /* 4) 非 root 注册被拒（临时把 euid 降到 1000，再还原） */
    saved_euid = p->euid;
    p->euid = 1000U;
    ports[2] = ipc_port_create(0U, p);
    rc = svc_sc(SYS_SVC_REGISTER, (uint32_t)(uintptr_t)svc_test_manifest2,
                (uint32_t)ports[2], 0U);
    p->euid = saved_euid;
    if (svc_ok(rc == -1, "non-root register", &ok)) {
        kprintln("svc: non-root register denied");
    }

    /* 5) 依赖顺序：内置 manifest 表按 requires 拓扑排序 */
    {
        char order[8][SVC_NAME_MAX];
        int n = svc_builtin_order(order, 8U);

        if (svc_ok(n >= 5 && svc_test_order_matches(order), "dependency order",
                   &ok)) {
            kprintln("svc: dependency order ok (logd->fsd->netd->pkgd->shell)");
        }
    }

    /* 6) 环：a -> b -> c -> a 必须被拒（返回 -1） */
    {
        svc_manifest_t cyc[3];
        uint32_t order[3], ordered = 0, skipped = 0;

        svc_test_node(&cyc[0], "cyc-a", "cyc-c");
        svc_test_node(&cyc[1], "cyc-b", "cyc-a");
        svc_test_node(&cyc[2], "cyc-c", "cyc-b");
        rc = svc_topo_sort(cyc, 3U, svc_test_nothing_provided, order, &ordered,
                           &skipped);
        if (svc_ok(rc == -1 && ordered == 0U, "cycle", &ok)) {
            kprintln("svc: cycle rejected");
        }
    }

    /* 6b) requires 缺失：该节点与依赖它的节点一起跳过（不启动、不报环） */
    {
        svc_manifest_t miss[2];
        uint32_t order[2], ordered = 0, skipped = 0;

        svc_test_node(&miss[0], "miss-a", "no-such-dep");
        svc_test_node(&miss[1], "miss-b", "miss-a");
        rc = svc_topo_sort(miss, 2U, svc_test_nothing_provided, order, &ordered,
                           &skipped);
        if (svc_ok(rc == 0 && ordered == 0U && skipped == 2U, "missing requires",
                   &ok)) {
            kprintf("svc: missing requires skipped (%u node(s))\n",
                    (unsigned)skipped);
        }
    }

    /* 7) 心跳：服务自身刷新成功 */
    rc = svc_sc(SYS_SVC_HEARTBEAT, (uint32_t)ports[0], 0U, 0U);
    if (svc_ok(rc == 0, "heartbeat", &ok)) {
        kprintln("svc: heartbeat ok (age=0 tick)");
    }

    /* 8) 心跳超时：把 now 推到超时之后，状态机把它标成 unhealthy */
    svc_supervise_step(timer_ticks() + SVC_HEARTBEAT_TIMEOUT_TICKS + 1U);
    rc = svc_registry_get(SVC_TEST_SERVICE, &snap);
    if (svc_ok(rc == 0 && snap.state == SVC_UNHEALTHY, "heartbeat timeout", &ok)) {
        kprintln("svc: heartbeat timeout -> unhealthy");
    }

    /* 9) on-failure 重启：模拟崩溃 -> 退避 -> spawn -> 新实例注册 -> 新 port */
    ports[3] = ipc_port_create(0U, p);
    rc = svc_sc(SYS_SVC_REGISTER, (uint32_t)(uintptr_t)svc_test_manifest_restart,
                (uint32_t)ports[3], 0U);
    svc_ok(rc == 0, "restart register", &ok);
    (void)svc_registry_note_failure(SVC_TEST_RESTART, "simulated crash");
    {
        uint32_t base = timer_ticks();
        uint32_t old_port = (uint32_t)ports[3];
        uint32_t new_pid = 0;
        int new_port = -1;

        /* 退避 1s 之后驱动一次 tick：spawn 同一个 entry（hello） */
        svc_supervise_step(base + 200U);
        rc = svc_registry_get(SVC_TEST_RESTART, &snap);
        if (svc_ok(rc == 0 && snap.state == SVC_STARTING && snap.pid != 0U,
                   "restart spawn", &ok)) {
            new_pid = snap.pid;
        }

        /* 重启后的“新实例”重新注册（新端口）：lookup 必须拿到新 port */
        ports[4] = ipc_port_create(0U, p);
        rc = svc_sc(SYS_SVC_REGISTER,
                    (uint32_t)(uintptr_t)svc_test_manifest_restart,
                    (uint32_t)ports[4], 0U);
        svc_ok(rc == 0, "restart re-register", &ok);
        new_port = svc_sc(SYS_SVC_LOOKUP,
                          (uint32_t)(uintptr_t)SVC_TEST_RESTART,
                          (uint32_t)strlen(SVC_TEST_RESTART), 0U);
        if (svc_ok(new_port == ports[4] && new_port != (int)old_port, "restart port",
                   &ok)) {
            kprintf("svc: restart on-failure ok (old port=%u new port=%d pid=%u)\n",
                    (unsigned)old_port, new_port, (unsigned)new_pid);
        }
        /* 旧端口已经回收：旧客户端会拿到 -1，必须重新 lookup */
        svc_ok(!ipc_port_exists(old_port), "old port released", &ok);
        svc_registry_remove(SVC_TEST_RESTART);
        if (ports[4] > 0) ipc_port_release((uint32_t)ports[4], p);
        ports[4] = -1;
    }

    /* 10) 隔离：spawn 持续失败（entry 不存在），1s/2s/4s 三次后 quarantined */
    ports[5] = ipc_port_create(0U, p);
    rc = svc_sc(SYS_SVC_REGISTER, (uint32_t)(uintptr_t)svc_test_manifest_quar,
                (uint32_t)ports[5], 0U);
    svc_ok(rc == 0, "quarantine register", &ok);
    (void)svc_registry_note_failure(SVC_TEST_QUAR, "simulated crash");
    {
        uint32_t base = timer_ticks();

        for (i = 0; i < 5; i++) {
            svc_supervise_step(base + 200U + (uint32_t)i * 500U);
        }
    }
    rc = svc_registry_get(SVC_TEST_QUAR, &snap);
    if (svc_ok(rc == 0 && snap.state == SVC_QUARANTINED && snap.fail_count == 4U,
               "quarantine", &ok)) {
        kprintf("svc: quarantine after limit ok (fail_count=%u state=%s)\n",
                (unsigned)snap.fail_count, svc_state_name(snap.state));
    }
    svc_ok(svc_sc(SYS_SVC_LOOKUP, (uint32_t)(uintptr_t)SVC_TEST_QUAR,
                  (uint32_t)strlen(SVC_TEST_QUAR), 0U) == -1,
           "quarantined lookup", &ok);

    /* 11) SYS_LOG_WRITE(93)：写带 tag 的 klog 行，再从环里读回来 */
    rc = svc_sc(SYS_LOG_WRITE, (uint32_t)(uintptr_t)svc_test_log_tag,
                (uint32_t)(uintptr_t)svc_test_log_msg,
                (uint32_t)strlen(svc_test_log_msg));
    if (svc_ok(rc == 0 && klog_contains(svc_test_log_msg), "log write", &ok)) {
        kprintln("svc: log write ok (tag=svclog)");
    }

    /* 清理：自检表项不留给 svcwatch，端口归还 */
    svc_registry_remove(SVC_TEST_SERVICE);
    svc_registry_remove(SVC_TEST_QUAR);
    for (i = 0; i < 6; i++) {
        if (ports[i] > 0) ipc_port_release((uint32_t)ports[i], p);
    }

    svc_supervise_pause(false);

    if (ok) kprintln("svc: selftest PASS");
    else    kprintln("svc: selftest FAIL");
}
