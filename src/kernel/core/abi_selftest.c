/* =============================================================================
 * core/abi_selftest.c - 用户态 ABI / 三类 API 版本的启动自检
 *
 * 只做三件事：
 *   1. 通过 int 0x81 调 SYS_ABI_VERSION，核对 (版本 << 16) | feature bits；
 *   2. 核对 include/abi.h 里的结构体大小（断言在编译期，这里打印出来）；
 *   3. 用纯判定函数核对 device / fs_backend / service manifest 三套
 *      API 版本规则：0 与当前版本接受，未知版本拒绝。
 *
 * 不做副作用注册；失败只打印一行 FAILED，由既有回归项覆盖的功能路径
 * 不变（本自检不新增 build.ps1 检查项）。
 * ============================================================================= */
#include "../types.h"
#include "include/abi.h"
#include "include/dev.h"
#include "include/fs.h"
#include "include/kernel.h"
#include "include/svc.h"

static int abi_call(int n, int a, int b, int c)
{
    int r;

    __asm__ volatile ("int $0x81"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

void abi_selftest(void)
{
    int r = abi_call((int)SYS_ABI_VERSION, 0, 0, 0);
    uint32_t raw = (uint32_t)r;
    uint32_t ver = (raw >> 16) & 0xFFFFU;
    uint32_t feat = raw & 0xFFFFU;
    int ok = (ver == OS_ABI_VERSION) && (feat == OS_ABI_FEATURES);
    int dev_ok = (dev_count() > 0U);

    /* 所有已注册设备都必须带当前 ops 版本（dev_register 自动写入）。 */
    for (uint32_t i = 0; i < dev_count(); i++) {
        const device_t *d = dev_at(i);

        if (d == 0 || d->api_version != DEV_API_VERSION) dev_ok = 0;
    }

    /* 纯判定：0 = 旧格式按 v1 接受；未知版本必须 E_INVAL。 */
    int fs_ok = (fs_backend_version_ok(0U) == E_OK) &&
                (fs_backend_version_ok(FS_BACKEND_API_VERSION) == E_OK) &&
                (fs_backend_version_ok(FS_BACKEND_API_VERSION + 1U) == E_INVAL);
    int svc_ok = (service_manifest_version_ok(0U) == E_OK) &&
                 (service_manifest_version_ok(SERVICE_MANIFEST_VERSION) == E_OK) &&
                 (service_manifest_version_ok(SERVICE_MANIFEST_VERSION + 1U) == E_INVAL);

    if (ok && dev_ok && fs_ok && svc_ok) {
        kprintf("abi: version ok magic=%x abi=%u.%u features=%x "
                "structs=udp_req:%u,stat:%u,mouse:%u,resinfo:%u "
                "api=dev%u/fs%u/svc%u\n",
                (unsigned int)OS_ABI_MAGIC,
                (unsigned int)OS_ABI_VERSION_MAJOR,
                (unsigned int)OS_ABI_VERSION_MINOR,
                (unsigned int)feat,
                (unsigned int)sizeof(struct udp_req),
                (unsigned int)sizeof(sys_stat_t),
                (unsigned int)sizeof(sys_mouse_t),
                (unsigned int)sizeof(sys_resinfo_t),
                (unsigned int)DEV_API_VERSION,
                (unsigned int)FS_BACKEND_API_VERSION,
                (unsigned int)SERVICE_MANIFEST_VERSION);
    } else {
        kprintf("abi: version FAILED (r=%d ver=%u feat=%x dev=%d fs=%d svc=%d)\n",
                r, (unsigned int)ver, (unsigned int)feat,
                dev_ok, fs_ok, svc_ok);
    }
}
