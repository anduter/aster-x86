/*
 * ng WP1-4 host 单测入口：PMM / heap / memmap 三组纯逻辑测试。
 *
 * 编译成本机 PE（Windows clang），不链接内核，也不需要 QEMU。
 */
#include <stdio.h>

#include "test_util.h"

int test_pmm(void);
int test_heap(void);
int test_memmap(void);

int ut_checks;
int ut_failures;

void ut_check(int ok, const char *expr, const char *file, int line)
{
    ut_checks++;
    if (!ok) {
        ut_failures++;
        printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

int main(void)
{
    test_pmm();
    test_heap();
    test_memmap();

    printf("%d checks, %d failures\n", ut_checks, ut_failures);
    return (ut_failures == 0) ? 0 : 1;
}
