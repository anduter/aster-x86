/* =============================================================================
 * core/bootargs.c - 内核参数（boot cmdline）
 *
 * 最小实现：编译期注入一个以空格分隔的 token 串（-DOS_TEST_BUILD / 以后的
 * 加载器命令行都能落在这里）。当前只解释 autologin=root：
 *   - 测试模式（-Test 构建，OS_TEST_BUILD=1）默认 "autologin=root"，
 *     bootseq 直接 spawn shell，跳过 login，保证既有回归不被打断；
 *   - 生产构建 cmdline 为空 -> spawn getty -> 必须登录。
 * ============================================================================= */
#include "../include/types.h"
#include "include/kernel.h"

#ifndef OS_TEST_BUILD
#define OS_TEST_BUILD 0
#endif

#ifndef OS_BOOT_CMDLINE
#if OS_TEST_BUILD
#define OS_BOOT_CMDLINE "autologin=root console=tty0"
#else
#define OS_BOOT_CMDLINE ""
#endif
#endif

/* 去掉首尾空格后的编译期参数串（永远不会是 NULL） */
const char *boot_cmdline(void)
{
    return OS_BOOT_CMDLINE;
}

/* token 是否作为独立单词出现在 cmdline 里（"autologin=root" 不会匹配
 * "xautologin=root"）。 */
bool boot_cmdline_has(const char *token)
{
    const char *p = OS_BOOT_CMDLINE;
    uint32_t n;

    if (token == 0 || token[0] == '\0') return false;
    n = (uint32_t)strlen(token);

    while (*p != '\0') {
        const char *start;

        while (*p == ' ') p++;
        if (*p == '\0') break;
        start = p;
        while (*p != '\0' && *p != ' ') p++;
        if ((uint32_t)(p - start) == n && strncmp(start, token, n) == 0) return true;
    }
    return false;
}

/* 测试模式自动登录 root：等价于内核参数 autologin=root */
bool boot_cmdline_autologin_root(void)
{
    return boot_cmdline_has("autologin=root");
}

uint32_t boot_test_build(void)
{
    return (uint32_t)OS_TEST_BUILD;
}
