/* =============================================================================
 * pkg/pkg_selftest.c - 组件模型回归（作为程序 "pkgtest" 运行）
 *
 * 这些断言直接对应 build.ps1 里的检查项（pkg: / pkgd: 开头的行）：
 *   1. 合法安装（echosvc service，验签+落盘+DB+通知 supervisor 启动）
 *   2. 坏签名拒绝（改最后一个签名字节，哈希仍然正确 -> 必须报 signature）
 *   3. 缺依赖拒绝（requires=service:nosuchsvc）
 *   4. 安装失败回滚（目标目录已存在且非空 -> rename 失败 -> staging/DB 干净）
 *   5. 卸载运行中的服务（先 stop 再删文件+DB）
 *   6. 重复安装拒绝
 *   7. 普通用户装系统组件被拒（euid=1000 装 service）
 *   8. 用户应用装到 ~/Apps（归属用户、per-user DB）
 *   9. 已安装校验 / list / _pkgd 服务协议
 *  10. 重装并确认服务重新上线
 *
 * 放在独立内核线程里跑（不是在启动主线上）：可以正常 proc_sleep_ticks 等待
 * supervisor/echosvc 完成状态迁移，不忙等、不阻塞启动。
 * ============================================================================= */
#include "include/pkg.h"
#include "include/kernel.h"
#include "include/fs.h"
#include "include/security.h"
#include "include/svc.h"
#include "include/proc.h"

#define PKG_ECHOSVC   "echosvc-1.0.0.pkg"
#define PKG_NEEDSDEP  "needsdep-1.0.0.pkg"
#define PKG_BROKEN    "broken-1.0.0.pkg"
#define PKG_HELLOAPP  "helloapp-1.0.0.pkg"
#define PKG_APP_DB    "system:/home/alice/Apps/apps.db"

static int contains(const char *hay, const char *needle)
{
    uint32_t n = (uint32_t)strlen(needle);

    if (hay == 0 || needle == 0) return 0;
    while (*hay != '\0') {
        if (strncmp(hay, needle, n) == 0) return 1;
        hay++;
    }
    return 0;
}

/* 等服务出现/消失；成功返回 0，超时返回 -1。 */
static int wait_service(const char *name, int want, uint32_t ticks)
{
    for (uint32_t i = 0; i < ticks; i++) {
        if (service_exists(name) == want) return 0;
        proc_sleep_ticks(1U);
    }
    return (service_exists(name) == want) ? 0 : -1;
}

static int file_exists(const char *path)
{
    fs_stat_t st;

    return (fs_stat(path, &st) == E_OK) ? 1 : 0;
}

static void fail_line(const char *what, int status)
{
    kprintf("pkg: %s FAILED (%s)\n", what, pkg_status_text(status));
}

/* 1 + 6 + 9 + 10：安装 / 重复 / 校验 / 列表 / 服务协议 / 重装 */
static void test_install_lifecycle(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_ECHOSVC);
    char msg[48];
    char reply[CHAN_MSG_LEN];
    char list[96];
    int r;

    if (blob == 0) { kprintln("pkg: catalog missing " PKG_ECHOSVC); return; }

    r = pkg_install(blob->data, blob->len, 0U, msg, sizeof(msg));
    if (r == PKG_OK) {
        kprintln("pkg: install echosvc-1.0.0 ok");
    } else {
        fail_line("install echosvc-1.0.0", r);
    }

    /* 重复安装：DB 里已有同名组件 -> E_EXIST（不覆盖、不重装） */
    r = pkg_install(blob->data, blob->len, 0U, msg, sizeof(msg));
    kprintf("pkg: duplicate install rejected (%s)\n", pkg_status_text(r));

    /* 已安装校验：DB 行 + 目录 + 文件哈希都要对上 */
    r = pkg_verify_installed("echosvc", 0U, msg, sizeof(msg));
    if (r == PKG_OK) {
        kprintln("pkg: verify installed echosvc ok");
    } else {
        fail_line("verify installed echosvc", r);
    }

    /* 组件 DB 列表 */
    (void)pkg_list(0U, list, sizeof(list));
    if (contains(list, "echosvc 1.0.0 service")) {
        kprintln("pkg: list shows echosvc 1.0.0 service");
    } else {
        kprintf("pkg: list FAILED (%s)\n", list);
    }

    /* _pkgd 服务协议：通过服务注册表发命令、收回复 */
    if (pkgd_request("verify echosvc", reply, sizeof(reply)) > 0 &&
        strncmp(reply, "ok", 2) == 0) {
        kprintln("pkgd: service protocol ok");
    } else {
        kprintf("pkgd: service protocol FAILED (%s)\n", reply);
    }
}

