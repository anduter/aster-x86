/*
 * ng WP3-0: 进程/跨进程 IPC 自检接口。
 */
#ifndef NG_PROC_SELFTEST_H
#define NG_PROC_SELFTEST_H

/* 返回失败项数（0 = PASS）；证据行见 proc_selftest.c 文件头。 */
int proc_selftest(void);

#endif /* NG_PROC_SELFTEST_H */
