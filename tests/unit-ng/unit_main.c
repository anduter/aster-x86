/*
 * ng WP1-4/1-5/1-6/1-7/1-8 host 单测入口：PMM / heap / memmap / 调度器 /
 * 句柄/IPC / ELF64 校验与加载 / uaccess 页边界 / syscall 分发。
 *
 * 编译成本机 PE（Windows clang），不链接内核，也不需要 QEMU。
 */
#include <stdio.h>

#include "test_util.h"

int test_pmm(void);
int test_heap(void);
int test_memmap(void);
int test_sched(void);
int test_ipc(void);
int test_elf(void);
int test_uaccess(void);
int test_syscall(void);
int test_vfs(void);
int test_fat16(void);
int test_ata(void);
int test_drivers(void);
int test_auth(void);
int test_log(void);
int test_pkg(void);
int test_shell(void);
int test_proc(void);
int test_shm(void);
int test_desktop(void);
int test_parity(void);

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
    test_sched();
    test_ipc();
    test_elf();
    test_uaccess();
    test_syscall();
    test_vfs();
    test_fat16();
    test_ata();
    test_drivers();
    test_auth();
    test_log();
    test_pkg();
    test_shell();
    test_proc();
    test_shm();
    test_desktop();
    test_parity();

    printf("%d checks, %d failures\n", ut_checks, ut_failures);
    return (ut_failures == 0) ? 0 : 1;
}
