/*
 * ng WP1-5: 调度器自检入口（main.c 在 WP1-4 之后调用）。
 */
#ifndef NG_SCHED_SELFTEST_H
#define NG_SCHED_SELFTEST_H

/* 返回失败条数；0 = PASS。会打印 ng: sched * 证据行。 */
int sched_selftest(void);

#endif /* NG_SCHED_SELFTEST_H */
