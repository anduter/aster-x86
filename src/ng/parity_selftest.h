/*
 * ng WP2-7: parity 套件内核入口（100 项）。
 *
 * 由 main.c 在所有 WP 自检之后、ring3 demo 之前调用；逐项打印
 * `parity: NNN/100 [域] 描述 ... ok`，最后打印
 * `ng: parity 100/100 PASS`（run-ng / tools/parity-ng.ps1 断言这一行）。
 */
#ifndef NG_PARITY_SELFTEST_H
#define NG_PARITY_SELFTEST_H

/* 跑 100 项 parity；返回失败项数（0 = 全过）。 */
int parity_selftest(void);

#endif /* NG_PARITY_SELFTEST_H */