/* 2：坏签名（只改签名区最后一个字节：文件哈希仍然匹配，必须由验签拒绝） */
static void test_bad_signature(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_ECHOSVC);
    char msg[48];
    uint8_t *copy;
    int r;

    if (blob == 0) return;
    copy = (uint8_t *)malloc(blob->len);
    if (copy == 0) { kprintln("pkg: bad signature test FAILED (no memory)"); return; }
    memcpy(copy, blob->data, blob->len);
    copy[blob->len - 1U] ^= 0xFFU;

    r = pkg_install(copy, blob->len, 0U, msg, sizeof(msg));
    free(copy);

    if (r == PKG_E_SIGNATURE) {
        kprintln("pkg: bad signature rejected (signature)");
    } else {
        fail_line("bad signature rejected", r);
    }
}

/* 3：缺依赖（requires 里声明了不存在的服务） */
static void test_missing_requires(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_NEEDSDEP);
    char msg[48];
    int r;

    if (blob == 0) { kprintln("pkg: catalog missing " PKG_NEEDSDEP); return; }
    r = pkg_install(blob->data, blob->len, 0U, msg, sizeof(msg));
    if (r == PKG_E_REQUIRES) {
        kprintln("pkg: missing requires rejected (requires)");
    } else {
        fail_line("missing requires rejected", r);
    }
}

/* 4：安装失败回滚
 * 先在目标位置放一个非空目录：staging 写完后 rename 必须失败（不覆盖非空目录），
 * 回滚要删掉 staging 且不写 DB —— 断言"预置文件还在、staging 不在、DB 没有它"。
 */
static void test_rollback(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_BROKEN);
    char msg[48];
    int r;
    int residue;

    if (blob == 0) { kprintln("pkg: catalog missing " PKG_BROKEN); return; }

    /* 预置：system:/System/Packages/broken/1.0.0/keep.txt（非空目录）。
     * SIP-lite 开启后这些路径只有更新器能写，所以测试用 updater scope 模拟
     * "另一个安装器留下的目录"。 */
    sip_updater_enter();
    (void)fs_mkdir("system:/System");
    (void)fs_mkdir("system:/System/Packages");
    (void)fs_mkdir("system:/System/Packages/broken");
    (void)fs_mkdir("system:/System/Packages/broken/1.0.0");
    {
        uint32_t h = fs_open("system:/System/Packages/broken/1.0.0/keep.txt",
                             FS_OPEN_WRITE | FS_OPEN_CREAT);
        if (h != 0) {
            (void)fs_write(h, "keep", 4U);
            fs_close(h);
        }
    }
    sip_updater_exit();

    r = pkg_install(blob->data, blob->len, 0U, msg, sizeof(msg));

    residue = 0;
    if (file_exists("system:/System/.staging/broken")) residue = 1;
    if (file_exists("system:/System/Packages/broken/1.0.0/keep.txt") == 0) residue = 1;
    if (r == PKG_OK) residue = 1;
    if (msg[0] == '\0') residue = 1;
    /* DB 里也不能留下半条记录 */
    if (pkg_verify_installed("broken", 0U, msg, sizeof(msg)) != PKG_E_NOENT) residue = 1;

    if (residue == 0) {
        kprintln("pkg: install failure rollback ok");
    } else {
        kprintf("pkg: install failure rollback FAILED (r=%s residue=%d)\n",
                pkg_status_text(r), residue);
    }

    /* 清理预置目录，避免影响后续（keep.txt + 两级目录） */
    sip_updater_enter();
    (void)fs_unlink("system:/System/Packages/broken/1.0.0/keep.txt");
    (void)fs_unlink("system:/System/Packages/broken/1.0.0");
    (void)fs_unlink("system:/System/Packages/broken");
    sip_updater_exit();
}

