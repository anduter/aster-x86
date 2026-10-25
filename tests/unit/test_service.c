/* =============================================================================
 * tests/unit/test_service.c - svc/service.c（组件 manifest 校验）host 单元测试
 *
 * 编译时把内核字符串/kprintf 符号改名后由本文件提供实现；
 * 自旋锁与 port_connect 用测试桩，其余是真实 service.c 逻辑。
 * ========================================================================== */
#include "unit.h"
#include "../../src/kernel/core/error.h"
#include "../../src/kernel/svc/service.h"

/* 内核符号改名后的最小实现（见 tools/unit.ps1 的 -D 列表） */
unsigned int k_strlen(const char *s)
{
    unsigned int n = 0;

    while (s[n] != '\0') { n++; }
    return n;
}

int k_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *k_strncpy(char *dst, const char *src, unsigned int n)
{
    unsigned int i = 0;

    for (; i < n && src[i] != '\0'; i++) { dst[i] = src[i]; }
    for (; i < n; i++) { dst[i] = '\0'; }
    return dst;
}

void *k_memset(void *dst, int c, unsigned int n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}

void unit_kprintf(const char *fmt, ...) { (void)fmt; }

/* port 层桩：把 port 原样交给调用者，方便断言 connect 的语义 */
int port_connect(uint32_t port, uint32_t *client)
{
    if (client != 0) { *client = port; }
    return 0;
}

int main(void)
{
    service_manifest_t m;
    uint32_t client = 0;
    unsigned int i;
    char names[6][8];

    UNIT_BEGIN("service/manifest");

    /* 版本规则：0 = 旧格式按 v1；当前版本接受；未知版本拒绝 */
    CHECK_INT(service_manifest_version_ok(0U), E_OK);
    CHECK_INT(service_manifest_version_ok(SERVICE_MANIFEST_VERSION), E_OK);
    CHECK_INT(service_manifest_version_ok(SERVICE_MANIFEST_VERSION + 1U), E_INVAL);
    CHECK_INT(service_manifest_version_ok(0xFFFFFFFFU), E_INVAL);

    /* 参数校验 */
    CHECK_INT(service_manifest_check(0), E_INVAL);
    m.version = SERVICE_MANIFEST_VERSION;
    m.name = 0;
    m.port = 1;
    m.flags = 0;
    CHECK_INT(service_manifest_check(&m), E_INVAL);
    m.name = "";
    CHECK_INT(service_manifest_check(&m), E_INVAL);
    m.name = "svc";
    m.flags = 1;
    CHECK_INT(service_manifest_check(&m), E_INVAL);
    m.flags = 0;
    m.version = SERVICE_MANIFEST_VERSION + 1U;
    CHECK_INT(service_manifest_check(&m), E_INVAL);
    m.version = SERVICE_MANIFEST_VERSION;
    CHECK_INT(service_manifest_check(&m), E_OK);

    /* 注册 / 查重 / 查找 */
    CHECK_INT(service_register("demo", 42U), E_OK);
    CHECK_INT(service_register("demo", 43U), E_EXIST);
    CHECK(service_exists("demo") == 1);
    CHECK(service_exists("nope") == 0);
    CHECK(service_exists(0) == 0);

    CHECK_INT(service_connect("demo", &client), E_OK);
    CHECK_U32(client, 42U);
    CHECK_INT(service_connect("nope", &client), SERVICE_E_NOENT);
    CHECK_INT(service_connect(0, &client), E_INVAL);
    CHECK_INT(service_connect("demo", 0), E_INVAL);

    /* manifest 注册：合法写入；非法拒绝 */
    m.name = "alpha";
    m.port = 7;
    CHECK_INT(service_register_manifest(&m), E_OK);
    CHECK(service_exists("alpha") == 1);
    m.name = "";
    CHECK_INT(service_register_manifest(&m), E_INVAL);
    m.name = "beta";
    m.flags = 2;
    CHECK_INT(service_register_manifest(&m), E_INVAL);
    m.flags = 0;

    /* 表满：SERVICE_MAX = 8 -> 再注册 6 个后必须 E_FULL */
    for (i = 0; i < 6U; i++) {
        names[i][0] = 'n';
        names[i][1] = (char)('0' + i);
        names[i][2] = '\0';
        CHECK_INT(service_register(names[i], 100U + i), E_OK);
    }
    CHECK_INT(service_register("overflow", 999U), E_FULL);

    /* 名字过长（>= SERVICE_NAME）拒绝 */
    {
        char longname[SERVICE_NAME + 1U];
        for (i = 0; i < SERVICE_NAME; i++) { longname[i] = 'x'; }
        longname[SERVICE_NAME] = '\0';
        CHECK_INT(service_register(longname, 1U), E_INVAL);
    }

    /* 注销：同名再次注册成功；按端口注销不影响别的项 */
    service_unregister(42U);
    CHECK(service_exists("demo") == 0);
    CHECK(service_exists("alpha") == 1);
    CHECK_INT(service_register("demo", 44U), E_OK);
    /* 表已满：manifest 注册合法但重名 -> E_EXIST（不是 E_FULL） */
    m.name = "alpha";
    CHECK_INT(service_register_manifest(&m), E_EXIST);

    UNIT_END();
}