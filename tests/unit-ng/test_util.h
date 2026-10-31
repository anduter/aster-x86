/*
 * ng WP1-4 host 单测：极简检查框架。
 *
 * 每个 CHECK 记一次检查；失败时打印表达式/文件/行号。最终 unit_main.c 打印
 * `N checks, M failures`，tools/unit-ng.ps1 解析这一行决定退出码。
 */
#ifndef NG_TEST_UTIL_H
#define NG_TEST_UTIL_H

extern int ut_checks;
extern int ut_failures;

void ut_check(int ok, const char *expr, const char *file, int line);

#define CHECK(cond) ut_check((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

#endif /* NG_TEST_UTIL_H */