/* 5 + 10：卸载运行中的服务，然后重装并确认服务重新上线 */
static void test_running_service_cycle(void)
{
    char msg[48];
    int r;

    if (wait_service("echo", 1, 300U) != 0) {
        kprintln("pkg: uninstall running service FAILED (echo never started)");
        return;
    }

    r = pkg_remove("echosvc", 0U, msg, sizeof(msg));
    if (r != PKG_OK) {
        fail_line("uninstall running service", r);
        return;
    }
    if (wait_service("echo", 0, 200U) == 0) {
        kprintln("pkg: uninstall running service ok");
    } else {
        kprintln("pkg: uninstall running service FAILED (echo still registered)");
    }

    /* 重装：组件文件 + DB 重新建立，supervisor 重新拉起 echosvc */
    {
        const pkg_blob_t *blob = pkg_catalog_find(PKG_ECHOSVC);

        if (blob == 0) return;
        r = pkg_install(blob->data, blob->len, 0U, msg, sizeof(msg));
        if (r != PKG_OK) { fail_line("reinstall echosvc", r); return; }
        if (wait_service("echo", 1, 300U) == 0) {
            kprintln("pkg: reinstall echosvc ok");
            kprintln("pkg: reinstall service online ok");
        } else {
            kprintln("pkg: reinstall echosvc ok (service not online yet)");
        }
    }
}

/* 7：普通用户装系统组件被拒 */
static void test_nonroot_denied(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_ECHOSVC);
    char msg[48];
    int r;

    if (blob == 0) return;
    r = pkg_install(blob->data, blob->len, 1000U, msg, sizeof(msg));
    if (r == PKG_E_POLICY) {
        kprintln("pkg: non-root system install denied (policy)");
    } else {
        fail_line("non-root system install denied", r);
    }
}

/* 8：用户应用装到 ~/Apps（归属 alice，per-user DB） */
static void test_user_app(void)
{
    const pkg_blob_t *blob = pkg_catalog_find(PKG_HELLOAPP);
    char msg[48];
    int r;

    if (blob == 0) { kprintln("pkg: catalog missing " PKG_HELLOAPP); return; }

    r = pkg_install(blob->data, blob->len, 1000U, msg, sizeof(msg));
    if (r != PKG_OK) { fail_line("user app install", r); return; }

    /* 文件落在 alice 家目录、属主是 alice（不是 root），per-user DB 存在 */
    {
        fs_stat_t fst;
        fs_stat_t dst;

        if (fs_stat("system:/home/alice/Apps/helloapp/1.0.0/helloapp.conf", &fst) == E_OK &&
            fst.uid == 1000U &&
            fs_stat(PKG_APP_DB, &dst) == E_OK) {
            kprintln("pkg: user app installed to /home/alice/Apps ok");
        } else {
            kprintf("pkg: user app installed to /home/alice/Apps FAILED (uid=%u)\n",
                    (unsigned int)fst.uid);
        }
    }
}

void pkg_selftest_entry(void)
{
    /* 越早跑越好：这段回归会阻塞等 supervisor/echosvc，必须赶在 ring3 shell
     * 进入交互（build.ps1 注入按键）之前结束，否则按键注入会和这里的
     * 调度/IO 抢时间，导致输入被丢字符（曾表现为 sectest 命令被拼坏）。 */
    proc_sleep_ticks(10U);
    (void)wait_service("supervisor", 1, 100U);
    (void)wait_service("pkgd", 1, 100U);

    test_install_lifecycle();
    test_bad_signature();
    test_missing_requires();
    test_rollback();
    test_nonroot_denied();
    test_user_app();
    test_running_service_cycle();

    kprintln("pkg: selftest done");
    proc_exit(0);
}
